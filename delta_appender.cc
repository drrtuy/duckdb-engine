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

#include "delta_appender.h"

/*
  Stub implementations for DeltaAppender.
  Full batch DML via DuckDB Appender API will be implemented in a later phase.
*/

int DeltaAppender::append_row_insert(TABLE *, ulonglong,
                                     const MY_BITMAP *)
{
  return 0;
}

int DeltaAppender::append_row_update(TABLE *, ulonglong, const uchar *)
{
  return 0;
}

int DeltaAppender::append_row_delete(TABLE *, ulonglong, const uchar *)
{
  return 0;
}

bool DeltaAppender::Initialize(TABLE *)
{
  return false;
}

int DeltaAppender::append_mysql_field(const Field *, const MY_BITMAP *)
{
  return 0;
}

bool DeltaAppender::flush(bool)
{
  return false;
}

bool DeltaAppender::rollback(ulonglong)
{
  return false;
}

void DeltaAppender::cleanup() {}

void DeltaAppender::generateQuery(std::stringstream &, bool) {}

/* DeltaAppenders */

void DeltaAppenders::delete_appender(std::string &db, std::string &tb)
{
  auto key = std::make_pair(db, tb);
  m_append_infos.erase(key);
}

bool DeltaAppenders::flush_all(bool idempotent_flag, std::string &error_msg)
{
  for (auto &pair : m_append_infos)
  {
    if (pair.second->flush(idempotent_flag))
    {
      error_msg = "DeltaAppender flush failed";
      return true;
    }
  }
  return false;
}

void DeltaAppenders::reset_all()
{
  m_append_infos.clear();
}

bool DeltaAppenders::rollback_trx(ulonglong trx_no)
{
  for (auto &pair : m_append_infos)
  {
    if (pair.second->rollback(trx_no))
      return true;
  }
  return false;
}

DeltaAppender *DeltaAppenders::get_appender(std::string &db, std::string &tb,
                                            bool insert_only, TABLE *table)
{
  auto key = std::make_pair(db, tb);
  auto it = m_append_infos.find(key);
  if (it != m_append_infos.end())
    return it->second.get();

  auto appender = std::make_unique<DeltaAppender>(m_con, db, tb, !insert_only);
  if (appender->Initialize(table))
    return nullptr;

  auto *raw = appender.get();
  m_append_infos[key] = std::move(appender);
  return raw;
}

int DeltaAppenders::append_mysql_field(duckdb::Appender *, const Field *)
{
  return 0;
}
