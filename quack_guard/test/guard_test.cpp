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
#include "guarded_server.hpp"
#include "login.hpp"
#include "quack_message.hpp"

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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

static std::string quack_query(duckdb::Connection &con, const std::string &endpoint,
                               const std::string &fingerprint,
                               const std::string &auth, const std::string &sql)
{
  return scalar(con, "FROM quack_query('quack:" + endpoint + "', '" + sql +
                         "', token => '" + auth + "', ssl_fingerprint => '" +
                         fingerprint + "')");
}

/* A bare Quack client: sends single messages, so tests control ids,
   heartbeats and which endpoint (peer address) a message comes from. */
class RawClient
{
public:
  explicit RawClient(const std::string &endpoint)
  {
    auto colon= endpoint.rfind(':');
    std::string host= endpoint.substr(0, colon);
    if (host.size() > 2 && host.front() == '[')
      host= host.substr(1, host.size() - 2);
    client= std::make_unique<duckdb_httplib_openssl::SSLClient>(
        host, std::stoi(endpoint.substr(colon + 1)));
    client->enable_server_certificate_verification(false);
    client->set_keep_alive(true);
  }

  duckdb::unique_ptr<duckdb::QuackMessage> Send(const duckdb::QuackMessage &m)
  {
    duckdb::MemoryStream out;
    m.ToMemoryStream(out);
    auto res= client->Post("/quack", (const char *) out.GetData(),
                           (size_t) out.GetPosition(), "application/vnd.duckdb");
    if (!res || res->status != 200)
      return nullptr;
    duckdb::MemoryStream in;
    in.WriteData((duckdb::const_data_ptr_t) res->body.data(), res->body.size());
    in.Rewind();
    duckdb::BinaryDeserializer d(in);
    auto header= duckdb::QuackMessage::DeserializeHeader(d);
    return duckdb::QuackMessage::DeserializeMessage(d, header);
  }

  /* Session id, or "" with `error` set */
  std::string Login(const std::string &auth, uint64_t heartbeat,
                    std::string *error= nullptr)
  {
    auto response= Send(duckdb::ConnectionRequestMessage(auth, "", heartbeat));
    if (!response)
    {
      if (error)
        *error= "no response";
      return "";
    }
    if (response->Type() == duckdb::MessageType::ERROR_RESPONSE)
    {
      if (error)
        *error= response->Cast<duckdb::ErrorResponse>().ErrorMessage();
      return "";
    }
    return response->ConnectionId();
  }

  /* "" if the server accepted the heartbeat, else its error */
  std::string Heartbeat(const std::string &id)
  {
    auto response= Send(duckdb::HeartbeatRequestMessage(id));
    if (!response)
      return "no response";
    if (response->Type() == duckdb::MessageType::ERROR_RESPONSE)
      return response->Cast<duckdb::ErrorResponse>().ErrorMessage();
    return "";
  }

  void Disconnect(const std::string &id)
  {
    Send(duckdb::DisconnectMessage(id));
  }

private:
  std::unique_ptr<duckdb_httplib_openssl::SSLClient> client;
};

static quack_guard::ListenerOptions listener_options()
{
  quack_guard::ListenerOptions options;
  options.addresses= {"127.0.0.1"};
  options.cert_file= QUACK_TEST_CERT_DIR "/server.pem";
  options.key_file= QUACK_TEST_CERT_DIR "/private_key.pem";
  options.reap_interval_ms= 100;
  return options;
}

static bool has(const std::string &text, const std::string &part)
{
  return text.find(part) != std::string::npos;
}

template <class F> static bool throws(F f)
{
  try
  {
    f();
  }
  catch (const std::exception &)
  {
    return true;
  }
  return false;
}

static std::shared_ptr<MockAuthorizer> password_mock(const std::string &user,
                                                     const std::string &pw)
{
  auto mock= std::make_shared<MockAuthorizer>();
  mock->on_authenticate= [user, pw](const quack_guard::ClientClaims &c,
                                    const quack_guard::TransportFacts &) {
    return c.user == user && c.proof == pw
               ? quack_guard::Verdict::Allow()
               : quack_guard::Verdict::Deny(quack_guard::Reason::Denied);
  };
  return mock;
}

