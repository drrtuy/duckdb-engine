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

#include "guarded_server.hpp"
#include "allowlist.hpp"
#include "login.hpp"
#include "transport.hpp"

#include "quack_message.hpp"
#include "quack_random.hpp"
#include "quack_ssl_key_generator.hpp"

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/main/connection.hpp"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>

namespace quack_guard
{

namespace http= duckdb_httplib_openssl;
using duckdb::ErrorResponse;
using duckdb::MessageType;
using duckdb::QuackMessage;

/* One answer for every login failure: the client learns nothing about which
   part was wrong. */
static const char *const LOGIN_FAILED= "Authentication failed";
static const char *const BAD_SESSION= "Invalid connection id";
static const char *const TOO_MANY= "Too many Quack sessions";

/* ---- login state of the request being handled on this thread ---- */

struct LoginContext
{
  std::string peer;
  std::shared_ptr<const Identity> identity;
  std::shared_ptr<QuackAuthorizer> issuer;
  bool attached= false;
};

static thread_local LoginContext *tl_login= nullptr;

/* Publishes a LoginContext to this thread's hooks for one request, and on
   every exit wipes the request buffer and releases an identity that did not
   become a session. */
class LoginScope
{
public:
  LoginScope(LoginContext &login_p, duckdb::MemoryStream &body_p)
      : login(login_p), body(body_p)
  {
    tl_login= &login;
  }
  ~LoginScope()
  {
    tl_login= nullptr;
    std::memset(body.GetData(), 0, body.GetPosition());
    if (login.identity && !login.attached)
      NotifyClosed(login.issuer, *login.identity);
  }

private:
  LoginContext &login;
  duckdb::MemoryStream &body;
};

/* ---- one TLS listening socket ---- */

class GuardedQuackServer::Transport
{
public:
  std::unique_ptr<http::SSLServer> server;
  std::thread thread;
  uint16_t port= 0;
  /* Set when listen_after_bind() returned: httplib then closed the socket
     itself, and stop() would close the stale descriptor number again. */
  std::atomic<bool> exited{false};

