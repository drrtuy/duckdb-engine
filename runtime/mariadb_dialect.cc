#include "mariadb_dialect.h"

#include "duckdb/main/client_config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/peg/dialect_extension.hpp"
#include "duckdb/parser/peg/matcher_factory.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace myduck
{

void add_mariadb_expression_grammar(duckdb::ParsedGrammar &grammar);
void add_mariadb_select_grammar(duckdb::ParsedGrammar &grammar);

namespace
{

class MariadbMatcherFactory : public duckdb::MatcherFactory
{
public:
  explicit MariadbMatcherFactory(duckdb::CreateMatcherFactoryInput &input)
      : duckdb::MatcherFactory(input.allocator, input.parsed_grammar, input.rules,
                               input.keyword_helper, {})
  {
    AddPackratMemoizedRule("Identifier");
    for (auto &entry : input.terminal_rules)
      AddRuleOverride(entry.first.c_str(), std::move(entry.second));
    auto &root= CreateRootMatcher("Program");
    duckdb::ComputeFirstSets(root, input.keyword_helper.GetLiteralTable());
    input.allocator.InitializeListChains();
  }
};

class MariadbDialect : public duckdb::DialectExtension
{
public:
  MariadbDialect() : duckdb::DialectExtension("mariadb") {}

  void ApplyGrammarChanges(duckdb::GrammarChangesInput &input) override
  {
    add_mariadb_expression_grammar(input.parsed_grammar);
    add_mariadb_select_grammar(input.parsed_grammar);
  }

  duckdb::unique_ptr<duckdb::MatcherFactory> CreateMatcherFactory(
      duckdb::CreateMatcherFactoryInput &input) override
  {
    return duckdb::make_uniq<MariadbMatcherFactory>(input);
  }
};

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
      config, duckdb::make_uniq<MariadbDialect>());
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
