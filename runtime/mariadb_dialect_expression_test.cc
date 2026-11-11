#include "mariadb_dialect.h"
#include "mariadb_dialect_test_helpers.h"

#include "duckdb.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/parameter_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace
{

void require_expression(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}

duckdb::SelectNode &select_node(duckdb::SQLStatement &statement)
{
  return statement.Cast<duckdb::SelectStatement>()
      .node->Cast<duckdb::SelectNode>();
}

void require_expression_ast_parity(duckdb::Connection &connection,
                                   const char *source, const char *normalized)
{
  auto candidate= myduck::parse_mariadb_statement(connection, source);
  auto baseline= connection.ExtractStatements(normalized);
  require_expression(baseline.size() == 1, "Expected one base expression statement");
  require_expression(select_node(*candidate).Equals(&select_node(*baseline[0])),
                     "Expression grammar changed the expected AST");
  require_expression(candidate->named_param_map == baseline[0]->named_param_map,
                     "Expression grammar changed the parameter map");
  require_expression(candidate->has_anonymous_parameters ==
                         baseline[0]->has_anonymous_parameters,
                     "Expression grammar changed anonymous parameter tracking");
}

void test_base_expression_forms(duckdb::Connection &connection)
{
  auto date_statement= connection.ExtractStatements("SELECT CURRENT_DATE() AS d");
  require_expression(select_node(*date_statement[0]).select_list[0]
                         ->GetExpressionClass() == duckdb::ExpressionClass::FUNCTION,
                     "Base CURRENT_DATE() no longer produces a function AST");
  require_expression(!connection.Query("SELECT CURRENT_DATE() AS d")->HasError(),
                     "Base CURRENT_DATE() should already execute successfully");

  for (const char *sql : {
           "SELECT CAST(42 AS CHAR(8)) AS s",
           "SELECT 'abc' ~ 'b' AS matched, 'abc' !~ 'z' AS rejected",
           "SELECT NOT 'abc' ~ 'b' AND TRUE AS matched"})
    require_mariadb_query_parity(connection, sql, sql);

  for (const char *sql : {"SELECT CURRENT_TIME() AS t",
                          "SELECT CURRENT_TIMESTAMP() AS t"})
  {
    auto baseline= connection.ExtractStatements(sql);
    require_expression(select_node(*baseline[0]).select_list[0]
                           ->GetExpressionClass() == duckdb::ExpressionClass::FUNCTION,
                       "Base current-time empty calls no longer use function ASTs");
    auto result= connection.Query(sql);
    require_expression(result->HasError(),
                       "Pinned base current-time empty calls unexpectedly bind");
  }
}

void test_convert_expressions(duckdb::Connection &connection)
{
  const char *pairs[][2]= {
      {"SELECT CONVERT('42', INTEGER) AS value",
       "SELECT CAST('42' AS INTEGER) AS value"},
      {"SELECT CONVERT(42, CHAR) AS value",
       "SELECT CAST(42 AS VARCHAR) AS value"},
      {"SELECT CONVERT(42, SIGNED) AS value",
       "SELECT CAST(42 AS SIGNED) AS value"},
      {"SELECT CONVERT(-42, signed) AS value",
       "SELECT CAST(-42 AS signed) AS value"},
      {"SELECT CONVERT(42, CHAR(2)) AS value",
       "SELECT CAST(42 AS VARCHAR(2)) AS value"},
      {"SELECT CONVERT('123.45', DECIMAL(12,2)) AS value",
       "SELECT CAST('123.45' AS DECIMAL(12,2)) AS value"},
      {"SELECT CONVERT('2024-02-29', DATE) AS value",
       "SELECT CAST('2024-02-29' AS DATE) AS value"},
      {"SELECT CONVERT('12:34:56', TIME) AS value",
       "SELECT CAST('12:34:56' AS TIME) AS value"},
      {"SELECT CONVERT('2024-02-29 12:34:56', TIMESTAMP) AS value",
       "SELECT CAST('2024-02-29 12:34:56' AS TIMESTAMP) AS value"},
      {"SELECT CONVERT(NULL, INTEGER) AS value",
       "SELECT CAST(NULL AS INTEGER) AS value"},
      {"SELECT CONVERT('b', ENUM('a', 'b')) AS value",
       "SELECT CAST('b' AS ENUM('a', 'b')) AS value"},
      {"SELECT CONVERT(COALESCE(NULL, '42'), INTEGER) + 1 AS value",
       "SELECT CAST(COALESCE(NULL, '42') AS INTEGER) + 1 AS value"},
      {"SELECT CONVERT(CONVERT(42, CHAR), INTEGER) AS value",
       "SELECT CAST(CAST(42 AS VARCHAR) AS INTEGER) AS value"},
      {"SELECT CASE WHEN CONVERT('3', INTEGER) > 2 THEN CONVERT(7, CHAR) "
       "ELSE 'no' END AS value",
       "SELECT CASE WHEN CAST('3' AS INTEGER) > 2 THEN CAST(7 AS VARCHAR) "
       "ELSE 'no' END AS value"},
      {"SELECT CONVERT((SELECT 42), CHAR) AS value",
       "SELECT CAST((SELECT 42) AS VARCHAR) AS value"},
      {"SELECT cOnVeRt /* function */ (concat('1', ',', '2'), /* type */ CHAR(8)) AS value",
       "SELECT CAST(concat('1', ',', '2') AS VARCHAR(8)) AS value"},
      {"SELECT CONVERT(\n -- argument\n '42',\n INTEGER) AS value",
       "SELECT CAST('42' AS INTEGER) AS value"},
      {"SELECT CONVERT(`CONVERT`, CHAR) AS `RLIKE` "
       "FROM (VALUES (2), (1)) AS t(`CONVERT`) ORDER BY `RLIKE`",
       "SELECT CAST(\"CONVERT\" AS VARCHAR) AS \"RLIKE\" "
       "FROM (VALUES (2), (1)) AS t(\"CONVERT\") ORDER BY \"RLIKE\""},
      {"SELECT CONVERT(\"odd,name\", CHAR) AS value "
       "FROM (VALUES ('a''b')) AS t(\"odd,name\")",
       "SELECT CAST(\"odd,name\" AS VARCHAR) AS value "
       "FROM (VALUES ('a''b')) AS t(\"odd,name\")"}};
  for (const auto &pair : pairs)
    require_mariadb_query_parity(connection, pair[0], pair[1]);
  require_mariadb_query_error_parity(
      connection, "SELECT CONVERT(1234567890123, SIGNED) AS value",
      "SELECT CAST(1234567890123 AS SIGNED) AS value");
  require_mariadb_query_error_parity(
      connection, "SELECT CONVERT('18446744073709551615', UNSIGNED) AS value",
      "SELECT CAST('18446744073709551615' AS UNSIGNED) AS value");
}

void test_regex_expressions(duckdb::Connection &connection)
{
  const char *pairs[][2]= {
      {"SELECT 'abc' REGEXP 'b' AS value", "SELECT 'abc' ~ 'b' AS value"},
      {"SELECT 'abc' RLIKE 'b' AS value", "SELECT 'abc' ~ 'b' AS value"},
      {"SELECT 'abc' NOT REGEXP 'b' AS value", "SELECT 'abc' !~ 'b' AS value"},
      {"SELECT 'abc' NOT RLIKE 'z' AS value", "SELECT 'abc' !~ 'z' AS value"},
      {"SELECT NOT 'abc' REGEXP 'b' AND TRUE AS value",
       "SELECT NOT 'abc' ~ 'b' AND TRUE AS value"},
      {"SELECT NOT 'abc' NOT RLIKE 'b' AND FALSE OR TRUE AS value",
       "SELECT NOT 'abc' !~ 'b' AND FALSE OR TRUE AS value"},
      {"SELECT FALSE OR 'abc' NOT REGEXP 'z' AND 'def' RLIKE 'e' AS value",
       "SELECT FALSE OR 'abc' !~ 'z' AND 'def' ~ 'e' AS value"},
      {"SELECT NOT ('abc' REGEXP 'b' AND 'def' NOT RLIKE 'z') AS value",
       "SELECT NOT ('abc' ~ 'b' AND 'def' !~ 'z') AS value"},
      {"SELECT ('abc' REGEXP 'b') = ('def' RLIKE 'e') AS value",
       "SELECT ('abc' ~ 'b') = ('def' ~ 'e') AS value"},
      {"SELECT 'abc' REGEXP 'b' IS TRUE AS value",
       "SELECT 'abc' ~ 'b' IS TRUE AS value"},
      {"SELECT NULL REGEXP 'b' AS matched, 'abc' NOT RLIKE NULL AS rejected",
       "SELECT NULL ~ 'b' AS matched, 'abc' !~ NULL AS rejected"},
      {"SELECT ('a' || 'bc') ReGeXp ('b' || 'c') AS value",
       "SELECT ('a' || 'bc') ~ ('b' || 'c') AS value"},
      {"SELECT CONVERT(123, CHAR) REGEXP '2' AS value",
       "SELECT CAST(123 AS VARCHAR) ~ '2' AS value"},
      {"SELECT CONVERT('abc' NOT REGEXP 'b', INTEGER) AS value",
       "SELECT CAST('abc' !~ 'b' AS INTEGER) AS value"},
      {"SELECT CASE WHEN 'abc' RLIKE 'b' THEN 'yes' ELSE 'no' END AS value",
       "SELECT CASE WHEN 'abc' ~ 'b' THEN 'yes' ELSE 'no' END AS value"},
      {"SELECT 'abc' /* operand */ NOT /* operator */ REGEXP /* pattern */ 'z' AS value",
       "SELECT 'abc' !~ 'z' AS value"},
      {"SELECT `REGEXP` AS value FROM (VALUES ('abc'), ('def')) AS t(`REGEXP`) "
       "WHERE `REGEXP` RLIKE 'b' ORDER BY value",
       "SELECT \"REGEXP\" AS value FROM (VALUES ('abc'), ('def')) AS t(\"REGEXP\") "
       "WHERE \"REGEXP\" ~ 'b' ORDER BY value"},
      {"SELECT 'REGEXP RLIKE CONVERT(1, CHAR)' AS value",
       "SELECT 'REGEXP RLIKE CONVERT(1, CHAR)' AS value"}};
  for (const auto &pair : pairs)
    require_mariadb_query_parity(connection, pair[0], pair[1]);
}

void test_current_time_expressions(duckdb::Connection &connection)
{
  require_mariadb_query_parity(connection,
      "SELECT CURRENT_DATE(), CuRrEnT_Time(3), current_timestamp()",
      "SELECT current_date, current_time, current_timestamp");
  require_mariadb_query_parity(
      connection,
      "SELECT CURRENT_DATE() AS d, CURRENT_TIME() AS t, CURRENT_TIMESTAMP() AS ts",
      "SELECT CURRENT_DATE AS d, CURRENT_TIME AS t, CURRENT_TIMESTAMP AS ts");
  require_mariadb_query_parity(
      connection,
      "SELECT CURRENT_TIME /* empty */ ( ) AS t, current_timestamp( /* empty */ ) AS ts",
      "SELECT CURRENT_TIME AS t, CURRENT_TIMESTAMP AS ts");
  require_mariadb_query_parity(
      connection,
      "SELECT CONVERT(CURRENT_TIMESTAMP(), DATE) AS d, "
      "CURRENT_TIME() IS NOT NULL AND CURRENT_DATE() IS NOT NULL AS present",
      "SELECT CAST(CURRENT_TIMESTAMP AS DATE) AS d, "
      "CURRENT_TIME IS NOT NULL AND CURRENT_DATE IS NOT NULL AS present");
  for (const char *precision : {"0", "1", "2", "3", "4", "5", "6"})
  {
    std::string source= "SELECT CURRENT_TIME(";
    source+= precision;
    source+= ") AS t, CURRENT_TIMESTAMP(";
    source+= precision;
    source+= ") AS ts";
    require_mariadb_query_parity(
        connection, source.c_str(), "SELECT CURRENT_TIME AS t, CURRENT_TIMESTAMP AS ts");
  }
  require_mariadb_query_parity(
      connection,
      "SELECT CURRENT_DATE /* empty */ ( ) AS d, "
      "CURRENT_TIME( /* precision */ 3) AS t, CURRENT_TIMESTAMP(6 /* precision */) AS ts",
      "SELECT CURRENT_DATE AS d, CURRENT_TIME AS t, CURRENT_TIMESTAMP AS ts");
  require_mariadb_query_parity(
      connection,
      "SELECT CURRENT_DATE() AS d, CURRENT_TIME(3) AS t, CURRENT_TIMESTAMP(6) AS ts "
      "FROM (VALUES (11, 22, 33)) AS t(\"current_date\", \"current_time\", \"current_timestamp\")",
      "SELECT CURRENT_DATE AS d, CURRENT_TIME AS t, CURRENT_TIMESTAMP AS ts "
      "FROM (VALUES (11, 22, 33)) AS t(\"current_date\", \"current_time\", \"current_timestamp\")");
  auto base_shadow= connection.Query(
      "SELECT CURRENT_DATE() AS d FROM (VALUES (11)) AS t(\"current_date\")");
  require_expression(!base_shadow->HasError() &&
                         base_shadow->GetTypes()[0] == duckdb::LogicalType::DATE,
                     "Native CURRENT_DATE() should not bind the shadow column");
}

void test_expression_parameter_order(duckdb::Connection &connection)
{
  const char *source=
      "SELECT CONVERT(? + ?, DECIMAL(12,2)) AS amount, CURRENT_TIME(3) AS t, "
      "? REGEXP ? AS matched, CURRENT_TIMESTAMP(6) AS ts, "
      "CONVERT(CONVERT(?, CHAR), CHAR) NOT RLIKE ? AS rejected, ? AS last";
  const char *normalized=
      "SELECT CAST(? + ? AS DECIMAL(12,2)) AS amount, CURRENT_TIME AS t, "
      "? ~ ? AS matched, CURRENT_TIMESTAMP AS ts, "
      "CAST(CAST(? AS VARCHAR) AS VARCHAR) !~ ? AS rejected, ? AS last";
  require_expression_ast_parity(connection, source, normalized);
  auto statement= myduck::parse_mariadb_statement(connection, source);
  std::vector<std::string> parameters;
  for (const auto &expression : select_node(*statement).select_list)
    duckdb::ParsedExpressionIterator::VisitExpression<duckdb::ParameterExpression>(
        *expression, [&](const duckdb::ParameterExpression &parameter) {
          parameters.push_back(parameter.Identifier().GetIdentifierName());
        });
  require_expression(parameters == std::vector<std::string>{"1", "2", "3", "4", "5", "6", "7"},
                     "Expression transforms reordered anonymous parameters");
  require_expression_ast_parity(
      connection, "SELECT CONVERT($2, SIGNED) AS n, $1 NOT REGEXP $3 AS r",
      "SELECT CAST($2 AS SIGNED) AS n, $1 !~ $3 AS r");
  require_expression_ast_parity(
      connection, "SELECT CONVERT($value, CHAR) RLIKE $pattern AS r, $value AS v",
      "SELECT CAST($value AS VARCHAR) ~ $pattern AS r, $value AS v");
}

void test_expression_boundaries(duckdb::Connection &connection)
{
  for (const char *sql : {
           "SELECT MYCONVERT(?, CHAR) AS value",
           "SELECT CONVERTED(1, 2) AS value",
           "SELECT `CONVERT`(1, 2) AS value",
           "SELECT main.CONVERT(1, 2) AS value",
           "SELECT REGEXP_REPLACE('abc', 'b', 'x') AS value",
           "SELECT MYCURRENT_TIME(3) AS value",
           "SELECT `CURRENT_TIME`(3) AS value",
           "SELECT main.CURRENT_TIMESTAMP(3) AS value",
           "SELECT CURRENT_TIME(?) AS value",
           "SELECT CURRENT_TIMESTAMP(1 + 2) AS value"})
  {
    require_expression_ast_parity(connection, sql, sql);
    auto statement= myduck::parse_mariadb_statement(connection, sql);
    require_expression(select_node(*statement).select_list[0]
                           ->GetExpressionClass() == duckdb::ExpressionClass::FUNCTION,
                       "Expression extension captured an ordinary function call");
  }
  auto statement= myduck::parse_mariadb_statement(
      connection, "SELECT CONVERT(42, CHAR) AS value");
  auto &cast= select_node(*statement).select_list[0]->Cast<duckdb::CastExpression>();
  require_expression(cast.GetQueryLocation().IsValid() &&
                         cast.GetQueryLocation().Start() == 7 &&
                         cast.Child().GetQueryLocation().IsValid() &&
                         cast.Child().GetQueryLocation().Start() == 15,
                     "CONVERT lost expression source locations");
}

void test_invalid_expressions(duckdb::Connection &connection)
{
  for (const char *sql : {
           "SELECT CONVERT(, INTEGER)",
           "SELECT CONVERT()",
           "SELECT CONVERT(1)",
           "SELECT CONVERT(1, 2)",
           "SELECT CONVERT(1, INTEGER, 2)",
           "SELECT CONVERT('42',)",
           "SELECT CONVERT('42', DECIMAL(12,2)",
           "SELECT CONVERT('42' INTEGER)",
           "SELECT CONVERT(1, DECIMAL(nonconstant, 2))",
           "SELECT 'abc' REGEXP",
           "SELECT 'abc' NOT RLIKE",
           "SELECT 'abc' NOT NOT REGEXP 'a'",
           "SELECT 'abc' REGEXP 'a' ESCAPE '!'",
           "SELECT 'abc' NOT RLIKE 'a' ESCAPE '!'",
           "SELECT ('abc' RLIKE 'a' AND)",
           "SELECT CURRENT_TIME(,)",
           "SELECT CURRENT_TIME(7)",
           "SELECT CURRENT_TIMESTAMP(7)",
           "SELECT CURRENT_TIME(1.5)",
           "SELECT CURRENT_TIMESTAMP(3e0)",
           "SELECT CURRENT_DATE(0)",
           "SELECT CURRENT_TIMESTAMP("})
    require_mariadb_parse_error(connection, sql);

  std::string nested= "1";
  for (size_t i= 0; i < 1100; i++)
    nested= "CONVERT(" + nested + ", INTEGER)";
  nested= "SELECT " + nested + " AS value";
  require_mariadb_parse_error(connection, nested.c_str());
}

}

void test_mariadb_expression_grammar(duckdb::Connection &connection)
{
  test_base_expression_forms(connection);
  test_convert_expressions(connection);
  test_regex_expressions(connection);
  test_current_time_expressions(connection);
  test_expression_parameter_order(connection);
  test_expression_boundaries(connection);
  test_invalid_expressions(connection);
}
