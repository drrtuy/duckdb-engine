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

#ifndef HA_DUCKDB_PUSHDOWN_H
#define HA_DUCKDB_PUSHDOWN_H

#include "my_global.h"
#include "sql_class.h"
#include "select_handler.h"

#undef UNKNOWN

#include "duckdb.hpp"

extern handlerton *duckdb_hton;

/**
  select_handler implementation for DuckDB.

  Pushes entire SELECT queries down to the DuckDB engine when all
  referenced tables belong to DuckDB.
*/
class ha_duckdb_select_handler : public select_handler
{
public:
  ha_duckdb_select_handler(THD *thd_arg, SELECT_LEX *sel_lex,
                           SELECT_LEX_UNIT *sel_unit);
  ~ha_duckdb_select_handler() override;

protected:
  int init_scan() override;
  int next_row() override;
  int end_scan() override;

private:
  std::unique_ptr<duckdb::QueryResult> query_result;
  std::unique_ptr<duckdb::DataChunk> current_chunk;
  size_t current_row_index;

  StringBuffer<4096> query_string;
};

/**
  Factory function registered in hton->create_select.
  Returns a new ha_duckdb_select_handler if all tables in the query
  are DuckDB tables, otherwise returns nullptr.
*/
select_handler *create_duckdb_select_handler(THD *thd, SELECT_LEX *sel_lex,
                                             SELECT_LEX_UNIT *sel_unit);

#endif /* HA_DUCKDB_PUSHDOWN_H */
