#
# Register DuckDB error messages in the MariaDB error system.
#
# 1. Append duckdb_errors.txt to errmsg-utf8.txt (idempotent, at configure time).
# 2. Generate duckdb_error.h from mysqld_error.h (at build time, after comp_err).
#

SET(DUCKDB_ERRORS_TXT "${CMAKE_CURRENT_SOURCE_DIR}/duckdb_errors.txt")
SET(ERRMSG_FILE       "${PROJECT_SOURCE_DIR}/sql/share/errmsg-utf8.txt")
SET(DUCKDB_ERROR_H    "${CMAKE_CURRENT_SOURCE_DIR}/duckdb_error.h")
SET(MYSQLD_ERROR_H    "${CMAKE_BINARY_DIR}/include/mysqld_error.h")

# -- Step 1: append our errors to errmsg-utf8.txt (once, at configure time) ---

SET(DUCKDB_MARKER "# --- DuckDB storage engine errors ---")

FILE(READ "${ERRMSG_FILE}" _errmsg_content)
STRING(FIND "${_errmsg_content}" "${DUCKDB_MARKER}" _marker_pos)

IF(_marker_pos EQUAL -1)
  MESSAGE(STATUS "Appending DuckDB errors to errmsg-utf8.txt")
  FILE(READ "${DUCKDB_ERRORS_TXT}" _duckdb_errors)
  FILE(APPEND "${ERRMSG_FILE}" "\n${DUCKDB_MARKER}\n${_duckdb_errors}")
ELSE()
  MESSAGE(STATUS "DuckDB errors already present in errmsg-utf8.txt")
ENDIF()

# -- Step 2: generate duckdb_error.h after mysqld_error.h exists --------------
# mysqld_error.h is produced by GenError (extra/CMakeLists.txt).
# We create a custom command that depends on it and extracts ER_DUCKDB_* defines.

# Generate duckdb_error.h at build time.
# We use a stamp-based approach: the custom command always runs but
# copy_if_different avoids unnecessary rebuilds.
ADD_CUSTOM_TARGET(duckdb_error_h
  COMMAND ${CMAKE_COMMAND}
    -DMYSQLD_ERROR_H=${MYSQLD_ERROR_H}
    -DDUCKDB_ERROR_H=${DUCKDB_ERROR_H}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_duckdb_error_h.cmake"
  DEPENDS "${DUCKDB_ERRORS_TXT}"
  COMMENT "Generating duckdb_error.h from mysqld_error.h"
)

