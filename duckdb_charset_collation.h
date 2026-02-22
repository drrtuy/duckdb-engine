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

#ifndef DUCKDB_CHARSET_COLLATION_H
#define DUCKDB_CHARSET_COLLATION_H

#include <string>
#include <my_global.h>
#include "m_ctype.h"

namespace myduck
{

/** Get the corresponding duckdb collation according to mysql CHARSET_INFO.
  @param[in]  cs        Pointer to mysql CHARSET_INFO structure
  @param[in]  warn_msg  Warn message if there is any warning
  @return  DuckDB collation
*/
std::string get_duckdb_collation(const CHARSET_INFO *cs,
                                 std::string &warn_msg);

static std::string COLLATION_BINARY= "POSIX";
static std::string COLLATION_NOCASE= "NOCASE";
static std::string COLLATION_NOCASE_NOACCENT= "NOCASE.NOACCENT";
} // namespace myduck

#endif // DUCKDB_CHARSET_COLLATION_H
