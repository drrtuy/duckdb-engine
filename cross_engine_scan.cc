/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_class.h"
#include "field.h"
#include "handler.h"
#include "mysqld.h"
#include "log.h"

#undef UNKNOWN

#include "cross_engine_scan.h"
#include "ddl_convertor.h"
#include "duckdb_log.h"

#include "duckdb/main/database.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

namespace myduck
{

/* ----------------------------------------------------------------
   Thread-local registry of external (non-DuckDB) tables
   ---------------------------------------------------------------- */

static thread_local std::unordered_map<std::string, TABLE *>
    tls_external_tables;

void register_external_table(const std::string &name, TABLE *table)
{
  tls_external_tables[name]= table;
}

void clear_external_tables() { tls_external_tables.clear(); }

TABLE *find_external_table(const std::string &name)
{
  auto it= tls_external_tables.find(name);
  if (it != tls_external_tables.end())
    return it->second;
  return nullptr;
}

/* ----------------------------------------------------------------
   MariaDB Field → DuckDB LogicalType mapping
   ---------------------------------------------------------------- */

static duckdb::LogicalType field_to_logical_type(const Field *field)
{
  bool is_unsigned= (field->flags & UNSIGNED_FLAG) != 0;

  switch (field->real_type())
  {
  case MYSQL_TYPE_TINY:
    return is_unsigned ? duckdb::LogicalType::UTINYINT
                       : duckdb::LogicalType::TINYINT;
  case MYSQL_TYPE_SHORT:
    return is_unsigned ? duckdb::LogicalType::USMALLINT
                       : duckdb::LogicalType::SMALLINT;
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONG:
    return is_unsigned ? duckdb::LogicalType::UINTEGER
                       : duckdb::LogicalType::INTEGER;
  case MYSQL_TYPE_LONGLONG:
    return is_unsigned ? duckdb::LogicalType::UBIGINT
                       : duckdb::LogicalType::BIGINT;
  case MYSQL_TYPE_FLOAT:
    return duckdb::LogicalType::FLOAT;
  case MYSQL_TYPE_DOUBLE:
    return duckdb::LogicalType::DOUBLE;
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL: {
    auto *df= static_cast<const Field_new_decimal *>(field);
    uint prec= df->precision > 38 ? 38 : df->precision;
    return duckdb::LogicalType::DECIMAL(prec, df->dec);
  }
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_NEWDATE:
    return duckdb::LogicalType::DATE;
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_TIME2:
    return duckdb::LogicalType::TIME;
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_DATETIME2:
    return duckdb::LogicalType::TIMESTAMP;
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_TIMESTAMP2:
    return duckdb::LogicalType::TIMESTAMP_TZ;
  case MYSQL_TYPE_YEAR:
    return duckdb::LogicalType::INTEGER;
  case MYSQL_TYPE_BIT:
  case MYSQL_TYPE_GEOMETRY:
    return duckdb::LogicalType::BLOB;
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
    return field->has_charset() ? duckdb::LogicalType::VARCHAR
                                : duckdb::LogicalType::BLOB;
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_SET:
  case MYSQL_TYPE_ENUM:
    return duckdb::LogicalType::VARCHAR;
  default:
    return duckdb::LogicalType::VARCHAR;
  }
}

/* ----------------------------------------------------------------
   Read a MariaDB Field value → duckdb::Value
   ---------------------------------------------------------------- */

static duckdb::Value field_to_duckdb_value(Field *field)
{
  if (field->is_null())
    return duckdb::Value();

  switch (field->real_type())
  {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_YEAR: {
    if (field->is_unsigned())
      return duckdb::Value::UBIGINT(field->val_uint());
    return duckdb::Value::BIGINT(field->val_int());
  }
  case MYSQL_TYPE_FLOAT: {
    float v;
    v= static_cast<float>(field->val_real());
    return duckdb::Value::FLOAT(v);
  }
  case MYSQL_TYPE_DOUBLE:
    return duckdb::Value::DOUBLE(field->val_real());
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL: {
    String buf;
    field->val_str(&buf);
    return duckdb::Value(std::string(buf.ptr(), buf.length()));
  }
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_NEWDATE:
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_TIME2:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_DATETIME2:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_TIMESTAMP2: {
    String buf;
    field->val_str(&buf);
    return duckdb::Value(std::string(buf.ptr(), buf.length()));
  }
  case MYSQL_TYPE_BIT:
  case MYSQL_TYPE_GEOMETRY: {
    String buf;
    field->val_str(&buf);
    return duckdb::Value::BLOB(std::string(buf.ptr(), buf.length()));
  }
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB: {
    String buf;
    field->val_str(&buf);
    if (field->has_charset())
      return duckdb::Value(std::string(buf.ptr(), buf.length()));
    return duckdb::Value::BLOB(std::string(buf.ptr(), buf.length()));
  }
  default: {
    String buf;
    field->val_str(&buf);
    return duckdb::Value(std::string(buf.ptr(), buf.length()));
  }
  }
}

/* ----------------------------------------------------------------
   DuckDB table function: _mdb_scan
   Wraps ha_rnd_init / ha_rnd_next on a MariaDB TABLE*.
   ---------------------------------------------------------------- */

struct MdbScanBindData : duckdb::FunctionData
{
  std::string table_key;
  TABLE *table= nullptr;

