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

/* TLS transport helpers for the listener: thread pool, PEM, addresses. */

#pragma once

#include "duckdb/common/error_data.hpp"
#include "quack_server.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace quack_guard
{

/* httplib task queue: one thread per client connection, shared cap over
   all listeners; beyond the cap a connection is closed at once. */
class CappedPool : public duckdb_httplib_openssl::TaskQueue
{
public:
  CappedPool(std::shared_ptr<std::atomic<size_t>> live_p, size_t cap_p,
             std::shared_ptr<std::atomic<uint64_t>> refused_p);
  ~CappedPool() override;
  bool enqueue(std::function<void()> fn) override;
  void shutdown() override;

private:
  void JoinFinished();

  std::shared_ptr<std::atomic<size_t>> live;
  const size_t cap;
  std::shared_ptr<std::atomic<uint64_t>> refused;
  std::mutex lock;
  bool shutting_down= false;
  std::vector<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>>
      workers;
};

/* Whole file; throws with the OS error. */
std::string ReadFile(const std::string &path, const char *what);
/* Text of the last OpenSSL error. */
std::string SslError();
/* SHA-256 fingerprint of the first certificate in `pem`. */
std::string CertificateFingerprint(const std::string &pem);
/* "::1" or "[::1]" -> bind host "::1", shown "[::1]"; IPv4 and names as is */
void SplitAddress(const std::string &address, std::string &host,
                  std::string &shown);
/* shutdown() every accepted TCP socket of this process whose local port is
   one of `ports` (the listener's own client connections). */
void ShutdownAcceptedSockets(const std::vector<uint16_t> &ports);
/* Throws if `host` is neither a numeric address nor a resolvable name. */
void CheckResolvable(const std::string &host);

} /* namespace quack_guard */
