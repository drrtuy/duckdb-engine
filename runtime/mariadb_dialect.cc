#include "mariadb_dialect.h"

#include "duckdb/main/client_config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/peg/dialect_extension.hpp"
#include "duckdb/parser/peg/matcher_factory.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/parser.hpp"

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

bool mariadb_query_has_lexical_mismatch(std::string_view query,
                                      bool backslash_escapes, bool ansi_quotes)
{
  bool non_ascii= false;
  for (unsigned char c : query)
  {
    if (c == 0)
      return true;
    non_ascii|= c >= 0x80;
  }

  auto quote_end= [query](size_t start, bool escapes) {
    for (size_t i= start + 1; i < query.size(); i++)
    {
      if (query[i] == '\\' && escapes && i + 1 < query.size())
        i++;
      else if (query[i] == query[start])
      {
        if (i + 1 < query.size() && query[i + 1] == query[start])
          i++;
        else
          return i + 1;
      }
    }
    return std::string_view::npos;
  };
  auto identifier_start= [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };

  for (size_t i= 0; i < query.size();)
  {
    unsigned char c= query[i];
    if (c == '#' || c == '$' || c == '\\' || c >= 0x80 ||
        (c == '"' && !ansi_quotes))
      return true;
    if (c == '\'' || c == '"' || c == '`')
    {
      size_t end= quote_end(i, false);
      if (end == std::string_view::npos ||
          (c == '\'' && backslash_escapes && quote_end(i, true) != end))
        return true;
      i= end;
      continue;
    }
    if (c == '-' && i + 1 < query.size() && query[i + 1] == '-')
    {
      if (i + 2 < query.size() &&
          static_cast<unsigned char>(query[i + 2]) > ' ')
        return true;
      size_t end= query.find('\n', i + 2);
      if (end == std::string_view::npos)
        end= query.size();
      auto comment= query.substr(i + 2, end - i - 2);
      size_t cr= comment.find('\r');
      if (cr != std::string_view::npos &&
          (end == query.size() || cr + 1 != comment.size()))
        return true;
      i= end;
      continue;
    }
    if (c == '/' && i + 1 < query.size() && query[i + 1] == '*')
    {
      size_t end= query.find("*/", i + 2);
      if (end == std::string_view::npos)
        return true;
      auto comment= query.substr(i + 2, end - i - 2);
      if ((!comment.empty() && comment[0] == '!') ||
          (comment.size() >= 2 && (comment[0] == 'M' || comment[0] == 'm') &&
           comment[1] == '!') || comment.find("/*") != std::string_view::npos)
        return true;
      i= end + 2;
      continue;
    }
    if (c == '*' && i + 1 < query.size() && query[i + 1] == '/')
      return true;
    if (identifier_start(c))
    {
      size_t start= i++;
      while (i < query.size() &&
             (identifier_start(query[i]) ||
              (query[i] >= '0' && query[i] <= '9') || query[i] == '$'))
        i++;
      if (i < query.size() && query[i] == '\'' &&
          (query[start] == '_' ||
           (i == start + 1 && (c == 'E' || c == 'e'))))
        return true;
      continue;
    }
    i++;
  }
  if (non_ascii)
  {
    try
    {
      std::string original(query);
      return duckdb::Parser::NormalizeSQLString(original) != original;
    }
    catch (const duckdb::Exception &)
    {
      return true;
    }
  }
  return false;
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
