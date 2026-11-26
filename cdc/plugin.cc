#define MYSQL_SERVER 1
#include <my_global.h>
#include <mysql/plugin.h>
#include "mysqld.h"
#include "sql_class.h"
#include "sql_plugin.h"
#include "log.h"
#undef UNKNOWN
#include "duckdb_manager.h"
#include "plugin.h"
#include "reader.h"
#include "spool.h"
#include "events.h"
#include "applier.h"
#include "metrics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace myduck
{
namespace cdc
{

static my_bool enabled= false;
static char *source_host= nullptr;
static char *source_user= nullptr;
static char *source_password= nullptr;
static char *source_tls_ca= nullptr;
static char *source_tls_cert= nullptr;
static char *source_tls_key= nullptr;
static char *source_file= nullptr;
static char *database= nullptr;
static char *stream= nullptr;
static char *spool_directory= nullptr;
static unsigned int source_port= 3306;
static unsigned int server_id= 0;
static ulonglong source_position= 4;
static ulonglong batch_bytes= 4 * 1024 * 1024;
static ulonglong batch_timeout_ms= 100;
static ulonglong spool_bytes= 1024ULL * 1024 * 1024;
static ulonglong spool_group_bytes= 4 * 1024 * 1024;
static ulonglong spool_group_timeout_ms= 1000;
static constexpr size_t max_transaction= 64 * 1024 * 1024;

struct Statistics
{
  std::string state= "STOPPED";
  std::string error;
  ulonglong received= 0;
  ulonglong applied= 0;
  ulonglong retained= 0;
  ulonglong batches= 0;
  std::string received_file;
  std::string applied_file;
  std::string apply_stage= "STOPPED";
  ulonglong received_position= 0;
  ulonglong applied_position= 0;
  ulonglong pending_records= 0;
  ulonglong pending_bytes= 0;
  ulonglong oldest_pending_timestamp= 0;
  ulonglong received_commit_timestamp= 0;
  ulonglong applied_commit_timestamp= 0;
  ulonglong source_connected= 0;
  ulonglong metrics_ready= 0;
  ulonglong batch_records= 0;
  ulonglong batch_payload_bytes= 0;
  ulonglong last_apply_us= 0;
  ulonglong apply_us= 0;
  ulonglong apply_calls= 0;
  ulonglong last_injection_us= 0;
  ulonglong last_checkpoint_us= 0;
  ulonglong last_commit_us= 0;
  ulonglong last_ddl_us= 0;
  ulonglong buffered_records= 0;
  ulonglong buffered_bytes= 0;
  ulonglong spool_groups= 0;
  ulonglong last_spool_group_records= 0;
  ulonglong last_spool_group_bytes= 0;
  ulonglong last_spool_group_us= 0;
};

static Statistics statistics;
static std::mutex lock;
static std::condition_variable wake;
static std::thread worker;
static std::atomic<bool> terminating{false};
static std::atomic<bool> requested{false};
static std::atomic<uint64_t> generation{0};
static plugin_ref engine= nullptr;

static std::string Value(const char *s)
{
  return s ? s : "";
}

static void State(const std::string &value)
{
  std::lock_guard<std::mutex> guard(lock);
  statistics.state= value;
  if (value == "STOPPED")
  {
    statistics.source_connected= 0;
    statistics.apply_stage= "STOPPED";
  }
}

static bool Active()
{
  return requested.load() && !terminating.load();
}

static void Pause(unsigned milliseconds)
{
  std::unique_lock<std::mutex> guard(lock);
  wake.wait_for(guard, std::chrono::milliseconds(milliseconds));
}

static bool ServerReady()
{
  mysql_mutex_lock(&LOCK_server_started);
  bool ready= mysqld_server_started != 0;
  mysql_mutex_unlock(&LOCK_server_started);
  return ready && DuckdbManager::IsInitialized();
}

static void PendingSnapshot(const PendingMetrics &pending)
{
  statistics.pending_records= pending.Records();
  statistics.pending_bytes= pending.Bytes();
  statistics.oldest_pending_timestamp= pending.Oldest();
}

static void Run()
{
  {
    std::lock_guard<std::mutex> guard(lock);
    statistics.metrics_ready= 0;
    statistics.source_connected= 0;
    statistics.state= "RECOVERING";
    statistics.apply_stage= "RECOVERING";
  }
  if (Value(source_host).empty() || Value(source_user).empty() ||
      Value(source_file).empty() || Value(database).empty())
    throw std::runtime_error("CDC: source_host, source_user, source_file and database are required");
  if (!server_id)
    throw std::runtime_error("CDC: configure a unique nonzero server_id for this reader");
  std::string name= Value(stream);
  std::string identity= Value(source_host) + ":" + std::to_string(source_port) + "/" +
                        Value(database) + "/" + Value(source_file) + ":" +
                        std::to_string(source_position);
  Applier applier(Value(database), name, identity, Value(source_file), source_position);
  auto checkpoint= applier.Current();
  std::string directory= Value(spool_directory);
  if (directory.empty())
    directory= std::string(mysql_real_data_home) + "/duckdb_cdc";
  Spool spool(directory, identity + "/" + name, checkpoint.sequence + 1, spool_bytes);
  auto last= spool.Last();
  if (last.sequence && last.sequence < checkpoint.sequence)
    throw std::runtime_error("CDC: spool precedes applied checkpoint; restore the matching spool");
  if (last.sequence > checkpoint.sequence)
  {
    auto pending= spool.ReadAfter(checkpoint.sequence, 1);
    if (pending.empty() || pending.front().sequence != checkpoint.sequence + 1)
      throw std::runtime_error("CDC: gap between spool and checkpoint");
  }
  PendingMetrics pending_metrics;
  uint64_t recovered_commit= 0;
  for (uint64_t sequence= checkpoint.sequence; sequence < last.sequence;)
  {
    auto records= spool.ReadAfter(sequence, batch_bytes);
    if (records.empty())
      throw std::runtime_error("CDC: missing replay backlog");
    for (const auto &record : records)
    {
      auto times= PayloadTimes(record.payload);
      pending_metrics.Add(record.sequence, record.payload.size(), times);
      if (times.commit)
        recovered_commit= times.commit;
      sequence= record.sequence;
    }
  }
  ReaderConfig config;
  config.host= Value(source_host);
  config.port= source_port;
  config.user= Value(source_user);
  config.password= Value(source_password);
  config.server_id= server_id;
  config.tls_ca= Value(source_tls_ca);
  config.tls_cert= Value(source_tls_cert);
  config.tls_key= Value(source_tls_key);
  std::atomic<bool> reading{true};
  std::string reader_error;
  std::mutex reader_lock;
  std::mutex publication_lock;
  auto retained= spool.Bytes();
  uint64_t reader_retained= retained;
  const uint64_t reader_reserve= max_transaction + spool_group_bytes + 1024 * 1024;
  auto backpressure= [&] {
    if (reader_retained + reader_reserve >= spool_bytes)
      reader_retained= spool.Bytes();
    return reader_retained + reader_reserve >= spool_bytes;
  };
  {
    std::lock_guard<std::mutex> guard(lock);
    statistics.received= last.sequence ? last.sequence : checkpoint.sequence;
    statistics.applied= checkpoint.sequence;
    statistics.received_file= last.sequence ? last.source_file : checkpoint.file;
    statistics.received_position= last.sequence ? last.source_position : checkpoint.position;
    statistics.applied_file= checkpoint.file;
    statistics.applied_position= checkpoint.position;
    statistics.received_commit_timestamp= recovered_commit;
    statistics.applied_commit_timestamp= 0;
    PendingSnapshot(pending_metrics);
    statistics.metrics_ready= 1;
    statistics.apply_stage= "IDLE";
    statistics.retained= retained;
    statistics.buffered_records= 0;
    statistics.buffered_bytes= 0;
    statistics.error.clear();
    statistics.state= "CONNECTING";
  }
  std::vector<SpoolRecord> receive_group;
  uint64_t receive_group_bytes= 0;
  auto group_started= std::chrono::steady_clock::now();
  auto flush_group= [&] {
    if (receive_group.empty()) return;
    std::vector<EventTimes> times;
    times.reserve(receive_group.size());
    for (const auto &record : receive_group) times.push_back(PayloadTimes(record.payload));
    std::lock_guard<std::mutex> publication(publication_lock);
    auto started= std::chrono::steady_clock::now();
    auto last_sequence= spool.AppendBatch(receive_group);
    auto us= std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    auto bytes= spool.Bytes();
    reader_retained= bytes;
    {
      std::lock_guard<std::mutex> guard(lock);
      uint64_t sequence= last_sequence - receive_group.size() + 1;
      for (size_t i= 0; i < receive_group.size(); ++i)
      {
        pending_metrics.Add(sequence++, receive_group[i].payload.size(), times[i]);
        if (times[i].commit) statistics.received_commit_timestamp= times[i].commit;
      }
      statistics.received= last_sequence;
      statistics.received_file= receive_group.back().source_file;
      statistics.received_position= receive_group.back().source_position;
      PendingSnapshot(pending_metrics);
      statistics.retained= bytes;
      statistics.buffered_records= 0;
      statistics.buffered_bytes= 0;
      ++statistics.spool_groups;
      statistics.last_spool_group_records= receive_group.size();
      statistics.last_spool_group_bytes= receive_group_bytes;
      statistics.last_spool_group_us= us;
    }
    receive_group.clear();
    receive_group_bytes= 0;
    wake.notify_all();
  };
  auto flush_due= [&] {
    if (!receive_group.empty() && std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - group_started).count() >=
        static_cast<int64_t>(spool_group_timeout_ms))
      flush_group();
  };
  auto append= [&](const std::string &file, uint64_t position, std::string payload) {
    uint64_t size= SPOOL_RECORD_HEADER_BYTES + file.size() + payload.size();
    if (!receive_group.empty() && size > spool_group_bytes - receive_group_bytes)
      flush_group();
    if (receive_group.empty()) group_started= std::chrono::steady_clock::now();
    receive_group.push_back({0, file, position, std::move(payload)});
    receive_group_bytes+= size;
    {
      std::lock_guard<std::mutex> guard(lock);
      statistics.buffered_records= receive_group.size();
      statistics.buffered_bytes= receive_group_bytes;
    }
    if (receive_group_bytes >= spool_group_bytes) flush_group();
  };
  auto publish_applied= [&] {
    std::lock_guard<std::mutex> publication(publication_lock);
    std::lock_guard<std::mutex> guard(lock);
    statistics.applied= checkpoint.sequence;
    statistics.applied_file= checkpoint.file;
    statistics.applied_position= checkpoint.position;
    auto commit= pending_metrics.AppliedThrough(checkpoint.sequence);
    if (commit)
      statistics.applied_commit_timestamp= commit;
    PendingSnapshot(pending_metrics);
  };
  const std::string resume_file= last.sequence ? last.source_file : checkpoint.file;
  const uint64_t resume_position= last.sequence ? last.source_position : checkpoint.position;
  std::thread reader([&] {
    try
    {
      std::string file= resume_file;
      uint64_t position= resume_position;
      while (reading.load() && Active())
      {
        flush_due();
        if (backpressure())
        {
          flush_group();
          State("BACKPRESSURE");
          Pause(100);
          continue;
        }
        try
        {
          Reader connection(config);
          connection.Open(file, position);
          {
            std::lock_guard<std::mutex> guard(lock);
            statistics.state= "RUNNING";
            statistics.source_connected= 1;
            statistics.error.clear();
          }
          std::string format;
          std::string transaction;
          bool standalone= false;
          RawEvent event;
          while (reading.load() && Active())
          {
            flush_due();
            if (!connection.Next(event)) break;
            if (event.bytes.size() < 19)
              throw std::runtime_error("CDC: invalid upstream event");
            unsigned type= static_cast<unsigned char>(event.bytes[4]);
            if (type == 15)
            {
              if (!transaction.empty())
                throw std::runtime_error("CDC: format changed inside a transaction");
              format= event.bytes;
              continue;
            }
            if (type == 28 || ((type == 5 || type == 13 || type == 14) && transaction.empty()))
              continue;
            if (type == 4 || type == 27 || type == 161 || type == 163 || type == 3)
            {
              if (!transaction.empty())
                throw std::runtime_error("CDC: unexpected metadata inside a transaction");
              if (type == 4 && !format.empty() &&
                  (event.file != file || event.next_position != position))
              {
                append(event.file, event.next_position, format);
                file= event.file;
                position= event.next_position;
              }
              continue;
            }
            if (type == 162)
            {
              if (!transaction.empty() || format.empty() || event.bytes.size() < 32)
                throw std::runtime_error("CDC: invalid GTID boundary");
              auto flags= static_cast<unsigned char>(event.bytes[31]);
              if (flags & 0xc0)
                throw std::runtime_error("CDC: XA is unsupported");
              standalone= flags & 1;
              transaction= format;
            }
            if (transaction.empty())
              throw std::runtime_error("CDC: upstream transaction has no MariaDB GTID");
            if (event.bytes.size() > max_transaction - transaction.size())
              throw std::runtime_error("CDC: source transaction exceeds 64 MiB MVP limit");
            transaction+= event.bytes;
            bool finished= type == 16 || (standalone && type == 2);
            if (type == 2 && !standalone)
              finished= DecodeEffects(format + event.bytes, Value(database)).transaction_end;
            if (finished)
            {
              append(event.file, event.next_position, std::move(transaction));
              file= event.file;
              position= event.next_position;
              transaction.clear();
              {
                std::lock_guard<std::mutex> guard(lock);
                if (statistics.state == "CONNECTING" || statistics.state == "BACKPRESSURE")
                  statistics.state= "RUNNING";
              }
              wake.notify_all();
              if (backpressure())
                break;
            }
          }
          {
            std::lock_guard<std::mutex> guard(lock);
            statistics.source_connected= 0;
          }
        }
        catch (const ReaderError &error)
        {
          if (!error.retryable)
            throw;
          std::lock_guard<std::mutex> guard(lock);
          statistics.state= "CONNECTING";
          statistics.source_connected= 0;
          statistics.error= error.what();
        }
        flush_due();
        if (reading.load() && Active())
          Pause(100);
      }
      flush_group();
    }
    catch (const std::exception &error)
    {
      std::lock_guard<std::mutex> guard(reader_lock);
      reader_error= error.what();
    }
    catch (...)
    {
      std::lock_guard<std::mutex> guard(reader_lock);
      reader_error= "CDC: unexpected reader failure";
    }
    {
      std::lock_guard<std::mutex> guard(lock);
      statistics.source_connected= 0;
      statistics.buffered_records= 0;
      statistics.buffered_bytes= 0;
    }
    reading.store(false);
    wake.notify_all();
  });
  try
  {
    auto pending_since= std::chrono::steady_clock::now();
    bool pending= false;
    while (Active())
    {
      {
        std::lock_guard<std::mutex> guard(reader_lock);
        if (!reader_error.empty())
          throw std::runtime_error(reader_error);
      }
      auto records= spool.ReadAfter(checkpoint.sequence, batch_bytes);
      size_t bytes= 0;
      for (const auto &record : records)
        bytes+= record.payload.size();
      if (records.empty())
      {
        pending= false;
        Pause(20);
        continue;
      }
      auto now= std::chrono::steady_clock::now();
      if (!pending)
      {
        pending= true;
        pending_since= now;
      }
      auto elapsed= std::chrono::duration_cast<std::chrono::milliseconds>(now - pending_since).count();
      if (bytes < batch_bytes && uint64_t(elapsed) < batch_timeout_ms && reading.load())
      {
        {
          std::lock_guard<std::mutex> guard(lock);
          statistics.apply_stage= "BATCH_WAIT";
        }
        Pause(10);
        continue;
      }
      {
        std::lock_guard<std::mutex> guard(lock);
        statistics.batch_records= records.size();
        statistics.batch_payload_bytes= bytes;
        statistics.apply_stage= "APPLYING";
      }
      auto apply_started= std::chrono::steady_clock::now();
      auto finish_apply= [&](const char *stage) {
        auto us= std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - apply_started).count();
        auto stages= applier.Timings();
        std::lock_guard<std::mutex> guard(lock);
        statistics.last_injection_us= stages.injection_us;
        statistics.last_checkpoint_us= stages.checkpoint_us;
        statistics.last_commit_us= stages.commit_us;
        statistics.last_ddl_us= stages.ddl_us;
        statistics.last_apply_us= us;
        statistics.apply_us+= us;
        ++statistics.apply_calls;
        statistics.apply_stage= stage;
      };
      size_t count;
      try
      {
        count= applier.Apply(records);
      }
      catch (...)
      {
        finish_apply("ERROR");
        checkpoint= applier.Current();
        publish_applied();
        throw;
      }
      finish_apply("RECLAIMING");
      if (!count)
        throw std::runtime_error("CDC: applier made no progress");
      checkpoint= applier.Current();
      publish_applied();
      spool.DiscardThrough(checkpoint.sequence);
      pending= false;
      {
        std::lock_guard<std::mutex> publication(publication_lock);
        auto bytes= spool.Bytes();
        std::lock_guard<std::mutex> guard(lock);
        statistics.retained= bytes;
        statistics.apply_stage= "IDLE";
        ++statistics.batches;
      }
      wake.notify_all();
    }
  }
  catch (...)
  {
    reading.store(false);
    wake.notify_all();
    reader.join();
    throw;
  }
  reading.store(false);
  wake.notify_all();
  reader.join();
}

