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
  The plan gate: a post-bind hook that decides, from the bound plan, whether
  a Quack session may run a statement. Contexts without a principal (every
  MariaDB-side connection, DuckDB's internal ones) are not affected.
*/

#include "guard_internal.hpp"

#include "duckdb/planner/bound_statement.hpp"
#include "duckdb/planner/planner_extension.hpp"

namespace quack_guard
{

static void post_bind(duckdb::PlannerExtensionInput &input,
                      duckdb::BoundStatement &)
{
  if (!IsQuackSession(input.context))
    return;
  /* No statement is allowed yet. */
  throw duckdb::PermissionException("Quack: statement not allowed");
}

void RegisterPlanGate(duckdb::DatabaseInstance &db,
                      std::shared_ptr<GuardImpl> guard)
{
  duckdb::PlannerExtension extension;
  extension.post_bind_function= post_bind;
  extension.planner_info= duckdb::shared_ptr<duckdb::PlannerExtensionInfo>(
      std::shared_ptr<duckdb::PlannerExtensionInfo>(std::move(guard)));
  duckdb::PlannerExtension::Register(duckdb::DBConfig::GetConfig(db),
                                     std::move(extension));
}

} /* namespace quack_guard */
