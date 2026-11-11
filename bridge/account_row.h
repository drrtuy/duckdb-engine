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
  One account's attributes as stored in mysql.global_priv, read through the
  server's SQL service on the current (ACL worker) THD. The server extracts
  the JSON fields itself; nothing is cached.
*/

#pragma once

#include <cstdint>
#include <string>

namespace quack_bridge
{

struct AccountRow
{
  std::string plugin;               /* empty: mysql_native_password */
  std::string authentication_string;
  bool multiple_auth= false;        /* IDENTIFIED VIA ... OR ... */
  bool locked= false;
  bool is_role= false;
  int64_t password_last_changed= -1;  /* 0: expired by ALTER USER */
  int64_t password_lifetime= -1;      /* -1: the server default */
  int64_t ssl_type= 0;                /* 0 none, 1 SSL, 2 X509, 3 specified */
  int64_t max_user_connections= 0;
  std::string default_role;
  int64_t global_max_user_connections= 0;  /* @@max_user_connections */
};

/* False if the row could not be read (the caller denies). */
bool ReadAccount(const std::string &user, const std::string &host,
                 AccountRow &out);

} /* namespace quack_bridge */
