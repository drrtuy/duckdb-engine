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
#include "log.h"

#undef UNKNOWN

#include "duckdb_query.h"
#include "duckdb/common/exception.hpp"
#include "duckdb_context.h"
#include "duckdb_manager.h"
#include "duckdb_log.h"
#include "mariadb_dialect.h"

namespace myduck
{

SqlRegionType scan_sql_region(const std::string &sql, size_t start,
                              bool backslash_escapes, size_t &end)
{
  end= start;
  if (start >= sql.size())
    return SqlRegionType::NONE;

  char c= sql[start];
  if (c == '/' && start + 1 < sql.size() && sql[start + 1] == '*')
  {
    size_t close= sql.find("*/", start + 2);
    if (close == std::string::npos)
    {
      end= sql.size();
      return SqlRegionType::UNTERMINATED;
    }
    end= close + 2;
    return SqlRegionType::COMMENT;
  }

  if (c == '#' ||
      (c == '-' && start + 1 < sql.size() && sql[start + 1] == '-' &&
       (start + 2 == sql.size() ||
        static_cast<unsigned char>(sql[start + 2]) <= ' ')))
  {
    size_t newline= sql.find('\n', start + (c == '#' ? 1 : 2));
    end= newline == std::string::npos ? sql.size() : newline + 1;
    return SqlRegionType::COMMENT;
  }

  if (c != '\'' && c != '"' && c != '`')
    return SqlRegionType::NONE;

  for (size_t i= start + 1; i < sql.size(); i++)
  {
    if (sql[i] == '\\' && backslash_escapes && i + 1 < sql.size())
    {
      i++;
      continue;
    }
    if (sql[i] != c)
      continue;
    if (i + 1 < sql.size() && sql[i + 1] == c)
    {
      i++;
      continue;
    }
    end= i + 1;
    return SqlRegionType::QUOTED;
  }

  end= sql.size();
  return SqlRegionType::UNTERMINATED;
}

bool mariadb_query_has_lexical_mismatch(THD *thd, const char *query, size_t length)
{
  const std::string_view sql(query, length);
  const CHARSET_INFO *charset= thd->charset();
  const std::string_view charset_name(charset->cs_name.str, charset->cs_name.length);
  if (charset->mbminlen > 1)
    return true;
  if (charset_name != "utf8mb4" && charset_name != "utf8mb3" &&
      charset_name != "utf8")
    for (unsigned char c : sql)
      if (c >= 0x80)
        return true;
  return mariadb_query_has_lexical_mismatch(
      sql, thd->backslash_escapes(),
      thd->variables.sql_mode & MODE_ANSI_QUOTES);
}

/*
  Convert forwarded MariaDB SQL (the raw thd->query() text, plus any
  Item::print() fragments) from backtick-quoted identifiers into DuckDB SQL
  (double-quoted identifiers).

  MariaDB delimits identifiers with backticks and doubles an embedded backtick;
  DuckDB delimits with double quotes and doubles an embedded double quote. A
  naive character-by-character swap breaks identifiers that contain a double
  quote (MDEV-40653) and also corrupts backticks that appear inside string
  literals. Walk the string instead: copy string literals and already
  double-quoted identifiers verbatim, and rewrite only backtick-delimited
  identifiers, escaping any embedded double quote.
*/
static std::string backticks_to_double_quotes(const std::string &sql)
{
  std::string out;
  out.reserve(sql.size());
  const size_t n= sql.size();
  size_t i= 0;

  while (i < n)
  {
    char c= sql[i];
    size_t end;
    SqlRegionType region= scan_sql_region(sql, i, false, end);

    if (region == SqlRegionType::COMMENT ||
        region == SqlRegionType::UNTERMINATED)
    {
      out.append(sql, i, end - i);
      i= end;
      continue;
    }

    /* Single-quoted string literal: copy verbatim ('' and \' escapes). */
    if (region == SqlRegionType::QUOTED && c == '\'')
    {
      out.append(sql, i, end - i);
      i= end;
      continue;
    }

    /* Already double-quoted identifier: copy verbatim ("" escape). */
    if (region == SqlRegionType::QUOTED && c == '"')
    {
      out.append(sql, i, end - i);
      i= end;
      continue;
    }

    /* Backtick identifier: rewrite as a double-quoted identifier. */
    if (region == SqlRegionType::QUOTED && c == '`')
    {
      out.push_back('"');
      for (i++; i + 1 < end; i++)
      {
        char d= sql[i];
        if (d == '`' && i + 2 < end && sql[i + 1] == '`')
        {
          out.push_back('`');
          i++;
          continue;
        }
        if (d == '"')
          out.push_back('"'); /* escape " inside a DuckDB identifier */
        out.push_back(d);
      }
      out.push_back('"');
      i= end;
      continue;
    }

    out.push_back(c);
    i++;
  }

  return out;
}

duckdb::unique_ptr<duckdb::QueryResult>
duckdb_query(duckdb::Connection &connection, const std::string &query)
{
  const std::string q= backticks_to_double_quotes(query);

  if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY)
    sql_print_information("DuckDB query: %s", q.c_str());

