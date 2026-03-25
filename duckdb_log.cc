/*
  Copyright (c) 2025, Alibaba and/or its affiliates.
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin.

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

#include "duckdb_log.h"

#include <my_global.h>

namespace myduck
{

ulonglong duckdb_log_options= 0;

const char *duckdb_log_types[]= {"DUCKDB_QUERY", "DUCKDB_QUERY_RESULT",
                                 nullptr};

TYPELIB log_options_typelib= {array_elements(duckdb_log_types) - 1, "",
                              duckdb_log_types, NULL};

} // namespace myduck