static int closed(MockAuthorizer &mock)
{
  std::lock_guard<std::mutex> lock(mock.lock);
  return mock.closed_calls;
}

static bool wait_for(const std::function<bool()> &cond, int ms= 5000)
{
  for (int waited= 0; waited < ms; waited+= 50)
  {
    if (cond())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return cond();
}

static int counter(MockAuthorizer &mock, int MockAuthorizer::*field)
{
  std::lock_guard<std::mutex> lock(mock.lock);
  return mock.*field;
}

/* True if this host can listen on IPv6 loopback. */
static bool have_ipv6_loopback()
{
  int fd= socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  struct sockaddr_in6 addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin6_family= AF_INET6;
  addr.sin6_addr= in6addr_loopback;
  bool ok= bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0;
  close(fd);
  return ok;
}

static std::string error_of(const std::function<void()> &f)
{
  try
  {
    f();
  }
  catch (const std::exception &ex)
  {
    return ex.what();
  }
  return "";
}

/* Starting and stopping the listener. */
static void test_listener_lifecycle(const std::shared_ptr<quack_guard::Guard> &guard)
{
  auto options= listener_options();

  auto no_tls= options;
  no_tls.cert_file.clear();
  EXPECT(has(error_of([&] { guard->StartListener(no_tls); }), "certificate and key"));
  auto bad_cert= options;
  bad_cert.cert_file= "/nonexistent/cert.pem";
  EXPECT(has(error_of([&] { guard->StartListener(bad_cert); }), "cannot read the certificate"));
  auto bad_host= options;
  bad_host.addresses= {"no-such-host.invalid"};
  EXPECT(has(error_of([&] { guard->StartListener(bad_host); }), "cannot resolve"));

  auto listener= guard->StartListener(options);
  auto status= listener->Status();
  EXPECT(status.running);
  EXPECT(status.endpoints.size() == 1);
  EXPECT(status.endpoints[0].rfind("127.0.0.1:", 0) == 0);
  EXPECT(status.fingerprint.size() > 64);
  EXPECT(status.failure.empty());
  auto port= status.endpoints[0].substr(10);

  /* One listener per guard */
  EXPECT(has(error_of([&] { guard->StartListener(options); }), "already running"));
  EXPECT(listener->Status().running);

  listener->Stop();
  EXPECT(!listener->Status().running);
  EXPECT(listener->Status().endpoints.empty());
  listener->Stop();  /* idempotent */

  /* The second address fails (the same address and port again: no
     SO_REUSEPORT), and the first one must have been released. */
  auto fixed= options;
  fixed.port= (uint16_t) std::stoi(port);
  auto twice= fixed;
  twice.addresses= {"127.0.0.1", "127.0.0.1"};
  EXPECT(has(error_of([&] { guard->StartListener(twice); }),
             "cannot listen on 127.0.0.1 port " + port));
  auto same_port= guard->StartListener(fixed);
  EXPECT(same_port->Status().endpoints[0] == "127.0.0.1:" + port);

  /* Restart keeps the certificate */
  auto fingerprint= same_port->Status().fingerprint;
  same_port->Stop();
  auto restarted= guard->StartListener(fixed);
  EXPECT(restarted->Status().fingerprint == fingerprint);
  restarted.reset();  /* destroying the handle stops it */
  EXPECT(!has(error_of([&] { guard->StartListener(fixed)->Stop(); }), "already running"));
}

/* Login through the authorizer, with Quack's own client. */
static void test_listener_login(duckdb::DuckDB &db,
                                const std::shared_ptr<quack_guard::Guard> &guard)
{
  auto listener= guard->StartListener(listener_options());
  auto endpoint= listener->Status().endpoints[0];
  auto fp= listener->Status().fingerprint;
  duckdb::Connection client(db);

  /* No authorizer: nobody gets in */
  auto r= quack_query(client, endpoint, fp, "alice:pw", "SELECT 42");
  EXPECT(has(r, "Authentication failed"));

  auto mock= password_mock("alice", "pw");
  guard->SetAuthorizer(mock);
  r= quack_query(client, endpoint, fp, "alice:wrong", "SELECT 42");
  EXPECT(has(r, "Authentication failed"));
  r= quack_query(client, endpoint, fp, "al%ZZice:pw", "SELECT 42");
  EXPECT(has(r, "Authentication failed"));

  /* Right password: the session is created, then the statement is refused
     by Quack's closed statement callback */
  r= quack_query(client, endpoint, fp, "alice:pw", "SELECT 42");
  EXPECT(has(r, "Authorization failed"));
  {
    std::lock_guard<std::mutex> lock(mock->lock);
    /* A malformed auth string never reaches the authorizer */
    EXPECT(mock->authenticate_calls == 2);
    for (auto &t : mock->transports)
      EXPECT(t.peer_ip == "127.0.0.1" && t.tls);
  }
  /* Quack's client disconnects when done: the session is closed */
  EXPECT(wait_for([&] { return closed(*mock) == 1; }));
  EXPECT(guard->SessionCount() == 0);
  listener->Stop();
  guard->SetAuthorizer(nullptr);
}

/* Sessions: peer binding, ids, heartbeats, reaping, limits, stop. */
static void test_listener_sessions(const std::shared_ptr<quack_guard::Guard> &guard)
{
  bool ipv6= have_ipv6_loopback();
  if (!ipv6)
    std::fprintf(stderr, "note: no IPv6 loopback, peer-binding cases skipped\n");
  auto options= listener_options();
  options.addresses= ipv6 ? std::vector<std::string>{"127.0.0.1", "::1"}
                          : std::vector<std::string>{"127.0.0.1"};
  options.max_sessions= 3;
  options.max_heartbeat_seconds= 60;
  auto listener= guard->StartListener(options);
  auto endpoints= listener->Status().endpoints;
  EXPECT(endpoints.size() == (ipv6 ? 2u : 1u));
  if (ipv6)
  {
    EXPECT(endpoints[1].rfind("[::1]:", 0) == 0);
    EXPECT(endpoints[0].substr(10) == endpoints[1].substr(6));  /* one port */
  }

  auto mock= password_mock("alice", "pw");
  guard->SetAuthorizer(mock);
  RawClient v4(endpoints[0]);
  std::unique_ptr<RawClient> v6;
  if (ipv6)
    v6= std::make_unique<RawClient>(endpoints[1]);

  std::string error;
  auto id= v4.Login("alice:pw", 60, &error);
  EXPECT(!id.empty());
  EXPECT(guard->SessionCount() == 1);
  EXPECT(v4.Heartbeat(id).empty());
  EXPECT(has(v4.Heartbeat("0123456789abcdef0123456789abcdef"),
             "Invalid connection id"));
  std::string id6;
  if (ipv6)
  {
    /* The same id from another peer address is refused, also to close it */
    EXPECT(has(v6->Heartbeat(id), "Invalid connection id"));
    v6->Disconnect(id);
    EXPECT(closed(*mock) == 0);
    EXPECT(v4.Heartbeat(id).empty());  /* still usable from its own peer */
    /* An IPv6 peer reaches the authorizer as "::1" */
    id6= v6->Login("alice:pw", 60);
    EXPECT(!id6.empty());
    std::lock_guard<std::mutex> lock(mock->lock);
    EXPECT(mock->transports.back().peer_ip == "::1");
  }
  else
    id6= v4.Login("alice:pw", 60);

  /* Heartbeat leases are bounded; refused before the authorizer is asked */
  int asked= counter(*mock, &MockAuthorizer::authenticate_calls);
  EXPECT(v4.Login("alice:pw", 61, &error).empty());
  EXPECT(has(error, "heartbeat_timeout"));
  EXPECT(v4.Login("alice:pw", 0, &error).empty());
  EXPECT(has(error, "heartbeat_timeout"));
  EXPECT(counter(*mock, &MockAuthorizer::authenticate_calls) == asked);

  /* Session cap, also checked before the authorizer is asked */
  auto id3= v4.Login("alice:pw", 60);
  EXPECT(!id3.empty());
  asked= counter(*mock, &MockAuthorizer::authenticate_calls);
  EXPECT(v4.Login("alice:pw", 60, &error).empty());
  EXPECT(has(error, "Too many Quack sessions"));
  EXPECT(counter(*mock, &MockAuthorizer::authenticate_calls) == asked);
  EXPECT(guard->SessionCount() == 3);

  /* Disconnect closes exactly that session */
  v4.Disconnect(id3);
  EXPECT(closed(*mock) == 1);
  {
    std::lock_guard<std::mutex> lock(mock->lock);
    EXPECT(mock->closed_identities.size() == 1 &&
           has(mock->closed_identities[0], "#3"));
  }
  EXPECT(has(v4.Heartbeat(id3), "Invalid connection id"));

  /* A 1 s lease kept alive by heartbeats is not reaped... */
  auto short_lived= v4.Login("alice:pw", 1);
  EXPECT(!short_lived.empty());
  for (int i= 0; i < 8; i++)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT(v4.Heartbeat(short_lived).empty());
  }
  EXPECT(closed(*mock) == 1);
  /* ...an abandoned one is */
  EXPECT(wait_for([&] { return closed(*mock) == 2; }));
  EXPECT(has(v4.Heartbeat(short_lived), "Invalid connection id"));

  /* Sessions are closed by the authorizer that admitted them */
  auto other= password_mock("alice", "pw");
  guard->SetAuthorizer(other);
  listener->Stop();
  EXPECT(closed(*mock) == 4);  /* id and id6 */
  EXPECT(closed(*other) == 0);
  EXPECT(guard->SessionCount() == 0);
  guard->SetAuthorizer(nullptr);
}

