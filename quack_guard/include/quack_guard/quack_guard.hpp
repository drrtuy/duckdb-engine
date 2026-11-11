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

#include <memory>

namespace duckdb
{
class ClientContext;
class Connection;
class DatabaseInstance;
} /* namespace duckdb */

namespace quack_guard
{

/*
  The guard of one database: owns the authorizer (and, later, the listener).
  Without an authorizer every Quack session is denied.
*/
class Guard
{
public:
  virtual ~Guard()= default;

  /* Install or replace the authorizer; null removes it (deny all). */
  virtual void SetAuthorizer(std::shared_ptr<QuackAuthorizer> authorizer)= 0;
  virtual std::shared_ptr<QuackAuthorizer> GetAuthorizer() const= 0;
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
