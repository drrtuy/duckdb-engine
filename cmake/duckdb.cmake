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
# Build DuckDB static library from submodule source.
#
# The upstream DuckDB repo lives at storage/duckdb/third_parties/duckdb/
# as a git submodule.  We build it via ExternalProject so its CMake targets
# (including one named "duckdb") don't clash with the MariaDB plugin target
# of the same name created by MYSQL_ADD_PLUGIN().
#
# After the cmake build we merge every produced .a into a single
# libduckdb_bundle.a — the same thing DuckDB's own `make bundle-library` does.
#

SET(DUCKDB_SUBMODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_parties/duckdb")
SET(DUCKDB_INCLUDE_DIR   "${DUCKDB_SUBMODULE_DIR}/src/include")

IF(NOT EXISTS "${DUCKDB_SUBMODULE_DIR}/CMakeLists.txt")
  MESSAGE(FATAL_ERROR
    "DuckDB submodule not found at ${DUCKDB_SUBMODULE_DIR}\n"
    "Run:  git submodule update --init storage/duckdb/third_parties/duckdb"
  )
ENDIF()

INCLUDE(ExternalProject)

# Map MariaDB build type to a DuckDB-friendly one.
IF(CMAKE_BUILD_TYPE MATCHES "[Dd]ebug")
  SET(_DUCKDB_BUILD_TYPE "Debug")
ELSE()
  SET(_DUCKDB_BUILD_TYPE "Release")
ENDIF()

SET(_DUCKDB_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/duckdb-build")
SET(DUCKDB_LIB        "${_DUCKDB_BUILD_DIR}/libduckdb_bundle.a")

# Write a small helper script that merges all .a into one fat archive.
# Each archive is extracted into its own subdirectory to avoid object-name
# collisions between different libraries.
FILE(WRITE "${CMAKE_CURRENT_BINARY_DIR}/bundle_duckdb.sh"
[=[
#!/bin/sh
set -e
BUILD_DIR="$1"; OUTPUT="$2"; AR="$3"
TMPDIR="${BUILD_DIR}/_bundle_tmp"
rm -rf "${TMPDIR}"; mkdir -p "${TMPDIR}"
i=0
find "${BUILD_DIR}" -name '*.a' \
     ! -name 'libduckdb_bundle.a' \
     ! -path '*/_bundle_tmp/*' | while read -r lib; do
  i=$((i+1))
  d="${TMPDIR}/${i}"
  mkdir -p "$d"
  cd "$d" && "$AR" x "$lib"
done
find "${TMPDIR}" \( -name '*.o' -o -name '*.obj' \) -print0 \
  | xargs -0 "$AR" crs "${OUTPUT}"
rm -rf "${TMPDIR}"
]=]
)

MESSAGE(STATUS "=== Building DuckDB from submodule (${DUCKDB_SUBMODULE_DIR}) ===")

ExternalProject_Add(duckdb_build
  PREFIX          "${CMAKE_CURRENT_BINARY_DIR}/duckdb-prefix"
  SOURCE_DIR      "${DUCKDB_SUBMODULE_DIR}"
  BINARY_DIR      "${_DUCKDB_BUILD_DIR}"
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${_DUCKDB_BUILD_TYPE}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    "-DCMAKE_CXX_FLAGS=-D_GLIBCXX_USE_CXX11_ABI=0"
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DBUILD_SHELL=OFF
    -DBUILD_UNITTESTS=OFF
    -DENABLE_UNITTEST_CPP_TESTS=OFF
    -DBUILD_PYTHON=OFF
    -DBUILD_BENCHMARKS=OFF
    -DBUILD_TPCE=OFF
    -DDISABLE_BUILTIN_EXTENSIONS=TRUE
    -DENABLE_SANITIZER=FALSE
    -DENABLE_UBSAN=OFF
    -DOVERRIDE_GIT_DESCRIBE=v1.2.1-0-g0000000000
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${DUCKDB_LIB}"
)

# Bundle step: merge all static archives into one fat archive.
ExternalProject_Add_Step(duckdb_build bundle
  COMMAND sh "${CMAKE_CURRENT_BINARY_DIR}/bundle_duckdb.sh"
             "${_DUCKDB_BUILD_DIR}" "${DUCKDB_LIB}" "${CMAKE_AR}"
  DEPENDEES   build
  BYPRODUCTS  "${DUCKDB_LIB}"
  COMMENT     "Bundling DuckDB static libraries into libduckdb_bundle.a"
)

ADD_LIBRARY(libduckdb STATIC IMPORTED GLOBAL)
SET_TARGET_PROPERTIES(libduckdb PROPERTIES IMPORTED_LOCATION "${DUCKDB_LIB}")
ADD_DEPENDENCIES(libduckdb duckdb_build)

MESSAGE(STATUS "DuckDB include: ${DUCKDB_INCLUDE_DIR}")
MESSAGE(STATUS "DuckDB library: ${DUCKDB_LIB}")

INCLUDE_DIRECTORIES(BEFORE SYSTEM "${DUCKDB_INCLUDE_DIR}")
SET(DUCKDB_LIBRARY libduckdb)