/* Quack itself notices an expired lease when the client comes back. */
static void test_listener_lazy_expiry(const std::shared_ptr<quack_guard::Guard> &guard)
{
  auto options= listener_options();
  options.reap_interval_ms= 60000;  /* the reaper stays out of it */
  auto listener= guard->StartListener(options);
  auto mock= password_mock("alice", "pw");
  guard->SetAuthorizer(mock);
  RawClient raw(listener->Status().endpoints[0]);
  auto id= raw.Login("alice:pw", 1);
  EXPECT(!id.empty());
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  EXPECT(!raw.Heartbeat(id).empty());
  EXPECT(closed(*mock) == 1);
  EXPECT(guard->SessionCount() == 0);
  listener->Stop();
  EXPECT(closed(*mock) == 1);
  guard->SetAuthorizer(nullptr);
}

/* Destroying the handle with open sessions closes them. */
static void test_listener_handle_destroyed(const std::shared_ptr<quack_guard::Guard> &guard)
{
  auto listener= guard->StartListener(listener_options());
  auto mock= password_mock("alice", "pw");
  guard->SetAuthorizer(mock);
  RawClient raw(listener->Status().endpoints[0]);
  EXPECT(!raw.Login("alice:pw", 60).empty());
  listener.reset();
  EXPECT(closed(*mock) == 1);
  EXPECT(guard->SessionCount() == 0);
  guard->SetAuthorizer(nullptr);
}

