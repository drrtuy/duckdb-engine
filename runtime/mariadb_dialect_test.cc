#include "mariadb_dialect.h"
#include "mariadb_dialect_test_helpers.h"

#include "duckdb.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/main/query_result_stream.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/peg/dialect_extension.hpp"
#include "duckdb/parser/peg/compiled_grammar.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

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

void require_mariadb_query_parity(duckdb::Connection &connection,
                                  const char *source, const char *normalized)
{
  require_base(connection);
  require(!myduck::mariadb_query_has_lexical_mismatch(source, true, true),
          "Grammar test is outside the lexical eligibility corpus");
  connection.BeginTransaction();
  auto before= parser_entries.load();
  auto candidate= myduck::parse_mariadb_statement(connection, source);
  require(parser_entries == before + 1, "Dialect grammar parsed a query repeatedly");
  require_base(connection);
  auto baseline= connection.ExtractStatements(normalized);
  require(baseline.size() == 1 &&
              baseline[0]->type == duckdb::StatementType::SELECT_STATEMENT,
          "Invalid normalized baseline in grammar test");
  if (!candidate->Cast<duckdb::SelectStatement>().node->Equals(
          baseline[0]->Cast<duckdb::SelectStatement>().node.get()))
    throw std::runtime_error(std::string("AST parity mismatch: ") + source +
                             "\nCandidate: " + candidate->ToString() +
                             "\nBaseline: " + baseline[0]->ToString());

  before= parser_entries.load();
  auto actual= connection.Submit(std::move(candidate));
  actual->Complete();
  if (actual->HasError())
    throw std::runtime_error(std::string(source) + ": " + actual->GetError());
  auto expected= connection.Submit(std::move(baseline[0]));
  expected->Complete();
  if (expected->HasError())
    throw std::runtime_error(std::string(normalized) + ": " + expected->GetError());
  require(parser_entries == before, "AST parity execution reparsed SQL");
  require(actual->ColumnCount() == expected->ColumnCount(), "Result column counts differ");
  for (duckdb::idx_t i= 0; i < actual->ColumnCount(); i++)
    if (actual->GetNames()[i].GetIdentifierName() !=
        expected->GetNames()[i].GetIdentifierName())
      throw std::runtime_error(std::string("Column-name parity mismatch: ") + source +
          "\nCandidate: " + actual->GetNames()[i].GetIdentifierName() +
          "\nBaseline: " + expected->GetNames()[i].GetIdentifierName());
  if (!actual->Equals(*expected))
    throw std::runtime_error(std::string("Result/metadata parity mismatch: ") + source);
  connection.Rollback();
  require_base(connection);
}

void require_mariadb_query_error_parity(duckdb::Connection &connection,
                                        const char *source, const char *normalized)
{
  auto candidate= myduck::parse_mariadb_statement(connection, source);
  auto baseline= connection.ExtractStatements(normalized);
  require(baseline.size() == 1, "Invalid error-parity baseline");
  require(candidate->Cast<duckdb::SelectStatement>().node->Equals(
              baseline[0]->Cast<duckdb::SelectStatement>().node.get()),
          "Error-parity ASTs differ");
  auto before= parser_entries.load();
  auto expected= connection.Submit(std::move(baseline[0]));
  expected->Complete();
  auto actual= connection.Submit(std::move(candidate));
  actual->Complete();
  require(actual->HasError() && expected->HasError(), "Expected both AST executions to fail");
  require(actual->GetErrorType() == expected->GetErrorType(), "Dialect changed error category");
  require(parser_entries == before, "Error-parity execution reparsed SQL");
  require_base(connection);
}

