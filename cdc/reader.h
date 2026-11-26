#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace myduck {
namespace cdc {

class ReaderError : public std::runtime_error {
 public:
  ReaderError(const std::string &message, unsigned error_code, bool can_retry)
      : std::runtime_error(message), code(error_code), retryable(can_retry) {}

  const unsigned code;
  const bool retryable;
};

struct ReaderConfig {
  std::string host;
  std::string user;
  std::string password;
  unsigned port = 3306;
  unsigned server_id = 0x4455434b;
  std::string tls_ca;
  std::string tls_capath;
  std::string tls_cert;
  std::string tls_key;
};

struct RawEvent {
  std::string bytes;
  std::string file;
  uint64_t next_position = 0;
};

class Reader {
 public:
  explicit Reader(const ReaderConfig &config);
  ~Reader();
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;

  void Open(const std::string &file, uint64_t pos);
  bool Next(RawEvent &event);
  void Close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
}
