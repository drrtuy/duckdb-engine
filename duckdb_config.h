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

#ifndef DUCKDB_CONFIG_H
#define DUCKDB_CONFIG_H

#include "duckdb/common/types.hpp"
#include <my_global.h>

namespace myduck {
extern ulonglong global_memory_limit;
extern ulonglong global_max_temp_directory_size;
extern ulonglong global_max_threads;
extern ulonglong appender_allocator_flush_threshold;
extern ulonglong checkpoint_threshold;
extern my_bool global_use_dio;
extern my_bool global_scheduler_process_partial;
extern my_bool use_double_for_decimal;

std::string BytesToHumanReadableString(uint64_t bytes,
                                       uint64_t multiplier = 1024);
}  // namespace myduck

#endif // DUCKDB_CONFIG_H