static void Loop()
{
  if (my_thread_init())
  {
    std::lock_guard<std::mutex> guard(lock);
    statistics.state= "ERROR";
    statistics.apply_stage= "ERROR";
    statistics.error= "CDC: thread initialization failed";
    return;
  }
  uint64_t attempted= 0;
  while (!terminating.load())
  {
    if (!requested.load())
    {
      State("STOPPED");
      Pause(100);
      continue;
    }
    if (generation.load() == attempted)
    {
      Pause(100);
      continue;
    }
    if (!ServerReady())
    {
      State("WAITING_FOR_ENGINE");
      Pause(100);
      continue;
    }
    attempted= generation.load();
    try
    {
      Run();
      State("STOPPED");
    }
    catch (const std::exception &error)
    {
      std::lock_guard<std::mutex> guard(lock);
      statistics.state= "ERROR";
      statistics.apply_stage= "ERROR";
      statistics.error= error.what();
      sql_print_error("DuckDB CDC: %s", statistics.error.c_str());
    }
    catch (...)
    {
      std::lock_guard<std::mutex> guard(lock);
      statistics.state= "ERROR";
      statistics.apply_stage= "ERROR";
      statistics.error= "CDC: unexpected worker failure";
    }
    if (!requested.load())
      State("STOPPED");
  }
  my_thread_end();
  State("STOPPED");
}

