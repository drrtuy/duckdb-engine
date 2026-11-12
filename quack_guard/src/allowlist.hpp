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
  What a Quack session may run (milestone 1: read-only SELECT on base
  tables). Both gates use these lists; anything not listed is denied, so a
  new DuckDB statement, operator or function is denied until reviewed.
*/

#pragma once

#include <string>

namespace quack_guard
{

/* Scalar, aggregate or window function (or operator) by name, lower case:
   deterministic, no I/O, no settings, no nested SQL. */
bool FunctionAllowed(const std::string &name);

/* Schemas and tables a Quack session never reaches, whatever the grants. */
bool SchemaReserved(const std::string &schema);
bool TableReserved(const std::string &table);

/* The one message for every refusal by the gates. */
extern const char *const STATEMENT_DENIED;
extern const char *const ACCESS_DENIED;

} /* namespace quack_guard */
