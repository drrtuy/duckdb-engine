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
  The statement gate: decides from the parsed SQL, before anything is bound,
  whether a Quack session may run it. Binding already has effects (table
  functions open files, constant arguments are evaluated, binder errors name
  objects), so whatever the plan gate cannot see in time is refused here.
  Milestone 1: one SELECT (or EXPLAIN of one) over base tables, or a
  transaction statement. Everything not listed is refused.
*/

#include "allowlist.hpp"
#include "guard_internal.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/statement/transaction_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

#include <algorithm>
#include <set>

namespace quack_guard
{

using namespace duckdb;

namespace
{

/* Thrown to abandon the walk; never leaves this file. */
struct Refused
{
  GateDecision decision;
  std::string why;
};

[[noreturn]] void refuse(const std::string &why)
{
  throw Refused{GateDecision::StatementDenied, why};
}
/* About an object: answered like a missing or denied one */
[[noreturn]] void refuse_object(const std::string &why)
{
  throw Refused{GateDecision::AccessDenied, why};
}

std::string lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char) std::tolower(c); });
  return s;
}

/* Walk state: the session, the CTE names in scope, and the base tables to
   look up once the whole form has been checked (a form refusal must win
   over an object refusal, or the answer would tell whether a table exists) */
struct Walk
{
  ClientContext &context;
  std::vector<std::set<std::string>> ctes;
  std::vector<QualifiedName> tables;
};

thread_local Walk *walk_state= nullptr;  /* set for the duration of one StatementAllowed */

void check_node(QueryNode &node);

/* Built-in types a cast may target; no user types, nothing to autoload */
bool type_allowed(const TypeExpression &type)
{
  static const std::set<std::string> names= {
      "boolean", "bool", "tinyint", "int1", "smallint", "int2", "short",
      "integer", "int", "int4", "signed", "bigint", "int8", "long", "hugeint",
      "int128", "utinyint", "usmallint", "uinteger", "ubigint", "uhugeint",
      "float", "real", "float4", "double", "float8", "decimal", "numeric",
      "varchar", "text", "string", "char", "bpchar", "date", "time",
      "timestamp", "datetime", "timestamptz", "interval", "blob", "bytea",
      "uuid"};
  if (!type.GetCatalog().empty() || !type.GetSchema().empty())
    return false;
  if (!names.count(lower(type.GetTypeName().GetIdentifierName())))
    return false;
  for (auto &child : type.GetChildren())
    if (!child || child->GetExpressionClass() != ExpressionClass::CONSTANT)
      return false;
  return true;
}

void check_function_name(const QualifiedName &name)
{
  /* No catalog or schema qualification: only the built-in function */
  if (!name.Catalog().empty() || !name.Schema().empty())
    refuse("qualified function " + name.Name().GetIdentifierName());
  if (!FunctionAllowed(name.Name().GetIdentifierName()))
    refuse("function " + name.Name().GetIdentifierName());
}

