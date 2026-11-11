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
#include <type_traits>
#include "sql_class.h"
#include "sql_select.h"
#include "sql_window.h"
#include "log.h"

#undef UNKNOWN

#include "ha_duckdb_pushdown.h"
#include "duckdb_select.h"
#include "duckdb_query.h"
#include "duckdb_context.h"
#include "cross_engine_scan.h"
#include "duckdb_config.h"
#include "duckdb_log.h"

extern handlerton *duckdb_hton;

class Invisible_field_enumerator : public Field_enumerator
{
public:
  explicit Invisible_field_enumerator(THD *thd) : m_thd(thd) {}

  void visit_field(Item_field *item) override
  {
    if (m_found || !item->field || item->field->invisible == VISIBLE ||
        is_implicit_versioning_field(item))
      return;
    m_found= true;
  }

  bool found() const { return m_found; }

private:
  bool is_implicit_versioning_field(Item_field *item) const
  {
    for (TABLE_LIST *tbl= m_thd->lex->query_tables; tbl;
         tbl= tbl->next_global)
    {
      if (item == tbl->vers_conditions.field_start ||
          item == tbl->vers_conditions.field_end)
        return true;
    }
    return false;
  }

  THD *m_thd;
  bool m_found= false;
};

// MariaDB changed walk(processor, walk_subquery, arg) to
// walk(processor, arg, flags). Detect the signature for the installed SDK.
template <typename ItemType>
static void walk_field_refs(ItemType *item, Field_enumerator *enumerator)
{
  if constexpr (std::is_invocable_v<decltype(&ItemType::walk), ItemType *,
                                  decltype(&Item::enumerate_field_refs_processor),
                                  Field_enumerator *, unsigned>)
    item->walk(&Item::enumerate_field_refs_processor, enumerator, 0);
  else
    item->walk(&Item::enumerate_field_refs_processor, false, enumerator);
}

static void inspect_item_for_invisible_fields(
    Item *item, Invisible_field_enumerator &enumerator)
{
  if (item && !enumerator.found())
    walk_field_refs(item, &enumerator);
}

static bool query_references_invisible_fields(THD *thd)
{
  Invisible_field_enumerator enumerator(thd);

  for (SELECT_LEX *sl= thd->lex->all_selects_list; sl && !enumerator.found();
       sl= sl->next_select_in_list())
  {
    List_iterator_fast<Item> items(sl->item_list);
    Item *item;
    while ((item= items++))
      inspect_item_for_invisible_fields(item, enumerator);

    inspect_item_for_invisible_fields(sl->where, enumerator);
    inspect_item_for_invisible_fields(sl->having, enumerator);

    for (ORDER *order= sl->group_list.first; order; order= order->next)
      inspect_item_for_invisible_fields(*order->item, enumerator);
    for (ORDER *order= sl->order_list.first; order; order= order->next)
      inspect_item_for_invisible_fields(*order->item, enumerator);

    List_iterator_fast<Window_spec> windows(sl->window_specs);
    Window_spec *window;
    while ((window= windows++))
    {
      for (ORDER *order= window->partition_list->first; order;
           order= order->next)
        inspect_item_for_invisible_fields(*order->item, enumerator);
      for (ORDER *order= window->order_list->first; order;
           order= order->next)
        inspect_item_for_invisible_fields(*order->item, enumerator);
    }

    for (TABLE_LIST *tbl= sl->get_table_list(); tbl; tbl= tbl->next_local)
      inspect_item_for_invisible_fields(tbl->on_expr, enumerator);
  }

  return enumerator.found();
}

static bool query_has_explicit_system_time(THD *thd)
{
  for (TABLE_LIST *tbl= thd->lex->query_tables; tbl; tbl= tbl->next_global)
    if (tbl->vers_conditions.was_set())
      return true;
  return false;
}

