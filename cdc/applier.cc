#include "applier.h"
#include "ddl.h"
#include "ddl_recovery.h"
#include "events.h"
#include "duckdb_manager.h"

#include <mysql/service_sql.h>

#include <chrono>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#ifndef MYSQL_DYNAMIC_PLUGIN
#error CDC applier requires the dynamic plugin SQL service
#endif

namespace myduck::cdc
{
namespace
{
struct Duration
{
  uint64_t &value;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  ~Duration()
  {
    value += std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
  }
};
void Require(bool ok, const char *message)
{
  if (!ok) throw std::runtime_error(std::string("CDC applier: ") + message);
}
std::string Hex(const std::string &s)
{
  static const char digits[] = "0123456789ABCDEF";
  std::string out = "X'";
  for (unsigned char c : s) { out += digits[c >> 4]; out += digits[c & 15]; }
  return out + "'";
}
std::string Quote(const std::string &s)
{
  Require(!s.empty() && s.size() <= 64 && s.find('\0') == std::string::npos,
          "invalid identifier");
  std::string out = "`";
  for (char c : s) { out += c; if (c == '`') out += c; }
  return out + "`";
}
uint64_t Number(const std::string &s)
{
  Require(!s.empty(), "empty numeric metadata");
  uint64_t out = 0;
  for (unsigned char c : s)
  {
    Require(c >= '0' && c <= '9' &&
            out <= (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10,
            "invalid numeric metadata");
    out = out * 10 + c - '0';
  }
  return out;
}
bool IsDuckdb(std::string engine)
{
  for (char &c : engine) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
  return engine == "DUCKDB";
}
using Rows = std::vector<std::vector<std::string>>;
constexpr const char *CP = "`_duckdb_cdc`.`checkpoints`";
constexpr const char *INTENT = "`_duckdb_cdc`.`ddl_intents`";
constexpr const char *JOURNAL = "`_duckdb_cdc`.`ddl_recovery`";

void Cell(std::string &out, const std::string &value)
{
  out += std::to_string(value.size()) + ":" + value;
}
}

struct Applier::Impl
{
  MYSQL *mysql = nullptr;
  bool connected = false;
  std::string database, stream, identity, predicate;
  Checkpoint checkpoint;
  ApplyTimings timings;
  uint64_t packet = std::numeric_limits<unsigned long>::max();
  std::set<std::string> validated;
  bool failed = false;

