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
#include "mysqld_error.h"
#include "handler.h"
#include "table.h"

#include "duckdb_table.h"

extern handlerton *duckdb_hton;

namespace myduck
{

bool is_duckdb_table(const TABLE *table)
{
  if (table == nullptr || table->file == nullptr || table->file->ht == nullptr)
    return false;

  return (table->file->ht == duckdb_hton);
}

bool report_duckdb_table_struct_error(const std::string &err_msg)
{
  my_error(ER_UNKNOWN_ERROR, MYF(0), err_msg.c_str());
  return true;
}

} // namespace myduck