static bool has_supported_field_layout(TABLE *table,
                                       bool allow_trailing_invisible)
{
  bool found_invisible= false;

  for (Field **field= table->field; *field; field++)
  {
    if ((*field)->invisible == VISIBLE)
    {
      if (found_invisible)
        return false;
    }
    else
    {
      if (!allow_trailing_invisible)
        return false;
      found_invisible= true;
    }
  }

  return true;
}

/**
  Check whether a SELECT_LEX can be pushed down to DuckDB.

  Returns true if at least one table is DuckDB.  Non-DuckDB tables are
  collected in @p external_tables so they can be registered with the
  replacement-scan mechanism before executing the DuckDB query.
*/

static bool can_pushdown_to_duckdb(SELECT_LEX *sel_lex,
                                   std::vector<std::string> &external_tables,
                                   bool &has_duckdb_table,
                                   bool allow_trailing_invisible)
{
  for (TABLE_LIST *tbl= sel_lex->get_table_list(); tbl; tbl= tbl->next_global)
  {
    if (tbl->derived || !tbl->table)
      continue;

    if (tbl->table->file->ht == duckdb_hton)
      has_duckdb_table= true;
    else
    {
      if (!has_supported_field_layout(tbl->table,
                                      allow_trailing_invisible))
        return false;
      external_tables.emplace_back(tbl->table_name.str);
    }
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
                            bool &has_duckdb_table,
                            bool allow_trailing_invisible)
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
      {
        if (!has_supported_field_layout(tbl->table,
                                        allow_trailing_invisible))
          return false;
        external_tables.emplace_back(tbl->table_name.str);
      }
    }
  }

  return has_duckdb_table;
}

/* ----- Factory functions ----- */

static bool is_query_token(const std::string &sql, size_t start, size_t length)
{
  static const char select_kw[]= "select";
  static const char with_kw[]= "with";
  const char *keyword= length == 6 ? select_kw : length == 4 ? with_kw : nullptr;
  if (!keyword)
    return false;
  for (size_t i= 0; i < length; i++)
    if (tolower((unsigned char) sql[start + i]) != keyword[i])
      return false;
  return true;
}

static bool extract_source_query(THD *thd, std::string &source)
{
  if (myduck::mariadb_query_has_lexical_mismatch(
          thd, thd->query(), thd->query_length()))
    return false;
  const std::string sql(thd->query(), thd->query_length());
  const bool strip_prefix= thd->lex->sql_command == SQLCOM_INSERT_SELECT;
  const bool backslash_escapes= thd->backslash_escapes();
  size_t i= 0;
  int depth= 0;

  while (i < sql.size())
  {
    unsigned char c= (unsigned char) sql[i];
    if (isspace(c))
    {
      i++;
      continue;
    }
    size_t end;
    myduck::SqlRegionType region= myduck::scan_sql_region(
        sql, i, backslash_escapes &&
                    (c == '\'' ||
                     (c == '"' && !(thd->variables.sql_mode & MODE_ANSI_QUOTES))),
        end);
    if (region == myduck::SqlRegionType::UNTERMINATED)
      return false;
    if (region == myduck::SqlRegionType::COMMENT)
    {
      i= end;
      continue;
    }
    if (region == myduck::SqlRegionType::QUOTED)
    {
      if (!strip_prefix && depth == 0)
        return false;
      i= end;
      continue;
    }
    if (c == '(')
    {
      if (!strip_prefix && depth == 0)
        return false;
      depth++;
      i++;
      continue;
    }
    if (c == ')')
    {
      if (depth == 0)
        return false;
      depth--;
      i++;
      continue;
    }
    if (isalpha(c) || c == '_' || c >= 0x80)
    {
      size_t start= i++;
      while (i < sql.size())
      {
        unsigned char token_char= (unsigned char) sql[i];
        if (!isalnum(token_char) && token_char != '_' && token_char != '$' &&
            token_char < 0x80)
          break;
        i++;
      }
      bool query_token= depth == 0 && is_query_token(sql, start, i - start);
      if (!strip_prefix)
      {
        if (!query_token)
          return false;
        source= sql;
        return true;
      }
      if (query_token)
      {
        source= sql.substr(start);
        return true;
      }
      continue;
    }
    if (!strip_prefix && depth == 0)
      return false;
    i++;
  }
  return false;
}

