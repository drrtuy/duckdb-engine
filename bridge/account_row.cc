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

#include "account_row.h"

#include <mysql.h>
#include <mysql/service_sql.h>

#include <cstdlib>
#include <memory>

namespace quack_bridge
{

static std::string hex_literal(const std::string &s)
{
  static const char digits[]= "0123456789ABCDEF";
  if (s.empty())
    return "''";
  std::string out= "0x";
  for (unsigned char c : s)
  {
    out+= digits[c >> 4];
    out+= digits[c & 15];
  }
  return out;
}

bool ReadAccount(const std::string &user, const std::string &host,
                 AccountRow &out)
{
  /* Names go in as hex literals: no quoting to get wrong */
  std::string sql=
      /* With one entry in auth_or, it holds the method (as the server's
         own reader takes it) unless it names no plugin */
      "SELECT IF(JSON_EXISTS(Priv, '$.auth_or[0].plugin'),"
      "  JSON_VALUE(Priv, '$.auth_or[0].plugin'), JSON_VALUE(Priv, '$.plugin')),"
      " IF(JSON_EXISTS(Priv, '$.auth_or[0].plugin'),"
      "  COALESCE(JSON_VALUE(Priv, '$.auth_or[0].authentication_string'), ''),"
      "  JSON_VALUE(Priv, '$.authentication_string')),"
      " JSON_LENGTH(Priv, '$.auth_or'),"
      " JSON_VALUE(Priv, '$.account_locked'),"
      " JSON_VALUE(Priv, '$.is_role'),"
      " JSON_VALUE(Priv, '$.password_last_changed'),"
      " JSON_VALUE(Priv, '$.password_lifetime'),"
      " JSON_VALUE(Priv, '$.ssl_type'),"
      " JSON_VALUE(Priv, '$.max_user_connections'),"
      " JSON_VALUE(Priv, '$.default_role'),"
      " @@GLOBAL.max_user_connections"
      " FROM mysql.global_priv WHERE User = " + hex_literal(user) +
      " AND Host = " + hex_literal(host);

  MYSQL *mysql= sql_service->mysql_init_func(nullptr);
  if (!mysql)
    return false;
  std::unique_ptr<MYSQL, void (*)(MYSQL *)> closer(
      mysql, [](MYSQL *m) { sql_service->mysql_close_func(m); });
  if (!sql_service->mysql_real_connect_local_func(mysql) ||
      sql_service->mysql_real_query_func(mysql, sql.c_str(), sql.size()))
    return false;
  MYSQL_RES *result= sql_service->mysql_store_result_func(mysql);
  if (!result)
    return false;
  std::unique_ptr<MYSQL_RES, void (*)(MYSQL_RES *)> freer(
      result, [](MYSQL_RES *r) { sql_service->mysql_free_result_func(r); });
  if (sql_service->mysql_num_rows_func(result) != 1)
    return false;
  MYSQL_ROW row= sql_service->mysql_fetch_row_func(result);
  if (!row)
    return false;
  auto str= [&](int i) { return row[i] ? std::string(row[i]) : std::string(); };
  auto num= [&](int i, int64_t fallback) {
    return row[i] ? (int64_t) std::strtoll(row[i], nullptr, 10) : fallback;
  };
  auto flag= [&](int i) {
    return row[i] && (std::string(row[i]) == "true" || std::string(row[i]) == "1");
  };
  out.plugin= str(0);
  out.authentication_string= str(1);
  out.multiple_auth= num(2, 0) > 1;
  out.locked= flag(3);
  out.is_role= flag(4);
  out.password_last_changed= num(5, -1);
  out.password_lifetime= num(6, -1);
  out.ssl_type= num(7, 0);
  out.max_user_connections= num(8, 0);
  out.default_role= str(9);
  out.global_max_user_connections= num(10, 0);
  return true;
}

} /* namespace quack_bridge */
