#pragma once

#include "duckdb/common/unique_ptr.hpp"

#include <string>

namespace duckdb
{
class Connection;
class SQLStatement;
struct DBConfig;
}

namespace myduck
{

void register_mariadb_dialect(duckdb::DBConfig &config);

duckdb::unique_ptr<duckdb::SQLStatement>
parse_mariadb_statement(duckdb::Connection &connection, const std::string &query);

}
