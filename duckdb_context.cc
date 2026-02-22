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

#include <my_global.h>
#include "sql_class.h"
#include "log.h"

#undef UNKNOWN

#include "duckdb_context.h"
#include "duckdb_timezone.h"
#include "duckdb_charset_collation.h"

namespace myduck
{

void DuckdbThdContext::config_duckdb_env(THD *thd)
{
  /* Set timezone */
  std::string warn_msg;
  std::string tz_name= get_timezone_according_thd(thd, warn_msg);
  if (!warn_msg.empty())
    sql_print_warning("DuckDB: %s", warn_msg.c_str());

  std::string tz_query= "SET TimeZone = '" + tz_name + "'";
  auto res= duckdb_query(get_connection(), tz_query);
  if (res && res->HasError())
    sql_print_warning("DuckDB: failed to set timezone: %s",
                      res->GetError().c_str());
}

} // namespace myduck
