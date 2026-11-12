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
  A scriptable QuackAuthorizer for quack_guard tests: no MariaDB involved.
  Records every call so tests can assert what the guard asked for.
*/

#pragma once

#include "quack_guard/authorizer.hpp"

#include <functional>
#include <atomic>
#include <mutex>

namespace quack_guard_test
{

using namespace quack_guard;

class MockIdentity : public Identity
{
public:
  explicit MockIdentity(std::string name_p) : name(std::move(name_p)) {}
  std::string audit() const override { return name; }
  const std::string name;
};

class MockAuthorizer : public QuackAuthorizer
{
public:
  /* Defaults deny everything. */
  std::function<Verdict(const ClientClaims &, const TransportFacts &)>
      on_authenticate= [](const ClientClaims &, const TransportFacts &) {
        return Verdict::Deny(Reason::Denied);
      };
  std::function<Verdict(const Identity &, const std::vector<AccessRequest> &)>
      on_check= [](const Identity &, const std::vector<AccessRequest> &) {
        return Verdict::Deny(Reason::Denied);
      };
  std::function<std::vector<ObjectName>(const Identity &,
                                        const std::vector<ObjectName> &)>
      on_filter= [](const Identity &, const std::vector<ObjectName> &) {
        return std::vector<ObjectName>();
      };

  Verdict authenticate(const ClientClaims &claims,
                       const TransportFacts &transport,
                       std::shared_ptr<const Identity> &out) override
  {
    {
      std::lock_guard<std::mutex> guard(lock);
      authenticate_calls++;
      transports.push_back(transport);
    }
    Verdict v= on_authenticate(claims, transport);
    if (v.allow)
    {
      out= std::make_shared<MockIdentity>(
          claims.user + "@" + transport.peer_ip +
          (claims.role.empty() ? "" : "/" + claims.role) + "#" +
          std::to_string(++identities));
      std::lock_guard<std::mutex> guard(lock);
      allow_calls++;
    }
    return v;
  }

  /* Enforces the contract's denials before asking the script. */
  Verdict check(const Identity &identity,
                const std::vector<AccessRequest> &requests) override
  {
    {
      std::lock_guard<std::mutex> guard(lock);
      check_calls++;
      checks.push_back({identity.audit(), requests});
    }
    if (requests.empty())
      return Verdict::Deny(Reason::Denied);
    for (const auto &r : requests)
      if (r.columns.empty() && !r.any_column)
        return Verdict::Deny(Reason::Denied);
    return on_check(identity, requests);
  }

  std::vector<ObjectName>
  filterVisible(const Identity &identity,
                const std::vector<ObjectName> &objects) override
  {
    {
      std::lock_guard<std::mutex> guard(lock);
      filter_calls++;
    }
    return on_filter(identity, objects);
  }

  void sessionClosed(const Identity &identity) override
  {
    std::lock_guard<std::mutex> guard(lock);
    closed_calls++;
    closed_identities.push_back(identity.audit());
  }

  /* Guards the records below; read them only when no call is running. */
  std::mutex lock;
  int authenticate_calls= 0, check_calls= 0, filter_calls= 0, closed_calls= 0;
  int allow_calls= 0;
  std::atomic<int> identities{0};
  std::vector<std::string> closed_identities;
  std::vector<TransportFacts> transports;
  std::vector<std::pair<std::string, std::vector<AccessRequest>>> checks;
};

} /* namespace quack_guard_test */
