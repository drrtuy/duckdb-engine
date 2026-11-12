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

#include "guard_internal.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace quack_guard
{

const std::string PrincipalState::KEY= "mariadb_quack_principal";

std::shared_ptr<Guard> Install(duckdb::DatabaseInstance &db,
                               duckdb::Connection &con)
{
  Lockdown(db, con);
  auto guard= std::make_shared<GuardImpl>(db);
  RegisterPlanGate(db, guard);
  RegisterCatalogFunction(db, guard);
  return guard;
}

/* Quack's server registers this on every session it creates
   (QuackSessionState::KEY in quack_session_state.hpp). */
static const std::string QUACK_SESSION_KEY= "quack_session";

GuardCounters GuardImpl::Counters() const
{
  GuardCounters out;
  out.connections_refused= *connections_refused;
  out.logins= counters->logins;
  out.logins_refused= counters->logins_refused;
  out.statements= counters->statements;
  out.statements_refused= counters->statements_refused;
  out.acl_timeouts= counters->acl_timeouts;
  out.limit_refusals= counters->limit_refusals;
  out.sessions_evicted= counters->sessions_evicted;
  return out;
}

const char *ReasonText(Reason reason)
{
  switch (reason)
  {
  case Reason::Allowed: return "allowed";
  case Reason::Denied: return "denied";
  case Reason::NoSuchAccount: return "no such account";
  case Reason::Locked: return "account locked or blocked after wrong passwords";
  case Reason::Expired: return "password expired";
  case Reason::RequirementUnmet: return "REQUIRE not met";
  case Reason::Unsupported: return "authentication method or host not supported";
  case Reason::Timeout: return "ACL worker timeout";
  case Reason::Saturated: return "connection limit or ACL queue full";
  case Reason::Shutdown: return "shutting down";
  case Reason::Internal: return "internal error";
  }
  return "internal error";
}

/* Client-chosen text in a log line: no control characters (a forged log
   line), bounded length */
static std::string LogSafe(const std::string &text)
{
  static const size_t MAX_LENGTH= 200;
  std::string out;
  for (unsigned char c : text)
  {
    if (out.size() >= MAX_LENGTH)
    {
      out+= "...";
      break;
    }
    out+= (c < 0x20 || c == 0x7f) ? '?' : (char) c;
  }
  return out;
}

void GuardImpl::Audit(AuditEvent event)
{
  switch (event.kind)
  {
  case AuditEvent::Kind::Login: counters->logins++; break;
  case AuditEvent::Kind::LoginRefused: counters->logins_refused++; break;
  case AuditEvent::Kind::StatementRefused: counters->statements_refused++; break;
  case AuditEvent::Kind::SessionClosed: break;
  }
  if (event.reason == Reason::Timeout)
    counters->acl_timeouts++;
  else if (event.reason == Reason::Saturated)
    counters->limit_refusals++;
  std::shared_ptr<const AuditLog> log;
  {
    std::lock_guard<std::mutex> guard(lock);
    log= audit_log;
  }
  if (!log || !*log)
    return;
  event.principal= LogSafe(event.principal);
  event.detail= LogSafe(event.detail);
  try
  {
    (*log)(event);
  }
  catch (...)
  {
  }
}

bool IsQuackSession(duckdb::ClientContext &context)
{
  /* A Quack session without a principal (e.g. one a future code path
     forgot to attach) is still a Quack session, and gets nothing. */
  auto &registered= *context.registered_state;
  return registered.Get<duckdb::ClientContextState>(PrincipalState::KEY) ||
         registered.Get<duckdb::ClientContextState>(QUACK_SESSION_KEY);
}

void AttachPrincipal(duckdb::ClientContext &context,
                     std::shared_ptr<const Identity> identity,
                     const std::string &peer)
{
  if (!identity)
    throw duckdb::InternalException("Quack: no principal to attach");
  auto *wanted= identity.get();
  auto state= context.registered_state->GetOrCreate<PrincipalState>(
      PrincipalState::KEY, std::move(identity), peer);
  if (state->identity.get() != wanted)
    throw duckdb::InternalException("Quack: session already has a principal");
}

void HardenSession(duckdb::ClientContext &context)
{
  /* An unknown table name never becomes a file read (the statement gate
     already refuses unknown names; this is the second line) */
  context.config.use_replacement_scans= false;
}

duckdb::shared_ptr<PrincipalState> GetPrincipalState(duckdb::ClientContext &context)
{
  return context.registered_state->Get<PrincipalState>(PrincipalState::KEY);
}

std::shared_ptr<const Identity> GetPrincipal(duckdb::ClientContext &context)
{
  auto state= context.registered_state->Get<PrincipalState>(PrincipalState::KEY);
  return state ? state->identity : nullptr;
}

} /* namespace quack_guard */
