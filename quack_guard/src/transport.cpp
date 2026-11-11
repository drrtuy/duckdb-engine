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

#include "transport.hpp"

#include "quack_ssl_key_generator.hpp"

#include "duckdb/common/exception.hpp"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <netdb.h>
#include <sstream>

namespace quack_guard
{

namespace http= duckdb_httplib_openssl;

CappedPool::CappedPool(std::shared_ptr<std::atomic<size_t>> live_p,
                       size_t cap_p)
    : live(std::move(live_p)), cap(cap_p ? cap_p : 1)
{
}

CappedPool::~CappedPool() { shutdown(); }

bool CappedPool::enqueue(std::function<void()> fn)
{
  std::lock_guard<std::mutex> guard(lock);
  JoinFinished();
  if (shutting_down)
    return false;
  if (live->fetch_add(1) >= cap)
  {
    live->fetch_sub(1);
    return false;  /* httplib closes the socket */
  }
  auto done= std::make_shared<std::atomic<bool>>(false);
  try
  {
    workers.reserve(workers.size() + 1);  /* no throw once the thread runs */
    workers.emplace_back(std::thread([fn, done, counter= live] {
                           try
                           {
                             fn();
                           }
                           catch (...)
                           {
                           }
                           counter->fetch_sub(1);
                           *done= true;
                         }),
                         done);
  }
  catch (...)
  {
    live->fetch_sub(1);
    return false;
  }
  return true;
}

void CappedPool::shutdown()
{
  std::vector<std::pair<std::thread, std::shared_ptr<std::atomic<bool>>>> all;
  {
    std::lock_guard<std::mutex> guard(lock);
    shutting_down= true;
    all.swap(workers);
  }
  for (auto &w : all)
    if (w.first.joinable())
      w.first.join();
}

void CappedPool::JoinFinished()
{
  for (auto it= workers.begin(); it != workers.end();)
  {
    if (*it->second)
    {
      if (it->first.joinable())
        it->first.join();
      it= workers.erase(it);
    }
    else
      ++it;
  }
}


std::string ReadFile(const std::string &path, const char *what)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw duckdb::IOException("Quack: cannot read the %s %s: %s", what, path,
                              std::strerror(errno));
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string SslError()
{
  auto code= ERR_get_error();
  if (!code)
    return "unknown TLS error";
  char text[256];
  ERR_error_string_n(code, text, sizeof(text));
  return text;
}

std::string CertificateFingerprint(const std::string &pem)
{
  BIO *bio= BIO_new_mem_buf(pem.data(), (int) pem.size());
  X509 *cert= bio ? PEM_read_bio_X509(bio, nullptr, nullptr, nullptr) : nullptr;
  if (bio)
    BIO_free(bio);
  if (!cert)
    throw duckdb::IOException("Quack: the certificate is not PEM: %s",
                              SslError());
  std::string fingerprint;
  try
  {
    fingerprint= duckdb::SslKeyGenerator::CertificateFingerprint(cert);
  }
  catch (...)
  {
    X509_free(cert);
    throw;
  }
  X509_free(cert);
  return fingerprint;
}

/* "::1" -> bind host "::1", shown as "[::1]"; "[::1]" likewise */
void SplitAddress(const std::string &address, std::string &host,
                  std::string &shown)
{
  if (address.size() > 2 && address.front() == '[' && address.back() == ']')
  {
    host= address.substr(1, address.size() - 2);
    shown= address;
  }
  else if (address.find(':') != std::string::npos)
  {
    host= address;
    shown= "[" + address + "]";
  }
  else
  {
    host= address;
    shown= address;
  }
}

void ShutdownAcceptedSockets(const std::vector<uint16_t> &ports)
{
  DIR *dir= opendir("/proc/self/fd");
  if (!dir)
    return;
  while (struct dirent *entry= readdir(dir))
  {
    char *end;
    long fd= strtol(entry->d_name, &end, 10);
    if (*end || fd < 0 || fd == dirfd(dir))
      continue;
    struct stat st;
    if (fstat((int) fd, &st) != 0 || !S_ISSOCK(st.st_mode))
      continue;
    int listening= 0;
    socklen_t len= sizeof(listening);
    if (getsockopt((int) fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &len) != 0 ||
        listening)
      continue;
    struct sockaddr_storage local;
    socklen_t local_len= sizeof(local);
    if (getsockname((int) fd, (struct sockaddr *) &local, &local_len) != 0)
      continue;
    uint16_t port;
    if (local.ss_family == AF_INET)
      port= ntohs(((struct sockaddr_in *) &local)->sin_port);
    else if (local.ss_family == AF_INET6)
      port= ntohs(((struct sockaddr_in6 *) &local)->sin6_port);
    else
      continue;
    for (auto p : ports)
      if (p == port)
      {
        shutdown((int) fd, SHUT_RDWR);
        break;
      }
  }
  closedir(dir);
}

void CheckResolvable(const std::string &host)
{
  unsigned char buf[16];
  if (inet_pton(AF_INET, host.c_str(), buf) == 1 ||
      inet_pton(AF_INET6, host.c_str(), buf) == 1)
    return;
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_socktype= SOCK_STREAM;
  struct addrinfo *result= nullptr;
  int rc= getaddrinfo(host.c_str(), nullptr, &hints, &result);
  if (result)
    freeaddrinfo(result);
  if (rc != 0)
    throw duckdb::IOException("Quack: cannot resolve %s: %s", host,
                              gai_strerror(rc));
}

} /* namespace quack_guard */