  try
  {
    auto res= connection.Query(q);

    if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY_RESULT)
    {
      if (res->HasError())
        sql_print_information("DuckDB error: %s", res->GetError().c_str());
    }
    return res;
  }
  catch (duckdb::Exception &e)
  {
    return duckdb::make_uniq<duckdb::QueryResult>(duckdb::ErrorData(e.what()));
  }
  catch (std::exception &e)
  {
    return duckdb::make_uniq<duckdb::QueryResult>(duckdb::ErrorData(e.what()));
  }
}

duckdb::unique_ptr<DuckdbChunkStream>
duckdb_stream_query(duckdb::Connection &connection, const std::string &query,
                    std::string &error)
{
  const std::string q= backticks_to_double_quotes(query);

  if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY)
    sql_print_information("DuckDB query: %s", q.c_str());

  try
  {
    /*
      Submit() + QueryResultStream keeps the result streaming: chunks are
      produced as the caller fetches them. Connection::Query() would run the
      query to completion and retain the whole result first.
    */
    auto res= connection.Submit(q);
    if (res->HasError())
      error= res->GetError();
    else
      return duckdb::make_uniq<DuckdbChunkStream>(std::move(res));
  }
  catch (std::exception &e)
  {
    error= duckdb::ErrorData(e).Message();
  }

  if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY_RESULT)
    sql_print_information("DuckDB error: %s", error.c_str());
  return nullptr;
}

static std::string get_thd_schema(THD *thd)
{
  if (thd->db.str && thd->db.length > 0)
    return std::string(thd->db.str, thd->db.length);
  return {};
}

static const char lexical_mismatch_error[]=
    "Unsafe MariaDB lexical mismatch in forwarded SQL";

static duckdb::Connection &prepare_thd_connection(THD *thd, bool need_config)
{
  auto *ctx= get_thd_context(thd);

  if (need_config)
  {
    ctx->config_duckdb_env(get_thd_schema(thd));
    ctx->config_duckdb_session(thd);
  }
  return ctx->get_connection();
}

duckdb::unique_ptr<duckdb::QueryResult>
duckdb_query(THD *thd, const std::string &query, bool need_config)
{
  if (mariadb_query_has_lexical_mismatch(thd, query.data(), query.size()))
    return duckdb::make_uniq<duckdb::QueryResult>(
        duckdb::ErrorData(lexical_mismatch_error));

  return duckdb_query(prepare_thd_connection(thd, need_config), query);
}

duckdb::unique_ptr<DuckdbChunkStream>
duckdb_stream_query(THD *thd, const std::string &query, std::string &error,
                    bool need_config)
{
  if (mariadb_query_has_lexical_mismatch(thd, query.data(), query.size()))
  {
    error= lexical_mismatch_error;
    return nullptr;
  }

  auto &connection= prepare_thd_connection(thd, need_config);
  auto *ctx= get_thd_context(thd);
  /* Opened after the session setup so a kill cannot fail it. */
  ctx->begin_interruptible();
  auto stream= duckdb_stream_query(connection, query, error);
  ctx->interrupt_if_killed(thd);
  return stream;
}

duckdb::unique_ptr<duckdb::QueryResult>
duckdb_query(const std::string &query)
{
  auto connection= DuckdbManager::CreateConnection();
  return duckdb_query(*connection, query);
}

} // namespace myduck
