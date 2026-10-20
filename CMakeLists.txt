cmake_minimum_required(VERSION 3.12)
project(duckdb LANGUAGES C CXX)
find_package(mariadb-plugin REQUIRED)
INCLUDE(CheckCXXCompilerFlag)

IF(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  MESSAGE(STATUS "DuckDB: not Linux, skipping")
  return()
ENDIF()

IF(NOT (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR
        CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64"))
  MESSAGE(STATUS "DuckDB: not x86_64 or aarch64, skipping")
  return()
ENDIF()

# Catch -m32 / multilib builds (e.g. 32-bit build inside an x86_64 CI
# container): CMAKE_SYSTEM_PROCESSOR still reports x86_64, but the target is
# 32-bit. DuckDB (and its jemalloc extension) is 64-bit only.
IF(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  MESSAGE(STATUS "DuckDB: not a 64-bit build, skipping")
  return()
ENDIF()

# Check C++17 compiler support
CHECK_CXX_COMPILER_FLAG("-std=c++17" have_CXX__std_c__17)
IF(NOT have_CXX__std_c__17)
  MESSAGE(STATUS "DuckDB: C++ compiler does not support -std=c++17, skipping")
  RETURN()
ENDIF()

# libduckdb_bundle.a is built without debug STL wrappers.
# Mismatched _GLIBCXX_DEBUG changes sizeof(std::vector) → SIGSEGV.
SET(CMAKE_CXX_FLAGS_DEBUG
    "${CMAKE_CXX_FLAGS_DEBUG} -U_GLIBCXX_DEBUG -U_GLIBCXX_ASSERTIONS")

# Plugin sources (handler layer + UDFs whose extern "C" symbols must be
# in the final .so for dlsym() loading).
SET(DUCKDB_PLUGIN_SOURCES
  ha_duckdb.cc
  ha_duckdb_pushdown.cc
  duckdb_udf.cc
)

# The MariaDB test-data package owns the default plugin test path.
# Avoid duplicates there, then install this suite under its own name below.
IF(DEB)
  SET(DUCKDB_SAVED_MYSQLTESTDIR "${INSTALL_MYSQLTESTDIR}")
  SET(INSTALL_MYSQLTESTDIR "")
ENDIF()

MARIADB_ADD_PLUGIN(duckdb ${DUCKDB_PLUGIN_SOURCES} STORAGE_ENGINE MODULE_ONLY
                   AUTHOR "drrtuy,lfedorov"
                   VERSION 1.0
                   DESCRIPTION "DuckDB storage engine"
                   COMPONENT duckdb-engine
                   CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/duckdb.cnf)

IF(DEB)
  SET(INSTALL_MYSQLTESTDIR "${DUCKDB_SAVED_MYSQLTESTDIR}")
  UNSET(DUCKDB_SAVED_MYSQLTESTDIR)
  INSTALL(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/mysql-test/duckdb/"
    DESTINATION "${INSTALL_MYSQLTESTDIR}/suite/duckdb_engine"
    COMPONENT Test)
ENDIF()

SET(CPACK_RPM_duckdb-engine_PACKAGE_SUMMARY "DuckDB storage engine for MariaDB server")
SET(CPACK_RPM_duckdb-engine_PACKAGE_DESCRIPTION "The MariaDB DuckDB storage
engine embeds DuckDB as a pluggable storage engine, enabling fast analytical
(OLAP) query processing directly inside MariaDB.")

IF(TARGET duckdb)

  INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb.cmake)
  INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb_target_setup.cmake)

  # Provide MariaDB server include paths to sub-libraries.
  # MARIADB_ADD_PLUGIN does this only for the plugin target itself.
  LINK_LIBRARIES(mariadb_private mysqlservices)

  # Build sub-libraries.
  ADD_SUBDIRECTORY(common)
  ADD_SUBDIRECTORY(convertor)
  ADD_SUBDIRECTORY(runtime)

  # atomic: GCC lowers the 16-byte atomic ops of the AUTO_INCREMENT block
  # cache (Duckdb_share::autoinc_range) to libatomic calls.
  target_link_libraries(duckdb duckdb_runtime duckdb_convertor duckdb_common
    ${DUCKDB_LIBRARY} atomic
  )

  duckdb_setup_target(duckdb)
  TARGET_INCLUDE_DIRECTORIES(duckdb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/convertor
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
  )
ENDIF()

include(CPack)
