/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

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

#define MYSQL_SERVER 1
#include <my_global.h>
#include "sql_class.h"
#include "log.h"

#undef UNKNOWN

#include "duckdb_context.h"
#include "duckdb_charset_collation.h"
#include "duckdb_config.h"
#include "duckdb_types.h"
#include "duckdb_handler_errors.h"
#include "duckdb_timezone.h"

#include <vector>

extern handlerton *duckdb_hton;

namespace myduck
{

static void push_duckdb_warning(THD *thd, std::string &warn_msg)
{
  if (warn_msg.empty())
    return;
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                      "DuckDB: %s", warn_msg.c_str());
  warn_msg.clear();
}

static std::string disabled_optimizers_to_string(ulonglong val)
{
  std::string result;
  for (uint i= 0; disabled_optimizers_names[i] != NullS; i++)
  {
    if (val & (1ULL << i))
    {
      if (!result.empty())
        result+= ',';
      result+= disabled_optimizers_names[i];
    }
  }
  return result;
}

DuckdbThdContext *get_thd_context(THD *thd)
{
  auto *ctx=
      static_cast<DuckdbThdContext *>(thd_get_ha_data(thd, duckdb_hton));
  if (!ctx)
  {
    ctx= new DuckdbThdContext();
    thd_set_ha_data(thd, duckdb_hton, ctx);
  }
  return ctx;
}

void DuckdbThdContext::begin_interruptible(duckdb::Connection *con)
{
  std::lock_guard<std::mutex> guard(m_interrupt_mutex);
  m_interruptible= con ? con : m_con.get();
  m_interruptible->context->ClearInterrupt();
}

void DuckdbThdContext::interrupt_if_killed(THD *thd)
{
  std::lock_guard<std::mutex> guard(m_interrupt_mutex);
  if (m_interruptible && thd->killed)
    m_interruptible->Interrupt();
}

void DuckdbThdContext::end_interruptible()
{
  std::lock_guard<std::mutex> guard(m_interrupt_mutex);
  if (m_interruptible)
    m_interruptible->context->ClearInterrupt();
  m_interruptible= nullptr;
}

void DuckdbThdContext::interrupt()
{
  std::lock_guard<std::mutex> guard(m_interrupt_mutex);
  if (m_interruptible)
    m_interruptible->Interrupt();
}

void DuckdbThdContext::config_duckdb_env(const std::string &schema)
{
  if (schema.empty() || schema == m_current_schema)
    return;

  std::string sql1= "CREATE SCHEMA IF NOT EXISTS " +
                    quote_duckdb_identifier(schema);
  std::string sql2= "USE " + quote_duckdb_identifier(schema);
  m_current_schema= schema;

  for (auto &sql : {sql1, sql2})
  {
    auto res= duckdb_query(get_connection(), sql);
    if (res && res->HasError())
      sql_print_warning("DuckDB: config_duckdb_env failed: %s (sql=%s)",
                        res->GetError().c_str(), sql.c_str());
  }
}

static std::string quote_sql_string(const std::string &str)
{
  std::string quoted= "'";
  for (char c : str)
  {
    quoted+= c;
    if (c == '\'')
      quoted+= c;
  }
  return quoted + "'";
}

enum class ZoneCheck
{
  KNOWN,
  UNKNOWN,
  FAILED
};

/**
  Whether DuckDB's ICU accepts a zone name. Checked with a query rather
  than by trying SET TimeZone: a failing statement aborts an open DuckDB
  transaction. GMT+hh:mm ids come from get_fixed_offset_timezone() and
  are always valid, but pg_timezone_names() does not list them.
*/
static ZoneCheck duckdb_knows_timezone(duckdb::Connection &con,
                                       const std::string &zone)
{
  if (zone.compare(0, 4, "GMT+") == 0 || zone.compare(0, 4, "GMT-") == 0)
    return ZoneCheck::KNOWN;
  auto res= duckdb_query(con, "SELECT count(*) FROM pg_timezone_names() "
                              "WHERE lower(name) = lower(" +
                                  quote_sql_string(zone) + ")");
  if (!res || res->HasError())
    return ZoneCheck::FAILED;
  auto chunk= res->Fetch();
  if (!chunk || chunk->size() == 0)
    return ZoneCheck::FAILED;
  return chunk->GetValue(0, 0).GetValue<int64_t>() > 0 ? ZoneCheck::KNOWN
                                                        : ZoneCheck::UNKNOWN;
}

static bool set_duckdb_timezone(duckdb::Connection &con,
                                const std::string &zone)
{
  std::string sql= "SET TimeZone = " + quote_sql_string(zone);
  auto res= duckdb_query(con, sql);
  if (!res || res->HasError())
  {
    sql_print_warning("DuckDB: config_duckdb_session failed: %s (sql=%s)",
                      res ? res->GetError().c_str() : "no result",
                      sql.c_str());
    return false;
  }
  return true;
}

static bool set_duckdb_variable(duckdb::Connection &con, const char *name,
                                const std::string &sql_value)
{
  auto res= duckdb_query(con, std::string("SET VARIABLE ") + name + " = " +
                                  sql_value);
  return res && !res->HasError();
}

