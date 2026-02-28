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
#include "duckdb_config.h"
#include "duckdb_timezone.h"
#include "duckdb_charset_collation.h"

#include <sstream>
#include <vector>

namespace myduck
{

static std::string disabled_optimizers_to_string(ulonglong val)
{
  std::string result;
  for (uint i= 0; disabled_optimizers_names[i] != NullS; i++)
  {
    if (val & (1ULL << i))
    {
      if (!result.empty())
        result+= ',';
      result+= disabled_optimizers_names[i];
    }
  }
  return result;
}

void DuckdbThdContext::config_duckdb_env(THD *thd)
{
  std::vector<std::string> config_sql;

  /* Timezone */
  std::string warn_msg;
  std::string tz_name= get_timezone_according_thd(thd, warn_msg);
  if (!warn_msg.empty())
    sql_print_warning("DuckDB: %s", warn_msg.c_str());
  config_sql.push_back("SET TimeZone = '" + tz_name + "'");

  /* merge_join_threshold (session) */
  ulonglong mjt= get_thd_merge_join_threshold(thd);
  if (mjt != m_merge_join_threshold)
  {
    config_sql.push_back("SET merge_join_threshold = " + std::to_string(mjt));
    m_merge_join_threshold= mjt;
  }

  /* disabled_optimizers (session) */
  ulonglong dopt= get_thd_disabled_optimizers(thd);
  if (dopt != m_disabled_optimizers)
  {
    std::string val_str= disabled_optimizers_to_string(dopt);
    config_sql.push_back("SET disabled_optimizers = '" + val_str + "'");
    m_disabled_optimizers= dopt;
  }

  /* explain_output (session, only when EXPLAIN) */
  ulong eo= get_thd_explain_output(thd);
  std::string eo_str= explain_output_names[eo];
  if (eo_str != m_explain_output_str)
  {
    config_sql.push_back("SET explain_output = '" + eo_str + "'");
    m_explain_output_str= eo_str;
  }

  /* Execute all config statements */
  for (auto &sql : config_sql)
  {
    auto res= duckdb_query(get_connection(), sql);
    if (res && res->HasError())
      sql_print_warning("DuckDB: config_duckdb_env failed: %s (sql=%s)",
                        res->GetError().c_str(), sql.c_str());
  }
}

} // namespace myduck