static void UpdateEnabled(MYSQL_THD, st_mysql_sys_var *, void *ptr, const void *save)
{
  bool value= *static_cast<const my_bool *>(save);
  *static_cast<my_bool *>(ptr)= value;
  requested.store(value);
  generation.fetch_add(1);
  wake.notify_all();
}

int Init(void *)
{
  LEX_CSTRING name= {"DUCKDB", 6};
  engine= plugin_lock_by_name(nullptr, &name, MYSQL_STORAGE_ENGINE_PLUGIN);
  if (!engine)
    return 1;
  terminating.store(false);
  requested.store(enabled);
  generation.store(1);
  try
  {
    worker= std::thread(Loop);
    return 0;
  }
  catch (...)
  {
    plugin_unlock(nullptr, engine);
    engine= nullptr;
    return 1;
  }
}

void Shutdown()
{
  terminating.store(true);
  requested.store(false);
  wake.notify_all();
  if (worker.joinable())
    worker.join();
}

int Deinit(void *)
{
  Shutdown();
  if (engine)
    plugin_unlock(nullptr, engine);
  engine= nullptr;
  return 0;
}

static int ShowText(st_mysql_show_var *var, void *buf, const std::string &value)
{
  auto size= std::min(value.size(), size_t(SHOW_VAR_FUNC_BUFF_SIZE - 1));
  std::memcpy(buf, value.data(), size);
  static_cast<char *>(buf)[size]= 0;
  var->type= SHOW_CHAR;
  var->value= static_cast<char *>(buf);
  return 0;
}

