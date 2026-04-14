INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb.cmake)
INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb_errors.cmake)

SET(DUCKDB_SOURCES
  ha_duckdb.cc
  duckdb_manager.cc
  duckdb_types.cc
  ddl_convertor.cc
  dml_convertor.cc
  duckdb_select.cc
  duckdb_context.cc
  duckdb_query.cc
  delta_appender.cc
  duckdb_log.cc
  duckdb_config.cc
  duckdb_charset_collation.cc
  duckdb_timezone.cc
  ha_duckdb_pushdown.cc
  cross_engine_scan.cc
  duckdb_udf.cc
)

MYSQL_ADD_PLUGIN(duckdb ${DUCKDB_SOURCES}
  STORAGE_ENGINE
  MODULE_ONLY
  COMPONENT duckdb-engine
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/duckdb.cnf
  LINK_LIBRARIES ${DUCKDB_LIBRARY}
)

# libduckdb_bundle.a is built without debug STL wrappers.
# Strip the flags that MariaDB's debug build adds so that our plugin's
# object files see the same STL container layout as the library.
IF(TARGET duckdb)
  ADD_DEPENDENCIES(duckdb duckdb_error_h)
  # DuckDB headers and plugin code require C++17.
  SET_TARGET_PROPERTIES(duckdb PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  # libduckdb_bundle.a is built without debug STL wrappers.
  # -U unconditionally undefines the macro no matter how it was inherited
  # (CMAKE_CXX_FLAGS_DEBUG, directory properties, generator expressions, etc.).
  # Mismatched _GLIBCXX_DEBUG changes sizeof(std::vector) and friends → SIGSEGV.
  TARGET_COMPILE_OPTIONS(duckdb PRIVATE -U_GLIBCXX_DEBUG -U_GLIBCXX_ASSERTIONS)
  IF(DUCKDB_WERROR)
    TARGET_COMPILE_OPTIONS(duckdb PRIVATE -Werror)
  ENDIF()
ENDIF()

IF(TARGET duckdb)
  INSTALL(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/install.sql
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/uninstall.sql
    DESTINATION ${INSTALL_MYSQLSHAREDIR}/duckdb
    COMPONENT duckdb-engine)
ENDIF()
