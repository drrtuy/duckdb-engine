/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov <leontodys@gmail.com>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

/*
  Layer A tests: quack_guard on plain DuckDB with a mock authorizer.
  Built with -DDUCKDB_UNIT_TESTS=ON; run ./quack_guard_test (exit 0 = pass).
*/

#include "mock_authorizer.hpp"
#include "guard_internal.hpp"

#include "duckdb.hpp"

#include <cstdio>
#include <string>

extern "C" int32_t duckdb_register_static_extensions(void);

using namespace quack_guard_test;

static int failures= 0;

#define EXPECT(cond)                                                       \
  do                                                                       \
  {                                                                        \
    if (!(cond))                                                           \
    {                                                                      \
      std::fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__,      \
                   #cond);                                                 \
      failures++;                                                          \
    }                                                                      \
  } while (0)

static bool query_ok(duckdb::Connection &con, const std::string &sql,
                     std::string *error= nullptr)
{
  auto result= con.Query(sql);
  if (result->HasError())
  {
    if (error)
      *error= result->GetError();
    return false;
  }
  return true;
}

static std::string scalar(duckdb::Connection &con, const std::string &sql)
{
  auto result= con.Query(sql);
  if (result->HasError())
    return "ERROR: " + result->GetError();
  auto chunk= result->Fetch();
  if (!chunk || chunk->size() == 0)
    return "NO ROWS";
  return chunk->GetValue(0, 0).ToString();
}

/* Regular (non-Quack) connections are not affected by the guard. */
static void test_regular_connections_unaffected(duckdb::DuckDB &db)
{
  duckdb::Connection con(db);
  EXPECT(query_ok(con, "CREATE TABLE t(a INT, b VARCHAR)"));
  EXPECT(query_ok(con, "INSERT INTO t VALUES (1, 'x')"));
  EXPECT(scalar(con, "SELECT count(*) FROM t") == "1");
  EXPECT(!quack_guard::IsQuackSession(*con.context));
  EXPECT(quack_guard::GetPrincipal(*con.context) == nullptr);
}

/* The stock callbacks stay pinned and deny. */
static void test_stock_callbacks_pinned(duckdb::DuckDB &db)
{
  duckdb::Connection con(db);
  EXPECT(scalar(con, "SELECT current_setting('quack_authentication_function')") ==
         "system.main.mariadb_quack_deny_all");
  EXPECT(scalar(con, "SELECT current_setting('quack_authorization_function')") ==
         "system.main.mariadb_quack_deny_query");
  EXPECT(scalar(con, "SELECT system.main.mariadb_quack_deny_all('a','b','c')") ==
         "false");
  EXPECT(scalar(con, "SELECT system.main.mariadb_quack_deny_query('a','b') IS NULL") ==
         "true");
  std::string error;
  EXPECT(!query_ok(con, "CALL quack_serve('quack:localhost:0', token => 'secret')",
                   &error));
  EXPECT(error.find("disabled") != std::string::npos);
}

/* Fail-closed defaults of the bridge contract. */
static void test_contract_defaults()
{
  quack_guard::Verdict v;
  EXPECT(!v.allow);
  EXPECT(v.reason == quack_guard::Reason::Internal);
  EXPECT(!quack_guard::Verdict::Deny(quack_guard::Reason::Allowed).allow);
  MockAuthorizer mock;
  std::shared_ptr<const quack_guard::Identity> out;
  EXPECT(!mock.authenticate({"alice", "", "pw"}, {}, out).allow);
  EXPECT(out == nullptr);
  MockIdentity alice("alice");
  EXPECT(!mock.check(alice, {}).allow);
}

