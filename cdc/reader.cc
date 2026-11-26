#include "reader.h"

#include <mysql.h>
#include <mariadb_rpl.h>
#include <errmsg.h>

#include <cerrno>
#include <limits>
#include <stdexcept>
#include <utility>

namespace myduck {
namespace cdc {
namespace {

struct Connector {
  decltype(&mysql_init) init = &mysql_init;
  decltype(&mysql_close) close = &mysql_close;
  decltype(&mysql_options) options = &mysql_options;
  decltype(&mysql_real_connect) connect = &mysql_real_connect;
  decltype(&mysql_errno) error_number = &mysql_errno;
  decltype(&mysql_get_ssl_cipher) ssl_cipher = &mysql_get_ssl_cipher;
  decltype(&mysql_query) query = &mysql_query;
  decltype(&mysql_store_result) store_result = &mysql_store_result;
  decltype(&mysql_fetch_row) fetch_row = &mysql_fetch_row;
  decltype(&mysql_free_result) free_result = &mysql_free_result;
  decltype(&mysql_num_fields) num_fields = &mysql_num_fields;
  decltype(&mariadb_rpl_init_ex) rpl_init = &mariadb_rpl_init_ex;
  decltype(&mariadb_rpl_optionsv) rpl_options = &mariadb_rpl_optionsv;
  decltype(&mariadb_rpl_open) rpl_open = &mariadb_rpl_open;
  decltype(&mariadb_rpl_close) rpl_close = &mariadb_rpl_close;
  decltype(&mariadb_rpl_fetch) rpl_fetch = &mariadb_rpl_fetch;
  decltype(&mariadb_rpl_errno) rpl_errno = &mariadb_rpl_errno;
  decltype(&mariadb_free_rpl_event) free_event = &mariadb_free_rpl_event;

  Connector() {
    if (mysql_server_init(0, nullptr, nullptr)) {
      throw std::runtime_error("CDC reader: Connector/C initialization failed");
    }
  }

  ~Connector() { mysql_server_end(); }

  Connector(const Connector &) = delete;
  Connector &operator=(const Connector &) = delete;
};

Connector &GetConnector() {
  static Connector connector;
  return connector;
}

void Fail(const char *operation, unsigned error) {
  const bool retryable = error == 2002 || error == 2003 || error == 2006 ||
                         error == 2013 || error == 2055;
  throw ReaderError(std::string("CDC reader: ") + operation +
                        " (error " + std::to_string(error) + ")",
                    error, retryable);
}

uint64_t LittleEndian(const unsigned char *data, unsigned length) {
  uint64_t result = 0;
  for (unsigned i = 0; i < length; ++i) {
    result |= uint64_t(data[i]) << (8 * i);
  }
  return result;
}

bool ValidFile(const std::string &file) {
  return !file.empty() && file.size() <= 512 &&
         file.find('\0') == std::string::npos;
}

RawEvent DecodeEvent(const MARIADB_RPL_EVENT &event,
                     const std::string &file, uint64_t position, bool checksum = false) {
  if (!event.raw_data || event.raw_data_ofs > event.raw_data_size ||
      event.raw_data_size - event.raw_data_ofs < 19) {
    throw std::runtime_error("CDC reader: invalid raw event bounds");
  }
  const auto *data = event.raw_data + event.raw_data_ofs;
  const auto length = event.raw_data_size - event.raw_data_ofs;
  if (LittleEndian(data + 9, 4) != length || event.event_length != length ||
      data[4] != static_cast<unsigned>(event.event_type) ||
      LittleEndian(data + 13, 4) != event.next_event_pos ||
      LittleEndian(data + 17, 2) != event.flags) {
    throw std::runtime_error("CDC reader: inconsistent raw event header");
  }
  if (event.event_type == UNKNOWN_EVENT ||
      event.event_type == START_ENCRYPTION_EVENT ||
      event.event_type == QUERY_COMPRESSED_EVENT ||
      (event.event_type >= WRITE_ROWS_COMPRESSED_EVENT_V1 &&
       event.event_type <= DELETE_ROWS_COMPRESSED_EVENT)) {
    throw std::runtime_error("CDC reader: unsupported binlog event format");
  }

  RawEvent result;
  result.bytes.assign(reinterpret_cast<const char *>(data), length);
  result.file = file;
  result.next_position = position;
  const bool synthetic = (event.flags & LOG_EVENT_ARTIFICIAL_F) ||
      (event.event_type != ROTATE_EVENT && event.next_event_pos == 0) ||
      (event.event_type == ROTATE_EVENT && LittleEndian(data, 4) == 0);
  if (event.event_type == HEARTBEAT_LOG_EVENT ||
      (synthetic && event.event_type != ROTATE_EVENT)) {
    return result;
  }
  if (event.event_type == ROTATE_EVENT) {
    size_t trailer = checksum ? 4 : 0;
    if (length <= 27 + trailer) {
      throw std::runtime_error("CDC reader: truncated rotate event");
    }
    auto next = LittleEndian(data + 19, 8);
    if (next < 4 || next > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("CDC reader: invalid rotate position");
    }
    std::string next_file(reinterpret_cast<const char *>(data + 27), length - 27 - trailer);
    if (!ValidFile(next_file)) {
      throw std::runtime_error("CDC reader: invalid rotate filename");
    }
    if (synthetic && next_file == file) {
      return result;
    }
    if (next_file == file && next < position) {
      throw std::runtime_error("CDC reader: binlog position reset");
    }
    result.file = std::move(next_file);
    result.next_position = next;
  } else {
    if (event.next_event_pos <= position) {
      throw std::runtime_error("CDC reader: nonadvancing binlog position");
    }
    result.next_position = event.next_event_pos;
  }
  return result;
}

}

struct Reader::Impl {
  explicit Impl(const ReaderConfig &value) : config(value) {}

