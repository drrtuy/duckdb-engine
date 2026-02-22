/*
  Copyright (c) 2025, Alibaba and/or its affiliates. All Rights Reserved.
  Ported to MariaDB.

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

extern handlerton *duckdb_hton;

namespace myduck
{

std::unique_ptr<duckdb::QueryResult>
duckdb_query(duckdb::Connection &connection, const std::string &query)
{
  if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY)
    sql_print_information("DuckDB query: %s", query.c_str());

  try
  {
    auto res= connection.Query(query);

    if (myduck::duckdb_log_options & LOG_DUCKDB_QUERY_RESULT)
    {
      if (res->HasError())
        sql_print_information("DuckDB error: %s", res->GetError().c_str());
    }
    return res;
  }
  catch (duckdb::Exception &e)
  {
    auto result= duckdb::make_uniq<duckdb::MaterializedQueryResult>(
        duckdb::ErrorData(e.what()));
    return result;
  }
  catch (std::exception &e)
  {
    auto result= duckdb::make_uniq<duckdb::MaterializedQueryResult>(
        duckdb::ErrorData(e.what()));
    return result;
  }
}

std::unique_ptr<duckdb::QueryResult>
duckdb_query(THD *thd, const std::string &query, bool need_config)
{
  auto *ctx=
      static_cast<DuckdbThdContext *>(thd_get_ha_data(thd, duckdb_hton));
  if (!ctx)
    return duckdb_query(query);

  if (need_config)
    ctx->config_duckdb_env(thd);

  return duckdb_query(ctx->get_connection(), query);
}

std::unique_ptr<duckdb::QueryResult> duckdb_query(const std::string &query)
{
  auto connection= DuckdbManager::CreateConnection();
  return duckdb_query(*connection, query);
}

bool duckdb_query_and_send(THD *thd, const std::string &query,
                           bool send_result, bool push_error)
{
  if (thd->killed)
  {
    if (push_error)
      my_error(ER_UNKNOWN_ERROR, MYF(0),
               "current query or connection was killed");
    return true;
  }

  auto res= duckdb_query(thd, query, true);

  if (res->HasError())
  {
    if (push_error)
      my_error(ER_UNKNOWN_ERROR, MYF(0), res->GetError().c_str());
    return true;
  }
  /* TODO: implement send_result for SELECT pushdown */
  return false;
}

} // namespace myduck
