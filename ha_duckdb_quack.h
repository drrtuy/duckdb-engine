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
  The Quack server inside the plugin (MDEV-41383): start and stop, status
  variables and the audit log. The settings are duckdb_quack_* system
  variables, defined with the others in ha_duckdb.cc. Design: Quack.MD.
*/

#pragma once

#include <my_global.h>
#include <mysql/plugin.h>

struct handlerton;

/* Settings (ha_duckdb.cc) */
extern my_bool quack_enabled;
extern my_bool quack_bridge_test;
extern char *quack_bind_address, *quack_ssl_cert, *quack_ssl_key;
extern uint quack_port, quack_max_connections, quack_max_sessions,
    quack_max_transaction_seconds, quack_max_heartbeat_seconds;
extern ulong quack_acl_queue_size, quack_acl_timeout_ms;

/* At plugin init: start the ACL worker and, once the server is up, the
   listener; installs the handlerton's pre_shutdown */
void quack_init(handlerton *hton);
/* Stop the listener and the ACL worker. Idempotent. */
void quack_stop();

/* Duckdb_quack_* status variables, nested under "Duckdb_quack" */
extern struct st_mysql_show_var quack_status_variables[];
