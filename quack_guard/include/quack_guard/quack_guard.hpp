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
  quack_guard: exposes a DuckDB database over Quack with authentication and
  authorization delegated to a QuackAuthorizer. DuckDB-side only; see
  Quack.MD for the design.
*/

#pragma once

#include "quack_guard/authorizer.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duckdb
{
class ClientContext;
class Connection;
class DatabaseInstance;
} /* namespace duckdb */

namespace quack_guard
{

struct ListenerOptions
{
  /* Addresses to bind, one listener each: IPv4 or IPv6 literals
     ("127.0.0.1", "::1" or "[::1]") or host names (first resolved address) */
  std::vector<std::string> addresses;
  /* 0 = any free port: the first address picks one, the others reuse it */
  uint16_t port= 0;
  /* PEM server certificate and private key (unencrypted); TLS is mandatory */
  std::string cert_file;
  std::string key_file;
  /* Concurrent client connections over all addresses; more are refused */
  size_t max_connections= 64;
  /* Authenticated sessions at once; more logins are refused */
  size_t max_sessions= 256;
  /* Largest heartbeat lease a client may ask for, in seconds */
  uint64_t max_heartbeat_seconds= 300;
  /* Largest request body accepted */
  size_t max_request_bytes= 4 * 1024 * 1024;
  /* How often abandoned sessions are looked for, in milliseconds */
  uint64_t reap_interval_ms= 1000;
};

struct ListenerStatus
{
  bool running= false;
  /* "host:port" per bound address ("[::1]:port" for IPv6) */
  std::vector<std::string> endpoints;
  /* SHA-256 fingerprint of the served certificate, for clients to pin */
  std::string fingerprint;
  /* Why a listener thread stopped on its own, empty if none did */
  std::string failure;
};

/* A running listener. Owned by the caller; destroying it stops it. */
class Listener
{
public:
  virtual ~Listener()= default;
  virtual ListenerStatus Status() const= 0;
  /* Stop accepting, interrupt and close every session. Idempotent. Must
     run before the database is destroyed, never on a listener thread. */
  virtual void Stop()= 0;
};

/*
  The guard of one database: holds the authorizer and the session registry.
  Without an authorizer every Quack login is refused.
*/
class Guard
{
public:
  virtual ~Guard()= default;

  /* Install or replace the authorizer; null removes it (deny all). Sessions
     keep the authorizer that admitted them until they close. */
  virtual void SetAuthorizer(std::shared_ptr<QuackAuthorizer> authorizer)= 0;
  virtual std::shared_ptr<QuackAuthorizer> GetAuthorizer() const= 0;

  /* Start a listener. Throws std::exception (nothing is left listening). */
  virtual std::unique_ptr<Listener>
  StartListener(const ListenerOptions &options)= 0;

  /* Open Quack sessions */
  virtual size_t SessionCount() const= 0;
};

/*
  Load Quack, close its stock callbacks and register the guard's planner
  hook. Call once per database, before lock_configuration. Throws on
  failure; the caller must then not use the database.
*/
std::shared_ptr<Guard> Install(duckdb::DatabaseInstance &db,
                               duckdb::Connection &con);

/* True for a context that serves a Quack client (has a principal or was
   created by Quack's server). */
bool IsQuackSession(duckdb::ClientContext &context);

} /* namespace quack_guard */
