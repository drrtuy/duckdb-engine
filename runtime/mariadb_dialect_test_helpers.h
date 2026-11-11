#pragma once

namespace duckdb
{
class Connection;
}

void require_mariadb_query_parity(duckdb::Connection &connection,
                                  const char *source, const char *normalized);
void require_mariadb_parse_error(duckdb::Connection &connection, const char *source);
void require_mariadb_query_error_parity(duckdb::Connection &connection,
                                        const char *source, const char *normalized);

void test_mariadb_expression_grammar(duckdb::Connection &connection);
void test_mariadb_select_grammar(duckdb::Connection &connection);
