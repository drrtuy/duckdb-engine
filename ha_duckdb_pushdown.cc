/*
  Copyright (c) 2025, MariaDB Corporation.

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
#include "sql_select.h"
#include "log.h"

#undef UNKNOWN

#include "ha_duckdb_pushdown.h"
#include "duckdb_select.h"
#include "duckdb_query.h"
#include "duckdb_context.h"

extern handlerton *duckdb_hton;

/* ----- Helper: check all tables in a SELECT_LEX are DuckDB ----- */

static bool all_tables_are_duckdb(SELECT_LEX *sel_lex)
{
  if (!sel_lex->join)
    return false;

  for (TABLE_LIST *tbl= sel_lex->join->tables_list;
       tbl; tbl= tbl->next_local)
  {
    if (!tbl->table)
      return false;

    /* Skip derived tables — they will be checked recursively */
    if (tbl->derived)
      continue;

    if (tbl->table->file->ht != duckdb_hton)
      return false;
  }

  /* Check inner units (subqueries) recursively */
  for (SELECT_LEX_UNIT *un= sel_lex->first_inner_unit();
       un; un= un->next_unit())
  {
    for (SELECT_LEX *sl= un->first_select(); sl; sl= sl->next_select())
    {
      if (!all_tables_are_duckdb(sl))
        return false;
    }
  }

  return true;
}

/* ----- Factory function ----- */

select_handler *
create_duckdb_select_handler(THD *thd, SELECT_LEX *sel_lex,
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

  if (!all_tables_are_duckdb(sel_lex))
    return nullptr;

  /* Do not push down queries with side-effects (e.g. user variables) */
  if (sel_lex->uncacheable & UNCACHEABLE_SIDEEFFECT)
    return nullptr;

  return new ha_duckdb_select_handler(thd, sel_lex, sel_unit);
}

/* ----- ha_duckdb_select_handler implementation ----- */

ha_duckdb_select_handler::ha_duckdb_select_handler(THD *thd_arg,
                                                   SELECT_LEX *sel_lex,
                                                   SELECT_LEX_UNIT *sel_unit)
    : select_handler(thd_arg, duckdb_hton, sel_lex, sel_unit),
      current_row_index(0),
      query_string(thd_arg->charset())
{
  query_string.length(0);

  if (get_pushdown_type() == select_pushdown_type::SINGLE_SELECT)
  {
    /*
      Use SELECT_LEX_UNIT::print() to include possible CTEs
      stored at SELECT_LEX_UNIT::with_clause.
    */
    sel_lex->master_unit()->print(&query_string, PRINT_QUERY_TYPE);
  }
  else if (get_pushdown_type() == select_pushdown_type::PART_OF_UNIT)
  {
    sel_lex->print(thd_arg, &query_string, PRINT_QUERY_TYPE);
  }
  else
  {
    DBUG_ASSERT(0);
  }
}

ha_duckdb_select_handler::~ha_duckdb_select_handler() = default;

int ha_duckdb_select_handler::init_scan()
{
  DBUG_ENTER("ha_duckdb_select_handler::init_scan");

  std::string sql(query_string.ptr(), query_string.length());

  query_result= myduck::duckdb_query(thd, sql, true);

  if (!query_result || query_result->HasError())
  {
    if (query_result)
      my_error(ER_UNKNOWN_ERROR, MYF(0), query_result->GetError().c_str());
    else
      my_error(ER_UNKNOWN_ERROR, MYF(0), "DuckDB query returned null result");
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

  current_chunk.reset();
  query_result.reset();
  current_row_index= 0;

  free_tmp_table(thd, table);
  table= 0;

  DBUG_RETURN(0);
}