  void Stop()
  {
    if (!exited)
      server->stop();
  }
};

GuardedQuackServer::GuardedQuackServer(duckdb::ClientContext &context,
                                       std::shared_ptr<GuardImpl> guard_p,
                                       const ListenerOptions &options_p)
    : QuackServer(context, duckdb::QuackUri("quack:localhost"),
                  /* never checked: login goes through AuthenticateConnection */
                  duckdb::QuackRandomToken(*context.db)),
      guard(std::move(guard_p)), options(options_p)
{
  if (options.addresses.empty())
    throw duckdb::InvalidInputException("Quack: no listen address");
  if (options.max_transaction_seconds == 0 ||
      options.max_transaction_seconds > 7 * 24 * 3600)
    throw duckdb::InvalidInputException(
        "Quack: max_transaction_seconds must be 1..604800");
  if (options.max_heartbeat_seconds == 0 ||
      options.max_heartbeat_seconds > 7 * 24 * 3600)
    throw duckdb::InvalidInputException(
        "Quack: max_heartbeat_seconds must be 1..604800");
  if (options.cert_file.empty() || options.key_file.empty())
    throw duckdb::InvalidInputException(
        "Quack: a TLS certificate and key are required");
  std::string cert_pem= ReadFile(options.cert_file, "certificate");
  std::string key_pem= ReadFile(options.key_file, "private key");
  ssl_fingerprint= CertificateFingerprint(cert_pem);
  try
  {
    int port= options.port;
    for (const auto &address : options.addresses)
    {
      std::string host, shown;
      SplitAddress(address, host, shown);
      CheckResolvable(host);
      http::SSLServer::PemMemory pem{cert_pem.data(), cert_pem.size(),
                                     key_pem.data(), key_pem.size(),
                                     nullptr, 0,
                                     /* never prompt for a passphrase */ ""};
      auto transport= std::make_unique<Transport>();
      transport->server= std::make_unique<http::SSLServer>(pem);
      auto &server= *transport->server;
      if (!server.is_valid())
        throw duckdb::IOException("Quack: cannot set up TLS from %s and %s: %s",
                                  options.cert_file, options.key_file,
                                  SslError());
      server.set_socket_options([](socket_t sock) {
        int on= 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
      });
      server.set_ipv6_v6only(true);
      auto cap= options.max_connections;
      auto live_count= live;
      server.new_task_queue= [live_count, cap] {
        return new CappedPool(live_count, cap);
      };
      server.set_payload_max_length(options.max_request_bytes
                                        ? options.max_request_bytes
                                        : 1);
      server.set_keep_alive_timeout(30);
      server.set_keep_alive_max_count(1 << 20);
      server.set_tcp_nodelay(true);
      /* While stopping, every answer ends its connection */
      server.set_post_routing_handler(
          [this](const http::Request &, http::Response &res) {
            if (closing)
              res.set_header("Connection", "close");
          });
      server.Post("/quack", [this](const http::Request &req,
                                   http::Response &res,
                                   const http::ContentReader &reader) {
        duckdb::MemoryStream body;
        bool complete= reader([&](const char *data, size_t length) {
          body.WriteData((duckdb::data_ptr_t) data, length);
          return true;
        });
        if (!complete)
        {
          std::memset(body.GetData(), 0, body.GetPosition());
          res.status= 400;
          return;
        }
        std::string content, content_type;
        std::shared_ptr<QuackMessage> raw;
        HandlePost(req.remote_addr, body, content, content_type, raw);
        if (raw)
        {
          auto payload= raw->RawPayload();
          auto start= raw->RawPayloadStart();
          auto data= duckdb::const_char_ptr_cast(payload->GetData()) + start;
          auto size= payload->GetPosition() - start;
          res.set_content_provider(
              size, content_type,
              [raw, data](size_t offset, size_t length, http::DataSink &sink) {
                sink.write(data + offset, length);
                return true;
              });
        }
        else
          res.set_content(content, content_type);
      });

      errno= 0;
      int bound= port;
      if (port == 0)
        bound= server.bind_to_any_port(host);
      else if (!server.bind_to_port(host, port))
        bound= -1;
      if (bound < 0)
        throw duckdb::IOException("Quack: cannot listen on %s port %d: %s",
                                  address, port,
                                  errno ? std::strerror(errno) : "bind failed");
      port= bound;  /* other addresses reuse the port picked for the first */
      transport->port= (uint16_t) bound;
      endpoints.push_back(shown + ":" + std::to_string(bound));
      transports.push_back(std::move(transport));
    }

    for (auto &transport : transports)
    {
      auto *server= transport->server.get();
      auto *exited= &transport->exited;
      transport->thread= std::thread([this, server, exited] {
        try
        {
          if (!server->listen_after_bind() && !closing)
          {
            std::lock_guard<std::mutex> guard_failure(failure_lock);
            failure= "a Quack listener stopped accepting connections";
          }
        }
        catch (std::exception &ex)
        {
          std::lock_guard<std::mutex> guard_failure(failure_lock);
          failure= ex.what();
        }
        catch (...)
        {
          std::lock_guard<std::mutex> guard_failure(failure_lock);
          failure= "a Quack listener failed";
        }
        *exited= true;
      });
    }
    reaper= std::thread([this] { ReapLoop(); });
  }
  catch (...)
  {
    Wipe(key_pem);
    Teardown();
    throw;
  }
  Wipe(key_pem);
}

GuardedQuackServer::~GuardedQuackServer()
{
  try
  {
    Close();
  }
  catch (...)
  {
  }
}

void GuardedQuackServer::Teardown()
{
  closing= true;
  for (auto &transport : transports)
    transport->Stop();
  for (auto &transport : transports)
    if (transport->thread.joinable())
      transport->thread.join();
}

void GuardedQuackServer::Close()
{
  {
    std::lock_guard<std::mutex> guard_close(close_lock);
    if (closed)
      return;
    closed= true;
  }
  closing= true;  /* AuthenticateConnection refuses from now on */
  {
    std::lock_guard<std::mutex> guard_reaper(reaper_lock);
    reaper_wake.notify_all();
  }
  if (reaper.joinable())
    reaper.join();
  try
  {
    std::vector<uint16_t> ports;
    for (auto &transport : transports)
    {
      transport->Stop();
      ports.push_back(transport->port);
    }
    /* Interrupt and abort every session, and keep them, so a statement that
       starts after its first abort is aborted again until the handlers are
       gone. Client connections, logged in or not, are shut down so no
       handler waits on a slow client. */
    std::vector<duckdb::shared_ptr<duckdb::QuackConnection>> evicted;
    auto sweep= [&] {
      for (auto &id : guard->Sessions().Ids())
        if (auto connection= Evict(id))
          evicted.push_back(std::move(connection));
      for (auto &connection : evicted)
        CleanupExpiredConnection(*connection);
      ShutdownAcceptedSockets(ports);
    };
    sweep();
    while (live->load() > 0)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      sweep();
    }
    for (auto &transport : transports)
      if (transport->thread.joinable())
        transport->thread.join();
    sweep();
  }
  catch (...)
  {
    guard->ListenerStopped();
    throw;
  }
  guard->ListenerStopped();
}

