INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb.cmake)

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
  duckdb_table.cc
  ha_duckdb_pushdown.cc
  duckdb_udf.cc
)

MYSQL_ADD_PLUGIN(duckdb ${DUCKDB_SOURCES}
  STORAGE_ENGINE
  MODULE_ONLY
  LINK_LIBRARIES ${DUCKDB_LIBRARY}
)

# The pre-built libduckdb_bundle.a is compiled without debug STL wrappers
# and with the old std::string ABI.  Strip the flags that MariaDB's debug
# build adds so that our plugin's object files link cleanly against the lib.
IF(TARGET duckdb)
  TARGET_COMPILE_DEFINITIONS(duckdb PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
  GET_TARGET_PROPERTY(_duckdb_cxx_flags duckdb COMPILE_FLAGS)
  IF(_duckdb_cxx_flags)
    STRING(REPLACE "-D_GLIBCXX_DEBUG" "" _duckdb_cxx_flags "${_duckdb_cxx_flags}")
    STRING(REPLACE "-D_GLIBCXX_ASSERTIONS" "" _duckdb_cxx_flags "${_duckdb_cxx_flags}")
    SET_TARGET_PROPERTIES(duckdb PROPERTIES COMPILE_FLAGS "${_duckdb_cxx_flags}")
  ENDIF()
  # Also strip from the global DEBUG flags applied to this directory
  STRING(REPLACE "-D_GLIBCXX_DEBUG" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  STRING(REPLACE "-D_GLIBCXX_ASSERTIONS" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
ENDIF()
