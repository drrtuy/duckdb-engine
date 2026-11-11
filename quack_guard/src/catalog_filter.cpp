/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov <leontodys@gmail.com>

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

/*
  Catalog discovery for Quack clients. When a client attaches the server
  (ATTACH 'quack:...'), it loads the catalog with two fixed queries
  (QuackSchemaSet/QuackTableSet::GetLoadQuery()). The listener recognizes
  exactly those and runs mariadb_quack_catalog() instead: it executes the
  original query on a separate connection and returns only the rows the
  session's principal may see (QuackAuthorizer::filterVisible). Same columns
  and types as the original, so the client cannot tell the difference.
*/

#include "allowlist.hpp"
#include "guard_internal.hpp"

#include "storage/quack_schema.hpp"
#include "storage/quack_table.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <map>
#include <set>
#include <tuple>

namespace quack_guard
{

using namespace duckdb;

const char *const CATALOG_FUNCTION= "mariadb_quack_catalog";

namespace
{

struct CatalogFunctionInfo : public TableFunctionInfo
{
  std::weak_ptr<GuardImpl> guard;
};

struct CatalogBindData : public TableFunctionData
{
  std::vector<std::vector<Value>> rows;
};

struct CatalogScanState : public GlobalTableFunctionState
{
  idx_t next= 0;
};

/* The engine's tables in top-level schemas of the default database, with
   the client's three columns first (as in QuackTableSet::GetLoadQuery())
   and what filtering needs after them */
const char *const TABLES_QUERY=
    "SELECT t.schema_oid, t.sql, 'table', t.schema_name, t.table_name "
    "FROM duckdb_tables() t JOIN duckdb_schemas() s ON t.schema_oid = s.oid "
    "WHERE t.database_name = current_database() "
    "AND s.parent_schema_oid IS NULL";

struct Rows
{
  vector<LogicalType> types;
  vector<Identifier> names;
  std::vector<std::vector<Value>> rows;
};

Rows run(DatabaseInstance &db, const std::string &sql)
{
  Connection con(db);
  auto result= con.Query(sql);
  if (result->HasError())
    result->ThrowError();
  Rows out;
  out.types= result->GetTypes();
  out.names= result->GetNames();
  while (auto chunk= result->Fetch())
  {
    if (chunk->size() == 0)
      break;
    for (idx_t r= 0; r < chunk->size(); r++)
    {
      std::vector<Value> row;
      for (idx_t c= 0; c < chunk->ColumnCount(); c++)
        row.push_back(chunk->GetValue(c, r));
      out.rows.push_back(std::move(row));
    }
  }
  return out;
}

using Key= std::tuple<int, std::string, std::string>;  /* kind, schema, table */

Key key_of(const ObjectName &o)
{
  return Key((int) o.kind, o.schema, o.table);
}

/* What the principal may see of `asked`: only asked objects count, whatever
   the authorizer returns. */
std::set<Key> visible(GuardImpl *guard, ClientContext &context,
                      const std::vector<ObjectName> &asked)
{
  std::set<Key> result;
  auto identity= GetPrincipal(context);
  auto authorizer= guard ? guard->GetAuthorizer() : nullptr;
  if (!identity || !authorizer || asked.empty())
    return result;
  std::set<Key> asked_keys;
  for (auto &o : asked)
    asked_keys.insert(key_of(o));
  try
  {
    for (auto &o : authorizer->filterVisible(*identity, asked))
      if (asked_keys.count(key_of(o)))
        result.insert(key_of(o));
  }
  catch (...)
  {
    result.clear();
  }
  return result;
}

/* Every top-level, non-reserved schema of the default database that the
   principal may see, plus "main". Raw oids would reveal how many objects
   others created: replaced by dense ordinals (main is 1). */
struct VisibleSchemas
{
  std::vector<std::vector<Value>> rows;      /* the client's schema rows */
  std::map<int64_t, int64_t> ordinal_of;     /* server oid -> ordinal */
  std::map<int64_t, std::string> name_of;    /* server oid -> schema */
};

VisibleSchemas visible_schemas(GuardImpl *guard, ClientContext &context,
                               Rows &result, const std::string &database)
{
  /* columns: oid, database_name, schema_path, current_database() */
  if (result.types.size() != 4)
    throw PermissionException(ACCESS_DENIED);
  VisibleSchemas out;
  std::vector<std::vector<Value>> candidates;
  std::vector<ObjectName> asked;
  std::vector<Value> main_row;
  for (auto &row : result.rows)
  {
    if (row[1].IsNull() || row[2].IsNull() || row[1].ToString() != database)
      continue;
    auto &path= ListValue::GetChildren(row[2]);
    if (path.size() != 1)
      continue;  /* nested schemas */
    auto schema= path[0].ToString();
    if (schema == "main")
      main_row= row;
    else if (!SchemaReserved(schema))
    {
      asked.push_back({ObjectKind::Schema, database, schema, "", ""});
      candidates.push_back(row);
    }
  }
  auto seen= visible(guard, context, asked);
  int64_t next= 1;
  auto add= [&](std::vector<Value> row, const std::string &schema) {
    int64_t oid= row[0].GetValue<int64_t>();
    out.ordinal_of[oid]= next;
    out.name_of[oid]= schema;
    row[0]= Value::Numeric(row[0].type(), next++);
    out.rows.push_back(std::move(row));
  };
  if (!main_row.empty())
    add(main_row, "main");
  for (size_t i= 0; i < candidates.size(); i++)
    if (seen.count(key_of(asked[i])))
      add(candidates[i], asked[i].schema);
  return out;
}

unique_ptr<FunctionData> catalog_bind(ClientContext &context,
                                      TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types,
                                      vector<Identifier> &names)
{
  auto kind= input.inputs[0].IsNull() ? std::string()
                                      : input.inputs[0].GetValue<std::string>();
  if (kind != "schemas" && kind != "tables")
    throw PermissionException(ACCESS_DENIED);
  auto *info= dynamic_cast<CatalogFunctionInfo *>(input.info.get());
  auto guard_ref= info ? info->guard.lock() : nullptr;
  auto *guard= guard_ref.get();
  auto &db= *context.db;
  std::string database=
      DatabaseManager::GetDefaultDatabase(context).GetIdentifierName();
  auto data= make_uniq<CatalogBindData>();

  auto schema_result= run(db, QuackSchemaSet::GetLoadQuery());
  auto schemas= visible_schemas(guard, context, schema_result, database);
  if (kind == "schemas")
  {
    return_types= schema_result.types;
    names= schema_result.names;
    data->rows= std::move(schemas.rows);
    return std::move(data);
  }

  auto result= run(db, TABLES_QUERY);
  if (result.types.size() != 5)
    throw PermissionException(ACCESS_DENIED);
  for (idx_t i= 0; i < 3; i++)
  {
    return_types.push_back(result.types[i]);
    names.push_back(result.names[i]);
  }
  std::vector<std::vector<Value>> candidates;
  std::vector<ObjectName> asked;
  for (auto &row : result.rows)
  {
    auto oid= row[0].GetValue<int64_t>();
    auto schema= schemas.name_of.find(oid);
    /* Only tables of listed schemas other than main */
    if (schema == schemas.name_of.end() || schema->second == "main")
      continue;
    auto table= row[4].ToString();
    if (TableReserved(table))
      continue;
    asked.push_back({ObjectKind::Table, database, schema->second, table, ""});
    candidates.push_back({Value::Numeric(row[0].type(), schemas.ordinal_of[oid]),
                          row[1], row[2]});
  }
  /* Views are not reachable in milestone 1 and are never listed */
  auto seen= visible(guard, context, asked);
  for (size_t i= 0; i < candidates.size(); i++)
    if (seen.count(key_of(asked[i])))
      data->rows.push_back(std::move(candidates[i]));
  return std::move(data);
}

unique_ptr<GlobalTableFunctionState> catalog_init(ClientContext &,
                                                  TableFunctionInitInput &)
{
  return make_uniq<CatalogScanState>();
}

void catalog_scan(ClientContext &, TableFunctionInput &input, DataChunk &output)
{
  auto &data= input.bind_data->Cast<CatalogBindData>();
  auto &state= input.global_state->Cast<CatalogScanState>();
  idx_t count= 0;
  while (state.next < data.rows.size() && count < STANDARD_VECTOR_SIZE)
  {
    auto &row= data.rows[state.next++];
    for (idx_t c= 0; c < row.size(); c++)
      output.data[c].SetValue(count, row[c]);
    count++;
  }
  output.SetChildCardinality(count);
}

} /* namespace */

void RegisterCatalogFunction(DatabaseInstance &db,
                             const std::shared_ptr<GuardImpl> &guard)
{
  TableFunction fn(CATALOG_FUNCTION, {LogicalType::VARCHAR}, catalog_scan,
                   catalog_bind, catalog_init);
  auto info= make_shared_ptr<CatalogFunctionInfo>();
  info->guard= guard;
  fn.function_info= std::move(info);
  CreateTableFunctionInfo create(std::move(fn));
  create.on_conflict= OnCreateConflict::ERROR_ON_CONFLICT;
  auto &catalog= Catalog::GetSystemCatalog(db);
  auto transaction= CatalogTransaction::GetSystemTransaction(db);
  catalog.CreateFunction(transaction, create);
}

bool CatalogQuery(const std::string &sql, std::string &replacement)
{
  if (sql == QuackSchemaSet::GetLoadQuery())
    replacement= std::string("FROM ") + CATALOG_FUNCTION + "('schemas')";
  else if (sql == QuackTableSet::GetLoadQuery())
    replacement= std::string("FROM ") + CATALOG_FUNCTION + "('tables')";
  else
    return false;
  return true;
}

} /* namespace quack_guard */
