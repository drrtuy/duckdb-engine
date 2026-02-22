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

#include "duckdb_charset_collation.h"

#include <sstream>
#include <string>
#include <cstring>

namespace myduck
{

std::string get_duckdb_collation(const CHARSET_INFO *cs, std::string &warn_msg)
{
  /* Charsets other than utf8mb3 and utf8mb4 use POSIX Collation directly.
     DuckDB treats POSIX same as binary. */
  if (strcmp(cs->cs_name.str, "utf8mb3") &&
      strcmp(cs->cs_name.str, "utf8mb4") && strcmp(cs->cs_name.str, "ascii"))
  {
    std::ostringstream osst;
    osst << "Variable 'collation_connection' is set to " << cs->coll_name.str
         << " BINARY Collation is used for literal string in DuckDB."
         << " Recommend using collations of 'utf8mb3', 'utf8mb4' or 'ascii'.";
    warn_msg= osst.str();
    return COLLATION_BINARY;
  }

  /* _bin Collation */
  if (cs->state & MY_CS_BINSORT)
    return COLLATION_BINARY;

  /* utf8mb3_tolower_ci is _as_ci actually */
  if (cs->state & MY_CS_LOWER_SORT)
    return COLLATION_NOCASE;

  /* _ai_ci Collation */
  if (cs->levels_for_order == 1)
    return COLLATION_NOCASE_NOACCENT;

  /* _as_ci Collation */
  if (cs->levels_for_order == 2)
    return COLLATION_NOCASE;

  /* _as_cs Collation */
  return COLLATION_BINARY;
}

} // namespace myduck