static bool has_duckdb_insert_target(THD *thd)
{
  if (thd->lex->sql_command != SQLCOM_INSERT_SELECT)
    return false;
  TABLE_LIST *target= thd->lex->query_tables;
  return !target || !target->table || target->table->file->ht == duckdb_hton;
}

static bool has_unsupported_insert_select_clauses(THD *thd)
{
  return thd->lex->sql_command == SQLCOM_INSERT_SELECT &&
         (thd->lex->duplicates == DUP_UPDATE || thd->lex->has_returning());
}

select_handler *create_duckdb_select_handler(THD *thd, SELECT_LEX *sel_lex,
                                             SELECT_LEX_UNIT *sel_unit)
{
  /*
    Only handle plain SELECT and INSERT ... SELECT for now.
  */
  if (thd->lex->sql_command != SQLCOM_SELECT &&
      thd->lex->sql_command != SQLCOM_INSERT_SELECT)
    return nullptr;

  if (!sel_lex || has_duckdb_insert_target(thd) ||
      has_unsupported_insert_select_clauses(thd) ||
      query_has_explicit_system_time(thd) ||
      query_references_invisible_fields(thd))
    return nullptr;

  std::string query;
  if (!extract_source_query(thd, query))
    return nullptr;

  std::vector<std::string> external_tables;
  bool has_duckdb_table= false;

  if (!can_pushdown_to_duckdb(sel_lex, external_tables, has_duckdb_table,
                              !myduck::get_thd_cross_engine_ryow(thd)))
    return nullptr;

  /* At least one DuckDB table must participate */
  if (!has_duckdb_table)
    return nullptr;

  /* Do not push down queries with side-effects (e.g. user variables) */
  if (sel_lex->uncacheable & UNCACHEABLE_SIDEEFFECT)
    return nullptr;

  auto *handler=
      new ha_duckdb_select_handler(thd, sel_lex, sel_unit, std::move(query));
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
  if ((thd->lex->sql_command != SQLCOM_SELECT &&
       thd->lex->sql_command != SQLCOM_INSERT_SELECT) ||
      has_duckdb_insert_target(thd) ||
      has_unsupported_insert_select_clauses(thd) ||
      query_has_explicit_system_time(thd) ||
      query_references_invisible_fields(thd))
    return nullptr;

  if (thd->stmt_arena && thd->stmt_arena->is_stmt_prepare())
    return nullptr;

  if (!sel_unit)
    return nullptr;

  std::string query;
  if (!extract_source_query(thd, query))
    return nullptr;

  std::vector<std::string> external_tables;
  bool has_duckdb_table= false;

  if (!can_pushdown_unit_to_duckdb(
          sel_unit, external_tables, has_duckdb_table,
          !myduck::get_thd_cross_engine_ryow(thd)))
    return nullptr;

  if (!has_duckdb_table)
    return nullptr;

  auto *handler=
      new ha_duckdb_select_handler(thd, sel_unit, std::move(query));
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
                                                   SELECT_LEX_UNIT *sel_unit,
                                                   std::string &&query)
    : select_handler(thd_arg, duckdb_hton, sel_lex, sel_unit),
      current_row_index(0), query_string(thd_arg->charset())
{
  query_string.length(0);
  query_string.append(query.c_str(), query.length());
}

ha_duckdb_select_handler::ha_duckdb_select_handler(THD *thd_arg,
                                                   SELECT_LEX_UNIT *sel_unit,
                                                   std::string &&query)
    : select_handler(thd_arg, duckdb_hton, sel_unit), current_row_index(0),
      query_string(thd_arg->charset())
{
  query_string.length(0);
  query_string.append(query.c_str(), query.length());
}

