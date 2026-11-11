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

#include "ha_duckdb_quack.h"

#include <my_global.h>
#include "sql_class.h"
#include "mysqld.h"

#include "bridge/bridge.h"
#include "duckdb_log.h"
#include "duckdb_manager.h"
#include "quack_guard/quack_guard.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

static handlerton *quack_hton= nullptr;

/*
  Quack (MDEV-41383): the listener starts only once the server is fully up
  (grant tables loaded), from a thread that waits for it; it never starts
  with --skip-grant-tables. Stopping waits for that thread first.
*/
static std::thread quack_starter;
static std::atomic<bool> quack_stop_requested{false};

static quack_guard::ListenerOptions quack_listener_options()
{
  quack_guard::ListenerOptions options;
  std::string addresses= quack_bind_address ? quack_bind_address : "";
  size_t start= 0;
  while (start <= addresses.size())
  {
    size_t comma= addresses.find(',', start);
    if (comma == std::string::npos)
      comma= addresses.size();
    std::string address= addresses.substr(start, comma - start);
    address.erase(0, address.find_first_not_of(" \t"));
    address.erase(address.find_last_not_of(" \t") + 1);
    if (!address.empty())
      options.addresses.push_back(address);
    start= comma + 1;
  }
  options.port= (uint16_t) quack_port;
  options.cert_file= quack_ssl_cert ? quack_ssl_cert : "";
  options.key_file= quack_ssl_key ? quack_ssl_key : "";
  options.max_connections= quack_max_connections;
  options.max_sessions= quack_max_sessions;
  options.max_transaction_seconds= quack_max_transaction_seconds;
  options.max_heartbeat_seconds= quack_max_heartbeat_seconds;
  return options;
}

/*
  The audit log. Refused logins are logged with log_warnings > 1, as the
  server logs refused logins; with duckdb_log_options=DUCKDB_QUACK also
  opened and closed sessions and refused statements.
*/
static void quack_audit(const quack_guard::AuditEvent &event)
{
  using Kind= quack_guard::AuditEvent::Kind;
  bool all= myduck::duckdb_log_options & LOG_DUCKDB_QUACK;
  const char *who= event.principal.c_str();
  const char *peer= event.peer.empty() ? "?" : event.peer.c_str();
  const char *detail= event.detail.c_str();
  switch (event.kind)
  {
  case Kind::LoginRefused:
    if (all || global_system_variables.log_warnings > 1)
      sql_print_warning("DuckDB: Quack login refused for '%s' from %s: %s",
                        who, peer, detail);
    break;
  case Kind::StatementRefused:
    if (all)
      sql_print_warning("DuckDB: Quack statement refused for '%s' from %s: %s",
                        who, peer, detail);
    break;
  case Kind::Login:
    if (all)
      sql_print_information("DuckDB: Quack session opened for '%s' from %s",
                            who, peer);
    break;
  case Kind::SessionClosed:
    if (all)
      sql_print_information("DuckDB: Quack session closed for '%s' from %s%s%s",
                            who, peer, *detail ? ": " : "", detail);
    break;
  }
}

static bool quack_start_listener()
{
  while (!mysqld_server_started && !abort_loop && !quack_stop_requested)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (abort_loop || quack_stop_requested)
    return false;
  if (opt_noacl)
  {
    sql_print_error("DuckDB: Quack is not started with --skip-grant-tables");
    return false;
  }
  auto bridge= quack_bridge::Bridge();
  if (!bridge)
  {
    sql_print_error("DuckDB: Quack is not started: the ACL worker stopped");
    return false;
  }
  return myduck::DuckdbManager::StartQuack(quack_listener_options(), bridge,
                                           quack_audit);
}

static void quack_start_when_ready()
{
  bool started= false;
  try
  {
    started= quack_start_listener();
  }
  catch (...)
  {
    sql_print_error("DuckDB: Quack is not started: internal error");
  }
  /* The ACL worker serves only the listener (and the test function) */
  if (!started && !quack_bridge_test)
    quack_bridge::StopBridge();
}