template <std::string Statistics::*member>
static int ShowString(MYSQL_THD, st_mysql_show_var *var, void *buf,
                      system_status_var *, enum_var_type)
{
  std::string value;
  {
    std::lock_guard<std::mutex> guard(lock);
    value= statistics.*member;
  }
  return ShowText(var, buf, value);
}

template <bool lag>
static int ShowAge(MYSQL_THD, st_mysql_show_var *var, void *buf,
                   system_status_var *, enum_var_type)
{
  ulonglong records, oldest, connected, ready;
  {
    std::lock_guard<std::mutex> guard(lock);
    records= statistics.pending_records;
    oldest= statistics.oldest_pending_timestamp;
    connected= statistics.source_connected;
    ready= statistics.metrics_ready;
  }
  auto seconds= std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  uint64_t now= seconds > 0 ? seconds : 0;
  return ShowText(var, buf, !ready ? "UNKNOWN" :
      (lag ? ApplyLag(connected, records, oldest, now) : PendingAge(records, oldest, now)));
}

template <ulonglong Statistics::*member>
static int ShowCounter(MYSQL_THD, st_mysql_show_var *var, void *buf,
                       system_status_var *, enum_var_type)
{
  ulonglong value;
  {
    std::lock_guard<std::mutex> guard(lock);
    value= statistics.*member;
  }
  static_assert(sizeof(value) <= SHOW_VAR_FUNC_BUFF_SIZE, "CDC status buffer too small");
  std::memcpy(buf, &value, sizeof(value));
  var->type= SHOW_LONGLONG;
  var->value= static_cast<char *>(buf);
  return 0;
}