/* Authorizer misbehaviour never admits anyone or leaks a session. */
static void test_listener_authorizer_failures(const std::shared_ptr<quack_guard::Guard> &guard)
{
  auto listener= guard->StartListener(listener_options());
  RawClient raw(listener->Status().endpoints[0]);
  std::string error;

  struct DenyWithIdentity : MockAuthorizer
  {
    quack_guard::Verdict authenticate(const quack_guard::ClientClaims &c,
                                      const quack_guard::TransportFacts &t,
                                      std::shared_ptr<const quack_guard::Identity> &out) override
    {
      MockAuthorizer::authenticate(c, t, out);
      out= std::make_shared<MockIdentity>("leaked");
      return quack_guard::Verdict::Deny(quack_guard::Reason::Locked);
    }
  };
  auto leaky= std::make_shared<DenyWithIdentity>();
  guard->SetAuthorizer(leaky);
  EXPECT(raw.Login("alice:pw", 60, &error).empty());
  EXPECT(has(error, "Authentication failed"));
  EXPECT(closed(*leaky) == 1);
  EXPECT(guard->SessionCount() == 0);

  struct AllowWithoutIdentity : MockAuthorizer
  {
    quack_guard::Verdict authenticate(const quack_guard::ClientClaims &,
                                      const quack_guard::TransportFacts &,
                                      std::shared_ptr<const quack_guard::Identity> &) override
    {
      return quack_guard::Verdict::Allow();
    }
  };
  guard->SetAuthorizer(std::make_shared<AllowWithoutIdentity>());
  EXPECT(raw.Login("alice:pw", 60, &error).empty());
  EXPECT(has(error, "Authentication failed"));
  EXPECT(guard->SessionCount() == 0);

  struct Throwing : MockAuthorizer
  {
    quack_guard::Verdict authenticate(const quack_guard::ClientClaims &,
                                      const quack_guard::TransportFacts &,
                                      std::shared_ptr<const quack_guard::Identity> &) override
    {
      throw std::runtime_error("secret internal detail");
    }
  };
  guard->SetAuthorizer(std::make_shared<Throwing>());
  EXPECT(raw.Login("alice:pw", 60, &error).empty());
  EXPECT(has(error, "Authentication failed") && !has(error, "secret"));
  EXPECT(guard->SessionCount() == 0);

  /* sessionClosed() that throws: the session is still gone */
  struct ThrowOnClose : MockAuthorizer
  {
    void sessionClosed(const quack_guard::Identity &i) override
    {
      MockAuthorizer::sessionClosed(i);
      throw std::runtime_error("close failed");
    }
  };
  auto thrower= std::make_shared<ThrowOnClose>();
  thrower->on_authenticate= [](const quack_guard::ClientClaims &,
                               const quack_guard::TransportFacts &) {
    return quack_guard::Verdict::Allow();
  };
  guard->SetAuthorizer(thrower);
  auto a= raw.Login("alice:pw", 60), b= raw.Login("alice:pw", 60);
  EXPECT(!a.empty() && !b.empty());
  raw.Disconnect(a);
  EXPECT(guard->SessionCount() == 1);
  listener->Stop();
  EXPECT(closed(*thrower) == 2);
  EXPECT(guard->SessionCount() == 0);
  guard->SetAuthorizer(nullptr);
}

