#include "mariadb_dialect.h"
#include "mariadb_dialect_test_helpers.h"

#include "duckdb.hpp"
#include "duckdb/parser/expression/parameter_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

#include <stdexcept>
#include <string>

namespace
{

void require_select(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}

void select_setup_query(duckdb::Connection &connection, const char *sql)
{
  auto result= connection.Query(sql);
  if (result->HasError())
    throw std::runtime_error(result->GetError());
}

duckdb::LimitModifier &select_limit(duckdb::QueryNode &node)
{
  for (auto &modifier : node.modifiers)
  {
    if (modifier->type == duckdb::ResultModifierType::LIMIT_MODIFIER)
      return modifier->Cast<duckdb::LimitModifier>();
  }
  throw std::runtime_error("Comma LIMIT did not produce a LimitModifier");
}

void test_select_limit_parameters(duckdb::Connection &connection)
{
  const struct
  {
    const char *source;
    const char *normalized;
    duckdb::idx_t parameter_count;
  } cases[]= {
    {"SELECT ? AS probe FROM range(8) t(i) WHERE i >= ? ORDER BY i LIMIT ?, ?",
     "SELECT $1 AS probe FROM range(8) t(i) WHERE i >= $2 ORDER BY i LIMIT $4 OFFSET $3", 4},
    {"SELECT ? AS probe FROM (SELECT ? AS k FROM range(8) t(i) ORDER BY i LIMIT ?, ?) s "
     "ORDER BY k LIMIT ?, ?",
     "SELECT $1 AS probe FROM (SELECT $2 AS k FROM range(8) t(i) ORDER BY i LIMIT $4 OFFSET $3) s "
     "ORDER BY k LIMIT $6 OFFSET $5", 6},
    {"WITH q AS (SELECT ? AS k FROM range(8) t(i) ORDER BY i LIMIT ?, ?) "
     "SELECT ? AS probe FROM q ORDER BY k LIMIT ?, ?",
     "WITH q AS (SELECT $1 AS k FROM range(8) t(i) ORDER BY i LIMIT $3 OFFSET $2) "
     "SELECT $4 AS probe FROM q ORDER BY k LIMIT $6 OFFSET $5", 6}
  };
  for (const auto &entry : cases)
  {
    auto source= myduck::parse_mariadb_statement(connection, entry.source);
    auto normalized= connection.ExtractStatements(entry.normalized);
    require_select(normalized.size() == 1, "Expected one parameterized base statement");
    auto &node= *source->Cast<duckdb::SelectStatement>().node;
    auto &base_node= normalized[0]->Cast<duckdb::SelectStatement>().node;
    require_select(node.Equals(base_node.get()), "Comma LIMIT changed source parameter order in the AST");
    require_select(source->has_anonymous_parameters, "Anonymous parameter flag was lost");
    require_select(source->named_param_map.size() == entry.parameter_count,
                   "Comma LIMIT parameter map has an unexpected size");
    for (duckdb::idx_t i= 1; i <= entry.parameter_count; i++)
    {
      auto found= source->named_param_map.find(duckdb::Identifier(std::to_string(i)));
      require_select(found != source->named_param_map.end() && found->second == i,
                     "Comma LIMIT parameter map lost a source-order entry");
    }
    auto &limit= select_limit(node);
    require_select(limit.offset && limit.limit, "Comma LIMIT lost an operand");
    require_select(limit.offset->Cast<duckdb::ParameterExpression>().Identifier().GetIdentifierName() ==
                       std::to_string(entry.parameter_count - 1),
                   "Comma LIMIT offset is not the first source operand");
    require_select(limit.limit->Cast<duckdb::ParameterExpression>().Identifier().GetIdentifierName() ==
                       std::to_string(entry.parameter_count),
                   "Comma LIMIT count is not the second source operand");
  }
}

}

