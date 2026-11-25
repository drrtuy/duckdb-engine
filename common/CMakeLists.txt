INCLUDE(${PROJECT_SOURCE_DIR}/cmake/duckdb_target_setup.cmake)

SET(DUCKDB_COMMON_SOURCES
  duckdb_types.cc
  duckdb_log.cc
  duckdb_charset_collation.cc
  duckdb_timezone.cc
  mariadb_lexical_guard.cc
)

ADD_LIBRARY(duckdb_common STATIC ${DUCKDB_COMMON_SOURCES})
duckdb_setup_target(duckdb_common)

TARGET_INCLUDE_DIRECTORIES(duckdb_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

ADD_EXECUTABLE(ha_table_exists_compat_test EXCLUDE_FROM_ALL
  ha_table_exists_compat_test.cc)
duckdb_setup_target(ha_table_exists_compat_test)
TARGET_COMPILE_DEFINITIONS(ha_table_exists_compat_test PRIVATE MYSQL_SERVER)
