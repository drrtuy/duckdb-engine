#pragma once

#include "duckdb/common/unique_ptr.hpp"

#include <string>
#include <string_view>

namespace duckdb
{
class Connection;
class SQLStatement;
struct DBConfig;
}

namespace myduck
{

bool mariadb_query_has_lexical_mismatch(std::string_view query,
                                      bool backslash_escapes, bool ansi_quotes);

void register_mariadb_dialect(duckdb::DBConfig &config);

duckdb::unique_ptr<duckdb::SQLStatement>
parse_mariadb_statement(duckdb::Connection &connection, const std::string &query);

}
