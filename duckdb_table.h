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

#ifndef DUCKDB_TABLE_H
#define DUCKDB_TABLE_H

#include <string>

class TABLE;
class THD;
struct HA_CREATE_INFO;
class Alter_info;

namespace myduck
{

/** Checks whether the given table is a DuckDB table.
  @param table pointer to TABLE object
  @return true if the table is a DuckDB table, false otherwise
*/
bool is_duckdb_table(const TABLE *table);

/** Report error message of DuckDB table struct to user.
  @param[in]  err_msg   error message
  @return true always */
bool report_duckdb_table_struct_error(const std::string &err_msg);

} // namespace myduck

#endif // DUCKDB_TABLE_H
