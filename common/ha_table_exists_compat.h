/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov <leontodys@gmail.com>

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

/*
  ha_table_exists() has had several signatures, and servers of the same
  version do not always agree on them (some builds have no default
  arguments). Instead of guessing from MYSQL_VERSION_ID, take the parameter
  list from the declaration itself and pass every argument explicitly:
  the handlerton output gets &hton, other pointers get nullptr, unsigned
  integers (flags) get zero. A parameter of any other kind, or more of
  them, stops the build, so a new signature is reviewed instead of guessed.
*/

#pragma once

#include <my_global.h>
#include <type_traits>
#include "sql_class.h"
#include "handler.h"

namespace myduck
{

template <typename TableExists>
inline bool ha_table_exists_compat(TableExists table_exists, THD *thd,
                                   const LEX_CSTRING *db,
                                   const LEX_CSTRING *table, handlerton **hton)
{
  if constexpr (std::is_invocable_v<TableExists, THD *, const LEX_CSTRING *,
                                    const LEX_CSTRING *, LEX_CUSTRING *,
                                    handlerton **, bool *, uint>)
    return table_exists(thd, db, table, nullptr, hton, nullptr, 0);
  else if constexpr (std::is_invocable_v<TableExists, THD *, const LEX_CSTRING *,
                                         const LEX_CSTRING *, LEX_CUSTRING *,
                                         handlerton **, bool *>)
    return table_exists(thd, db, table, nullptr, hton, nullptr);
  else
    return table_exists(thd, db, table, nullptr, nullptr, hton, nullptr);
}

}