void DuckdbThdContext::config_duckdb_timezone(THD *thd)
{
  /* Variables read by runtime/duckdb_mariadb_tz.cc */
  if (!m_tz_rules_set &&
      set_duckdb_variable(get_connection(), MARIADB_TZ_RULES_VAR, "true"))
    m_tz_rules_set= true;
  int repeated_later= is_system_timezone(thd) ? 1 : 0;
  if (repeated_later != m_repeated_later &&
      set_duckdb_variable(get_connection(), MARIADB_TZ_REPEATED_LATER_VAR,
                          repeated_later ? "true" : "false"))
    m_repeated_later= repeated_later;
  std::string system_zone= get_system_timezone(thd);
  if (system_zone != m_system_timezone &&
      set_duckdb_variable(get_connection(), MARIADB_SYSTEM_TZ_VAR,
                          quote_sql_string(system_zone)))
    m_system_timezone= system_zone;

  std::string zone= get_timezone_according_thd(thd);
  if (zone == m_current_timezone)
    return;

  if (zone != m_unknown_timezone)
  {
    switch (duckdb_knows_timezone(get_connection(), zone))
    {
    case ZoneCheck::KNOWN:
      if (set_duckdb_timezone(get_connection(), zone))
        m_current_timezone= zone;
      return;
    case ZoneCheck::FAILED:
      return; /* retried on the next statement */
    case ZoneCheck::UNKNOWN:
      m_unknown_timezone= zone;
      break;
    }
  }

  std::string fixed= get_fixed_offset_timezone(thd);
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                      "DuckDB: unknown time zone '%s', using its current "
                      "offset %s",
                      zone.c_str(), fixed.c_str());
  if (fixed != m_current_timezone &&
      set_duckdb_timezone(get_connection(), fixed))
    m_current_timezone= fixed;
}

void DuckdbThdContext::config_duckdb_session(THD *thd)
{
  std::vector<std::string> config_sql;

  config_duckdb_timezone(thd);

  /* Collation: force_no_collation overrides to binary (POSIX) */
  std::string warn_msg;
  std::string collation;
  if (get_thd_force_no_collation(thd))
    collation= COLLATION_BINARY;
  else
    collation=
        get_duckdb_collation(thd->variables.collation_connection, warn_msg);
  if (collation != m_collation)
  {
    config_sql.push_back("SET default_collation = '" + collation + "'");
    /* Only warn when collation is explicitly changed, not on initial setup */
    if (!m_collation.empty())
      push_duckdb_warning(thd, warn_msg);
    m_collation= collation;
  }

  /* merge_join_threshold (session) */
  ulonglong mjt= get_thd_merge_join_threshold(thd);
  if (mjt != m_merge_join_threshold)
  {
    config_sql.push_back("SET merge_join_threshold = " + std::to_string(mjt));
    m_merge_join_threshold= mjt;
  }

  /* disabled_optimizers (session) */
  ulonglong dopt= get_thd_disabled_optimizers(thd);
  if (dopt != m_disabled_optimizers)
  {
    std::string val_str= disabled_optimizers_to_string(dopt);
    config_sql.push_back("SET disabled_optimizers = '" + val_str + "'");
    m_disabled_optimizers= dopt;
  }

  /* explain_output (session, only when EXPLAIN) */
  ulong eo= get_thd_explain_output(thd);
  std::string eo_str= explain_output_names[eo];
  if (eo_str != m_explain_output_str)
  {
    config_sql.push_back("SET explain_output = '" + eo_str + "'");
    m_explain_output_str= eo_str;
  }

  /* Execute all config statements */
  for (auto &sql : config_sql)
  {
    auto res= duckdb_query(get_connection(), sql);
    if (res && res->HasError())
      sql_print_warning("DuckDB: config_duckdb_session failed: %s (sql=%s)",
                        res->GetError().c_str(), sql.c_str());
  }
}

DeltaAppender *DuckdbThdContext::get_appender(TABLE *table)
{
  if (!m_appenders)
    m_appenders= std::make_unique<DeltaAppenders>(m_con);

  DatabaseTableNames dt(table->s->normalized_path.str);
  std::string db= dt.db_name;
  std::string tb= dt.table_name;

  return m_appenders->get_appender(
      db, tb, batch_state == BatchState::IN_INSERT_ONLY_BATCH, table);
}

int DuckdbThdContext::append_row_insert(TABLE *table,
                                        const MY_BITMAP *blob_map)
{
  DeltaAppender *delta= get_appender(table);
  return delta ? delta->append_row_insert(table, 0, blob_map)
               : HA_DUCKDB_APPEND_ERROR;
}

int DuckdbThdContext::append_row_update(TABLE *table, const uchar *old_row)
{
  DeltaAppender *delta= get_appender(table);
  return delta ? delta->append_row_update(table, 0, old_row)
               : HA_DUCKDB_APPEND_ERROR;
}

int DuckdbThdContext::append_row_delete(TABLE *table)
{
  DeltaAppender *delta= get_appender(table);
  return delta ? delta->append_row_delete(table, 0) : HA_DUCKDB_APPEND_ERROR;
}

bool thd_is_replication_applier(THD *thd)
{
  return thd->rgi_slave != nullptr;
}

bool thd_is_row_injection(THD *thd)
{
  return thd->lex->is_stmt_row_injection();
}

bool reject_xa_if_active(THD *thd)
{
  if (!thd->transaction->xid_state.is_explicit_XA())
    return false;

  static const char *xa_state_names[]= {"ACTIVE", "IDLE", "PREPARED",
                                         "ROLLBACK ONLY"};
  my_error(ER_XAER_RMFAIL, MYF(0),
           xa_state_names[thd->transaction->xid_state.get_state_code()]);
  return true;
}

} // namespace myduck