  ReaderConfig config;
  Connector *connector = nullptr;
  MYSQL *mysql = nullptr;
  MARIADB_RPL *rpl = nullptr;
  std::string file;
  uint64_t position = 0;

  void Option(enum mysql_option option, const void *value) {
    if (connector->options(mysql, option, value)) {
      Fail("connection option failed", connector->error_number(mysql));
    }
  }

  void Close() {
    if (rpl) {
      connector->rpl_close(rpl);
      rpl = nullptr;
    }
    if (mysql) {
      connector->close(mysql);
      mysql = nullptr;
    }
  }
};

Reader::Reader(const ReaderConfig &config) : impl_(new Impl(config)) {
  if (config.host.empty() || config.user.empty() || !config.port ||
      config.port > 65535 || !config.server_id ||
      config.host.find('\0') != std::string::npos ||
      config.user.find('\0') != std::string::npos ||
      config.password.find('\0') != std::string::npos ||
      config.tls_ca.find('\0') != std::string::npos ||
      config.tls_capath.find('\0') != std::string::npos ||
      config.tls_cert.find('\0') != std::string::npos ||
      config.tls_key.find('\0') != std::string::npos) {
    throw std::runtime_error("CDC reader: invalid source configuration");
  }
}

Reader::~Reader() { Close(); }

void Reader::Open(const std::string &file, uint64_t pos) {
  Close();
  if (!ValidFile(file) || pos < 4 ||
      pos > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("CDC reader: invalid binlog coordinates");
  }
  auto &state = *impl_;
  state.connector = &GetConnector();
  auto &api = *state.connector;
  state.mysql = api.init(nullptr);
  if (!state.mysql) {
    throw std::runtime_error("CDC reader: connection allocation failed");
  }
  try {
    const unsigned timeout = 1;
    const unsigned protocol = MYSQL_PROTOCOL_TCP;
    const my_bool yes = 1;
    const my_bool no = 0;
    state.Option(MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    state.Option(MYSQL_OPT_READ_TIMEOUT, &timeout);
    state.Option(MYSQL_OPT_WRITE_TIMEOUT, &timeout);
    state.Option(MYSQL_OPT_PROTOCOL, &protocol);
    state.Option(MYSQL_OPT_RECONNECT, &no);
    const size_t packet_limit = 64ULL * 1024 * 1024;
    state.Option(MYSQL_OPT_MAX_ALLOWED_PACKET, &packet_limit);
    const unsigned local_infile = 0;
    state.Option(MYSQL_OPT_LOCAL_INFILE, &local_infile);
    state.Option(MARIADB_OPT_RESTRICTED_AUTH, "mysql_native_password");
    state.Option(MYSQL_OPT_SSL_ENFORCE, &yes);
    state.Option(MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &yes);
    const auto &config = state.config;
    if (!config.tls_ca.empty()) {
      state.Option(MYSQL_OPT_SSL_CA, config.tls_ca.c_str());
    }
    if (!config.tls_capath.empty()) {
      state.Option(MYSQL_OPT_SSL_CAPATH, config.tls_capath.c_str());
    }
    if (!config.tls_cert.empty()) {
      state.Option(MYSQL_OPT_SSL_CERT, config.tls_cert.c_str());
    }
    if (!config.tls_key.empty()) {
      state.Option(MYSQL_OPT_SSL_KEY, config.tls_key.c_str());
    }
    if (!api.connect(state.mysql, config.host.c_str(), config.user.c_str(),
                     config.password.c_str(), nullptr, config.port, nullptr, 0)) {
      Fail("upstream connection failed", api.error_number(state.mysql));
    }
    if (!api.ssl_cipher(state.mysql)) {
      throw std::runtime_error("CDC reader: upstream connection requires TLS");
    }
    if (api.query(state.mysql, "SELECT @@global.binlog_format,@@global.binlog_row_image,@@global.binlog_checksum")) {
      Fail("source settings check failed", api.error_number(state.mysql));
    }
    {
      std::unique_ptr<MYSQL_RES, decltype(api.free_result)> settings(
          api.store_result(state.mysql), api.free_result);
      auto row = settings && api.num_fields(settings.get()) == 3 ? api.fetch_row(settings.get()) : nullptr;
      if (!row || !row[0] || !row[1] || !row[2] || std::string(row[0]) != "ROW" ||
          std::string(row[1]) != "FULL" || std::string(row[2]) != "CRC32") {
        throw std::runtime_error("CDC reader: source requires ROW, FULL and CRC32");
      }
    }
    if (api.query(state.mysql, "SET @mariadb_slave_capability=4")) {
      Fail("capability negotiation failed", api.error_number(state.mysql));
    }
    if (api.query(state.mysql, "SET @master_binlog_checksum=@@global.binlog_checksum")) {
      Fail("checksum negotiation failed", api.error_number(state.mysql));
    }
    state.rpl = api.rpl_init(state.mysql, MARIADB_RPL_VERSION);
    if (!state.rpl) {
      throw std::runtime_error("CDC reader: replication allocation failed");
    }
    if (api.rpl_options(state.rpl, MARIADB_RPL_FILENAME,
                         const_cast<char *>(file.c_str()), file.size()) ||
        api.rpl_options(state.rpl, MARIADB_RPL_START, static_cast<unsigned long>(pos)) ||
        api.rpl_options(state.rpl, MARIADB_RPL_SERVER_ID, config.server_id) ||
        api.rpl_options(state.rpl, MARIADB_RPL_FLAGS,
                         static_cast<unsigned>(MARIADB_RPL_BINLOG_DUMP_NON_BLOCK)) ||
        api.rpl_options(state.rpl, MARIADB_RPL_VERIFY_CHECKSUM, uint32_t(1)) ||
        api.rpl_options(state.rpl, MARIADB_RPL_UNCOMPRESS, uint32_t(0)) ||
        api.rpl_open(state.rpl)) {
      Fail("replication open failed", api.rpl_errno(state.rpl));
    }
    state.file = file;
    state.position = pos;
  } catch (...) {
    Close();
    throw;
  }
}

bool Reader::Next(RawEvent &event) {
  auto &state = *impl_;
  if (!state.rpl) {
    throw std::runtime_error("CDC reader: stream is not open");
  }
  auto &api = *state.connector;
  errno = 0;
  auto *raw = api.rpl_fetch(state.rpl, nullptr);
  const int fetch_errno = errno;
  if (!raw) {
    const unsigned rpl_error = api.rpl_errno(state.rpl);
    const unsigned connection_error = api.error_number(state.mysql);
    Close();
    if ((!rpl_error && !connection_error) ||
        (!rpl_error && connection_error == CR_SERVER_LOST && fetch_errno == ETIMEDOUT)) {
      return false;
    }
    Fail("replication read failed", rpl_error ? rpl_error : connection_error);
  }
  std::unique_ptr<MARIADB_RPL_EVENT, decltype(api.free_event)> owned(raw, api.free_event);
  try {
    auto decoded = DecodeEvent(*raw, state.file, state.position,
                               state.rpl->use_checksum || state.rpl->artificial_checksum);
    state.file = decoded.file;
    state.position = decoded.next_position;
    event = std::move(decoded);
    return true;
  } catch (...) {
    owned.reset();
    Close();
    throw;
  }
}

void Reader::Close() { impl_->Close(); }

}
}
