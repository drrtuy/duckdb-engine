cmake_minimum_required(VERSION 3.12)
project(duckdb LANGUAGES C CXX)
find_package(mariadb-plugin REQUIRED)
INCLUDE("${CMAKE_CURRENT_SOURCE_DIR}/VERSIONS")
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
  ha_duckdb_quack.cc
  duckdb_udf.cc
  duckdb_udf_wrappers.cc
  bridge/bridge_test_udf.cc
)
OPTION(DUCKDB_CDC "Build the experimental CDC daemon plugin" OFF)
IF(DUCKDB_CDC)
  LIST(APPEND DUCKDB_PLUGIN_SOURCES cdc/plugin.cc cdc/events.cc cdc/spool.cc cdc/ddl.cc cdc/applier.cc)
ENDIF()

IF(RPM)
  SET(INSTALL_MYSQLTESTDIR "")
ENDIF()

MARIADB_ADD_PLUGIN(duckdb ${DUCKDB_PLUGIN_SOURCES} STORAGE_ENGINE MODULE_ONLY
                   AUTHOR "drrtuy,lfedorov"
                   VERSION "${DUCKDB_PLUGIN_VERSION}"
                   DESCRIPTION "DuckDB storage engine"
                   CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/duckdb.cnf)

SET(CPACK_RPM_duckdb-engine_PACKAGE_DESCRIPTION "The MariaDB DuckDB storage
engine embeds DuckDB as a pluggable storage engine, enabling fast analytical
(OLAP) query processing directly inside MariaDB.")

IF(TARGET duckdb)

  # Quack (MDEV-41383) serves HTTPS through OpenSSL. Linked dynamically: on
  # distribution builds this is the same libssl/libcrypto mariadbd loads.
  FIND_PACKAGE(OpenSSL)
  IF(NOT OPENSSL_FOUND)
    MESSAGE(FATAL_ERROR "DuckDB: OpenSSL development files are required "
            "(libssl-dev / openssl-devel)")
  ENDIF()

  INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb.cmake)
  INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb_target_setup.cmake)

  IF(DUCKDB_CDC)
    TARGET_COMPILE_DEFINITIONS(duckdb PRIVATE WITH_DUCKDB_CDC)
    FIND_PATH(DUCKDB_CDC_CLIENT_INCLUDE_DIR mariadb_rpl.h PATH_SUFFIXES mariadb)
    FIND_LIBRARY(DUCKDB_CDC_CLIENT_ARCHIVE NAMES libmariadb.a)
    FIND_LIBRARY(DUCKDB_CDC_ZLIB NAMES z libz.so.1)
    IF(NOT DUCKDB_CDC_CLIENT_INCLUDE_DIR OR NOT DUCKDB_CDC_CLIENT_ARCHIVE OR NOT DUCKDB_CDC_ZLIB)
      MESSAGE(FATAL_ERROR "DuckDB CDC requires Connector/C headers, its PIC static archive and ZLIB")
    ENDIF()
    ADD_LIBRARY(duckdb_cdc_reader OBJECT cdc/reader.cc)
    TARGET_INCLUDE_DIRECTORIES(duckdb_cdc_reader PRIVATE ${DUCKDB_CDC_CLIENT_INCLUDE_DIR})
    duckdb_setup_target(duckdb_cdc_reader)
    TARGET_SOURCES(duckdb PRIVATE $<TARGET_OBJECTS:duckdb_cdc_reader>)
    TARGET_LINK_LIBRARIES(duckdb ${DUCKDB_CDC_CLIENT_ARCHIVE} OpenSSL::SSL OpenSSL::Crypto
                                 ${DUCKDB_CDC_ZLIB} ${CMAKE_DL_LIBS} m pthread)
    SET_PROPERTY(TARGET duckdb APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--exclude-libs,libmariadb.a")
  ENDIF()

  # DuckDB-side Quack module; added first so it gets no MariaDB headers.
  ADD_SUBDIRECTORY(quack_guard)

  # Provide MariaDB server include paths to sub-libraries.
  # MARIADB_ADD_PLUGIN does this only for the plugin target itself.
  LINK_LIBRARIES(mariadb_private mysqlservices)

  # Build sub-libraries.
  ADD_SUBDIRECTORY(common)
  ADD_SUBDIRECTORY(convertor)
  ADD_SUBDIRECTORY(runtime)
  ADD_SUBDIRECTORY(bridge)

  # atomic: GCC lowers the 16-byte atomic ops of the AUTO_INCREMENT block
  # cache (Duckdb_share::autoinc_range) to libatomic calls.
  target_link_libraries(duckdb duckdb_runtime duckdb_convertor duckdb_common
    quack_bridge quack_guard ${DUCKDB_LIBRARY} atomic
  )
  TARGET_SOURCES(duckdb PRIVATE ${DUCKDB_STATIC_EXTENSION_LOADER})

  ADD_EXECUTABLE(duckdb_cdc_spool_test EXCLUDE_FROM_ALL cdc/spool.cc cdc/spool_test.cc)
  duckdb_setup_target(duckdb_cdc_spool_test)
  TARGET_LINK_LIBRARIES(duckdb_cdc_spool_test pthread)
  ADD_EXECUTABLE(duckdb_cdc_spool_group_test EXCLUDE_FROM_ALL cdc/spool.cc cdc/spool_group_test.cc)
  duckdb_setup_target(duckdb_cdc_spool_group_test)
  TARGET_LINK_LIBRARIES(duckdb_cdc_spool_group_test pthread)
  SET_PROPERTY(TARGET duckdb_cdc_spool_group_test APPEND_STRING PROPERTY LINK_FLAGS " -Wl,--wrap=fsync")
  ADD_EXECUTABLE(duckdb_cdc_ddl_test EXCLUDE_FROM_ALL cdc/ddl.cc cdc/ddl_test.cc)
  duckdb_setup_target(duckdb_cdc_ddl_test)
  ADD_EXECUTABLE(duckdb_cdc_events_test EXCLUDE_FROM_ALL cdc/events.cc cdc/ddl.cc cdc/events_test.cc)
  duckdb_setup_target(duckdb_cdc_events_test)
  ADD_EXECUTABLE(duckdb_cdc_ddl_recovery_test EXCLUDE_FROM_ALL cdc/ddl_recovery_test.cc)
  duckdb_setup_target(duckdb_cdc_ddl_recovery_test)
  ADD_EXECUTABLE(duckdb_cdc_metrics_test EXCLUDE_FROM_ALL cdc/metrics_test.cc cdc/events.cc)
  duckdb_setup_target(duckdb_cdc_metrics_test)

  duckdb_setup_target(duckdb)
  TARGET_INCLUDE_DIRECTORIES(duckdb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/convertor
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
  )
ENDIF()

include(CPack)
