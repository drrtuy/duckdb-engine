#include "mariadb_dialect.h"

#include "duckdb.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/peg/dialect_extension.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/sql_statement.hpp"

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>

extern "C" int32_t duckdb_register_static_extensions(void);

static std::atomic<size_t> parser_entries{0};
static std::atomic<size_t> grammar_compilations{0};

duckdb::unique_ptr<duckdb::SQLStatement> real_parse(
    duckdb::TokenIterator &, duckdb::ParserOptions &, const duckdb::CompiledGrammar &,
    duckdb::optional_idx &)
    __asm__("__real__ZN6duckdb21PEGTransformerFactory26TransformTopLevelStatementERNS_13TokenIteratorERNS_13ParserOptionsERKNS_15CompiledGrammarERNS_12optional_idxE");

duckdb::unique_ptr<duckdb::SQLStatement> counted_parse(
    duckdb::TokenIterator &, duckdb::ParserOptions &, const duckdb::CompiledGrammar &,
    duckdb::optional_idx &)
    __asm__("__wrap__ZN6duckdb21PEGTransformerFactory26TransformTopLevelStatementERNS_13TokenIteratorERNS_13ParserOptionsERKNS_15CompiledGrammarERNS_12optional_idxE");

duckdb::unique_ptr<duckdb::SQLStatement> counted_parse(
    duckdb::TokenIterator &tokens, duckdb::ParserOptions &options,
    const duckdb::CompiledGrammar &grammar, duckdb::optional_idx &failure_index)
{
  parser_entries++;
  return real_parse(tokens, options, grammar, failure_index);
}

class CountingDialect : public duckdb::DialectExtension
{
public:
  CountingDialect() : duckdb::DialectExtension("mariadb") {}

  void ApplyGrammarChanges(duckdb::GrammarChangesInput &) override
  {
    grammar_compilations++;
  }
};

static void require(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}

static void require_base(duckdb::Connection &connection)
{
  require(!duckdb::ClientConfig::GetConfig(*connection.context).current_dialect,
          "MariaDB dialect leaked into the base connection");
}

static void require_query(duckdb::Connection &connection, const std::string &sql)
{
  auto result= connection.Query(sql);
  if (result->HasError())
    throw std::runtime_error(result->GetError());
}

static void test_single_parse(duckdb::Connection &connection)
{
  auto before= parser_entries.load();
  auto statement= myduck::parse_mariadb_statement(
      connection, "SELECT 42 AS `value`");
  require_base(connection);
  require(parser_entries == before + 1, "Extraction did not parse exactly once");
  statement->query= "not valid SQL: source is diagnostic only";

  duckdb::QueryResultStream<duckdb::ChunkFormat> stream(
      connection.Submit(std::move(statement)));
  auto chunk= stream.Fetch();
  require(chunk && chunk->size() == 1, "Expected one streamed row");
  require(chunk->GetValue(0, 0) == duckdb::Value::INTEGER(42),
          "AST submission returned an unexpected value");
  require(!stream.Fetch() && !stream.HasError(), "Stream did not finish cleanly");
  require(parser_entries == before + 1, "AST submission or fetch parsed SQL again");
  require_base(connection);
}

static void test_refusals(duckdb::Connection &connection)
{
  for (const char *sql : {"SELECT (", "SELECT 1; SELECT 2", "CREATE TABLE unwanted(i INT)"})
  {
    bool refused= false;
    try
    {
      myduck::parse_mariadb_statement(connection, sql);
    }
    catch (const duckdb::Exception &)
    {
      refused= true;
    }
    require(refused, "Invalid or non-SELECT input was accepted");
    require_base(connection);
    require_query(connection, "SELECT 7 AS \"base_identifier\"");
  }

  auto statement= myduck::parse_mariadb_statement(connection, "SELECT missing_column");
  auto before= parser_entries.load();
  auto result= connection.Submit(std::move(statement));
  require(result->HasError(), "Expected a binding error");
  require(parser_entries == before, "Binding error reparsed the source");
  require_base(connection);
  require_query(connection, "SELECT 1");

  duckdb::CurrentDialectSetting::SetLocal(*connection.context, duckdb::Value("mariadb"));
  bool refused= false;
  try
  {
    myduck::parse_mariadb_statement(connection, "SELECT 1");
  }
  catch (const duckdb::Exception &)
  {
    refused= true;
  }
  require(refused, "Unexpected entry dialect was silently replaced");
  require(duckdb::ClientConfig::GetConfig(*connection.context).current_dialect == "mariadb",
          "Refused scope changed the existing dialect");
  duckdb::CurrentDialectSetting::ResetLocal(*connection.context);
  require_base(connection);
}

static void test_reparse_counter(duckdb::Connection &connection)
{
  require_query(connection, "SET debug_verify_statement='REPARSE_STATEMENT'");
  auto before= parser_entries.load();
  auto statement= myduck::parse_mariadb_statement(connection, "SELECT 42");
  duckdb::QueryResultStream<duckdb::ChunkFormat> stream(
      connection.Submit(std::move(statement)));
  require(static_cast<bool>(stream.Fetch()), "Verification query returned no row");
  require(!stream.Fetch() && !stream.HasError(), "Verification query failed");
  auto entries= parser_entries.load() - before;
  require_query(connection, "SET debug_verify_statement='NONE'");
  require(entries == 2, "Parser counter did not detect the verification reparse");
  require_base(connection);
}