ListenerStatus GuardedQuackServer::Status() const
{
  ListenerStatus status;
  {
    std::lock_guard<std::mutex> guard_failure(failure_lock);
    status.failure= failure;
  }
  status.running= !closing && status.failure.empty();
  if (!closing)
  {
    status.endpoints= endpoints;
    status.fingerprint= ssl_fingerprint;
  }
  return status;
}

void GuardedQuackServer::HandlePost(const std::string &raw_peer,
                                    duckdb::MemoryStream &body,
                                    std::string &content,
                                    std::string &content_type,
                                    std::shared_ptr<QuackMessage> &raw)
{
  content_type= "application/vnd.duckdb";
  duckdb::unique_ptr<QuackMessage> response;
  try
  {
    response= Dispatch(body, CanonicalPeer(raw_peer));
  }
  catch (std::exception &ex)
  {
    response= duckdb::make_uniq<ErrorResponse>(duckdb::ErrorData(ex));
  }
  catch (...)
  {
    response= duckdb::make_uniq<ErrorResponse>("Unknown error");
  }
  if (response->RawPayload())
  {
    raw= std::shared_ptr<QuackMessage>(std::move(response));
    return;
  }
  duckdb::MemoryStream out;
  response->ToMemoryStream(out);
  content.assign((const char *) out.GetData(), out.GetPosition());
}

duckdb::unique_ptr<QuackMessage>
GuardedQuackServer::Dispatch(duckdb::MemoryStream &body,
                             const std::string &peer)
{
  if (peer.empty())
    return duckdb::make_uniq<ErrorResponse>(BAD_SESSION);
  body.Rewind();
  duckdb::BinaryDeserializer deserializer(body);
  auto header= QuackMessage::DeserializeHeader(deserializer);

  if (header.type == MessageType::CONNECTION_REQUEST)
  {
    LoginContext login;
    login.peer= peer;
    LoginScope scope(login, body);
    return HandleMessage(body);
  }

  /* Any other message: only for a session we admitted, from its peer */
  SessionRegistry::Entry entry;
  if (!guard->Sessions().Find(header.connection_id, entry) ||
      entry.peer_ip != peer)
    return duckdb::make_uniq<ErrorResponse>(BAD_SESSION);
  auto response= HandleMessage(body);
  /* Only errors that cannot tell about objects the client may not see reach
     it as they are: the gates' own answers, and execution errors, which
     only happen once access was granted. Binder, catalog, I/O, extension
     and anything new get one answer ("did you mean ...", paths, ...). */
  if (response->Type() == MessageType::ERROR_RESPONSE)
  {
    /* Our own answers pass as they are. Of the others, only execution
       errors pass, and only once the plan gate granted the statement:
       several of them are also thrown while binding, before authorization. */
    auto &error= response->Cast<ErrorResponse>().Error();
    if (error.Type() == duckdb::ExceptionType::PERMISSION &&
        (error.RawMessage() == STATEMENT_DENIED ||
         error.RawMessage() == ACCESS_DENIED))
      return response;
    bool authorized= false;
    if (auto connection= GetConnection(header.connection_id))
      if (connection->duckdb_connection)
        if (auto state= GetPrincipalState(*connection->duckdb_connection->context))
          authorized= state->authorized;
    switch (authorized ? error.Type() : duckdb::ExceptionType::INVALID)
    {
    case duckdb::ExceptionType::CONVERSION:
    case duckdb::ExceptionType::OUT_OF_RANGE:
    case duckdb::ExceptionType::DIVIDE_BY_ZERO:
    case duckdb::ExceptionType::INVALID_INPUT:
    case duckdb::ExceptionType::INTERRUPT:
    case duckdb::ExceptionType::OUT_OF_MEMORY:
      break;
    default:
      return duckdb::make_uniq<ErrorResponse>(
          duckdb::ErrorData(duckdb::ExceptionType::PERMISSION, ACCESS_DENIED));
    }
  }
  return response;
}