  duckdb::unique_ptr<duckdb::FunctionData> Copy() const override
  {
    auto copy= duckdb::make_uniq<MdbScanBindData>();
    copy->table_key= table_key;
    copy->table= table;
    return duckdb::unique_ptr<duckdb::FunctionData>(std::move(copy));
  }

  bool Equals(const duckdb::FunctionData &other) const override
  {
    return table_key == other.Cast<MdbScanBindData>().table_key;
  }
};

struct MdbScanGlobalState : duckdb::GlobalTableFunctionState
{
  bool scan_started= false;
  bool finished= false;
  TABLE *table= nullptr;
  duckdb::vector<duckdb::idx_t> column_ids;

  idx_t MaxThreads() const override { return 1; }
};

static duckdb::unique_ptr<duckdb::FunctionData>
mdb_scan_bind(duckdb::ClientContext &context,
              duckdb::TableFunctionBindInput &input,
              duckdb::vector<duckdb::LogicalType> &return_types,
              duckdb::vector<duckdb::string> &names)
{
  auto key= input.inputs[0].GetValue<std::string>();

  TABLE *tbl= find_external_table(key);
  if (!tbl)
    throw duckdb::BinderException("_mdb_scan: table '%s' not found in "
                                  "external table registry",
                                  key.c_str());

  for (Field **f= tbl->field; *f; f++)
  {
    names.push_back((*f)->field_name.str);
    return_types.push_back(field_to_logical_type(*f));
  }

  auto data= duckdb::make_uniq<MdbScanBindData>();
  data->table_key= key;
  data->table= tbl;
  return duckdb::unique_ptr<duckdb::FunctionData>(std::move(data));
}

static duckdb::unique_ptr<duckdb::GlobalTableFunctionState>
mdb_scan_init_global(duckdb::ClientContext &context,
                     duckdb::TableFunctionInitInput &input)
{
  auto &bind_data= input.bind_data->Cast<MdbScanBindData>();
  auto state= duckdb::make_uniq<MdbScanGlobalState>();
  state->table= bind_data.table;
  state->column_ids= input.column_ids;
  return duckdb::unique_ptr<duckdb::GlobalTableFunctionState>(
      std::move(state));
}

static void mdb_scan_function(duckdb::ClientContext &context,
                              duckdb::TableFunctionInput &input,
                              duckdb::DataChunk &output)
{
  auto &state= input.global_state->Cast<MdbScanGlobalState>();

  if (state.finished)
  {
    output.SetCardinality(0);
    return;
  }

  TABLE *tbl= state.table;
  if (!tbl)
  {
    output.SetCardinality(0);
    state.finished= true;
    return;
  }

  /* Adopt the MariaDB THD on this DuckDB worker thread so that
     handler assertions (table->in_use == current_thd) pass. */
  THD *prev_thd= _current_thd();
  if (tbl->in_use && tbl->in_use != prev_thd)
    set_current_thd(tbl->in_use);

  if (!state.scan_started)
  {
    bitmap_clear_all(tbl->read_set);
    for (auto col_idx : state.column_ids)
      bitmap_set_bit(tbl->read_set, static_cast<uint>(col_idx));

    if (tbl->file->ha_rnd_init(true))
    {
      state.finished= true;
      output.SetCardinality(0);
      if (_current_thd() != prev_thd)
        set_current_thd(prev_thd);
      return;
    }
    state.scan_started= true;
  }

  duckdb::idx_t count= 0;
  duckdb::idx_t ncols= state.column_ids.size();

  while (count < STANDARD_VECTOR_SIZE)
  {
    int err= tbl->file->ha_rnd_next(tbl->record[0]);
    if (err)
    {
      tbl->file->ha_rnd_end();
      state.finished= true;
      break;
    }

    for (duckdb::idx_t i= 0; i < ncols; i++)
    {
      Field *field= tbl->field[state.column_ids[i]];
      duckdb::Value val= field_to_duckdb_value(field);
      output.data[i].SetValue(count, val);
    }
    count++;
  }

  output.SetCardinality(count);

  if (_current_thd() != prev_thd)
    set_current_thd(prev_thd);
}

/* ----------------------------------------------------------------
   Replacement scan callback
   ---------------------------------------------------------------- */

duckdb::unique_ptr<duckdb::TableRef> mariadb_replacement_scan(
    duckdb::ClientContext &context, duckdb::ReplacementScanInput &input,
    duckdb::optional_ptr<duckdb::ReplacementScanData> data)
{
  TABLE *tbl= find_external_table(input.table_name);
  if (!tbl)
    return nullptr;

  auto ref= duckdb::make_uniq<duckdb::TableFunctionRef>();

  duckdb::vector<duckdb::unique_ptr<duckdb::ParsedExpression>> children;
  children.push_back(duckdb::make_uniq<duckdb::ConstantExpression>(
      duckdb::Value(input.table_name)));

  ref->function= duckdb::make_uniq<duckdb::FunctionExpression>(
      "_mdb_scan", std::move(children));
  ref->alias= input.table_name;

  if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY)
    sql_print_information(
        "DuckDB cross-engine: replacement scan redirected '%s' to _mdb_scan",
        input.table_name.c_str());

  return duckdb::unique_ptr<duckdb::TableRef>(std::move(ref));
}

/* ----------------------------------------------------------------
   Registration
   ---------------------------------------------------------------- */

void register_cross_engine_scan(duckdb::DatabaseInstance &db)
{
  duckdb::TableFunction mdb_scan("_mdb_scan", {duckdb::LogicalType::VARCHAR},
                                 mdb_scan_function, mdb_scan_bind,
                                 mdb_scan_init_global);
  mdb_scan.projection_pushdown= true;
  mdb_scan.filter_pushdown= false;

  duckdb::CreateTableFunctionInfo info(std::move(mdb_scan));
  auto &catalog= duckdb::Catalog::GetSystemCatalog(db);
  auto transaction= duckdb::CatalogTransaction::GetSystemTransaction(db);
  catalog.CreateFunction(transaction, info);

  auto &config= duckdb::DBConfig::GetConfig(db);
  config.replacement_scans.emplace_back(mariadb_replacement_scan);

  sql_print_information("DuckDB: cross-engine scan registered "
                        "(_mdb_scan + replacement scan)");
}

} /* namespace myduck */