static void test_transaction_error(duckdb::DuckDB &database)
{
  duckdb::Connection baseline(database);
  duckdb::Connection candidate(database);
  require_query(baseline, "BEGIN");
  require_query(candidate, "BEGIN");
  auto error= baseline.Submit("SELECT (");
  require(error->HasError(), "Baseline syntax error was not reported");
  bool refused= false;
  try
  {
    myduck::parse_mariadb_statement(candidate, "SELECT (");
  }
  catch (const duckdb::Exception &)
  {
    refused= true;
  }
  require(refused, "Dialect syntax error was not reported");
  require_base(candidate);
  auto baseline_next= baseline.Query("SELECT 1");
  auto candidate_next= candidate.Query("SELECT 1");
  require(baseline_next->HasError() == candidate_next->HasError(),
          "Parse error changed transaction invalidation behavior");
  require_query(baseline, "ROLLBACK");
  require_query(candidate, "ROLLBACK");
}

static void test_interrupt(duckdb::Connection &connection)
{
  auto before= parser_entries.load();
  auto statement= myduck::parse_mariadb_statement(
      connection, "SELECT sum(i) FROM range(1000000000) t(i)");
  duckdb::QueryResultStream<duckdb::ChunkFormat> stream(
      connection.Submit(std::move(statement)));
  connection.Interrupt();
  while (stream.Fetch())
  {
  }
  require(stream.HasError(), "Interrupted stream did not report an error");
  require(parser_entries == before + 1, "Interrupted execution parsed SQL again");
  stream.Close();
  require_base(connection);
  require_query(connection, "SELECT 1");
}

static void test_shared_grammar(duckdb::DuckDB &database, duckdb::Connection &connection)
{
  auto dialect= duckdb::ExtensionCallbackManager::Get(*connection.context)
                    .GetDialectExtension("mariadb");
  require(static_cast<bool>(dialect), "MariaDB dialect was not registered");
  auto grammar= dialect->GetCompiledGrammar(*connection.context);
  auto compilations= grammar_compilations.load();
  auto before= parser_entries.load();
  std::atomic<bool> failed{false};
  auto parse= [&]() {
    try
    {
      duckdb::Connection other(database);
      for (int i= 0; i < 4; i++)
      {
        myduck::parse_mariadb_statement(other, "SELECT 1");
        require_base(other);
        require(dialect->GetCompiledGrammar(*other.context) == grammar,
                "Dialect grammar was not shared between connections");
      }
    }
    catch (...)
    {
      failed= true;
    }
  };
  std::thread first(parse);
  std::thread second(parse);
  first.join();
  second.join();
  require(!failed, "Concurrent parsing or restoration failed");
  require(parser_entries == before + 8, "Concurrent queries were parsed repeatedly");
  require(grammar_compilations == compilations, "Warmed grammar was compiled again");
}

static void test_registration()
{
  duckdb::DBConfig config;
  config.options.maximum_threads= 1;
  config.options.async_threads= 1;
  myduck::register_mariadb_dialect(config);
  duckdb::DuckDB database(nullptr, &config);
  duckdb::Connection connection(database);
  auto dialect= duckdb::ExtensionCallbackManager::Get(*connection.context)
                    .GetDialectExtension("mariadb");
  require(static_cast<bool>(dialect), "Production registration did not install the dialect");
  myduck::parse_mariadb_statement(connection, "SELECT 1");
  auto grammar= dialect->GetCompiledGrammar(*connection.context);
  myduck::parse_mariadb_statement(connection, "SELECT 2");
  require(dialect->GetCompiledGrammar(*connection.context) == grammar,
          "Production dialect did not reuse its warmed grammar");
  require_base(connection);
}

int main()
{
  try
  {
    require(duckdb_register_static_extensions() == 0,
            "Could not register statically linked extensions");
    test_registration();
    duckdb::DBConfig config;
    config.options.maximum_threads= 1;
    config.options.async_threads= 1;
    duckdb::DialectExtension::Register(config, duckdb::make_uniq<CountingDialect>());
    duckdb::DuckDB database(nullptr, &config);
    duckdb::Connection connection(database);
    require_query(connection, "SELECT 1");

    auto compilations= grammar_compilations.load();
    myduck::parse_mariadb_statement(connection, "SELECT 1");
    require(grammar_compilations == compilations + 1,
            "Warm-up did not compile the dialect exactly once");
    require_base(connection);
    test_reparse_counter(connection);
    test_transaction_error(database);
    require_query(connection, "SET allowed_configs=['threads']");
    require_query(connection, "SET lock_configuration=true");

    test_single_parse(connection);
    test_refusals(connection);
    test_interrupt(connection);
    test_shared_grammar(database, connection);
    require(grammar_compilations == compilations + 1,
            "Statements caused per-query grammar compilation");
    std::puts("PASS: MariaDB dialect Phase 0");
    return 0;
  }
  catch (const std::exception &error)
  {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