duckdb::unique_ptr<QuackMessage> GuardedQuackServer::AuthorizeStatement(
    duckdb::DatabaseInstance &, duckdb::QuackConnection &connection,
    const std::string &sql, std::string &effective_sql)
{
  /* The client's own catalog queries (ATTACH): answered filtered */
  if (CatalogQuery(sql, effective_sql))
    return nullptr;
  std::string why;
  auto decision=
      connection.duckdb_connection
          ? StatementAllowed(*connection.duckdb_connection->context, sql, why)
          : GateDecision::StatementDenied;
  if (decision != GateDecision::Allowed)
    return duckdb::make_uniq<ErrorResponse>(duckdb::ErrorData(
        duckdb::ExceptionType::PERMISSION,
        decision == GateDecision::AccessDenied ? ACCESS_DENIED
                                               : STATEMENT_DENIED));
  effective_sql= sql;
  return nullptr;
}

duckdb::unique_ptr<QuackMessage> GuardedQuackServer::AuthenticateConnection(
    duckdb::DatabaseInstance &, duckdb::ConnectionRequestMessage &request,
    const std::string &)
{
  auto *login= tl_login;
  if (!login || closing)
    return duckdb::make_uniq<ErrorResponse>(LOGIN_FAILED);
  if (request.HeartbeatTimeoutSeconds() > options.max_heartbeat_seconds)
    return duckdb::make_uniq<ErrorResponse>(
        "heartbeat_timeout out of range - must be between 1 and %llu seconds",
        (unsigned long long) options.max_heartbeat_seconds);
  /* Checked again, atomically, when the session is registered */
  if (guard->Sessions().Count() >= options.max_sessions)
    return duckdb::make_uniq<ErrorResponse>(TOO_MANY);
  auto authorizer= guard->GetAuthorizer();
  if (!authorizer)
    return duckdb::make_uniq<ErrorResponse>(LOGIN_FAILED);

  ClientClaims claims;
  std::string auth= request.AuthString();
  request.WipeAuthString();
  bool parsed= ParseAuthString(auth, claims);
  Wipe(auth);
  if (!parsed)
  {
    Wipe(claims.proof);
    return duckdb::make_uniq<ErrorResponse>(LOGIN_FAILED);
  }
  TransportFacts transport;
  transport.peer_ip= login->peer;
  transport.tls= true;

  std::shared_ptr<const Identity> identity;
  Verdict verdict;
  try
  {
    verdict= authorizer->authenticate(claims, transport, identity);
  }
  catch (...)
  {
    verdict= Verdict::Deny(Reason::Internal);
  }
  Wipe(claims.proof);
  if (!verdict.allow || !identity)
  {
    if (identity)
      NotifyClosed(authorizer, *identity);
    return duckdb::make_uniq<ErrorResponse>(LOGIN_FAILED);
  }
  login->identity= std::move(identity);
  login->issuer= std::move(authorizer);
  return nullptr;
}

