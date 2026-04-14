#
# Extract ER_DUCKDB_* defines from mysqld_error.h into duckdb_error.h.
#

FILE(STRINGS "${MYSQLD_ERROR_H}" _all_lines)

SET(_lines "/* Auto-generated from duckdb_errors.txt — do not edit. */\n#pragma once\n")

FOREACH(_line IN LISTS _all_lines)
  IF(_line MATCHES "^#define ER_DUCKDB_")
    STRING(APPEND _lines "\n${_line}")
  ENDIF()
ENDFOREACH()

STRING(APPEND _lines "\n")

FILE(WRITE "${DUCKDB_ERROR_H}.tmp" "${_lines}")

EXECUTE_PROCESS(
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${DUCKDB_ERROR_H}.tmp" "${DUCKDB_ERROR_H}"
)
FILE(REMOVE "${DUCKDB_ERROR_H}.tmp")
