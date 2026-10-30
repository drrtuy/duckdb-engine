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

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "duckdb.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/query_result_stream.hpp"

class THD;

namespace myduck
{

using DuckdbChunkStream= duckdb::QueryResultStream<duckdb::ChunkFormat>;

enum class SqlRegionType
{
  NONE,
  COMMENT,
  QUOTED,
  UNTERMINATED
};

SqlRegionType scan_sql_region(const std::string &sql, size_t start,
                              bool backslash_escapes, size_t &end);

bool mariadb_query_has_unsafe_quote_escape(THD *thd, const char *query,
                                            size_t length);

/*
  Blocking: runs the query to completion and returns the retained
  (materialized) result. For DML, DDL and metadata queries.
*/
duckdb::unique_ptr<duckdb::QueryResult>
duckdb_query(duckdb::Connection &connection, const std::string &query);

/*
  Submits the query and opens a chunk stream on it, so rows are produced
  on demand and the result is never materialized. On failure returns
  nullptr and sets @p error.
*/
duckdb::unique_ptr<DuckdbChunkStream>
duckdb_stream_query(duckdb::Connection &connection, const std::string &query,
                    std::string &error);

duckdb::unique_ptr<duckdb::QueryResult>
duckdb_query(THD *thd, const std::string &query, bool need_config= true);

duckdb::unique_ptr<DuckdbChunkStream>
duckdb_stream_query(THD *thd, const std::string &query, std::string &error,
                    bool need_config= true);

duckdb::unique_ptr<duckdb::QueryResult>
duckdb_query(const std::string &query);

} // namespace myduck
