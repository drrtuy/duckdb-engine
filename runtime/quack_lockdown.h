/*
  Copyright (c) 2026, MariaDB Foundation.

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

#pragma once

#include "duckdb.hpp"

namespace myduck
{

/*
  Load the statically linked Quack extension and close its stock callbacks:
  quack_authentication_function is pinned to a function that rejects every
  client and quack_authorization_function to one that rejects every
  statement. Defence in depth: quack_serve() itself is disabled by
  patches/duckdb-quack. Must run before lock_configuration. Throws on failure.
*/
void quack_lockdown(duckdb::DatabaseInstance &db, duckdb::Connection &con);

} /* namespace myduck */
