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
  The bridge between quack_guard (DuckDB side) and the MariaDB server.

  This is the only header both sides include. quack_guard never includes
  MariaDB headers; the MariaDB side implements QuackAuthorizer on top of
  MariaDB accounts, roles and grants.

  Contract (every implementation and every caller):
  - Anything but an explicit allow is a deny. A default Verdict denies.
  - check() with no requests denies; a request with no columns and
    any_column == false denies.
  - filterVisible() returns a new list; on any error it returns an empty one.
  - Names cross the bridge byte for byte as DuckDB stores them; the MariaDB
    side normalizes them (lower_case_table_names etc.).
  - Bridge calls never call back into DuckDB and may block (bounded by the
    implementation's own timeout).
*/

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace quack_guard
{

enum class Reason
{
  Allowed,
  Denied,
  NoSuchAccount,
  Locked,
  Expired,
  RequirementUnmet,
  Unsupported,
  Timeout,
  Saturated,
  Shutdown,
  Internal
};

struct Verdict
{
  bool allow= false;
  Reason reason= Reason::Internal;

  static Verdict Allow() { return {true, Reason::Allowed}; }
  static Verdict Deny(Reason r) { return {false, r}; }
};

/* Facts about the transport, filled by the server from the socket, never
   from message content. */
struct TransportFacts
{
  std::string peer_ip;
  bool tls= false;
  bool client_cert_verified= false;
  std::string cert_subject;
  std::string cert_issuer;
  std::string cipher;
};

/* What the client claims. The caller wipes `proof` after the call; an
   implementation that copies it (e.g. onto a worker queue) wipes its copy. */
struct ClientClaims
{
  std::string user;
  std::string role;           /* empty: the account's default role */
  std::string proof;          /* e.g. a password */
};

/* An authenticated MariaDB principal. Opaque to quack_guard except for
   audit(); created only by the authorizer, immutable afterwards.
   An Identity is self-contained: it holds no MariaDB resources, can be
   destroyed on any thread and may outlive the authorizer. */
class Identity
{
public:
  virtual ~Identity()= default;
  /* "user@host[/role]", for logs and errors only, never for decisions */
  virtual std::string audit() const= 0;
};

enum class Access
{
  Select
};

struct AccessRequest
{
  std::string catalog;
  std::string schema;
  std::string table;
  std::vector<std::string> columns;
  bool any_column= false;  /* e.g. count(*): any privilege on the table */
  Access want= Access::Select;
};

enum class ObjectKind
{
  Schema,
  Table,
  Column
};

struct ObjectName
{
  ObjectKind kind= ObjectKind::Table;
  std::string catalog, schema, table, column;
};

class QuackAuthorizer
{
public:
  virtual ~QuackAuthorizer()= default;

  /* On allow, sets `out` to the principal. */
  virtual Verdict authenticate(const ClientClaims &claims,
                               const TransportFacts &transport,
                               std::shared_ptr<const Identity> &out)= 0;

  /* All requests of one statement; allowed only if every one is. */
  virtual Verdict check(const Identity &identity,
                        const std::vector<AccessRequest> &requests)= 0;

  /* The subset of `objects` the principal may see. */
  virtual std::vector<ObjectName>
  filterVisible(const Identity &identity,
                const std::vector<ObjectName> &objects)= 0;

  /* Exactly once per identity returned by authenticate(), when its session
     ends or could not be created. */
  virtual void sessionClosed(const Identity &identity)= 0;
};

} /* namespace quack_guard */