/* Connection and request size limits. */
static void test_listener_limits(const std::shared_ptr<quack_guard::Guard> &guard)
{
  auto options= listener_options();
  options.max_connections= 2;
  options.max_request_bytes= 256;
  auto listener= guard->StartListener(options);
  auto endpoint= listener->Status().endpoints[0];
  auto mock= password_mock("alice", "pw");
  guard->SetAuthorizer(mock);

  /* Two kept-alive client connections use both slots */
  auto c1= std::make_unique<RawClient>(endpoint);
  auto c2= std::make_unique<RawClient>(endpoint);
  std::string error;
  EXPECT(!c1->Login("alice:pw", 60).empty());
  EXPECT(!c2->Login("alice:pw", 60).empty());
  RawClient c3(endpoint);
  EXPECT(c3.Login("alice:pw", 60, &error).empty());
  EXPECT(error == "no response");
  /* A slot frees when a client goes away */
  c1.reset();
  EXPECT(wait_for([&] { return !c3.Login("alice:pw", 60).empty(); }));

  /* An oversized request is refused before anything is parsed */
  c2.reset();
  int asked= counter(*mock, &MockAuthorizer::authenticate_calls);
  RawClient big(endpoint);
  EXPECT(big.Login("alice:" + std::string(1024, 'p'), 60, &error).empty());
  EXPECT(error == "no response");
  EXPECT(counter(*mock, &MockAuthorizer::authenticate_calls) == asked);
  listener->Stop();
  guard->SetAuthorizer(nullptr);
}

