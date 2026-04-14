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
#include <algorithm>
#include <my_global.h>
#include "sql_class.h"
#include "sql_select.h"
#include "log.h"

#undef UNKNOWN

#include "ha_duckdb_pushdown.h"
#include "duckdb_select.h"
#include "duckdb_error.h"
#include "duckdb_query.h"
#include "duckdb_context.h"
#include "cross_engine_scan.h"
#include "duckdb_log.h"

extern handlerton *duckdb_hton;

/**
  Check whether a SELECT_LEX can be pushed down to DuckDB.

  Returns true if at least one table is DuckDB.  Non-DuckDB tables are
  collected in @p external_tables so they can be registered with the
  replacement-scan mechanism before executing the DuckDB query.
*/

static bool can_pushdown_to_duckdb(SELECT_LEX *sel_lex,
                                   std::vector<std::string> &external_tables,
                                   bool &has_duckdb_table)
{
  for (TABLE_LIST *tbl= sel_lex->get_table_list(); tbl; tbl= tbl->next_global)
  {
    if (tbl->derived || !tbl->table)
      continue;

    if (tbl->table->file->ht == duckdb_hton)
      has_duckdb_table= true;
    else
      external_tables.emplace_back(tbl->table_name.str);
  }

  return has_duckdb_table;
}

/**
  Check whether a SELECT_LEX_UNIT (UNION/EXCEPT/INTERSECT) can be
  pushed down to DuckDB.  Walks every SELECT_LEX in the unit.
*/

static bool
can_pushdown_unit_to_duckdb(SELECT_LEX_UNIT *unit,
                            std::vector<std::string> &external_tables,
                            bool &has_duckdb_table)
{
  for (SELECT_LEX *sl= unit->first_select(); sl; sl= sl->next_select())
  {
    for (TABLE_LIST *tbl= sl->get_table_list(); tbl; tbl= tbl->next_global)
    {
      if (tbl->derived || !tbl->table)
        continue;

      if (tbl->table->file->ht == duckdb_hton)
        has_duckdb_table= true;
      else
        external_tables.emplace_back(tbl->table_name.str);
    }
  }

  return has_duckdb_table;
}

/* ----- Factory functions ----- */

select_handler *create_duckdb_select_handler(THD *thd, SELECT_LEX *sel_lex,
                                             SELECT_LEX_UNIT *sel_unit)
{
  /*
    Only handle plain SELECT and INSERT ... SELECT for now.
  */
  if (thd->lex->sql_command != SQLCOM_SELECT &&
      thd->lex->sql_command != SQLCOM_INSERT_SELECT)
    return nullptr;

  if (!sel_lex)
    return nullptr;

  std::vector<std::string> external_tables;
  bool has_duckdb_table= false;

  if (!can_pushdown_to_duckdb(sel_lex, external_tables, has_duckdb_table))
    return nullptr;

  /* At least one DuckDB table must participate */
  if (!has_duckdb_table)
    return nullptr;

  /* Do not push down queries with side-effects (e.g. user variables) */
  if (sel_lex->uncacheable & UNCACHEABLE_SIDEEFFECT)
    return nullptr;

  auto *handler= new ha_duckdb_select_handler(thd, sel_lex, sel_unit);
  if (!external_tables.empty())
  {
    handler->set_cross_engine(std::move(external_tables));

    if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY)
      sql_print_information("DuckDB: cross-engine pushdown with %zu "
                            "external table(s)",
                            handler->external_table_count());
  }
  return handler;
}

select_handler *create_duckdb_unit_handler(THD *thd, SELECT_LEX_UNIT *sel_unit)
{
  if (thd->lex->sql_command == SQLCOM_CREATE_VIEW)
    return nullptr;

  if (thd->stmt_arena && thd->stmt_arena->is_stmt_prepare())
    return nullptr;

  if (!sel_unit)
    return nullptr;

  std::vector<std::string> external_tables;
  bool has_duckdb_table= false;

  if (!can_pushdown_unit_to_duckdb(sel_unit, external_tables,
                                   has_duckdb_table))
    return nullptr;

  if (!has_duckdb_table)
    return nullptr;

  auto *handler= new ha_duckdb_select_handler(thd, sel_unit);
  if (!external_tables.empty())
  {
    handler->set_cross_engine(std::move(external_tables));

    if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY)
      sql_print_information("DuckDB: cross-engine UNION pushdown with %zu "
                            "external table(s)",
                            handler->external_table_count());
  }
  return handler;
}

/* ----- ha_duckdb_select_handler implementation ----- */

ha_duckdb_select_handler::ha_duckdb_select_handler(THD *thd_arg,
                                                   SELECT_LEX *sel_lex,
                                                   SELECT_LEX_UNIT *sel_unit)
    : select_handler(thd_arg, duckdb_hton, sel_lex, sel_unit),
      current_row_index(0), query_string(thd_arg->charset())
{
  query_string.length(0);

  /*
    Use the original SQL text from THD instead of SELECT_LEX::print().
    SELECT_LEX::print() converts implicit (comma) joins into explicit
    "JOIN" without ON clauses, which DuckDB's parser rejects.
    The select_handler intercepts the full query in all cases
    (simple SELECT and UNION), so the original text is always usable.
  */
  query_string.append(thd_arg->query(), thd_arg->query_length());
}