void check_expression(const ParsedExpression &expr)
{
  switch (expr.GetExpressionClass())
  {
  case ExpressionClass::COLUMN_REF:
  case ExpressionClass::CONSTANT:
  case ExpressionClass::COMPARISON:
  case ExpressionClass::CONJUNCTION:
  case ExpressionClass::CASE:
  case ExpressionClass::BETWEEN:
  case ExpressionClass::POSITIONAL_REFERENCE:
    break;
  case ExpressionClass::OPERATOR:
    switch (expr.GetExpressionType())
    {
    case ExpressionType::OPERATOR_NOT:
    case ExpressionType::OPERATOR_IS_NULL:
    case ExpressionType::OPERATOR_IS_NOT_NULL:
    case ExpressionType::COMPARE_IN:
    case ExpressionType::COMPARE_NOT_IN:
    case ExpressionType::OPERATOR_NULLIF:
    case ExpressionType::OPERATOR_COALESCE:
      break;
    default:
      refuse("operator " + ExpressionTypeToString(expr.GetExpressionType()));
    }
    break;
  case ExpressionClass::CAST:
    if (!type_allowed(expr.Cast<CastExpression>().TargetType()))
      refuse("cast target type");
    break;
  case ExpressionClass::STAR:
  {
    /* Plain "*" or "t.*": no COLUMNS(), EXCLUDE, REPLACE, RENAME, which
       would reveal column names through what they match */
    auto &star= expr.Cast<StarExpression>();
    if (star.IsColumns() || star.Expression() || !star.ExcludeList().empty() ||
        !star.ReplaceList().empty() || !star.RenameList().empty())
      refuse("star form");
    break;
  }
  case ExpressionClass::FUNCTION:
    check_function_name(expr.Cast<FunctionExpression>().GetQualifiedName());
    break;
  case ExpressionClass::WINDOW:
    check_function_name(expr.Cast<WindowExpression>().GetQualifiedName());
    break;
  case ExpressionClass::SUBQUERY:
  {
    auto &subquery= expr.Cast<SubqueryExpression>().Subquery();
    if (!subquery || !subquery->node)
      refuse("empty subquery");
    check_node(*subquery->node);
    break;
  }
  default:
    refuse("expression " + ExpressionClassToString(expr.GetExpressionClass()));
  }
  ParsedExpressionIterator::EnumerateChildren(
      expr, [](const ParsedExpression &child) { check_expression(child); });
}

void check_expression(const unique_ptr<ParsedExpression> &expr)
{
  if (expr)
    check_expression(*expr);
}

/* A base table reference: CTE names in scope are fine; anything else is
   looked up after the walk (look_up_tables). */
void check_base_table(BaseTableRef &base)
{
  if (base.at_clause)
    refuse("AT clause");
  auto &name= base.GetQualifiedName();
  if (name.Path().size() > 3)
    refuse("nested schema path");
  std::string table= name.Name().GetIdentifierName();
  if (name.Catalog().empty() && name.Schema().empty())
    for (auto &scope : walk_state->ctes)
      if (scope.count(lower(table)))
        return;  /* a CTE in scope */
  walk_state->tables.push_back(name);
}

/*
  Every base table must exist, as a table (not a view), in the engine's own
  database and outside the reserved schemas. Looked up on a separate
  connection, all in one transaction, so that binding never sees an unknown
  name: unknown names are where replacement scans read files and extensions
  get autoloaded.
*/
void look_up_tables()
{
  if (walk_state->tables.empty())
    return;
  auto &context= walk_state->context;
  auto database= DatabaseManager::GetDefaultDatabase(context);
  for (auto &name : walk_state->tables)
  {
    if (!name.Catalog().empty() &&
        lower(name.Catalog().GetIdentifierName()) !=
            lower(database.GetIdentifierName()))
      refuse_object("other catalog");
    std::string schema= lower(name.Schema().GetIdentifierName());
    if (SchemaReserved(schema.empty() ? "main" : schema) ||
        TableReserved(name.Name().GetIdentifierName()))
      refuse_object("reserved name");
  }
  bool all_tables= true;
  Connection lookup(*context.db);
  lookup.context->RunFunctionInTransaction([&] {
    for (auto &name : walk_state->tables)
    {
      EntryLookupInfo info(CatalogType::TABLE_ENTRY,
                           QualifiedName(database, name.Schema(), name.Name()));
      auto entry= Catalog::GetEntry(*lookup.context, info,
                                    OnEntryNotFound::RETURN_NULL);
      if (!entry || entry->type != CatalogType::TABLE_ENTRY)
        all_tables= false;
    }
  });
  if (!all_tables)
    refuse_object("no such table");
}

void check_ref(TableRef &ref)
{
  switch (ref.type)
  {
  case TableReferenceType::BASE_TABLE:
    check_base_table(ref.Cast<BaseTableRef>());
    break;
  case TableReferenceType::EMPTY_FROM:
    break;
  case TableReferenceType::JOIN:
  {
    auto &join= ref.Cast<JoinRef>();
    if (join.ref_type == JoinRefType::NEAREST || join.ranking_expression ||
        !join.duplicate_eliminated_columns.empty() ||
        join.ref_type == JoinRefType::DEPENDENT)
      refuse("join form");
    check_ref(*join.left);
    check_ref(*join.right);
    check_expression(join.condition);
    break;
  }
  case TableReferenceType::SUBQUERY:
  {
    auto &subquery= ref.Cast<SubqueryRef>().subquery;
    if (!subquery || !subquery->node)
      refuse("empty subquery");
    check_node(*subquery->node);
    break;
  }
  case TableReferenceType::EXPRESSION_LIST:
    for (auto &row : ref.Cast<ExpressionListRef>().values)
      for (auto &value : row)
        check_expression(value);
    break;
  default:
    refuse("table reference " + EnumUtil::ToString(ref.type));
  }
  if (ref.sample)
    refuse("SAMPLE");
}