void require_mariadb_parse_error(duckdb::Connection &connection, const char *source)
{
  bool refused= false;
  try
  {
    myduck::parse_mariadb_statement(connection, source);
  }
  catch (const duckdb::Exception &)
  {
    refused= true;
  }
  if (!refused)
    throw std::runtime_error(std::string("Invalid dialect syntax was accepted: ") + source);
  require_base(connection);
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

static void test_shared_grammar(duckdb::DuckDB &database, duckdb::Connection &connection,
                                const char *query= "SELECT 1")
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
        myduck::parse_mariadb_statement(other, query);
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

static void benchmark_mariadb_parser(duckdb::Connection &connection)
{
  duckdb::DBConfig probe_config;
  probe_config.options.maximum_threads= 1;
  probe_config.options.async_threads= 1;
  duckdb::DialectExtension::Register(probe_config,
      duckdb::make_uniq<duckdb::DialectExtension>("mariadb"));
  duckdb::DuckDB probe_database(nullptr, &probe_config);
  duckdb::Connection probe(probe_database);
  std::vector<double> base, empty_dialect, dialect_normalized, dialect;
  const char *normalized= "SELECT CAST(42 AS CHAR) AS v LIMIT 1 OFFSET 0";
  myduck::parse_mariadb_statement(probe, normalized);
  auto measure= [&](int mode) {
    auto start= std::chrono::steady_clock::now();
    if (mode == 1)
      myduck::parse_mariadb_statement(probe, normalized);
    else if (mode)
      myduck::parse_mariadb_statement(connection, mode == 2 ? normalized :
          "SELECT CONVERT(42, CHAR) AS v LIMIT 0,1");
    else
      connection.ExtractStatements(normalized);
    return std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count();
  };
  for (int i= 0; i < 1000; i++)
  {
    if (i % 2)
    {
      dialect.push_back(measure(3));
      dialect_normalized.push_back(measure(2));
      empty_dialect.push_back(measure(1));
      base.push_back(measure(0));
    }
    else
    {
      base.push_back(measure(0));
      empty_dialect.push_back(measure(1));
      dialect_normalized.push_back(measure(2));
      dialect.push_back(measure(3));
    }
  }
  std::sort(base.begin(), base.end());
  std::sort(empty_dialect.begin(), empty_dialect.end());
  std::sort(dialect_normalized.begin(), dialect_normalized.end());
  std::sort(dialect.begin(), dialect.end());
  std::printf("Normalized base PEG parse (us): p50=%.2f p95=%.2f p99=%.2f\n",
              base[500], base[950], base[990]);
  std::printf("Normalized SQL under empty dialect + scope (us): p50=%.2f p95=%.2f p99=%.2f\n",
              empty_dialect[500], empty_dialect[950], empty_dialect[990]);
  std::printf("Normalized SQL under MariaDB dialect + scope (us): p50=%.2f p95=%.2f p99=%.2f\n",
              dialect_normalized[500], dialect_normalized[950], dialect_normalized[990]);
  std::printf("MariaDB source under dialect + scope (us): p50=%.2f p95=%.2f p99=%.2f\n",
              dialect[500], dialect[950], dialect[990]);
}

static void test_registration(bool benchmark)
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
  require(grammar->ProgramMatcher().first_set.computed &&
              grammar->TopLevelStatementMatcher().first_set.computed,
          "Dialect matcher graph was published without FIRST-set finalization");
  myduck::parse_mariadb_statement(connection, "SELECT 2");
  require(dialect->GetCompiledGrammar(*connection.context) == grammar,
          "Production dialect did not reuse its warmed grammar");
  require_base(connection);
  test_mariadb_select_grammar(connection);
  test_mariadb_expression_grammar(connection);
  require_mariadb_query_parity(connection, "SELECT CONVERT(42, CHAR) AS v",
                              "SELECT CAST(42 AS CHAR) AS v");
  test_shared_grammar(database, connection, "SELECT CONVERT(42, CHAR) AS v LIMIT 0,1");
  if (benchmark)
    benchmark_mariadb_parser(connection);
}