ha_duckdb_select_handler::ha_duckdb_select_handler(THD *thd_arg,
                                                   SELECT_LEX_UNIT *sel_unit)
    : select_handler(thd_arg, duckdb_hton, sel_unit), current_row_index(0),
      query_string(thd_arg->charset())
{
  query_string.length(0);
  query_string.append(thd_arg->query(), thd_arg->query_length());
}

ha_duckdb_select_handler::~ha_duckdb_select_handler()= default;

void ha_duckdb_select_handler::set_cross_engine(
    std::vector<std::string> &&tables)
{
  has_cross_engine= true;
  external_table_names= std::move(tables);
}

size_t ha_duckdb_select_handler::external_table_count() const
{
  return external_table_names.size();
}

int ha_duckdb_select_handler::init_scan()
{
  DBUG_ENTER("ha_duckdb_select_handler::init_scan");

  /* Register external tables with the thread-local replacement scan registry
   */
  if (has_cross_engine)
  {
    auto register_tables_from_sel= [](SELECT_LEX *sl) {
      for (TABLE_LIST *tbl= sl->get_table_list(); tbl; tbl= tbl->next_global)
      {
        if (tbl->derived || !tbl->table)
          continue;
        if (tbl->table->file->ht != duckdb_hton)
          myduck::register_external_table(tbl->table_name.str, tbl->table);
      }
    };

    if (select_lex)
    {
      register_tables_from_sel(select_lex);
    }
    else if (lex_unit)
    {
      for (SELECT_LEX *sl= lex_unit->first_select(); sl; sl= sl->next_select())
        register_tables_from_sel(sl);
    }
  }

  std::string sql(query_string.ptr(), query_string.length());

  /*
    Rewrite MariaDB-specific SQL syntax that DuckDB does not understand.
    GROUP BY ... WITH ROLLUP  →  GROUP BY ROLLUP(...)
  */
  {
    /* Case-insensitive search for "GROUP BY ... WITH ROLLUP" */
    std::string upper_sql= sql;
    std::transform(upper_sql.begin(), upper_sql.end(), upper_sql.begin(),
                   ::toupper);

    const char *rollup_str= " WITH ROLLUP";
    size_t rollup_pos= upper_sql.find(rollup_str);
    if (rollup_pos != std::string::npos)
    {
      /* Find "GROUP BY" before WITH ROLLUP */
      const char *group_str= "GROUP BY ";
      size_t group_pos= upper_sql.rfind(group_str, rollup_pos);
      if (group_pos != std::string::npos)
      {
        size_t cols_start= group_pos + strlen(group_str);
        std::string cols= sql.substr(cols_start, rollup_pos - cols_start);
        std::string replacement= "GROUP BY ROLLUP(" + cols + ")";
        sql.replace(group_pos, rollup_pos + strlen(rollup_str) - group_pos,
                    replacement);
      }
    }
  }

  query_result= myduck::duckdb_query(thd, sql, true);

  if (!query_result || query_result->HasError())
  {
    if (query_result)
      my_error(ER_DUCKDB_CLIENT, MYF(0),
                      query_result->GetError().c_str());
    else
      my_error(ER_DUCKDB_CLIENT, MYF(0),
                      "DuckDB query returned null result");
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  current_chunk.reset();
  current_row_index= 0;

  DBUG_RETURN(0);
}

int ha_duckdb_select_handler::next_row()
{
  DBUG_ENTER("ha_duckdb_select_handler::next_row");

  if (!query_result)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  /* Fetch a new chunk when the current one is exhausted */
  if (!current_chunk || current_row_index >= current_chunk->size())
  {
    current_chunk.reset();
    current_chunk= query_result->Fetch();

    if (!current_chunk || current_chunk->size() == 0)
      DBUG_RETURN(HA_ERR_END_OF_FILE);

    current_row_index= 0;
  }

  /*
    Store the fields into table->record[0].
    The select_handler framework provides `table` which is a temporary
    table with one Field per item in the select list.
  */
  size_t col_count= current_chunk->ColumnCount();
  size_t field_count= 0;

  for (Field **f= table->field; *f; f++)
    field_count++;

  size_t ncols= (col_count < field_count) ? col_count : field_count;

  for (size_t col_idx= 0; col_idx < ncols; col_idx++)
  {
    duckdb::Value value= current_chunk->GetValue(col_idx, current_row_index);
    Field *field= table->field[col_idx];
    store_duckdb_field_in_mysql_format(field, value, thd);
  }

  current_row_index++;
  DBUG_RETURN(0);
}

int ha_duckdb_select_handler::end_scan()
{
  DBUG_ENTER("ha_duckdb_select_handler::end_scan");

  if (has_cross_engine)
    myduck::clear_external_tables();

  current_chunk.reset();
  query_result.reset();
  current_row_index= 0;

  free_tmp_table(thd, table);
  table= 0;

  DBUG_RETURN(0);
}