void GuardedQuackServer::OnConnectionCreated(duckdb::QuackConnection &connection)
{
  auto *login= tl_login;
  if (!login || !login->identity || !connection.duckdb_connection)
    throw duckdb::InternalException("Quack: session without a login");
  auto &context= *connection.duckdb_connection->context;
  AttachPrincipal(context, login->identity);
  HardenSession(context);
  if (!guard->Sessions().Add(
          connection.connection_id,
          SessionRegistry::Entry{login->peer, login->identity, login->issuer},
          options.max_sessions))
    throw duckdb::InvalidInputException(TOO_MANY);
  login->attached= true;
}

void GuardedQuackServer::OnConnectionClosed(duckdb::QuackConnection &connection)
{
  guard->Sessions().Remove(connection.connection_id);
}

duckdb::shared_ptr<duckdb::QuackConnection>
GuardedQuackServer::Evict(const std::string &connection_id)
{
  auto connection= GetConnection(connection_id);
  if (!DisconnectConnection(connection_id))
    guard->Sessions().Remove(connection_id);  /* Quack had dropped it */
  /* Interrupt, and wake handlers parked on the session's streams */
  if (connection)
    CleanupExpiredConnection(*connection);
  return connection;
}

void GuardedQuackServer::ReapExpired()
{
  auto now= std::chrono::steady_clock::now();
  /* A transaction keeps one snapshot of the shared database open: bounded.
     Observed from the session's real state, not predicted from statements
     (those can fail, be interrupted or run in another order). */
  std::vector<std::string> too_long;
  {
    std::lock_guard<std::mutex> guard_tx(transactions_lock);
    std::map<std::string, std::chrono::steady_clock::time_point> still_open;
    for (auto &id : guard->Sessions().Ids())
    {
      auto connection= GetConnection(id);
      if (!connection || !connection->duckdb_connection ||
          connection->duckdb_connection->context->transaction.IsAutoCommit())
        continue;
      auto seen= transactions.find(id);
      auto since= seen == transactions.end() ? now : seen->second;
      if (now - since > std::chrono::seconds(options.max_transaction_seconds))
        too_long.push_back(id);
      else
        still_open.emplace(id, since);
    }
    transactions.swap(still_open);
  }
  for (auto &id : too_long)
    Evict(id);
  for (auto &id : guard->Sessions().Ids())
  {
    auto connection= GetConnection(id);
    if (!connection)
    {
      guard->Sessions().Remove(id);
      continue;
    }
    bool expired;
    {
      duckdb::annotated_lock_guard<duckdb::annotated_mutex> lease(
          connection->lease_lock);
      expired= connection->LeaseExpiredLocked(now);
    }
    if (expired)
      Evict(id);
  }
}

void GuardedQuackServer::ReapLoop()
{
  std::unique_lock<std::mutex> lock(reaper_lock);
  while (!closing)
  {
    reaper_wake.wait_for(lock,
                         std::chrono::milliseconds(options.reap_interval_ms
                                                       ? options.reap_interval_ms
                                                       : 1000));
    if (closing)
      break;
    lock.unlock();
    try
    {
      ReapExpired();
    }
    catch (...)
    {
    }
    lock.lock();
  }
}

/* ---- GuardImpl's listener part ---- */

std::unique_ptr<Listener>
GuardImpl::StartListener(const ListenerOptions &options)
{
  if (listener_active.exchange(true))
    throw duckdb::InvalidInputException("Quack: a listener is already running");
  try
  {
    auto database= db.lock();
    if (!database)
      throw duckdb::InvalidInputException("Quack: the database is closed");
    duckdb::Connection con(*database);
    auto server= std::make_shared<GuardedQuackServer>(*con.context,
                                                      shared_from_this(),
                                                      options);
    return std::make_unique<ListenerHandle>(std::move(server));
  }
  catch (...)
  {
    listener_active= false;
    throw;
  }
}

} /* namespace quack_guard */