/* Concurrent logins, a session-cap race, and stop under load. */
static void test_listener_concurrency(const std::shared_ptr<quack_guard::Guard> &guard)
{
  {
    auto options= listener_options();
    options.max_sessions= 1000;
    auto listener= guard->StartListener(options);
    auto endpoint= listener->Status().endpoints[0];
    auto mock= password_mock("alice", "pw");
    guard->SetAuthorizer(mock);
    std::atomic<int> admitted{0}, lost{0};
    std::vector<std::thread> threads;
    for (int t= 0; t < 8; t++)
      threads.emplace_back([&, t] {
        RawClient raw(endpoint);
        for (int i= 0; i < 5; i++)
        {
          std::string error;
          auto id= raw.Login((i + t) % 2 ? "alice:pw" : "alice:no", 60, &error);
          if (error == "no response")
            lost++;
          if (id.empty())
            continue;
          admitted++;
          if (i % 2)
            raw.Disconnect(id);
        }
      });
    for (auto &t : threads)
      t.join();
    EXPECT(lost == 0);
    EXPECT(admitted == 20);
    EXPECT(counter(*mock, &MockAuthorizer::authenticate_calls) == 40);
    listener->Stop();
    EXPECT(closed(*mock) == 20);
    EXPECT(guard->SessionCount() == 0);
  }
  {
    /* Session cap under a login race: never more than 2 sessions, and every
       identity the authorizer handed out is released */
    auto options= listener_options();
    options.max_sessions= 2;
    auto listener= guard->StartListener(options);
    auto endpoint= listener->Status().endpoints[0];
    auto mock= password_mock("alice", "pw");
    guard->SetAuthorizer(mock);
    std::atomic<bool> go{false};
    std::atomic<size_t> most{0};
    std::vector<std::thread> threads;
    for (int t= 0; t < 8; t++)
      threads.emplace_back([&] {
        RawClient raw(endpoint);
        while (!go)
          std::this_thread::yield();
        raw.Login("alice:pw", 60);
        size_t now= guard->SessionCount();
        size_t seen= most;
        while (now > seen && !most.compare_exchange_weak(seen, now))
          ;
      });
    go= true;
    for (auto &t : threads)
      t.join();
    EXPECT(most <= 2);
    listener->Stop();
    EXPECT(closed(*mock) == counter(*mock, &MockAuthorizer::allow_calls));
    EXPECT(guard->SessionCount() == 0);
  }
  {
    /* Stop while logins are still coming in */
    auto options= listener_options();
    options.max_sessions= 1000;
    auto listener= guard->StartListener(options);
    auto endpoint= listener->Status().endpoints[0];
    auto mock= password_mock("alice", "pw");
    guard->SetAuthorizer(mock);
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t= 0; t < 8; t++)
      threads.emplace_back([&] {
        RawClient raw(endpoint);
        while (!stop)
          raw.Login("alice:pw", 60);
      });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    listener->Stop();
    EXPECT(guard->SessionCount() == 0);
    stop= true;
    for (auto &t : threads)
      t.join();
    EXPECT(closed(*mock) == counter(*mock, &MockAuthorizer::allow_calls));
    EXPECT(guard->SessionCount() == 0);
    RawClient late(endpoint);
    EXPECT(late.Login("alice:pw", 60).empty());
  }
  guard->SetAuthorizer(nullptr);
}

