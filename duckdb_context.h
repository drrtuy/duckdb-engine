/*
  Copyright (c) 2025, Alibaba and/or its affiliates. All Rights Reserved.
  Ported to MariaDB.

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

#pragma once

#include "duckdb_manager.h"
#include "duckdb_query.h"

#include <my_global.h>
#include <my_bitmap.h>

class THD;
class TABLE;

namespace myduck
{

enum class BatchState
{
  UNDEFINED= 0,
  NOT_IN_BATCH,
  IN_INSERT_ONLY_BATCH,
  IN_MIX_BATCH
};

class DuckdbThdContext
{
public:
  DuckdbThdContext() : batch_state(BatchState::UNDEFINED)
  {
    m_con= DuckdbManager::CreateConnection();
  }

  ~DuckdbThdContext()
  {
    if (has_transaction())
    {
      std::string error_msg;
      duckdb_trans_rollback(error_msg);
    }
  }

  bool has_transaction() { return m_con && m_con->HasActiveTransaction(); }

  bool duckdb_trans_begin()
  {
    if (!m_con || m_con->HasActiveTransaction())
      return true;
    auto result= duckdb_query(*m_con, "BEGIN");
    return result->HasError();
  }

  bool duckdb_trans_commit(std::string &error_msg)
  {
    if (!m_con)
      return true;

    if (m_con->HasActiveTransaction())
    {
      auto result= duckdb_query(*m_con, "COMMIT");
      if (result->HasError())
      {
        error_msg= result->GetError().c_str();
        return true;
      }
    }
    set_batch_state(BatchState::UNDEFINED);
    return false;
  }

  bool duckdb_trans_rollback(std::string &error_msg)
  {
    if (!m_con)
      return true;

    if (m_con->HasActiveTransaction())
    {
      auto result= duckdb_query(*m_con, "ROLLBACK");
      if (result->HasError())
      {
        error_msg= result->GetError().c_str();
        return true;
      }
    }
    set_batch_state(BatchState::UNDEFINED);
    return false;
  }

  duckdb::Connection &get_connection() { return *m_con; }

  /** Configure DuckDB environment (timezone, charset) for this thread. */
  void config_duckdb_env(THD *thd);

  void delete_appender(const std::string &schema, const std::string &table)
  {
    /* TODO: implement when DeltaAppender is fully ported */
  }

  bool flush_appenders(std::string &error_msg)
  {
    /* TODO: implement batch appender flushing */
    set_batch_state(BatchState::UNDEFINED);
    return false;
  }

  int append_row_insert(TABLE *table, const MY_BITMAP *blob_map)
  {
    /* TODO: implement via DeltaAppender */
    return 0;
  }

  int append_row_update(TABLE *table, const uchar *old_row)
  {
    /* TODO: implement via DeltaAppender */
    return 0;
  }

  int append_row_delete(TABLE *table)
  {
    /* TODO: implement via DeltaAppender */
    return 0;
  }

  void set_batch_state(BatchState state) { batch_state= state; }
  BatchState get_batch_state() { return batch_state; }

private:
  std::shared_ptr<duckdb::Connection> m_con;
  BatchState batch_state;
};

} // namespace myduck