/* A Quack session is denied every statement until the plan gate allows it. */
static void test_quack_session_denied(duckdb::DuckDB &db)
{
  duckdb::Connection setup(db);
  EXPECT(query_ok(setup, "CREATE TABLE denied_t(a INT)"));

  duckdb::Connection con(db);
  quack_guard::AttachPrincipal(*con.context,
                               std::make_shared<MockIdentity>("alice@localhost"));
  EXPECT(quack_guard::IsQuackSession(*con.context));
  EXPECT(quack_guard::GetPrincipal(*con.context)->audit() == "alice@localhost");

  /* Every statement kind that is planned is rejected by the plan gate. */
  const char *statements[]= {
      "SELECT 1",
      "SELECT * FROM denied_t",
      "INSERT INTO denied_t VALUES (1)",
      "UPDATE denied_t SET a= 2",
      "DELETE FROM denied_t",
      "CREATE TABLE created_t(a INT)",
      "DROP TABLE denied_t",
      "SET threads=1",
      "EXPLAIN SELECT 1",
      "CALL pragma_version()",
      "BEGIN TRANSACTION",
      "ATTACH ':memory:' AS other",
      "LOAD json",
      "PREPARE p AS SELECT 1",
      "SELECT 1; SELECT 2",
  };
  for (const char *sql : statements)
  {
    std::string error;
    bool ok= query_ok(con, sql, &error);
    if (ok || error.find("not allowed") == std::string::npos)
    {
      std::fprintf(stderr, "statement not denied by the gate: %s -> %s\n", sql,
                   ok ? "OK" : error.c_str());
      failures++;
    }
  }
  /* Through the prepared-statement API as well */
  auto prepared= con.Prepare("SELECT 1");
  EXPECT(prepared->HasError());
  EXPECT(prepared->GetError().find("not allowed") != std::string::npos);

  /* Nothing changed, and the principal survived the failures */
  EXPECT(scalar(setup, "SELECT count(*) FROM denied_t") == "0");
  EXPECT(scalar(setup, "SELECT count(*) FROM duckdb_tables() WHERE table_name = 'created_t'") == "0");
  EXPECT(quack_guard::IsQuackSession(*con.context));

  /* Other connections, including new ones, are unaffected */
  duckdb::Connection fresh(db);
  EXPECT(!quack_guard::IsQuackSession(*fresh.context));
  EXPECT(scalar(fresh, "SELECT count(*) FROM denied_t") == "0");

  /* A principal cannot be replaced */
  bool threw= false;
  try
  {
    quack_guard::AttachPrincipal(*con.context,
                                 std::make_shared<MockIdentity>("root@localhost"));
  }
  catch (const std::exception &)
  {
    threw= true;
  }
  EXPECT(threw);
  EXPECT(quack_guard::GetPrincipal(*con.context)->audit() == "alice@localhost");

  /* A null principal is rejected */
  threw= false;
  try
  {
    quack_guard::AttachPrincipal(*fresh.context, nullptr);
  }
  catch (const std::exception &)
  {
    threw= true;
  }
  EXPECT(threw);
  EXPECT(!quack_guard::IsQuackSession(*fresh.context));
}

/* A session Quack created without a principal is still guarded. */
static void test_unattached_quack_session_denied(duckdb::DuckDB &db)
{
  duckdb::Connection con(db);
  con.context->registered_state->Insert(
      "quack_session", duckdb::make_shared_ptr<duckdb::ClientContextState>());
  EXPECT(quack_guard::IsQuackSession(*con.context));
  EXPECT(quack_guard::GetPrincipal(*con.context) == nullptr);
  std::string error;
  EXPECT(!query_ok(con, "SELECT 1", &error));
  EXPECT(error.find("not allowed") != std::string::npos);
}

/* The guard starts without an authorizer; it can be set and removed. */
static void test_guard_authorizer(const std::shared_ptr<quack_guard::Guard> &guard)
{
  EXPECT(guard->GetAuthorizer() == nullptr);
  auto mock= std::make_shared<MockAuthorizer>();
  guard->SetAuthorizer(mock);
  EXPECT(guard->GetAuthorizer() == mock);
  guard->SetAuthorizer(nullptr);
  EXPECT(guard->GetAuthorizer() == nullptr);
}

int main()
{
  if (duckdb_register_static_extensions() != 0)
  {
    std::fprintf(stderr, "cannot register static extensions\n");
    return 2;
  }
  duckdb::DuckDB db(nullptr);
  std::shared_ptr<quack_guard::Guard> guard;
  {
    duckdb::Connection con(db);
    guard= quack_guard::Install(*db.instance, con);
  }
  test_contract_defaults();
  test_regular_connections_unaffected(db);
  test_stock_callbacks_pinned(db);
  test_quack_session_denied(db);
  test_unattached_quack_session_denied(db);
  test_guard_authorizer(guard);
  if (failures)
  {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("quack_guard_test: all passed\n");
  return 0;
}
