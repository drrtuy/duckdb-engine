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

#include "duckdb_log.h"

#include <my_global.h>
#include "log.h" /* sql_print_information */
#include <cstdio>

namespace myduck
{

ulonglong duckdb_log_options= 0;

const char *duckdb_log_types[]= {
    "DUCKDB_MULTI_TRX_BATCH_COMMIT", "DUCKDB_MULTI_TRX_BATCH_DETAIL",
    "DUCKDB_QUERY", "DUCKDB_QUERY_RESULT", nullptr};

TYPELIB log_options_typelib= {array_elements(duckdb_log_types) - 1, "",
                              duckdb_log_types, NULL};

bool log_duckdb_multi_trx_batch_commit(const char *reason)
{
  sql_print_information("DuckDB: commit duckdb batch due to %s", reason);
  return false;
}

bool log_duckdb_apply_event_type(const char *type)
{
  sql_print_information("DuckDB: apply event, type = %s", type);
  return false;
}

bool log_duckdb_gtid(const char *prefix, int type, int sidno, int64_t gno)
{
  sql_print_information("DuckDB: %s, type = %d, sidno = %d, gno = %lld",
                        prefix, type, sidno, (long long) gno);
  return false;
}
} // namespace myduck
