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

#include "duckdb_config.h"
#include "duckdb/common/string_util.hpp"

namespace myduck {

ulonglong global_memory_limit = 0;
ulonglong global_max_temp_directory_size = 0;
ulonglong global_max_threads = 0;
ulonglong appender_allocator_flush_threshold = 0;
ulonglong checkpoint_threshold = 268435456;  /* 256 MB */
my_bool global_use_dio = FALSE;
my_bool global_scheduler_process_partial = TRUE;
my_bool use_double_for_decimal = FALSE;

std::string BytesToHumanReadableString(uint64_t bytes, uint64_t multiplier) {
  return duckdb::StringUtil::BytesToHumanReadableString(bytes, multiplier);
}

}  // namespace myduck
