/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

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

#ifndef DUCKDB_TIMEZONE_H
#define DUCKDB_TIMEZONE_H

#include <string>

class THD;
class Field;

namespace myduck
{
constexpr long days_at_timestart= 719528;

/** DuckDB session variable: true when a repeated local time resolves to
  the later instant (see is_system_timezone()). */
#define MARIADB_TZ_REPEATED_LATER_VAR "mariadb_tz_repeated_later"

/** TIMESTAMPTZ value standing for MariaDB's zero TIMESTAMP, which has no
  instant: 0000-00-00 encoded as days since 1970 (calc_daynr(0,0,0) - 719528
  days). The batch appender has always written this value. */
constexpr long long zero_timestamp_epoch_us=
    -(long long) days_at_timestart * 86400LL * 1000000LL;

/** DuckDB (ICU) time zone name for the THD's time_zone.
  Fixed offsets become ICU custom ids ("GMT+05:30"), named zones pass
  through, and SYSTEM is resolved to the IANA zone the server process
  uses (TZ or /etc/localtime), so DST rules are kept. When SYSTEM cannot
  be resolved this falls back to get_fixed_offset_timezone().
*/
std::string get_timezone_according_thd(THD *thd);

/** Whether the THD's time_zone is SYSTEM. MariaDB resolves a repeated
  local time (clocks going back) to the later instant under SYSTEM (libc)
  and to the earlier one under named zones. */
bool is_system_timezone(THD *thd);

/** ICU custom id for the THD's time_zone offset at the current moment.
  Used when DuckDB does not know the zone name; correct only until the
  zone's next offset change.
*/
std::string get_fixed_offset_timezone(THD *thd);

/** Microseconds since the Unix epoch of a TIMESTAMP field's value.
  MariaDB TIMESTAMP is an instant, and DuckDB keeps it in a TIMESTAMPTZ
  column as one. The zero timestamp gives zero_timestamp_epoch_us.
*/
long long get_timestamp_epoch_us(Field *field);

/** Store a TIMESTAMPTZ instant into a TIMESTAMP, DATETIME or DATE field.
  TIMESTAMP keeps the instant (instants outside its range, including
  zero_timestamp_epoch_us, become the zero timestamp); DATETIME and DATE
  get the wall-clock time in the THD's time_zone.
*/
void store_epoch_us(THD *thd, Field *field, long long epoch_us);

} // namespace myduck

#endif // DUCKDB_TIMEZONE_H
