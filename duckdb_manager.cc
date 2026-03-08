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

#include "duckdb_manager.h"

#include <my_global.h>
#include "mysqld.h"
#include "log.h"
#include "duckdb_config.h"

namespace myduck
{

DuckdbManager *DuckdbManager::m_instance= nullptr;

DuckdbManager::DuckdbManager() : m_database(nullptr) {}

bool DuckdbManager::Initialize()
{
  if (m_database != nullptr)
    return false;

  std::lock_guard<std::mutex> lock(m_mutex);

  if (m_database != nullptr)
    return false;

  duckdb::DBConfig config;

  config.options.use_direct_io= global_use_dio;

  if (global_max_threads != 0)
    config.options.maximum_threads= global_max_threads;

  /*
    When memory_limit sysvar is 0 (default), DuckDB tries to auto-detect
    available RAM (80%).  Inside some Docker/cgroup environments this
    detection fails and returns 0, making DuckDB unable to allocate
    anything.  Use an explicit 1 GB fallback in that case.
  */
  static constexpr uint64_t DUCKDB_DEFAULT_MEMORY_FALLBACK= 1ULL
                                                            << 30; /* 1 GB */
  if (global_memory_limit != 0)
    config.options.maximum_memory= global_memory_limit;
  else
    config.options.maximum_memory= DUCKDB_DEFAULT_MEMORY_FALLBACK;

  if (global_max_temp_directory_size != 0)
    config.options.maximum_swap_space= global_max_temp_directory_size;

  config.options.checkpoint_wal_size= checkpoint_threshold;

  /* Temp directory: user-specified or default (data directory) */
  {
    char tmp_path[FN_REFLEN];
    if (global_duckdb_temp_directory && global_duckdb_temp_directory[0])
      config.options.temporary_directory= global_duckdb_temp_directory;
    else
    {
      fn_format(tmp_path, DUCKDB_DEFAULT_TMP_NAME, mysql_real_data_home, "",
                MYF(0));
      config.options.temporary_directory= tmp_path;
    }
  }

  /* Store all tables in one file in the data directory */
  char path[FN_REFLEN];
  fn_format(path, DUCKDB_FILE_NAME, mysql_real_data_home, "", MYF(0));
  m_database= new duckdb::DuckDB(path, &config);

  if (m_database == nullptr)
    return true;

  sql_print_information("DuckDB: DuckdbManager::Initialize succeed, path=%s",
                        path);

  return false;
}

bool DuckdbManager::CreateInstance()
{
  DBUG_ASSERT(m_instance == nullptr);
  m_instance= new DuckdbManager();
  if (m_instance == nullptr)
  {
    sql_print_error("DuckDB: DuckdbManager::CreateInstance failed");
    return true;
  }
  return false;
}

DuckdbManager::~DuckdbManager()
{
  if (m_database != nullptr)
  {
    delete m_database;
    m_database= nullptr;
  }
}

void DuckdbManager::Cleanup()
{
  if (m_instance == nullptr)
    return;
  delete m_instance;
  m_instance= nullptr;
}

DuckdbManager &DuckdbManager::Get()
{
  DBUG_ASSERT(m_instance != nullptr);
  bool ret= m_instance->Initialize();
  DBUG_ASSERT(!ret);
  return *m_instance;
}

std::shared_ptr<duckdb::Connection> DuckdbManager::CreateConnection()
{
  auto &instance= Get();
  auto connection= std::make_shared<duckdb::Connection>(*instance.m_database);
  return connection;
}

} // namespace myduck
