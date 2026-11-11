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

/* Internal to quack_guard. */

#pragma once

#include "quack_guard/quack_guard.hpp"
#include "session_registry.hpp"

#include "duckdb.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/planner/planner_extension.hpp"

#include <atomic>
#include <mutex>

namespace quack_guard
{

/* The guard of one database. Owned by the database (as the planner
   extension's info) and by the plugin; reached from DuckDB hooks through the
   planner extension's info. */
class GuardImpl : public Guard,
                  public duckdb::PlannerExtensionInfo,
                  public std::enable_shared_from_this<GuardImpl>
{
public:
  explicit GuardImpl(duckdb::DatabaseInstance &db_p)
      : db(db_p.shared_from_this())
  {
  }

  void SetAuthorizer(std::shared_ptr<QuackAuthorizer> authorizer_p) override
  {
    std::lock_guard<std::mutex> guard(lock);
    authorizer= std::move(authorizer_p);
  }
  std::shared_ptr<QuackAuthorizer> GetAuthorizer() const override
  {
    std::lock_guard<std::mutex> guard(lock);
    return authorizer;
  }
  std::unique_ptr<Listener>
  StartListener(const ListenerOptions &options) override;
  size_t SessionCount() const override { return sessions.Count(); }

  SessionRegistry &Sessions() { return sessions; }
  /* One listener per guard: its reaper owns the registry. */
  void ListenerStopped() { listener_active= false; }

private:
  std::atomic<bool> listener_active{false};
  /* Not owning: the guard is owned by the database */
  duckdb::weak_ptr<duckdb::DatabaseInstance> db;
  mutable std::mutex lock;
  std::shared_ptr<QuackAuthorizer> authorizer;
  SessionRegistry sessions;
};

/* Load Quack and pin its stock callbacks to deny-all system functions. */
void Lockdown(duckdb::DatabaseInstance &db, duckdb::Connection &con);

enum class GateDecision
{
  Allowed,
  /* The form of the statement: answered STATEMENT_DENIED */
  StatementDenied,
  /* About an object (missing, reserved, not a table): answered ACCESS_DENIED
     like a denied one, so existence is not revealed */
  AccessDenied
};

/* The statement gate: may a Quack session run `sql`? Parses, and looks up
   referenced tables on a separate connection; binds nothing. On refusal
   `why` says what was refused (for logs, never for clients). */
GateDecision StatementAllowed(duckdb::ClientContext &context,
                              const std::string &sql, std::string &why);

/* Per-session hardening of a new Quack session's context. */
void HardenSession(duckdb::ClientContext &context);

/* Catalog discovery (catalog_filter.cpp): the filtering table function, and
   recognizing the client's catalog queries. */
extern const char *const CATALOG_FUNCTION;
void RegisterCatalogFunction(duckdb::DatabaseInstance &db,
                             const std::shared_ptr<GuardImpl> &guard);
/* True if `sql` is one of Quack's catalog load queries; `replacement` is
   then the filtered query to run instead. */
bool CatalogQuery(const std::string &sql, std::string &replacement);

/* Register the post-bind hook that authorizes Quack sessions. */
void RegisterPlanGate(duckdb::DatabaseInstance &db,
                      std::shared_ptr<GuardImpl> guard);

/* The principal of a Quack session; stored in ClientContext::registered_state. */
class PrincipalState : public duckdb::ClientContextState
{
public:
  static const std::string KEY;

  explicit PrincipalState(std::shared_ptr<const Identity> identity_p)
      : identity(std::move(identity_p))
  {
  }

  /* Each statement starts unauthorized; the plan gate sets it once the
     statement's access is granted. Errors before that are masked. */
  void QueryBegin(duckdb::ClientContext &) override { authorized= false; }
  std::atomic<bool> authorized{false};

  /* Re-plan (and so re-authorize) every EXECUTE of a prepared statement. */
  duckdb::RebindQueryInfo
  OnRebindPreparedStatement(duckdb::ClientContext &,
                            duckdb::BindPreparedStatementCallbackInfo &,
                            duckdb::RebindQueryInfo) override
  {
    return duckdb::RebindQueryInfo::ATTEMPT_TO_REBIND;
  }

  const std::shared_ptr<const Identity> identity;
};

/*
  Attach `identity` to the context of a newly created Quack session, before
  the client learns the session id. Once only; it cannot be replaced.
  Called by the listener; not part of the public API.
*/
void AttachPrincipal(duckdb::ClientContext &context,
                     std::shared_ptr<const Identity> identity);

/* The session's principal state, or null for a non-Quack context. */
duckdb::shared_ptr<PrincipalState> GetPrincipalState(duckdb::ClientContext &context);

/* The session's principal, or null for a non-Quack context. */
std::shared_ptr<const Identity> GetPrincipal(duckdb::ClientContext &context);

} /* namespace quack_guard */
