# Extensions required by the DuckDB storage engine plugin for MariaDB.
# This config is passed to DuckDB via DUCKDB_EXTENSION_CONFIGS.
# Keep in sync with _DUCKDB_LINKED_EXTENSIONS in cmake/duckdb.cmake.

duckdb_extension_load(core_functions)
duckdb_extension_statically_link(core_functions)
duckdb_extension_load(icu)
duckdb_extension_statically_link(icu)
duckdb_extension_load(json)
duckdb_extension_statically_link(json)
duckdb_extension_load(parquet)
duckdb_extension_statically_link(parquet)

# httpfs gives read_parquet() and friends https://, http:// and s3:// paths.
# It is out of tree since DuckDB 1.3; the pin is DuckDB v2.0.0's own
# (.github/config/extensions/httpfs.cmake). Linked statically, like the
# extensions above, because mysqld never installs extensions at runtime.
if(WITH_DUCKDB_HTTPFS)
  duckdb_extension_load(httpfs
      GIT_URL https://github.com/duckdb/duckdb-httpfs
      GIT_TAG a2485c0fd240cbddc0d467019f30201c9a46cf06
      APPLY_PATCHES)
  set(_httpfs_source "${DUCKDB_EXTENSION_HTTPFS_PATH}")
  set(DUCKDB_EXTENSION_HTTPFS_PATH "${CMAKE_CURRENT_BINARY_DIR}/httpfs-patched")
  file(COPY "${_httpfs_source}/" DESTINATION "${DUCKDB_EXTENSION_HTTPFS_PATH}"
      PATTERN ".git" EXCLUDE)
  set(DUCKDB_EXTENSION_HTTPFS_INCLUDE_PATH "${DUCKDB_EXTENSION_HTTPFS_PATH}/src/include")
  set(DUCKDB_EXTENSION_HTTPFS_TEST_PATH "${DUCKDB_EXTENSION_HTTPFS_PATH}/test/sql")
  set(_httpfs_patch "${CMAKE_CURRENT_LIST_DIR}/../patches/duckdb-httpfs/0001-explicit-move-http-params.patch")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_httpfs_patch}")
  find_program(_httpfs_patch_executable patch REQUIRED)
  execute_process(
      COMMAND "${_httpfs_patch_executable}" -p1 --forward --batch --fuzz=0 --dry-run -i "${_httpfs_patch}"
      WORKING_DIRECTORY "${DUCKDB_EXTENSION_HTTPFS_PATH}"
      RESULT_VARIABLE _httpfs_patch_check OUTPUT_QUIET ERROR_QUIET)
  if(_httpfs_patch_check EQUAL 0)
    execute_process(
        COMMAND "${_httpfs_patch_executable}" -p1 --forward --batch --fuzz=0 --no-backup-if-mismatch -i "${_httpfs_patch}"
        WORKING_DIRECTORY "${DUCKDB_EXTENSION_HTTPFS_PATH}"
        RESULT_VARIABLE _httpfs_patch_result ERROR_VARIABLE _httpfs_patch_error)
    if(NOT _httpfs_patch_result EQUAL 0)
      message(FATAL_ERROR "Cannot apply ${_httpfs_patch}: ${_httpfs_patch_error}")
    endif()
  else()
    execute_process(
        COMMAND "${_httpfs_patch_executable}" -p1 --reverse --batch --fuzz=0 --dry-run -i "${_httpfs_patch}"
        WORKING_DIRECTORY "${DUCKDB_EXTENSION_HTTPFS_PATH}"
        RESULT_VARIABLE _httpfs_patch_result ERROR_VARIABLE _httpfs_patch_error)
    if(NOT _httpfs_patch_result EQUAL 0)
      message(FATAL_ERROR "Cannot apply or verify ${_httpfs_patch}: ${_httpfs_patch_error}")
    endif()
  endif()
  duckdb_extension_statically_link(httpfs)
endif()

# Quack client/server protocol: third_parties/duckdb-quack (pinned to the
# commit DuckDB itself pins) plus patches/duckdb-quack, staged by
# cmake/duckdb.cmake into MARIADB_QUACK_SOURCE_DIR.
duckdb_extension_load(quack
    SOURCE_DIR ${MARIADB_QUACK_SOURCE_DIR}
    INCLUDE_DIR ${MARIADB_QUACK_SOURCE_DIR}/src/include
)
duckdb_extension_statically_link(quack)