static void quack_start()
{
  if (!quack_enabled)
    return;
  if (!quack_bridge::StartBridge(quack_hton))
  {
    sql_print_error("DuckDB: Quack is not started: the ACL worker did not "
                    "start");
    return;
  }
  quack_stop_requested= false;
  try
  {
    quack_starter= std::thread(quack_start_when_ready);
  }
  catch (...)
  {
    sql_print_error("DuckDB: Quack is not started: no thread");
  }
}

void quack_stop()
{
  quack_stop_requested= true;
  if (quack_starter.joinable())
    quack_starter.join();
  myduck::DuckdbManager::StopQuack();
  quack_bridge::StopBridge();
}

/* After client connections are closed, before plugins go down */
static void quack_pre_shutdown(void) { quack_stop(); }

void quack_init(handlerton *hton)
{
  quack_hton= hton;
  quack_bridge::acl_queue_size= quack_acl_queue_size;
  quack_bridge::acl_timeout_ms= quack_acl_timeout_ms;
  hton->pre_shutdown= quack_pre_shutdown;
  if (quack_bridge_test && !quack_bridge::StartBridge(hton))
    sql_print_warning("DuckDB: could not start the Quack bridge");
  quack_start();
}


/* Status: copied into the status buffer of the caller */
static int show_quack_running(MYSQL_THD, struct st_mysql_show_var *var,
                              void *buff, system_status_var *, enum_var_type)
{
  var->type= SHOW_BOOL;
  var->value= (char *) buff;
  *(bool *) buff= myduck::DuckdbManager::QuackStatus().running;
  return 0;
}

static int show_quack_endpoints(MYSQL_THD, struct st_mysql_show_var *var,
                                void *buff, system_status_var *, enum_var_type)
{
  std::string endpoints;
  for (auto &endpoint : myduck::DuckdbManager::QuackStatus().endpoints)
    endpoints+= (endpoints.empty() ? "" : ",") + endpoint;
  var->type= SHOW_CHAR;
  var->value= (char *) buff;
  strmake((char *) buff, endpoints.c_str(), SHOW_VAR_FUNC_BUFF_SIZE - 1);
  return 0;
}

static int show_quack_fingerprint(MYSQL_THD, struct st_mysql_show_var *var,
                                  void *buff, system_status_var *,
                                  enum_var_type)
{
  var->type= SHOW_CHAR;
  var->value= (char *) buff;
  strmake((char *) buff,
          myduck::DuckdbManager::QuackStatus().fingerprint.c_str(),
          SHOW_VAR_FUNC_BUFF_SIZE - 1);
  return 0;
}

static int show_quack_sessions(MYSQL_THD, struct st_mysql_show_var *var,
                               void *buff, system_status_var *, enum_var_type)
{
  var->type= SHOW_LONGLONG;
  var->value= (char *) buff;
  *(longlong *) buff= (longlong) myduck::DuckdbManager::QuackSessions();
  return 0;
}

/* Counters since server start */
template <uint64_t quack_guard::GuardCounters::*field>
static int show_quack_counter(MYSQL_THD, struct st_mysql_show_var *var,
                              void *buff, system_status_var *, enum_var_type)
{
  var->type= SHOW_LONGLONG;
  var->value= (char *) buff;
  *(longlong *) buff=
      (longlong) (myduck::DuckdbManager::QuackCounters().*field);
  return 0;
}

#define QUACK_STATUS(name, function) {name, (char *) function, SHOW_SIMPLE_FUNC}
#define QUACK_COUNTER(field)                                                  \
  QUACK_STATUS(#field, show_quack_counter<&quack_guard::GuardCounters::field>)

struct st_mysql_show_var quack_status_variables[]= {
    QUACK_STATUS("running", show_quack_running),
    QUACK_STATUS("endpoints", show_quack_endpoints),
    QUACK_STATUS("ssl_fingerprint", show_quack_fingerprint),
    QUACK_STATUS("sessions", show_quack_sessions),
    QUACK_COUNTER(connections_refused),
    QUACK_COUNTER(logins),
    QUACK_COUNTER(logins_refused),
    QUACK_COUNTER(statements),
    QUACK_COUNTER(statements_refused),
    QUACK_COUNTER(acl_timeouts),
    QUACK_COUNTER(limit_refusals),
    QUACK_COUNTER(sessions_evicted),
    {NullS, NullS, SHOW_LONG}};
