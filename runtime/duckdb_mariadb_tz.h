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

#pragma once

namespace duckdb
{
class DatabaseInstance;
}

namespace myduck
{

/**
  MariaDB's local time rules in DuckDB. MariaDB resolves a local time
  that a DST change skips to the start of the gap, and one that occurs
  twice to the earlier instant (to the later one under time_zone=SYSTEM);
  DuckDB's ICU casts resolve both the other way.

  Registers mariadb_unix_timestamp(), mariadb_to_timestamptz() and
  mariadb_convert_tz(), and an optimizer extension that, in sessions that
  set MARIADB_TZ_RULES_VAR (common/duckdb_timezone.h), replaces casts from
  TIMESTAMP, DATE and VARCHAR to TIMESTAMPTZ with mariadb_to_timestamptz().
  Quack sessions do not set the variable and keep DuckDB's rules.
*/
void register_mariadb_time_zone_functions(duckdb::DatabaseInstance &db);

} // namespace myduck