/* select_handler::execute() skips end_scan() on some error paths. */
ha_duckdb_select_handler::~ha_duckdb_select_handler()
{
  if (has_cross_engine)
    myduck::clear_external_tables();
  end_interruptible();
}

void ha_duckdb_select_handler::end_interruptible()
{
  if (interruptible_ctx)
  {
    interruptible_ctx->end_interruptible();
    interruptible_ctx= nullptr;
  }
}

/* A DuckDB error caused by KILL / max_statement_time is reported as such. */
static void report_duckdb_error(THD *thd, const std::string &error)
{
  if (thd->killed)
    thd->send_kill_message();
  else
    my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_INTERNAL_ERROR, error.c_str(),
             "DuckDB");
}

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
    myduck::register_cross_engine_ryow(myduck::get_thd_cross_engine_ryow(thd));

    auto register_tables_from_sel= [this](SELECT_LEX *sl) {
      for (TABLE_LIST *tbl= sl->get_table_list(); tbl; tbl= tbl->next_global)
      {
        if (tbl->derived || !tbl->table)
          continue;
        if (tbl->table->file->ht != duckdb_hton)
        {
          myduck::register_external_table(tbl->table_name.str, tbl->table);

          if (sl->where)
          {
            COND *table_cond=
                make_cond_for_table(thd, sl->where, tbl->table->map,
                                    tbl->table->map, -1, false, false);
            if (table_cond)
            {
              StringBuffer<1024> buf;
              table_cond->print(&buf,
                  QT_ITEM_IDENT_DISABLE_DB_TABLE_NAMES);
              if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY)
                sql_print_information(
                    "CROSS-ENGINE: WHERE for '%s': %.*s",
                    tbl->table_name.str, (int) buf.length(), buf.ptr());
              myduck::register_external_where(
                  tbl->table_name.str,
                  std::string(buf.ptr(), buf.length()));
            }
          }
        }
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

  const std::string sql(query_string.ptr(), query_string.length());

  interruptible_ctx= myduck::get_thd_context(thd);

  std::string error;
  query_stream= myduck::duckdb_stream_mariadb_query(thd, sql, error, true);

  if (!query_stream)
  {
    if (has_cross_engine)
      myduck::clear_external_tables();
    end_interruptible();
    report_duckdb_error(thd, error);
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  current_chunk.reset();
  current_row_index= 0;

  DBUG_RETURN(0);
}

int ha_duckdb_select_handler::next_row()
{
  DBUG_ENTER("ha_duckdb_select_handler::next_row");

  if (!query_stream)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  /* Fetch a new chunk when the current one is exhausted */
  if (!current_chunk || current_row_index >= current_chunk->size())
  {
    /*
      QueryResultStream::Fetch() runs query tasks on this thread until a
      chunk is buffered. It returns null at the end of the stream and on
      an execution error (recorded on the stream), and throws once the
      stream has failed.
    */
    try
    {
      do
        current_chunk= query_stream->Fetch();
      while (current_chunk && current_chunk->size() == 0);
    }
    catch (std::exception &e)
    {
      current_chunk.reset();
      if (!query_stream->HasError())
        query_stream->SetError(duckdb::ErrorData(e));
    }

    if (!current_chunk)
    {
      if (query_stream->HasError())
      {
        report_duckdb_error(thd, query_stream->GetError());
        DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
      }
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    }

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

  if (col_count != field_count)
  {
    my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_INTERNAL_ERROR,
             "DuckDB result column count does not match MariaDB metadata",
             "DuckDB");
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  for (size_t col_idx= 0; col_idx < col_count; col_idx++)
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
  query_stream.reset();
  current_row_index= 0;
  end_interruptible();

  free_tmp_table(thd, table);
  table= 0;

  DBUG_RETURN(0);
}
