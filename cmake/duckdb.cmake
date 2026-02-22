# Copyright (c) 2025, Alibaba and/or its affiliates. All Rights Reserved.
# Ported to MariaDB.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA

#
# Build DuckDB library.
#
# Mode 1 (from source): If extra/duckdb/Makefile exists, build via ExternalProject.
# Mode 2 (pre-built):   Otherwise use libduckdb_bundle.a + duckdb_include/ from
#                        the storage/duckdb/ directory.
#

SET(DUCKDB_NAME "duckdb")
SET(DUCKDB_DIR "extra/${DUCKDB_NAME}")
SET(DUCKDB_SOURCE_DIR "${CMAKE_SOURCE_DIR}/${DUCKDB_DIR}")
SET(DUCKDB_PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

IF(EXISTS "${DUCKDB_SOURCE_DIR}/Makefile")
  # --- Mode 1: build from source ---
  INCLUDE(ExternalProject)

  IF(CMAKE_BUILD_TYPE STREQUAL "Debug")
    SET(DUCKDB_BUILD_TYPE "bundle-library-debug")
    SET(DUCKDB_BUILD_DIR "debug")
  ELSE()
    SET(DUCKDB_BUILD_TYPE "bundle-library")
    SET(DUCKDB_BUILD_DIR "release")
  ENDIF()

  MESSAGE(STATUS "=== Building DuckDB from source (${DUCKDB_DIR}) ===")

  SET(BINARY_DIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/${DUCKDB_DIR}/build")
  SET(DUCKDB_INCLUDE_DIR "${DUCKDB_SOURCE_DIR}/src/include")

  ExternalProject_Add(duckdb_proj
    PREFIX      "${DUCKDB_DIR}"
    SOURCE_DIR  "${DUCKDB_SOURCE_DIR}"
    BINARY_DIR  "${BINARY_DIR}"
    STAMP_DIR   "${BINARY_DIR}/${DUCKDB_BUILD_DIR}/stamp"
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make -C "${DUCKDB_SOURCE_DIR}" "${DUCKDB_BUILD_TYPE}" > /dev/null 2>&1
    INSTALL_COMMAND ""
    BUILD_ALWAYS OFF
  )

  SET(DUCKDB_LIB "${BINARY_DIR}/${DUCKDB_BUILD_DIR}/libduckdb_bundle.a")

  ADD_LIBRARY(libduckdb STATIC IMPORTED GLOBAL)
  SET_TARGET_PROPERTIES(libduckdb PROPERTIES IMPORTED_LOCATION "${DUCKDB_LIB}")
  ADD_DEPENDENCIES(libduckdb duckdb_proj)

ELSE()
  # --- Mode 2: use pre-built library ---
  SET(DUCKDB_INCLUDE_DIR "${DUCKDB_PLUGIN_DIR}/duckdb_include")
  SET(DUCKDB_LIB "${DUCKDB_PLUGIN_DIR}/libduckdb_bundle.a")

  IF(NOT EXISTS "${DUCKDB_LIB}")
    MESSAGE(FATAL_ERROR
      "DuckDB library not found.\n"
      "Either place DuckDB source under extra/duckdb/\n"
      "or provide a pre-built libduckdb_bundle.a in storage/duckdb/"
    )
  ENDIF()

  MESSAGE(STATUS "DuckDB: using pre-built library ${DUCKDB_LIB}")

  ADD_LIBRARY(libduckdb STATIC IMPORTED GLOBAL)
  SET_TARGET_PROPERTIES(libduckdb PROPERTIES IMPORTED_LOCATION "${DUCKDB_LIB}")

ENDIF()

MESSAGE(STATUS "DuckDB include: ${DUCKDB_INCLUDE_DIR}")
MESSAGE(STATUS "DuckDB library: ${DUCKDB_LIB}")

INCLUDE_DIRECTORIES(BEFORE SYSTEM "${DUCKDB_INCLUDE_DIR}")
SET(DUCKDB_LIBRARY libduckdb)
