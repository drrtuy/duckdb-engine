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
  a Quack session may run a statement, and asks the authorizer for every
  base table and column it reads. Contexts that are not Quack sessions
  (every MariaDB-side connection, DuckDB's internal ones) return at once.
  Milestone 1: read-only queries over base tables; operators, expressions
  and functions outside the lists are refused.
*/

#include "allowlist.hpp"
#include "guard_internal.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/planner/bound_statement.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/planner_extension.hpp"

#include <map>
#include <set>
#include <tuple>

namespace quack_guard
{

using namespace duckdb;

namespace
{

/* Thrown to abandon the walk; turned into the uniform refusal. */
struct Refused
{
  std::string why;
};

[[noreturn]] void refuse(const std::string &why) { throw Refused{why}; }

/* Base tables read by the statement: (catalog, schema, table) -> columns */
struct Reads
{
  std::map<std::tuple<std::string, std::string, std::string>,
           std::pair<std::set<std::string>, bool /* any_column */>>
      tables;
};

void check_plan(const LogicalOperator &op, Reads &reads);

void check_window(const BoundWindowExpression &window)
{
  if (window.AggregateFunction())
  {
    if (!FunctionAllowed(window.AggregateFunction()->GetName().GetIdentifierName()))
      refuse("window aggregate");
    return;
  }
  switch (window.GetExpressionType())
  {
  case ExpressionType::WINDOW_ROW_NUMBER:
  case ExpressionType::WINDOW_RANK:
  case ExpressionType::WINDOW_RANK_DENSE:
  case ExpressionType::WINDOW_PERCENT_RANK:
  case ExpressionType::WINDOW_CUME_DIST:
  case ExpressionType::WINDOW_NTILE:
  case ExpressionType::WINDOW_LEAD:
  case ExpressionType::WINDOW_LAG:
  case ExpressionType::WINDOW_FIRST_VALUE:
  case ExpressionType::WINDOW_LAST_VALUE:
  case ExpressionType::WINDOW_NTH_VALUE:
    return;
  default:
    refuse("window function");
  }
}

void check_expression(const Expression &expr, Reads &reads)
{
  switch (expr.GetExpressionClass())
  {
  case ExpressionClass::BOUND_COLUMN_REF:
  case ExpressionClass::BOUND_REF:
  case ExpressionClass::BOUND_CONSTANT:
  case ExpressionClass::LEGACY_BOUND_CAST:
  case ExpressionClass::LEGACY_BOUND_COMPARISON:
  case ExpressionClass::BOUND_CONJUNCTION:
  case ExpressionClass::BOUND_CASE:
  case ExpressionClass::BOUND_OPERATOR:
  case ExpressionClass::LEGACY_BOUND_BETWEEN:
    break;
  case ExpressionClass::BOUND_FUNCTION:
  {
    auto &name=
        expr.Cast<BoundFunctionExpression>().Function().GetName().GetIdentifierName();
    if (!FunctionAllowed(name))
      refuse("function " + name);
  }
    break;
  case ExpressionClass::BOUND_AGGREGATE:
  {
    auto &name=
        expr.Cast<BoundAggregateExpression>().Function().GetName().GetIdentifierName();
    if (!FunctionAllowed(name))
      refuse("aggregate " + name);
  }
    break;
  case ExpressionClass::BOUND_WINDOW:
    check_window(expr.Cast<BoundWindowExpression>());
    break;
  case ExpressionClass::BOUND_SUBQUERY:
  {
    auto &plan= expr.Cast<BoundSubqueryExpression>().Subquery().plan;
    if (!plan)
      refuse("subquery");
    check_plan(*plan, reads);
    break;
  }
  default:
    refuse("expression " + ExpressionClassToString(expr.GetExpressionClass()));
  }
  ExpressionIterator::EnumerateChildren(
      expr, [&](const Expression &child) { check_expression(child, reads); });
}

thread_local Identifier default_database;

void check_get(const LogicalGet &get, Reads &reads)
{
  auto table= get.GetTable();
  if (!table || get.function.GetName().GetIdentifierName() != "seq_scan")
    refuse("table function");
  auto &catalog= table->ParentCatalog();
  if (catalog.IsSystemCatalog() || catalog.IsTemporaryCatalog() ||
      catalog.GetName() != default_database)
    refuse("other catalog");
  std::string schema= table->ParentSchema().name.GetIdentifierName();
  std::string name= table->name.GetIdentifierName();
  if (SchemaReserved(schema) || TableReserved(name))
    refuse("reserved table");
  auto &entry= reads.tables[std::make_tuple(
      catalog.GetName().GetIdentifierName(), schema, name)];
  auto &columns= get.GetColumnIds();
  if (columns.empty())
    entry.second= true;  /* count(*) and the like */
  for (auto &column : columns)
  {
    if (column.IsVirtualColumn())
    {
      entry.second= true;  /* rowid etc.: any privilege on the table */
      continue;
    }
    entry.first.insert(
        table->GetColumn(column.ToLogical()).Name().GetIdentifierName());
  }
}

void check_plan(const LogicalOperator &op, Reads &reads)
{
  switch (op.type)
  {
  case LogicalOperatorType::LOGICAL_PROJECTION:
  case LogicalOperatorType::LOGICAL_FILTER:
  case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
  case LogicalOperatorType::LOGICAL_WINDOW:
  case LogicalOperatorType::LOGICAL_LIMIT:
  case LogicalOperatorType::LOGICAL_ORDER_BY:
  case LogicalOperatorType::LOGICAL_TOP_N:
  case LogicalOperatorType::LOGICAL_DISTINCT:
  case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
  case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
  case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
  case LogicalOperatorType::LOGICAL_CTE_REF:
  case LogicalOperatorType::LOGICAL_DELIM_GET:
  case LogicalOperatorType::LOGICAL_JOIN:
  case LogicalOperatorType::LOGICAL_DELIM_JOIN:
  case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
  case LogicalOperatorType::LOGICAL_ANY_JOIN:
  case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
  case LogicalOperatorType::LOGICAL_POSITIONAL_JOIN:
  case LogicalOperatorType::LOGICAL_ASOF_JOIN:
  case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN:
  case LogicalOperatorType::LOGICAL_UNION:
  case LogicalOperatorType::LOGICAL_EXCEPT:
  case LogicalOperatorType::LOGICAL_INTERSECT:
  case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
  case LogicalOperatorType::LOGICAL_EXPLAIN:
    break;
  case LogicalOperatorType::LOGICAL_GET:
    check_get(op.Cast<LogicalGet>(), reads);
    break;
  default:
    refuse("operator " + LogicalOperatorToString(op.type));
  }
  LogicalOperatorVisitor::EnumerateExpressions(
      op, [&](const unique_ptr<Expression> *expr) {
        if (*expr)
          check_expression(**expr, reads);
      });
  for (auto &child : op.children)
    check_plan(*child, reads);
}

void post_bind(PlannerExtensionInput &input, BoundStatement &statement)
{
  if (!IsQuackSession(input.context))
    return;
  auto identity= GetPrincipal(input.context);
  auto *guard= dynamic_cast<GuardImpl *>(input.info.get());
  auto authorizer= guard ? guard->GetAuthorizer() : nullptr;
  if (!identity || !authorizer || !statement.plan)
    throw PermissionException(ACCESS_DENIED);

  Reads reads;
  default_database= DatabaseManager::GetDefaultDatabase(input.context);
  try
  {
    check_plan(*statement.plan, reads);
  }
  catch (const Refused &)
  {
    /* The plan exists only because every name resolved: answering
       differently from a missing object would reveal that it exists. */
    throw PermissionException(ACCESS_DENIED);
  }
  auto state= GetPrincipalState(input.context);
  if (reads.tables.empty())
  {
    if (state)
      state->authorized= true;
    return;  /* nothing to authorize: constants */
  }

  std::vector<AccessRequest> requests;
  for (auto &entry : reads.tables)
  {
    AccessRequest request;
    std::tie(request.catalog, request.schema, request.table)= entry.first;
    request.columns.assign(entry.second.first.begin(),
                           entry.second.first.end());
    request.any_column= entry.second.second;
    requests.push_back(std::move(request));
  }
  Verdict verdict;
  try
  {
    verdict= authorizer->check(*identity, requests);
  }
  catch (...)
  {
    verdict= Verdict::Deny(Reason::Internal);
  }
  if (!verdict.allow)
    throw PermissionException(ACCESS_DENIED);
  if (state)
    state->authorized= true;
}

} /* namespace */

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