static void test_lexical_preflight()
{
  struct Case
  {
    const char *sql;
    bool unsafe;
    bool needs_ansi= false;
  };
  const Case cases[]= {
      {"SELECT 1 # hidden SQL", true},
      {"SELECT REPEAT('a',5) AS $$ FROM t # $$ WHERE 0 UNION ALL SELECT 'hidden'", true},
      {"SELECT $$text$$", true},
      {"SELECT $tag$text$tag$", true},
      {"SELECT $unterminated$", true},
      {"SELECT $name", true},
      {"SELECT $1", true},
      {"SELECT 1$$text$$", true},
      {"SELECT 10--1", true},
      {"SELECT 1 /* outer /* nested */ + 1", true},
      {"SELECT 1 /*! + 1 */", true},
      {"SELECT 1 /*M! + 1 */", true},
      {"SELECT 1 /*m!100100 + 1 */", true},
      {"SELECT 1 /* unterminated", true},
      {"SELECT 1 */", true},
      {"SELECT 'unterminated", true},
      {"SELECT \"unterminated", true},
      {"SELECT `unterminated", true},
      {"SELECT E'escaped'", true},
      {"SELECT e'escaped'", true},
      {"SELECT _utf8mb4'test'", true},
      {"SELECT 1 -- comment\r+ 1", true},
      {"SELECT 1 -- comment\r", true},
      {"SELECT café FROM t", true},
      {"SELECT name\xc2\xa0$$tag$$ FROM t", true},
      {"SELECT `a\u00a0b` FROM t", true},
      {"SELECT `a'b`, 'x\u00a0y' FROM t", true},
      {"SELECT '\xc3'", true},
      {"SELECT 'a\u00a0b'", false},
      {"SELECT \"a\u00a0b\" FROM t", false, true},
      {"", false},
      {"SELECT 1", false},
      {"SELECT 10 - -1", false},
      {"SELECT col$tag$, name$ FROM t", false},
      {"SELECT 'literal # $$ $tag$ /* /*! -- E'", false},
      {"SELECT 'doubled '' # $$ -- quote'", false},
      {"SELECT `quoted``#$$/*name` FROM t", false},
      {"SELECT \"quoted\"\"#$$/*name\" FROM t", false, true},
      {"SELECT 'Привет', `café`, \"текст\"", false, true},
      {"SELECT 1 /* ordinary '# $$ E\\' comment */", false},
      {"SELECT 1 -- comment '# $$ /* E\\'\n + 1", false},
      {"SELECT 1 -- comment\r\n + 1", false},
      {"SELECT 1 -- comment at EOF", false},
      {"SELECT 1 --\x01 ignored SELECT 99\n + 1", false},
      {"SELECT 1 --\x7f ignored", true},
      {"SELECT N'abc', X'61', B'01'", false},
      {"SELECT enum_value, e FROM t", false}};
  auto before= parser_entries.load();
  for (bool escapes : {false, true})
    for (bool ansi : {false, true})
    {
      for (const auto &entry : cases)
      {
        bool unsafe= myduck::mariadb_query_has_lexical_mismatch(entry.sql, escapes, ansi);
        if (unsafe != (entry.unsafe || (entry.needs_ansi && !ansi)))
          throw std::runtime_error(std::string("Lexical preflight mismatch: ") + entry.sql);
      }
      require(myduck::mariadb_query_has_lexical_mismatch(
                  "SELECT 'a\\' OR true -- '", escapes, ansi) == escapes,
              "Single-quoted escape mode was not respected");
      require(myduck::mariadb_query_has_lexical_mismatch(
                  "SELECT \"a\\\" OR true -- \"", escapes, ansi) == !ansi,
              "ANSI_QUOTES escape mode was not respected");
      require(!myduck::mariadb_query_has_lexical_mismatch(
                  "SELECT `a\\` FROM t", escapes, ansi),
              "Backslash in an identifier was treated as a string escape");
      require(myduck::mariadb_query_has_lexical_mismatch(
                  std::string_view("SELECT '\0'", 10), escapes, ansi),
              "Embedded NUL was accepted");
    }
  require(parser_entries == before, "Lexical preflight invoked the PEG parser");
}

int main(int argc, char **argv)
{
  try
  {
    bool benchmark= argc == 2 && std::string(argv[1]) == "--benchmark";
    require(argc == 1 || benchmark, "Usage: mariadb_dialect_test [--benchmark]");
    test_lexical_preflight();
    require(duckdb_register_static_extensions() == 0,
            "Could not register statically linked extensions");
    test_registration(benchmark);
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
    std::puts("PASS: MariaDB dialect phases 0 through 3");
    return 0;
  }
  catch (const std::exception &error)
  {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