void test_mariadb_select_grammar(duckdb::Connection &connection)
{
  select_setup_query(connection,
      "CREATE TEMP TABLE mariadb_phase2_select_a AS "
      "SELECT * FROM (VALUES (1,10), (2,10), (3,20), (4,NULL)) t(id,grp)");
  select_setup_query(connection,
      "CREATE TEMP TABLE mariadb_phase2_select_b AS "
      "SELECT * FROM (VALUES (1,100), (2,200), (5,500)) t(id,val)");
  select_setup_query(connection,
      "CREATE TEMP TABLE mariadb_phase2_select_c AS "
      "SELECT * FROM (VALUES (1,7), (3,9)) t(id,extra)");

  const struct
  {
    const char *source;
    const char *normalized;
  } cases[]= {
    {"SELECT id FROM mariadb_phase2_select_a ORDER BY id LIMIT 1,2",
     "SELECT id FROM mariadb_phase2_select_a ORDER BY id LIMIT 2 OFFSET 1"},
    {"SELECT id FROM mariadb_phase2_select_a ORDER BY id LIMIT 0,0",
     "SELECT id FROM mariadb_phase2_select_a ORDER BY id LIMIT 0 OFFSET 0"},
    {"SELECT id FROM mariadb_phase2_select_a ORDER BY id LIMIT (1+1),1",
     "SELECT id FROM mariadb_phase2_select_a ORDER BY id LIMIT 1 OFFSET (1+1)"},
    {"WITH q AS (SELECT id FROM mariadb_phase2_select_a ORDER BY id LIMIT 1,3) "
     "SELECT id FROM (SELECT id FROM q ORDER BY id LIMIT 0,2) s ORDER BY id LIMIT 1,1",
     "WITH q AS (SELECT id FROM mariadb_phase2_select_a ORDER BY id LIMIT 3 OFFSET 1) "
     "SELECT id FROM (SELECT id FROM q ORDER BY id LIMIT 2 OFFSET 0) s ORDER BY id LIMIT 1 OFFSET 1"},
    {"(SELECT id FROM mariadb_phase2_select_a ORDER BY id LIMIT 0,1) UNION ALL "
     "(SELECT id FROM mariadb_phase2_select_a ORDER BY id DESC LIMIT 0,1) ORDER BY id LIMIT 0,2",
     "(SELECT id FROM mariadb_phase2_select_a ORDER BY id LIMIT 1 OFFSET 0) UNION ALL "
     "(SELECT id FROM mariadb_phase2_select_a ORDER BY id DESC LIMIT 1 OFFSET 0) ORDER BY id LIMIT 2 OFFSET 0"},
    {"SELECT grp, count(*) AS n FROM mariadb_phase2_select_a GROUP BY grp WITH ROLLUP ORDER BY grp,n",
     "SELECT grp, count(*) AS n FROM mariadb_phase2_select_a GROUP BY ROLLUP(grp) ORDER BY grp,n"},
    {"SELECT grp+1 AS g, id%2 AS k, count(*) AS n FROM mariadb_phase2_select_a "
     "GROUP BY grp+1,id%2 WITH ROLLUP HAVING count(*)>0 ORDER BY g,k,n LIMIT 1,5",
     "SELECT grp+1 AS g, id%2 AS k, count(*) AS n FROM mariadb_phase2_select_a "
     "GROUP BY ROLLUP(grp+1,id%2) HAVING count(*)>0 ORDER BY g,k,n LIMIT 5 OFFSET 1"},
    {"SELECT grp, count(*) AS n FROM mariadb_phase2_select_a GROUP BY grp,grp WITH ROLLUP ORDER BY grp,n",
     "SELECT grp, count(*) AS n FROM mariadb_phase2_select_a GROUP BY ROLLUP(grp,grp) ORDER BY grp,n"},
    {"SELECT (id,grp) AS k, count(*) AS n FROM mariadb_phase2_select_a "
     "GROUP BY (id,grp) WITH ROLLUP ORDER BY k,n",
     "SELECT (id,grp) AS k, count(*) AS n FROM mariadb_phase2_select_a "
     "GROUP BY ROLLUP((id,grp)) ORDER BY k,n"},
    {"WITH q AS (SELECT grp,count(*) AS n FROM mariadb_phase2_select_a GROUP BY grp WITH ROLLUP) "
     "SELECT grp,sum(n) AS total FROM (SELECT grp,n FROM q ORDER BY grp,n LIMIT 0,8) s "
     "GROUP BY grp WITH ROLLUP ORDER BY grp,total",
     "WITH q AS (SELECT grp,count(*) AS n FROM mariadb_phase2_select_a GROUP BY ROLLUP(grp)) "
     "SELECT grp,sum(n) AS total FROM (SELECT grp,n FROM q ORDER BY grp,n LIMIT 8 OFFSET 0) s "
     "GROUP BY ROLLUP(grp) ORDER BY grp,total"},
    {"SELECT a.id AS a_id,b.id AS b_id FROM mariadb_phase2_select_a a JOIN mariadb_phase2_select_b b "
     "ORDER BY a_id,b_id LIMIT 2,4",
     "SELECT a.id AS a_id,b.id AS b_id FROM mariadb_phase2_select_a a CROSS JOIN mariadb_phase2_select_b b "
     "ORDER BY a_id,b_id LIMIT 4 OFFSET 2"},
    {"SELECT a.id AS a_id,b.id AS b_id FROM mariadb_phase2_select_a a STRAIGHT_JOIN "
     "mariadb_phase2_select_b b ORDER BY a_id,b_id",
     "SELECT a.id AS a_id,b.id AS b_id FROM mariadb_phase2_select_a a CROSS JOIN "
     "mariadb_phase2_select_b b ORDER BY a_id,b_id"},
    {"SELECT a.id AS a_id,b.val FROM mariadb_phase2_select_a a STRAIGHT_JOIN "
     "mariadb_phase2_select_b b ON a.id=b.id ORDER BY a_id",
     "SELECT a.id AS a_id,b.val FROM mariadb_phase2_select_a a JOIN "
     "mariadb_phase2_select_b b ON a.id=b.id ORDER BY a_id"},
    {"SELECT id,val FROM mariadb_phase2_select_a STRAIGHT_JOIN mariadb_phase2_select_b USING(id) ORDER BY id",
     "SELECT id,val FROM mariadb_phase2_select_a JOIN mariadb_phase2_select_b USING(id) ORDER BY id"},
    {"SELECT STRAIGHT_JOIN a.id AS a_id,b.id AS b_id FROM mariadb_phase2_select_a a "
     "STRAIGHT_JOIN mariadb_phase2_select_b b ON a.id=b.id ORDER BY a_id",
     "SELECT a.id AS a_id,b.id AS b_id FROM mariadb_phase2_select_a a "
     "JOIN mariadb_phase2_select_b b ON a.id=b.id ORDER BY a_id"},
    {"SELECT a.id AS a_id,b.id AS b_id,c.id AS c_id FROM mariadb_phase2_select_a a "
     "JOIN mariadb_phase2_select_b b LEFT JOIN mariadb_phase2_select_c c ON b.id=c.id ORDER BY a_id,b_id,c_id",
     "SELECT a.id AS a_id,b.id AS b_id,c.id AS c_id FROM mariadb_phase2_select_a a "
     "CROSS JOIN mariadb_phase2_select_b b LEFT JOIN mariadb_phase2_select_c c ON b.id=c.id ORDER BY a_id,b_id,c_id"},
    {"SELECT a.id AS a_id,b.id AS b_id,c.id AS c_id FROM mariadb_phase2_select_a a "
     "LEFT JOIN mariadb_phase2_select_b b ON a.id=b.id JOIN mariadb_phase2_select_c c ORDER BY a_id,b_id,c_id",
     "SELECT a.id AS a_id,b.id AS b_id,c.id AS c_id FROM mariadb_phase2_select_a a "
     "LEFT JOIN mariadb_phase2_select_b b ON a.id=b.id CROSS JOIN mariadb_phase2_select_c c ORDER BY a_id,b_id,c_id"},
    {"SELECT a.id AS a_id,b.id AS b_id,c.id AS c_id FROM mariadb_phase2_select_a a, "
     "mariadb_phase2_select_b b JOIN mariadb_phase2_select_c c ORDER BY a_id,b_id,c_id",
     "SELECT a.id AS a_id,b.id AS b_id,c.id AS c_id FROM mariadb_phase2_select_a a, "
     "mariadb_phase2_select_b b CROSS JOIN mariadb_phase2_select_c c ORDER BY a_id,b_id,c_id"},
    {"SELECT a.id AS a_id,b.id AS b_id,c.id AS c_id FROM mariadb_phase2_select_a a "
     "STRAIGHT_JOIN (mariadb_phase2_select_b b JOIN mariadb_phase2_select_c c USING(id)) "
     "ON a.id=b.id ORDER BY a_id,b_id,c_id",
     "SELECT a.id AS a_id,b.id AS b_id,c.id AS c_id FROM mariadb_phase2_select_a a "
     "JOIN (mariadb_phase2_select_b b JOIN mariadb_phase2_select_c c USING(id)) "
     "ON a.id=b.id ORDER BY a_id,b_id,c_id"},
    {"SELECT a.id AS a_id,b.id AS b_id,c.id AS c_id FROM mariadb_phase2_select_a a "
     "JOIN (mariadb_phase2_select_b b NATURAL LEFT JOIN mariadb_phase2_select_c c) ORDER BY a_id,b_id,c_id",
     "SELECT a.id AS a_id,b.id AS b_id,c.id AS c_id FROM mariadb_phase2_select_a a "
     "CROSS JOIN (mariadb_phase2_select_b b NATURAL LEFT JOIN mariadb_phase2_select_c c) ORDER BY a_id,b_id,c_id"},
    {"SELECT a.id AS a_id,b.id AS b_id,c.id AS c_id FROM mariadb_phase2_select_a a "
     "STRAIGHT_JOIN mariadb_phase2_select_b b JOIN mariadb_phase2_select_c c "
     "ON b.id=c.id ON a.id=b.id ORDER BY a_id,b_id,c_id",
     "SELECT a.id AS a_id,b.id AS b_id,c.id AS c_id FROM mariadb_phase2_select_a a "
     "JOIN mariadb_phase2_select_b b JOIN mariadb_phase2_select_c c "
     "ON b.id=c.id ON a.id=b.id ORDER BY a_id,b_id,c_id"},
    {"SELECT id FROM mariadb_phase2_select_a FORCE INDEX (idx_id) ORDER BY id",
     "SELECT id FROM mariadb_phase2_select_a ORDER BY id"},
    {"SELECT \"FORCE\".id FROM mariadb_phase2_select_a \"FORCE\" USE KEY (\"SQL_CACHE\") ORDER BY id",
     "SELECT \"FORCE\".id FROM mariadb_phase2_select_a \"FORCE\" ORDER BY id"},
    {"SELECT a.id FROM mariadb_phase2_select_a AS a FORCE KEY FOR JOIN (PRIMARY,idx_grp) "
     "USE INDEX FOR ORDER BY (idx_id) IGNORE KEY FOR GROUP BY (idx_grp) ORDER BY a.id",
     "SELECT a.id FROM mariadb_phase2_select_a AS a ORDER BY a.id"},
    {"SELECT a.id FROM mariadb_phase2_select_a a USE KEY () ORDER BY a.id",
     "SELECT a.id FROM mariadb_phase2_select_a a ORDER BY a.id"},
    {"SELECT id FROM mariadb_phase2_select_a USE INDEX () IGNORE INDEX (\"FORCE\",\"KEY\") ORDER BY id",
     "SELECT id FROM mariadb_phase2_select_a ORDER BY id"},
    {"SELECT a.id,b.val FROM mariadb_phase2_select_a a FORCE INDEX(idx_id) "
     "STRAIGHT_JOIN mariadb_phase2_select_b b USE KEY FOR JOIN(idx_id) ON a.id=b.id ORDER BY a.id",
     "SELECT a.id,b.val FROM mariadb_phase2_select_a a "
     "JOIN mariadb_phase2_select_b b ON a.id=b.id ORDER BY a.id"},
    {"WITH q AS (SELECT id FROM mariadb_phase2_select_a USE INDEX(idx_id)) "
     "SELECT id FROM q IGNORE KEY FOR ORDER BY(idx_id) ORDER BY id LIMIT 1,2",
     "WITH q AS (SELECT id FROM mariadb_phase2_select_a) "
     "SELECT id FROM q ORDER BY id LIMIT 2 OFFSET 1"},
    {"SELECT DISTINCTROW SQL_NO_CACHE grp FROM mariadb_phase2_select_a ORDER BY grp",
     "SELECT DISTINCT grp FROM mariadb_phase2_select_a ORDER BY grp"},
    {"SELECT SQL_SMALL_RESULT DISTINCT SQL_NO_CACHE STRAIGHT_JOIN grp "
     "FROM mariadb_phase2_select_a ORDER BY grp",
     "SELECT DISTINCT grp FROM mariadb_phase2_select_a ORDER BY grp"},
    {"SELECT HIGH_PRIORITY DISTINCT ON(grp) SQL_CACHE grp,id "
     "FROM mariadb_phase2_select_a ORDER BY grp,id",
     "SELECT DISTINCT ON(grp) grp,id FROM mariadb_phase2_select_a ORDER BY grp,id"},
    {"SELECT 1 AS \"SQL_NO_CACHE\", 2 AS \"STRAIGHT_JOIN\", 'FORCE INDEX(x)' AS literal",
     "SELECT 1 AS \"SQL_NO_CACHE\", 2 AS \"STRAIGHT_JOIN\", 'FORCE INDEX(x)' AS literal"},
    {"SELECT \"STRAIGHT_JOIN\".id FROM mariadb_phase2_select_a AS \"STRAIGHT_JOIN\" ORDER BY id",
     "SELECT \"STRAIGHT_JOIN\".id FROM mariadb_phase2_select_a AS \"STRAIGHT_JOIN\" ORDER BY id"},
    {"SELECT force.id FROM mariadb_phase2_select_a force ORDER BY force.id",
     "SELECT force.id FROM mariadb_phase2_select_a force ORDER BY force.id"},
    {"SELECT id FROM mariadb_phase2_select_a use ORDER BY id",
     "SELECT id FROM mariadb_phase2_select_a use ORDER BY id"},
    {"SELECT id FROM mariadb_phase2_select_a ignore ORDER BY id",
     "SELECT id FROM mariadb_phase2_select_a ignore ORDER BY id"},
    {"SELECT \"SQL_CACHE\" FROM (SELECT 17 AS \"SQL_CACHE\") s",
     "SELECT \"SQL_CACHE\" FROM (SELECT 17 AS \"SQL_CACHE\") s"},
    {"SELECT sql_no_cache_value FROM (SELECT 17 AS sql_no_cache_value) s",
     "SELECT sql_no_cache_value FROM (SELECT 17 AS sql_no_cache_value) s"}
  };
  for (const auto &entry : cases)
    require_mariadb_query_parity(connection, entry.source, entry.normalized);

  for (const char *hint : {"HIGH_PRIORITY", "SQL_NO_CACHE", "SQL_CACHE", "SQL_BUFFER_RESULT",
                           "SQL_SMALL_RESULT", "SQL_BIG_RESULT", "SQL_CALC_FOUND_ROWS", "STRAIGHT_JOIN"})
  {
    for (const char *prefix : {"SELECT ", "SELECT DISTINCT ", "SELECT ALL "})
    {
      std::string source= std::string(prefix) + hint +
                          " id FROM mariadb_phase2_select_a ORDER BY id";
      std::string normalized= std::string(prefix) +
                              "id FROM mariadb_phase2_select_a ORDER BY id";
      require_mariadb_query_parity(connection, source.c_str(), normalized.c_str());
    }
  }

  for (const char *sql : {
      "SELECT id FROM mariadb_phase2_select_a ORDER BY id LIMIT 2 OFFSET 1",
      "SELECT id FROM mariadb_phase2_select_a ORDER BY id OFFSET 1 LIMIT 2",
      "SELECT id FROM mariadb_phase2_select_a ORDER BY id OFFSET 1 ROWS FETCH NEXT 2 ROWS ONLY",
      "SELECT grp,count(*) AS n FROM mariadb_phase2_select_a GROUP BY ROLLUP(grp) ORDER BY grp,n",
      "SELECT grp,count(*) AS n FROM mariadb_phase2_select_a GROUP BY CUBE(grp) ORDER BY grp,n",
      "SELECT grp,count(*) AS n FROM mariadb_phase2_select_a GROUP BY GROUPING SETS((grp),()) ORDER BY grp,n",
      "SELECT * FROM mariadb_phase2_select_a FORCE (idx_id)",
      "SELECT a.id,b.val FROM mariadb_phase2_select_a a JOIN mariadb_phase2_select_b b ON a.id=b.id ORDER BY a.id",
      "SELECT id,val FROM mariadb_phase2_select_a INNER JOIN mariadb_phase2_select_b USING(id) ORDER BY id",
      "SELECT id,val FROM mariadb_phase2_select_a NATURAL JOIN mariadb_phase2_select_b ORDER BY id",
      "SELECT id,val FROM mariadb_phase2_select_a NATURAL LEFT JOIN mariadb_phase2_select_b ORDER BY id",
      "SELECT a.id,b.val FROM mariadb_phase2_select_a a LEFT OUTER JOIN mariadb_phase2_select_b b "
      "ON a.id=b.id ORDER BY a.id",
      "SELECT a.id,b.val FROM mariadb_phase2_select_a a RIGHT JOIN mariadb_phase2_select_b b "
      "ON a.id=b.id ORDER BY a.id,b.val",
      "SELECT a.id,b.val FROM mariadb_phase2_select_a a FULL OUTER JOIN mariadb_phase2_select_b b "
      "ON a.id=b.id ORDER BY a.id,b.val"})
    require_mariadb_query_parity(connection, sql, sql);

  for (const char *sql : {
      "SELECT 1 LIMIT 1,", "SELECT 1 LIMIT ,1", "SELECT 1 LIMIT 1,2,3",
      "SELECT 1 LIMIT 1,2 OFFSET 3", "SELECT 1 LIMIT ALL,2", "SELECT 1 LIMIT 1,2 PERCENT",
      "SELECT 1 GROUP BY WITH ROLLUP", "SELECT 1 GROUP BY 1 WITH ROLLUP extra",
      "SELECT 1 GROUP BY 1 WITH ROLLUPS", "SELECT SQL_NO_CACHE", "SELECT DISTINCT SQL_CACHE",
      "SELECT SQL_NO_CACHE DISTINCT ALL id FROM mariadb_phase2_select_a",
      "SELECT * FROM mariadb_phase2_select_a STRAIGHT_JOIN",
      "SELECT * FROM mariadb_phase2_select_a LEFT JOIN mariadb_phase2_select_b",
      "SELECT * FROM mariadb_phase2_select_a NATURAL STRAIGHT_JOIN mariadb_phase2_select_b",
      "SELECT * FROM mariadb_phase2_select_a CROSS JOIN mariadb_phase2_select_b ON TRUE",
      "SELECT * FROM mariadb_phase2_select_a FORCE INDEX ()",
      "SELECT * FROM mariadb_phase2_select_a IGNORE KEY ()",
      "SELECT * FROM mariadb_phase2_select_a FORCE INDEX (idx_id,)",
      "SELECT * FROM mariadb_phase2_select_a FORCE INDEX (idx_id",
      "SELECT * FROM mariadb_phase2_select_a FORCE INDEX FOR WHERE (idx_id)",
      "SELECT * FROM mariadb_phase2_select_a FORCE INDEX FOR ORDER (idx_id)",
      "SELECT * FROM mariadb_phase2_select_a USE INDEX (idx_id) AS a"})
    require_mariadb_parse_error(connection, sql);

  test_select_limit_parameters(connection);
  select_setup_query(connection, "DROP TABLE mariadb_phase2_select_c");
  select_setup_query(connection, "DROP TABLE mariadb_phase2_select_b");
  select_setup_query(connection, "DROP TABLE mariadb_phase2_select_a");
}