  ~Impl()
  {
    if (mysql)
    {
      if (connected) mysql_real_query(mysql, "ROLLBACK", 8);
      mysql_close(mysql);
    }
  }
  Rows Sql(const std::string &sql, unsigned fields = 0)
  {
    Require(sql.size() < packet && sql.size() <= std::numeric_limits<unsigned long>::max(),
            "statement exceeds target max_allowed_packet");
    if (mysql_real_query(mysql, sql.data(), static_cast<unsigned long>(sql.size())))
      throw std::runtime_error("CDC applier: local SQL failed (errno " +
                               std::to_string(mysql_errno(mysql)) + ")");
    MYSQL_RES *raw = mysql_store_result(mysql);
    std::unique_ptr<MYSQL_RES, void (*)(MYSQL_RES *)> result(raw,
        [](MYSQL_RES *r) { if (r) mysql_free_result(r); });
    Require(raw || (!fields && !mysql_errno(mysql)), "missing SQL result");
    Rows rows;
    if (!raw) return rows;
    Require(fields && mysql_num_fields(raw) == fields && mysql_num_rows(raw) <= 2048,
            "unexpected metadata result");
    while (auto row = mysql_fetch_row(raw))
    {
      auto lengths = mysql_fetch_lengths(raw);
      Require(lengths != nullptr, "missing metadata lengths");
      std::vector<std::string> values;
      for (unsigned i = 0; i < fields; ++i)
      {
        Require(row[i] && lengths[i] <= 1024 * 1024, "invalid metadata value");
        values.emplace_back(row[i], lengths[i]);
      }
      rows.push_back(std::move(values));
    }
    Require(!mysql_errno(mysql), "metadata fetch failed");
    return rows;
  }
  std::string TableFilter(const std::string &db, const std::string &table)
  {
    return "TABLE_SCHEMA=" + Hex(db) + " AND TABLE_NAME=" + Hex(table);
  }
  void Validate(const std::string &db, const std::string &table, bool primary = true)
  {
    Quote(table);
    auto filter = TableFilter(db, table);
    auto rows = Sql("SELECT ENGINE FROM information_schema.TABLES WHERE " + filter, 1);
    Require(rows.size() == 1 && IsDuckdb(rows[0][0]), "target table must use DuckDB");
    if (!primary) return;
    rows = Sql("SELECT COUNT(*) FROM information_schema.STATISTICS WHERE " + filter +
               " AND INDEX_NAME='PRIMARY'", 1);
    Require(rows.size() == 1 && Number(rows[0][0]) > 0, "target table requires a primary key");
  }
  void Init(const std::string &db, const std::string &name, const std::string &binding,
            const std::string &file, uint64_t position)
  {
    Quote(db);
    for (unsigned char c : db)
      Require(c >= 32 && c <= 126 && c != '\\' && c != '/' && c != '.',
              "source database must use unambiguous ASCII identifiers");
    Require(db.back() != ' ' && db.rfind("#mysql50#", 0) != 0,
            "legacy source database aliases are unsupported");
    std::string lower = db;
    for (char &c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    Require(lower != "mysql" && lower != "information_schema" && lower != "performance_schema" &&
            lower != "sys" && lower != "_duckdb_cdc", "reserved source database");
    Require(!name.empty() && name.size() <= 255 && !binding.empty() && binding.size() <= 65536 &&
            file.size() <= 4096 && position >= 4, "invalid stream binding or position");
    database = db; stream = name; identity = binding;
    predicate = " WHERE `stream`=" + Hex(stream);
    mysql = mysql_init(nullptr);
    Require(mysql && mysql_real_connect_local(mysql), "cannot open local SQL service");
    connected = true;
    Sql("SET SESSION sql_log_bin=0");
    Sql("SET SESSION autocommit=1");
    Sql("SET SESSION lock_wait_timeout=2");
    Sql("SET SESSION sql_quote_show_create=1");
    Sql("SET SESSION sql_if_exists=0");
    Sql("SET SESSION sql_mode='NO_BACKSLASH_ESCAPES,STRICT_ALL_TABLES,NO_ENGINE_SUBSTITUTION'");
    Require(mysql_set_character_set(mysql, "utf8mb4") == 0, "cannot set SQL character set");
    auto rows = Sql("SELECT @@max_allowed_packet", 1);
    Require(rows.size() == 1, "missing packet limit");
    packet = Number(rows[0][0]);
    Sql("CREATE DATABASE IF NOT EXISTS `_duckdb_cdc`");
    Sql(std::string("CREATE TABLE IF NOT EXISTS ") + CP +
        " (`stream` VARBINARY(255) PRIMARY KEY, `identity` LONGBLOB NOT NULL,"
        " `source_database` VARBINARY(256) NOT NULL, `sequence` BIGINT UNSIGNED NOT NULL,"
        " `file` BLOB NOT NULL, `position` BIGINT UNSIGNED NOT NULL) ENGINE=DuckDB");
    Sql(std::string("CREATE TABLE IF NOT EXISTS ") + INTENT +
        " (`stream` VARBINARY(255) PRIMARY KEY, `sequence` BIGINT UNSIGNED NOT NULL,"
        " `file` BLOB NOT NULL, `position` BIGINT UNSIGNED NOT NULL,"
        " `ddl` LONGBLOB NOT NULL) ENGINE=DuckDB");
    Sql(std::string("CREATE TABLE IF NOT EXISTS ") + JOURNAL +
        " (`stream` VARBINARY(255) PRIMARY KEY, `sequence` BIGINT UNSIGNED NOT NULL,"
        " `file` BLOB NOT NULL, `position` BIGINT UNSIGNED NOT NULL, `ddl` LONGBLOB NOT NULL,"
        " `before_table` LONGBLOB NOT NULL, `before_new` LONGBLOB NOT NULL,"
        " `after_table` LONGBLOB NOT NULL, `after_new` LONGBLOB NOT NULL, `done` INT NOT NULL) ENGINE=DuckDB");
    Validate("_duckdb_cdc", "checkpoints");
    Validate("_duckdb_cdc", "ddl_intents");
    Validate("_duckdb_cdc", "ddl_recovery");
    Require(Sql(std::string("SELECT `stream` FROM ") + INTENT + " LIMIT 1", 1).empty(),
            "unresolved DDL intent from legacy format; manual catalog reconciliation required");
    rows = Sql(std::string("SELECT `identity`,`source_database`,`sequence`,`file`,`position` FROM ") +
               CP + predicate, 5);
    if (rows.empty())
    {
      Sql(std::string("INSERT INTO ") + CP + " VALUES (" + Hex(stream) + "," + Hex(identity) +
          "," + Hex(database) + ",0," + Hex(file) + "," + std::to_string(position) + ")");
      checkpoint = {0, file, position};
    }
    else
    {
      Require(rows.size() == 1 && rows[0][0] == identity && rows[0][1] == database,
              "stream identity or database mismatch");
      checkpoint = {Number(rows[0][2]), rows[0][3], Number(rows[0][4])};
    }
    Require(checkpoint.file.size() <= 4096 && checkpoint.position >= 4,
            "invalid stored checkpoint coordinates");
    Recover();
  }
  void CheckBinlog(const std::string &bytes)
  {
    Require(packet > 16 && bytes.size() <= ((packet - 16) / 4) * 3,
            "BINLOG exceeds target max_allowed_packet");
  }
  void Binlog(const std::string &bytes)
  {
    Duration duration{timings.injection_us};
    CheckBinlog(bytes);
    Sql("BINLOG '" + Base64(bytes) + "'");
  }
  void Save(const SpoolRecord &record)
  {
    Duration duration{timings.checkpoint_us};
    Require(record.source_file.size() <= 4096 && record.source_position >= 4,
            "invalid checkpoint coordinates");
    Sql(std::string("UPDATE ") + CP + " SET `sequence`=" + std::to_string(record.sequence) +
        ",`file`=" + Hex(record.source_file) + ",`position`=" +
        std::to_string(record.source_position) + predicate);
    Require(mysql_affected_rows(mysql) == 1, "checkpoint row changed or disappeared");
  }
  std::string Snapshot(const std::string &table)
  {
    if (table.empty()) return {};
    auto filter = TableFilter(database, table);
    auto tables = Sql("SELECT ENGINE FROM information_schema.TABLES WHERE " + filter, 1);
    Require(tables.empty() || (tables.size() == 1 && IsDuckdb(tables[0][0])),
            "DDL target table must use DuckDB");
    auto sql_rows = Sql("SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE " + filter +
                        " ORDER BY ORDINAL_POSITION", 1);
    auto connection = DuckdbManager::CreateConnection();
    Require(bool(connection), "DuckDB catalog connection unavailable");
    auto prepared = connection->Prepare("SELECT column_name,data_type,is_nullable,column_default "
        "FROM information_schema.columns WHERE table_catalog=current_database() "
        "AND table_schema=? AND table_name=? ORDER BY ordinal_position");
    Require(!prepared->HasError(), "DuckDB catalog prepare failed");
    auto result = prepared->Execute(database, table);
    Require(!result->HasError() && result->RowCount() == sql_rows.size() &&
            (tables.empty() ? sql_rows.empty() : !sql_rows.empty()), "DDL catalog mismatch");
    std::string snapshot;
    size_t i = 0;
    while (auto chunk = result->Fetch())
    {
      for (size_t row = 0; row < chunk->size(); ++row, ++i)
      {
        Require(i < sql_rows.size() && chunk->GetValue(0, row).ToString() == sql_rows[i][0],
                "DDL column catalog mismatch");
        for (size_t col = 0; col < 4; ++col)
        {
          auto value = chunk->GetValue(col, row);
          Cell(snapshot, value.IsNull() ? "N" : "V" + value.ToString());
        }
      }
    }
    if (tables.empty()) return {};
    auto definition = Sql("SHOW CREATE TABLE " + Quote(database) + "." + Quote(table), 2);
    Require(definition.size() == 1, "missing table definition");
    Cell(snapshot, SchemaDefinition(definition[0][1], table));
    return snapshot;
  }
  void VerifyDdl(const DdlPlan &plan, const std::string &current, const std::string &current_new)
  {
    bool old_exists = plan.operation != "DROP" && plan.operation != "RENAME";
    Require(old_exists == !current.empty(), "DDL table presence mismatch");
    Require(plan.new_table.empty() == current_new.empty(), "DDL destination presence mismatch");
  }
  void Commit()
  {
    Duration duration{timings.commit_us};
    Sql("COMMIT");
  }
  void FinishDdl(const SpoolRecord &record)
  {
    Sql("BEGIN");
    Save(record);
    Sql(std::string("DELETE FROM ") + JOURNAL + predicate);
    Commit();
    checkpoint = {record.sequence, record.source_file, record.source_position};
    validated.clear();
  }
  void ExecuteDdl(const DdlPlan &plan, const SpoolRecord &record)
  {
    Sql(plan.sql);
    auto after = Snapshot(plan.table);
    auto after_new = Snapshot(plan.new_table);
    VerifyDdl(plan, after, after_new);
    Sql(std::string("UPDATE ") + JOURNAL + " SET `after_table`=" + Hex(after) +
        ",`after_new`=" + Hex(after_new) + ",`done`=1" + predicate);
    Require(mysql_affected_rows(mysql) == 1, "DDL journal row changed or disappeared");
    FinishDdl(record);
  }
  void Recover()
  {
    auto owners = Sql(std::string("SELECT `stream` FROM ") + JOURNAL, 1);
    Require(owners.empty() || (owners.size() == 1 && owners[0][0] == stream),
            "DDL journal belongs to another stream");
    auto rows = Sql(std::string("SELECT `sequence`,`file`,`position`,`ddl`,`before_table`,"
        "`before_new`,`after_table`,`after_new`,`done` FROM ") + JOURNAL + predicate, 9);
    if (rows.empty()) return;
    const auto &row = rows.front();
    SpoolRecord record{Number(row[0]), row[1], Number(row[2]), {}};
    Require(checkpoint.sequence != std::numeric_limits<uint64_t>::max() &&
            record.sequence == checkpoint.sequence + 1 && (row[8] == "0" || row[8] == "1"),
            "DDL journal checkpoint mismatch");
    auto plan = PlanDdl(row[3], database, database);
    Require(plan.sql == row[3], "noncanonical DDL journal");
    auto current = Snapshot(plan.table);
    auto current_new = Snapshot(plan.new_table);
    auto action = RecoveryAction(plan.operation, row[8] == "1", row[4], row[5],
                                 row[6], row[7], current, current_new);
    if (action == DdlRecoveryAction::Execute) ExecuteDdl(plan, record);
    else
    {
      VerifyDdl(plan, current, current_new);
      FinishDdl(record);
    }
  }
  void Ddl(const SpoolRecord &record, const Effects &effects)
  {
    Duration duration{timings.ddl_us};
    auto plan = PlanDdl(effects.ddl.query, effects.ddl.database, database);
    auto before = Snapshot(plan.table);
    auto before_new = Snapshot(plan.new_table);
    if (plan.operation != "CREATE" && plan.operation != "DROP")
      Require(!before.empty(), "DDL target table is missing");
    Require(before_new.empty(), "DDL destination already exists");
    Sql(std::string("INSERT INTO ") + JOURNAL + " VALUES (" + Hex(stream) + "," +
        std::to_string(record.sequence) + "," + Hex(record.source_file) + "," +
        std::to_string(record.source_position) + "," + Hex(plan.sql) + "," + Hex(before) +
        "," + Hex(before_new) + ",X'',X'',0)");
    ExecuteDdl(plan, record);
  }
  size_t Apply(const std::vector<SpoolRecord> &records)
  {
    Require(!failed, "applier failed; reopen and reconcile any DDL intent");
    size_t begin = 0;
    while (begin < records.size() && records[begin].sequence <= checkpoint.sequence) ++begin;
    if (begin == records.size()) return begin;
    uint64_t next = checkpoint.sequence;
    std::vector<Effects> effects;
    std::string format;
    size_t end = begin;
    for (; end < records.size(); ++end)
    {
      const auto &record = records[end];
      Require(next != std::numeric_limits<uint64_t>::max() && record.sequence == next + 1,
              "nonconsecutive spool sequence");
      Require(record.source_file.size() <= 4096 && record.source_position >= 4,
              "invalid checkpoint coordinates");
      auto effect = DecodeEffects(record.payload, database);
      if (!effect.ddl.query.empty())
      {
        if (end != begin) break;
        Require(!effect.rollback, "DDL with rollback is unsupported");
        Ddl(record, effect);
        checkpoint = {record.sequence, record.source_file, record.source_position};
        return end + 1;
      }
      auto key = FormatKey(effect.format);
      if (!effects.empty() && key != format) break;
      format = std::move(key);
      CheckBinlog(effect.format);
      for (const auto &statement : effect.statements) CheckBinlog(statement);
      for (const auto &table : effect.tables)
        if (!validated.count(table)) { Validate(database, table); validated.insert(table); }
      effects.push_back(std::move(effect));
      next = record.sequence;
    }
    Binlog(effects.front().format);
    Sql("BEGIN");
    std::string chunk;
    const uint64_t budget = ((packet - 16) / 4) * 3;
    for (const auto &effect : effects)
    {
      for (const auto &statement : effect.statements)
      {
        if (statement.size() > budget - chunk.size())
        {
          Binlog(chunk);
          chunk.clear();
        }
        chunk += statement;
      }
    }
    if (!chunk.empty()) Binlog(chunk);
    Save(records[end - 1]);
    Commit();
    checkpoint = {records[end - 1].sequence, records[end - 1].source_file,
                  records[end - 1].source_position};
    return end;
  }
};

Applier::Applier(const std::string &database, const std::string &stream,
                 const std::string &identity, const std::string &initial_file,
                 uint64_t initial_position) : impl_(std::make_unique<Impl>())
{
  impl_->Init(database, stream, identity, initial_file, initial_position);
}
Applier::~Applier() = default;
Checkpoint Applier::Current() { return impl_->checkpoint; }
ApplyTimings Applier::Timings() const { return impl_->timings; }
size_t Applier::Apply(const std::vector<SpoolRecord> &records)
{
  impl_->timings = {};
  try { return impl_->Apply(records); }
  catch (...)
  {
    impl_->failed = true;
    mysql_real_query(impl_->mysql, "ROLLBACK", 8);
    throw;
  }
}

}
