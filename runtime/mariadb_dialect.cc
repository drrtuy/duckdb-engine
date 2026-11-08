#include "mariadb_dialect.h"

#include "duckdb/main/client_config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/peg/dialect_extension.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace myduck
{
namespace
{

class MariadbDialectScope
{
public:
  explicit MariadbDialectScope(duckdb::Connection &connection)
      : context(*connection.context)
  {
    auto &config= duckdb::ClientConfig::GetConfig(context);
    if (config.current_dialect || config.connected_grammar)
      throw duckdb::InvalidInputException(
          "MariaDB dialect parsing requires the base DuckDB dialect");
    duckdb::CurrentDialectSetting::SetLocal(context, duckdb::Value("mariadb"));
  }

  ~MariadbDialectScope()
  {
    duckdb::CurrentDialectSetting::ResetLocal(context);
  }

  MariadbDialectScope(const MariadbDialectScope &)= delete;
  MariadbDialectScope &operator=(const MariadbDialectScope &)= delete;

private:
  duckdb::ClientContext &context;
};

}

void register_mariadb_dialect(duckdb::DBConfig &config)
{
  duckdb::DialectExtension::Register(
      config, duckdb::make_uniq<duckdb::DialectExtension>("mariadb"));
}

duckdb::unique_ptr<duckdb::SQLStatement>
parse_mariadb_statement(duckdb::Connection &connection, const std::string &query)
{
  MariadbDialectScope scope(connection);
  auto statements= connection.ExtractStatements(query);
  if (statements.size() != 1 ||
      statements[0]->type != duckdb::StatementType::SELECT_STATEMENT)
    throw duckdb::InvalidInputException(
        "MariaDB dialect parsing requires exactly one SELECT statement");
  return std::move(statements[0]);
}

}
