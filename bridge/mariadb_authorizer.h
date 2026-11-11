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
  The MariaDB side of the Quack bridge: QuackAuthorizer on top of MariaDB
  accounts, roles, PUBLIC and grants. Every call re-resolves the account
  from the live ACL caches on the ACL worker; nothing is cached, so GRANT,
  REVOKE and DROP USER take effect at the next statement.
*/

#pragma once

#include "acl_worker.h"
#include "quack_guard/authorizer.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <string>

struct handlerton;

namespace quack_bridge
{

/* A MariaDB principal: the login claims plus what the server matched */
class MariaDBIdentity : public quack_guard::Identity
{
public:
  std::string user;       /* the login name */
  std::string host;       /* "localhost" for loopback, else the IP literal */
  std::string ip;         /* the peer address */
  std::string priv_user;  /* the matched account, pinned at login */
  std::string priv_host;
  std::string role;       /* active role, empty for none */

  std::string audit() const override
  {
    return user + "@" + host + (role.empty() ? "" : "/" + role);
  }
};

struct BridgeSettings
{
  /* Read on every call */
  std::atomic<size_t> *queue_size= nullptr;
  std::atomic<uint64_t> *timeout_ms= nullptr;
};

class MariaDBAuthorizer : public quack_guard::QuackAuthorizer
{
public:
  /* `engine`: the DuckDB handlerton; only its tables are ever allowed */
  MariaDBAuthorizer(handlerton *engine, BridgeSettings settings);
  ~MariaDBAuthorizer() override;

  bool Start() { return worker.Start(); }
  void Stop() { worker.Stop(); }

  quack_guard::Verdict
  authenticate(const quack_guard::ClientClaims &claims,
               const quack_guard::TransportFacts &transport,
               std::shared_ptr<const quack_guard::Identity> &out) override;

  quack_guard::Verdict
  check(const quack_guard::Identity &identity,
        const std::vector<quack_guard::AccessRequest> &requests) override;

  std::vector<quack_guard::ObjectName>
  filterVisible(const quack_guard::Identity &identity,
                const std::vector<quack_guard::ObjectName> &objects) override;

  void sessionClosed(const quack_guard::Identity &identity) override;

  /*
    Resolve `user` at a peer address to the account MariaDB would match (no
    credential check); for the bridge test entry point and for K7's login.
  */
  quack_guard::Verdict resolve(const std::string &user, const std::string &ip,
                               const std::string &role,
                               std::shared_ptr<const MariaDBIdentity> &out);

private:
  quack_guard::Verdict run(AclWorker::Job job);
  quack_guard::Verdict
  authenticate_unsafe(const quack_guard::ClientClaims &claims,
                      const quack_guard::TransportFacts &transport,
                      std::shared_ptr<const quack_guard::Identity> &out);
  /* The public calls wrap these and turn any exception into a denial */
  quack_guard::Verdict resolve_unsafe(const std::string &user,
                                      const std::string &ip,
                                      const std::string &role,
                                      std::shared_ptr<const MariaDBIdentity> &out);
  quack_guard::Verdict
  check_unsafe(const quack_guard::Identity &identity,
               const std::vector<quack_guard::AccessRequest> &requests);
  std::vector<quack_guard::ObjectName>
  filter_unsafe(const quack_guard::Identity &identity,
                const std::vector<quack_guard::ObjectName> &objects);

  handlerton *engine;
  BridgeSettings settings;
  AclWorker worker;
  /* Per account ("priv_user@priv_host"): wrong passwords in a row, and
     open Quack sessions */
  std::mutex state_lock;
  struct Failures
  {
    std::string row_version;  /* the account as it was when counted */
    int count= 0;
  };
  std::map<std::string, Failures> failures;
  std::map<std::string, int64_t> sessions;
};

/* The account host for a peer address, as MariaDB derives it at login:
   "localhost" for loopback unless skip_name_resolve, else the address.
   (Non-loopback peers are only supported with skip_name_resolve.) */
std::string HostForPeer(const std::string &ip);

} /* namespace quack_bridge */