static MYSQL_SYSVAR_BOOL(enabled, enabled, PLUGIN_VAR_OPCMDARG,
                         "Enable the DuckDB CDC worker", nullptr, UpdateEnabled, false);
#define CDC_STRING(name, def) \
  static MYSQL_SYSVAR_STR(name, name, PLUGIN_VAR_READONLY | PLUGIN_VAR_RQCMDARG, \
                          "CDC " #name, nullptr, nullptr, def)
CDC_STRING(source_host, "localhost");
CDC_STRING(source_user, "");
CDC_STRING(source_tls_ca, "");
CDC_STRING(source_tls_cert, "");
CDC_STRING(source_tls_key, "");
CDC_STRING(source_file, "");
CDC_STRING(database, "");
CDC_STRING(stream, "main");
CDC_STRING(spool_directory, "");
static MYSQL_SYSVAR_STR(source_password, source_password,
                        PLUGIN_VAR_READONLY | PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_NOSYSVAR,
                        "CDC upstream password", nullptr, nullptr, "");
static MYSQL_SYSVAR_UINT(source_port, source_port, PLUGIN_VAR_READONLY | PLUGIN_VAR_RQCMDARG,
                         "CDC source TCP port", nullptr, nullptr, 3306, 1, 65535, 0);
static MYSQL_SYSVAR_UINT(server_id, server_id, PLUGIN_VAR_READONLY | PLUGIN_VAR_RQCMDARG,
                         "CDC replication server ID (unique, nonzero when enabled)", nullptr, nullptr, 0, 0, UINT_MAX, 0);
#define CDC_NUMBER(name, def, min) \
  static MYSQL_SYSVAR_ULONGLONG(name, name, PLUGIN_VAR_READONLY | PLUGIN_VAR_RQCMDARG, \
                                "CDC " #name, nullptr, nullptr, def, min, ULONGLONG_MAX, 0)
CDC_NUMBER(source_position, 4, 4);
CDC_NUMBER(batch_bytes, 4 * 1024 * 1024, 1024);
CDC_NUMBER(batch_timeout_ms, 100, 1);
CDC_NUMBER(spool_bytes, 1024ULL * 1024 * 1024, 256 * 1024 * 1024);
static MYSQL_SYSVAR_ULONGLONG(spool_group_bytes, spool_group_bytes,
    PLUGIN_VAR_READONLY | PLUGIN_VAR_RQCMDARG, "CDC durable spool group byte budget",
    nullptr, nullptr, 4 * 1024 * 1024, 1024, 64 * 1024 * 1024, 0);
static MYSQL_SYSVAR_ULONGLONG(spool_group_timeout_ms, spool_group_timeout_ms,
    PLUGIN_VAR_READONLY | PLUGIN_VAR_RQCMDARG, "CDC spool group accumulation timeout",
    nullptr, nullptr, 1000, 1, 60000, 0);

st_mysql_sys_var *variables[]= {
    MYSQL_SYSVAR(enabled), MYSQL_SYSVAR(source_host), MYSQL_SYSVAR(source_port),
    MYSQL_SYSVAR(source_user), MYSQL_SYSVAR(source_password), MYSQL_SYSVAR(source_tls_ca),
    MYSQL_SYSVAR(source_tls_cert), MYSQL_SYSVAR(source_tls_key), MYSQL_SYSVAR(source_file),
    MYSQL_SYSVAR(source_position), MYSQL_SYSVAR(database), MYSQL_SYSVAR(stream),
    MYSQL_SYSVAR(spool_directory), MYSQL_SYSVAR(server_id), MYSQL_SYSVAR(batch_bytes),
    MYSQL_SYSVAR(batch_timeout_ms), MYSQL_SYSVAR(spool_bytes),
    MYSQL_SYSVAR(spool_group_bytes), MYSQL_SYSVAR(spool_group_timeout_ms), nullptr};

st_mysql_show_var status[]= {
    {"Duckdb_cdc_state", reinterpret_cast<char *>(ShowString<&Statistics::state>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_error", reinterpret_cast<char *>(ShowString<&Statistics::error>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_received_file", reinterpret_cast<char *>(ShowString<&Statistics::received_file>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_applied_file", reinterpret_cast<char *>(ShowString<&Statistics::applied_file>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_apply_stage", reinterpret_cast<char *>(ShowString<&Statistics::apply_stage>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_received_position", reinterpret_cast<char *>(ShowCounter<&Statistics::received_position>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_applied_position", reinterpret_cast<char *>(ShowCounter<&Statistics::applied_position>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_pending_records", reinterpret_cast<char *>(ShowCounter<&Statistics::pending_records>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_pending_bytes", reinterpret_cast<char *>(ShowCounter<&Statistics::pending_bytes>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_oldest_pending_timestamp", reinterpret_cast<char *>(ShowCounter<&Statistics::oldest_pending_timestamp>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_received_commit_timestamp", reinterpret_cast<char *>(ShowCounter<&Statistics::received_commit_timestamp>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_applied_commit_timestamp", reinterpret_cast<char *>(ShowCounter<&Statistics::applied_commit_timestamp>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_source_connected", reinterpret_cast<char *>(ShowCounter<&Statistics::source_connected>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_metrics_ready", reinterpret_cast<char *>(ShowCounter<&Statistics::metrics_ready>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_batch_records", reinterpret_cast<char *>(ShowCounter<&Statistics::batch_records>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_batch_bytes", reinterpret_cast<char *>(ShowCounter<&Statistics::batch_payload_bytes>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_last_apply_us", reinterpret_cast<char *>(ShowCounter<&Statistics::last_apply_us>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_last_injection_us", reinterpret_cast<char *>(ShowCounter<&Statistics::last_injection_us>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_last_checkpoint_us", reinterpret_cast<char *>(ShowCounter<&Statistics::last_checkpoint_us>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_last_commit_us", reinterpret_cast<char *>(ShowCounter<&Statistics::last_commit_us>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_last_ddl_us", reinterpret_cast<char *>(ShowCounter<&Statistics::last_ddl_us>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_apply_us", reinterpret_cast<char *>(ShowCounter<&Statistics::apply_us>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_apply_calls", reinterpret_cast<char *>(ShowCounter<&Statistics::apply_calls>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_pending_age_seconds", reinterpret_cast<char *>(ShowAge<false>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_apply_lag_seconds", reinterpret_cast<char *>(ShowAge<true>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_received", reinterpret_cast<char *>(ShowCounter<&Statistics::received>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_applied", reinterpret_cast<char *>(ShowCounter<&Statistics::applied>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_spool_bytes", reinterpret_cast<char *>(ShowCounter<&Statistics::retained>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_buffered_records", reinterpret_cast<char *>(ShowCounter<&Statistics::buffered_records>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_buffered_bytes", reinterpret_cast<char *>(ShowCounter<&Statistics::buffered_bytes>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_spool_groups", reinterpret_cast<char *>(ShowCounter<&Statistics::spool_groups>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_last_spool_group_records", reinterpret_cast<char *>(ShowCounter<&Statistics::last_spool_group_records>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_last_spool_group_bytes", reinterpret_cast<char *>(ShowCounter<&Statistics::last_spool_group_bytes>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_last_spool_group_us", reinterpret_cast<char *>(ShowCounter<&Statistics::last_spool_group_us>), SHOW_SIMPLE_FUNC},
    {"Duckdb_cdc_batches", reinterpret_cast<char *>(ShowCounter<&Statistics::batches>), SHOW_SIMPLE_FUNC},
    {nullptr, nullptr, SHOW_LONG}};

st_mysql_daemon daemon= {MYSQL_DAEMON_INTERFACE_VERSION};

}
}