void check_node(QueryNode &node)
{
  /* A CTE is visible to the CTEs after it and to the node's body, never to
     itself or to earlier ones: names become visible only once walked. */
  walk_state->ctes.emplace_back();
  struct Pop
  {
    ~Pop() { walk_state->ctes.pop_back(); }
  } pop;
  for (auto &entry : node.cte_map.map)
  {
    auto &cte= *entry.second;
    if (!cte.key_targets.empty() || !cte.payload_aggregates.empty() ||
        !cte.query_node)
      refuse("CTE form");
    check_node(*cte.query_node);
    walk_state->ctes.back().insert(lower(entry.first.GetIdentifierName()));
  }
  ParsedExpressionIterator::EnumerateQueryNodeModifiers(
      node, [](unique_ptr<ParsedExpression> &child) { check_expression(child); });

  switch (node.type)
  {
  case QueryNodeType::SELECT_NODE:
  {
    auto &select= node.Cast<SelectNode>();
    if (select.sample)
      refuse("SAMPLE");
    for (auto &expr : select.select_list)
      check_expression(expr);
    if (select.from_table)
      check_ref(*select.from_table);
    check_expression(select.where_clause);
    for (auto &expr : select.groups.group_expressions)
      check_expression(expr);
    check_expression(select.having);
    check_expression(select.qualify);
    break;
  }
  case QueryNodeType::SET_OPERATION_NODE:
    for (auto &child : node.Cast<SetOperationNode>().children)
      check_node(*child);
    break;
  default:
    refuse("query " + EnumUtil::ToString(node.type));
  }
}

void check_statement(SQLStatement &statement)
{
  switch (statement.type)
  {
  case StatementType::SELECT_STATEMENT:
    check_node(*statement.Cast<SelectStatement>().node);
    break;
  case StatementType::TRANSACTION_STATEMENT:
  {
    /* Quack's own client wraps work on an attached database in these */
    auto &info= *statement.Cast<TransactionStatement>().info;
    if (info.type != TransactionType::BEGIN_TRANSACTION &&
        info.type != TransactionType::COMMIT &&
        info.type != TransactionType::ROLLBACK)
      refuse("transaction form");
    break;
  }
  case StatementType::EXPLAIN_STATEMENT:
  {
    auto &explain= statement.Cast<ExplainStatement>();
    if (explain.explain_type != ExplainType::EXPLAIN_STANDARD ||
        explain.stmt->type != StatementType::SELECT_STATEMENT)
      refuse("EXPLAIN form");
    check_statement(*explain.stmt);
    break;
  }
  default:
    refuse("statement " + EnumUtil::ToString(statement.type));
  }
}

} /* namespace */

GateDecision StatementAllowed(ClientContext &context, const std::string &sql,
                              std::string &why)
{
  Walk walk{context, {}, {}};
  walk_state= &walk;
  struct Reset
  {
    ~Reset() { walk_state= nullptr; }
  } reset;
  try
  {
    Parser parser(context);
    parser.ParseQuery(sql);
    if (parser.statements.size() != 1)
    {
      why= "not exactly one statement";
      return GateDecision::StatementDenied;
    }
    check_statement(*parser.statements[0]);
    look_up_tables();
    return GateDecision::Allowed;
  }
  catch (const Refused &refused)
  {
    why= refused.why;
    return refused.decision;
  }
  catch (const std::exception &)
  {
    why= "does not parse";
  }
  return GateDecision::StatementDenied;
}

} /* namespace quack_guard */
