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
  return guard;
}

/* Quack's server registers this on every session it creates
   (QuackSessionState::KEY in quack_session_state.hpp). */
static const std::string QUACK_SESSION_KEY= "quack_session";

bool IsQuackSession(duckdb::ClientContext &context)
{
  /* A Quack session without a principal (e.g. one a future code path
     forgot to attach) is still a Quack session, and gets nothing. */
  auto &registered= *context.registered_state;
  return registered.Get<duckdb::ClientContextState>(PrincipalState::KEY) ||
         registered.Get<duckdb::ClientContextState>(QUACK_SESSION_KEY);
}

void AttachPrincipal(duckdb::ClientContext &context,
                     std::shared_ptr<const Identity> identity)
{
  if (!identity)
    throw duckdb::InternalException("Quack: no principal to attach");
  auto *wanted= identity.get();
  auto state= context.registered_state->GetOrCreate<PrincipalState>(
      PrincipalState::KEY, std::move(identity));
  if (state->identity.get() != wanted)
    throw duckdb::InternalException("Quack: session already has a principal");
}

std::shared_ptr<const Identity> GetPrincipal(duckdb::ClientContext &context)
{
  auto state= context.registered_state->Get<PrincipalState>(PrincipalState::KEY);
  return state ? state->identity : nullptr;
}

} /* namespace quack_guard */
