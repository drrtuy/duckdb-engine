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
  The plugin's Quack listener: Quack's QuackServer behind our own TLS
  transport, with login decided by the guard's QuackAuthorizer through the
  hooks added by patches/duckdb-quack/0004. See Quack.MD, "Listener".
*/

#pragma once

#include "guard_internal.hpp"

#include "quack_server.hpp"

#include <atomic>
#include <condition_variable>
#include <thread>

namespace quack_guard
{

class GuardedQuackServer : public duckdb::QuackServer
{
public:
  /* Binds every address and starts the listener threads; throws on failure
     with nothing left listening. */
  GuardedQuackServer(duckdb::ClientContext &context,
                     std::shared_ptr<GuardImpl> guard,
                     const ListenerOptions &options);
  ~GuardedQuackServer() override;

  /* See Listener::Stop() */
  void Close() override;
  ListenerStatus Status() const;

protected:
  duckdb::unique_ptr<duckdb::QuackMessage>
  AuthenticateConnection(duckdb::DatabaseInstance &db,
                         duckdb::ConnectionRequestMessage &request,
                         const std::string &connection_id) override;
  void OnConnectionCreated(duckdb::QuackConnection &connection) override;
  void OnConnectionClosed(duckdb::QuackConnection &connection) override;

private:
  class Transport;

  void HandlePost(const std::string &raw_peer, duckdb::MemoryStream &body,
                  std::string &content, std::string &content_type,
                  std::shared_ptr<duckdb::QuackMessage> &raw_response);
  duckdb::unique_ptr<duckdb::QuackMessage>
  Dispatch(duckdb::MemoryStream &body, const std::string &peer);
  void ReapLoop();
  void ReapExpired();
  /* Drop the session from Quack and abort what it runs; returns it. */
  duckdb::shared_ptr<duckdb::QuackConnection>
  Evict(const std::string &connection_id);
  /* Undo a partly constructed server. */
  void Teardown();

  std::shared_ptr<GuardImpl> guard;
  ListenerOptions options;
  std::vector<std::unique_ptr<Transport>> transports;
  /* Client connections being served, over all transports */
  std::shared_ptr<std::atomic<size_t>> live=
      std::make_shared<std::atomic<size_t>>(0);
  std::vector<std::string> endpoints;
  std::atomic<bool> closing{false};
  std::mutex close_lock;
  bool closed= false;
  std::thread reaper;
  std::mutex reaper_lock;
  std::condition_variable reaper_wake;
  mutable std::mutex failure_lock;
  std::string failure;
};

/* The Listener handle given to the plugin. */
class ListenerHandle : public Listener
{
public:
  explicit ListenerHandle(std::shared_ptr<GuardedQuackServer> server_p)
      : server(std::move(server_p))
  {
  }
  ~ListenerHandle() override { Stop(); }
  ListenerStatus Status() const override { return server->Status(); }
  void Stop() override
  {
    try
    {
      server->Close();
    }
    catch (...)
    {
    }
  }

private:
  std::shared_ptr<GuardedQuackServer> server;
};

} /* namespace quack_guard */