static void test_parse_auth_string()
{
  quack_guard::ClientClaims c;
  EXPECT(quack_guard::ParseAuthString("bob:p:w", c));
  EXPECT(c.user == "bob" && c.proof == "p:w" && c.role.empty());
  EXPECT(quack_guard::ParseAuthString("bob/analyst:pw", c));
  EXPECT(c.user == "bob" && c.role == "analyst" && c.proof == "pw");
  EXPECT(quack_guard::ParseAuthString("a%3Ab%2Fc%25:pw", c));
  EXPECT(c.user == "a:b/c%" && c.role.empty() && c.proof == "pw");
  EXPECT(quack_guard::ParseAuthString("bob:", c));
  EXPECT(c.user == "bob" && c.proof.empty());
  EXPECT(quack_guard::ParseAuthString(":pw", c));  /* anonymous: authorizer decides */
  EXPECT(c.user.empty() && c.proof == "pw");
  EXPECT(!quack_guard::ParseAuthString("bob", c));
  EXPECT(!quack_guard::ParseAuthString("", c));
  EXPECT(!quack_guard::ParseAuthString("bob/:pw", c));
  EXPECT(!quack_guard::ParseAuthString("b%4:pw", c));
  EXPECT(!quack_guard::ParseAuthString("b%:pw", c));
  EXPECT(!quack_guard::ParseAuthString(std::string(513, 'u') + ":pw", c));
  EXPECT(quack_guard::ParseAuthString(std::string(512, 'u') + ":pw", c));
  EXPECT(!quack_guard::ParseAuthString("u/" + std::string(513, 'r') + ":pw", c));
  EXPECT(quack_guard::ParseAuthString("u/" + std::string(512, 'r') + ":pw", c));
  EXPECT(!quack_guard::ParseAuthString("u:" + std::string(1025, 'p'), c));
  EXPECT(quack_guard::ParseAuthString("u:" + std::string(1024, 'p'), c));
  /* Limits apply after unescaping */
  std::string escaped;
  for (int i= 0; i < 512; i++)
    escaped+= "%41";
  EXPECT(quack_guard::ParseAuthString(escaped + ":pw", c));
  EXPECT(c.user == std::string(512, 'A'));
  EXPECT(!quack_guard::ParseAuthString("root%00x:pw", c));
  EXPECT(quack_guard::ParseAuthString("r%C3%B6%FF:pw", c));
  EXPECT(c.user == "r\xC3\xB6\xFF");
  EXPECT(quack_guard::ParseAuthString("/role:pw", c));
  EXPECT(c.user.empty() && c.role == "role");
  EXPECT(quack_guard::ParseAuthString("bob:p/w", c));
  EXPECT(c.user == "bob" && c.role.empty() && c.proof == "p/w");
  EXPECT(quack_guard::ParseAuthString("bob/a/b:pw", c));
  EXPECT(c.user == "bob" && c.role == "a/b");
}

static void test_canonical_peer()
{
  EXPECT(quack_guard::CanonicalPeer("127.0.0.1") == "127.0.0.1");
  EXPECT(quack_guard::CanonicalPeer("::ffff:10.0.0.5") == "10.0.0.5");
  EXPECT(quack_guard::CanonicalPeer("::1") == "::1");
  EXPECT(quack_guard::CanonicalPeer("fe80::1%eth0") == "fe80::1");
  EXPECT(quack_guard::CanonicalPeer("0:0:0:0:0:0:0:1") == "::1");
  EXPECT(quack_guard::CanonicalPeer("").empty());
  EXPECT(quack_guard::CanonicalPeer("localhost").empty());
  EXPECT(quack_guard::CanonicalPeer("::ffff:7f00:1") == "127.0.0.1");
  EXPECT(quack_guard::CanonicalPeer("%eth0").empty());
  EXPECT(quack_guard::CanonicalPeer("127.0.0.1%x") == "127.0.0.1");
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
  test_parse_auth_string();
  test_canonical_peer();
  test_listener_lifecycle(guard);
  test_listener_login(db, guard);
  test_listener_sessions(guard);
  test_listener_lazy_expiry(guard);
  test_listener_handle_destroyed(guard);
  test_listener_authorizer_failures(guard);
  test_listener_limits(guard);
  test_listener_concurrency(guard);
  if (failures)
  {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("quack_guard_test: all passed\n");
  return 0;
}
