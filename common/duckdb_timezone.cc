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

#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_class.h"
#include "tztime.h"
#include "my_time.h"
#include "log.h"

#include "duckdb_timezone.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>

namespace myduck
{

/** Format a UTC offset as an ICU custom zone id; ICU's sign is the
  usual one (GMT+05:30 is east of Greenwich). */
static std::string offset_zone_name(long offset)
{
  char sign= offset < 0 ? '-' : '+';
  long abs_offset= offset < 0 ? -offset : offset;
  long hours= abs_offset / 3600, minutes= abs_offset / 60 % 60,
       seconds= abs_offset % 60;
  char buf[32];
  if (seconds)
    snprintf(buf, sizeof(buf), "GMT%c%02ld:%02ld:%02ld", sign, hours, minutes,
             seconds);
  else
    snprintf(buf, sizeof(buf), "GMT%c%02ld:%02ld", sign, hours, minutes);
  return buf;
}

/** UTC offset of a MariaDB time zone at the current moment, in seconds. */
static long current_offset(const Time_zone *tz)
{
  my_time_t now= (my_time_t) time(nullptr);
  MYSQL_TIME lt;
  tz->gmt_sec_to_TIME(&lt, now);
  longlong days=
      (longlong) calc_daynr(lt.year, lt.month, lt.day) - days_at_timestart;
  longlong local= days * SECONDS_IN_24H + lt.hour * 3600LL +
                  lt.minute * 60LL + lt.second;
  return (long) (local - (longlong) now);
}

/**
  Bare "GMT-3" style zoneinfo names carry the POSIX sign (GMT-3 is
  UTC+3), while ICU reads them as custom ids with the opposite sign.
  ICU's Etc/GMT-3 keeps the POSIX meaning.
*/
static std::string posix_signed_name(const std::string &name)
{
  if (name.size() > 3 &&
      (name.compare(0, 3, "GMT") == 0 || name.compare(0, 3, "UTC") == 0 ||
       name.compare(0, 3, "UCT") == 0) &&
      (name[3] == '+' || name[3] == '-'))
    return "Etc/GMT" + name.substr(3);
  return name;
}

/** Zone name for a zoneinfo path, or "" when ICU cannot use it. */
static std::string zone_name_from_path(const std::string &path)
{
  char buf[PATH_MAX];
  if (!realpath(path.c_str(), buf))
    return "";
  std::string real(buf);
  size_t pos= real.rfind("/zoneinfo/");
  if (pos == std::string::npos)
    return "";
  std::string name= real.substr(pos + sizeof("/zoneinfo/") - 1);
  if (name.compare(0, 6, "posix/") == 0)
    name.erase(0, 6);
  else if (name.compare(0, 6, "right/") == 0)
    return ""; /* leap-second zones: ICU has no equivalent */
  return posix_signed_name(name);
}

/**
  IANA name of the zone the server process runs in, as libc resolves it:
  TZ if set, otherwise /etc/localtime (or /etc/timezone when
  /etc/localtime is a copy rather than a link). Returns "" for POSIX
  rule strings such as "UTC0" or "EST5EDT,M3.2.0,M11.1.0" that name no
  zoneinfo file.
*/
static std::string resolve_system_zone()
{
  const char *tz= getenv("TZ");
  if (tz)
  {
    if (*tz == ':')
      tz++;
    if (!*tz)
      return "UTC";
    if (*tz == '/')
      return zone_name_from_path(tz);
    const char *dir= getenv("TZDIR");
    std::string base= dir && *dir ? dir : "/usr/share/zoneinfo";
    return zone_name_from_path(base + "/" + tz);
  }
  std::string name= zone_name_from_path("/etc/localtime");
  if (name.empty())
  {
    std::ifstream f("/etc/timezone");
    std::string line;
    if (f && std::getline(f, line) && !line.empty())
      name= zone_name_from_path("/usr/share/zoneinfo/" + line);
  }
  return name;
}

bool is_system_timezone(THD *thd)
{
  return thd->variables.time_zone == my_tz_SYSTEM;
}

std::string get_fixed_offset_timezone(THD *thd)
{
  return offset_zone_name(current_offset(thd->variables.time_zone));
}

std::string mariadb_zone_name_to_icu(const std::string &name)
{
  if (!name.empty() && (name[0] == '+' || name[0] == '-'))
  {
    /* As MariaDB's str_to_offset(): [+-]digits:digits, minutes <= 59,
       from -12:59 to +13:00 */
    size_t colon= name.find(':');
    if (name.size() < 4 || colon == std::string::npos || colon == 1 ||
        colon + 1 >= name.size())
      return "";
    long hours= 0, minutes= 0;
    for (size_t i= 1; i < name.size(); i++)
    {
      if (i == colon)
        continue;
      if (name[i] < '0' || name[i] > '9' || hours > 100 || minutes > 100)
        return "";
      long &part= i < colon ? hours : minutes;
      part= part * 10 + (name[i] - '0');
    }
    long offset= hours * 3600 + minutes * 60;
    if (name[0] == '-')
      offset= -offset;
    if (minutes > 59 || offset < -13 * 3600 + 1 || offset > 13 * 3600)
      return "";
    return offset_zone_name(offset);
  }
  std::string icu= name;
  if (icu.compare(0, 6, "posix/") == 0)
    icu.erase(0, 6);
  return posix_signed_name(icu);
}

std::string get_system_timezone(THD *thd)
{
  /* The server's zone cannot change while it runs. */
  static const std::string system_zone= [] {
    std::string name= resolve_system_zone();
    if (name.empty())
      sql_print_warning("DuckDB: cannot resolve the system time zone to "
                        "a zone name; time_zone=SYSTEM uses the current "
                        "UTC offset, without daylight saving changes");
    return name;
  }();
  if (!system_zone.empty())
    return system_zone;
  return offset_zone_name(current_offset(my_tz_SYSTEM));
}

std::string get_timezone_according_thd(THD *thd)
{
  Time_zone *tz= thd->variables.time_zone;
  if (tz == my_tz_SYSTEM)
    return get_system_timezone(thd);

  const String *tz_name= tz->get_name();
  std::string name_str(tz_name->ptr(), tz_name->length());

  /* Offset zone like +08:00: exact as an ICU custom id */
  if (!name_str.empty() && (name_str[0] == '+' || name_str[0] == '-'))
    return get_fixed_offset_timezone(thd);

  return mariadb_zone_name_to_icu(name_str);
}

long long get_timestamp_epoch_us(Field *field)
{
  ulong sec_part;
  my_time_t sec= field->get_timestamp(&sec_part);
  if (sec == 0 && sec_part == 0)
    return zero_timestamp_epoch_us;
  return (long long) sec * 1000000LL + (long long) sec_part;
}

/* Field::store_timestamp_dec() takes a struct timeval before MariaDB 11.5
   and a my_timeval after; both have tv_sec and tv_usec. */
template <typename T> struct timeval_arg;
template <typename C, typename R, typename A, typename B>
struct timeval_arg<R (C::*)(const A &, B)>
{
  using type= A;
};
using field_timeval=
    timeval_arg<decltype(&Field::store_timestamp_dec)>::type;

void store_epoch_us(THD *thd, Field *field, long long epoch_us)
{
  long long sec= epoch_us / 1000000LL;
  long long usec= epoch_us % 1000000LL;
  if (usec < 0)
  {
    sec--;
    usec+= 1000000LL;
  }

  if (field->type() == MYSQL_TYPE_TIMESTAMP)
  {
    if (sec <= 0 || sec > (long long) TIMESTAMP_MAX_VALUE)
    {
      field->reset(); /* zero timestamp */
      return;
    }
    field_timeval tv;
    tv.tv_sec= sec;
    tv.tv_usec= (ulong) usec;
    field->store_timestamp_dec(tv, field->decimals());
    return;
  }

  MYSQL_TIME tm;
  if (epoch_us == zero_timestamp_epoch_us)
  {
    set_zero_time(&tm, MYSQL_TIMESTAMP_DATETIME);
    field->store_time(&tm);
    return;
  }
  thd->variables.time_zone->gmt_sec_to_TIME(&tm, (my_time_t) sec);
  tm.second_part= (ulong) usec;
  if (field->type() == MYSQL_TYPE_DATE || field->type() == MYSQL_TYPE_NEWDATE)
  {
    tm.hour= tm.minute= tm.second= 0;
    tm.second_part= 0;
    tm.time_type= MYSQL_TIMESTAMP_DATE;
  }
  field->store_time(&tm);
}

} // namespace myduck
