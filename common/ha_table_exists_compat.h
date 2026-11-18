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

#include <type_traits>

#include "handler.h"

namespace myduck
{
namespace ha_table_exists_detail
{

template <typename T>
inline T arg_for(handlerton **hton)
{
  if constexpr (std::is_same_v<T, handlerton **>)
    return hton;
  else if constexpr (std::is_pointer_v<T>)
    return nullptr;
  else
    return T{};
}

template <typename R, typename... Rest>
inline bool call(R (*fn)(THD *, const LEX_CSTRING *, const LEX_CSTRING *,
                         Rest...),
                 THD *thd, const LEX_CSTRING *db, const LEX_CSTRING *table,
                 handlerton **hton)
{
  static_assert((std::is_same_v<Rest, handlerton **> || ...),
                "ha_table_exists() has no handlerton** parameter");
  static_assert(sizeof...(Rest) <= 4 &&
                    ((std::is_pointer_v<Rest> || std::is_unsigned_v<Rest>) &&
                     ...),
                "unknown ha_table_exists() signature, review the arguments");
  return fn(thd, db, table, arg_for<Rest>(hton)...);
}

} /* namespace ha_table_exists_detail */

/*
  True if the table exists; *hton is set to its engine (may stay null for a
  table the server cannot attribute to an engine).
*/
inline bool table_exists(THD *thd, const LEX_CSTRING *db,
                         const LEX_CSTRING *table, handlerton **hton)
{
  return ha_table_exists_detail::call(&ha_table_exists, thd, db, table, hton);
}

} /* namespace myduck */
