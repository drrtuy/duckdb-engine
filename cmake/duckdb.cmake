# Copyright (c) 2026, MariaDB Foundation.
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
# Build DuckDB static libraries from submodule source.
#
# The upstream DuckDB repo lives at storage/duckdb/third_parties/duckdb/
# as a git submodule.  We build it via ExternalProject so its CMake targets
# (including one named "duckdb") don't clash with the MariaDB plugin target
# of the same name created by MYSQL_ADD_PLUGIN().
#

SET(DUCKDB_SUBMODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_parties/duckdb")
SET(DUCKDB_INCLUDE_DIR   "${DUCKDB_SUBMODULE_DIR}/src/include")

INCLUDE(ExternalProject)

# Map MariaDB build type to a DuckDB-friendly one.
IF(CMAKE_BUILD_TYPE MATCHES "[Dd]ebug")
  SET(_DUCKDB_BUILD_TYPE "Debug")
ELSE()
  SET(_DUCKDB_BUILD_TYPE "Release")
ENDIF()

SET(_DUCKDB_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/duckdb-build")

# Statically linked DuckDB capabilities and extensions, in load order.
# Extensions must also be listed in cmake/duckdb_extensions.cmake.
# local_extension_repository is left out on purpose: it would redirect
# automatic installs to the DuckDB build tree.
SET(_DUCKDB_LINKED_EXTENSIONS
  httplib loadable_extensions core_functions icu json parquet quack)

SET(_DUCKDB_STATIC_LIBS
  "${_DUCKDB_BUILD_DIR}/src/main/http/libduckdb_httplib.a"
  "${_DUCKDB_BUILD_DIR}/src/main/extension/libduckdb_loadable_extensions.a"
  "${_DUCKDB_BUILD_DIR}/extension/core_functions/libcore_functions_extension.a"
  "${_DUCKDB_BUILD_DIR}/extension/icu/libicu_extension.a"
  "${_DUCKDB_BUILD_DIR}/extension/json/libjson_extension.a"
  "${_DUCKDB_BUILD_DIR}/extension/parquet/libparquet_extension.a"
  "${_DUCKDB_BUILD_DIR}/extension/quack/libquack_extension.a"
  "${_DUCKDB_BUILD_DIR}/src/libduckdb_static.a"
)

MESSAGE(STATUS "=== Building DuckDB from submodule (${DUCKDB_SUBMODULE_DIR}) ===")
IF(NOT EXISTS "${DUCKDB_SUBMODULE_DIR}/CMakeLists.txt")
  MESSAGE(FATAL_ERROR "DuckDB submodule is missing: run 'git submodule update --init third_parties/duckdb'")
ENDIF()
IF(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_parties/duckdb-quack/CMakeLists.txt")
  MESSAGE(FATAL_ERROR "Quack submodule is missing: run 'git submodule update --init third_parties/duckdb-quack'")
ENDIF()

# Quack serves HTTPS through httplib's OpenSSL backend. OpenSSL was found in
# CMakeLists.txt; Quack's own find_package(OpenSSL) in the DuckDB build gets
# the same paths (see ExternalProject_Add below).

SET(_QUACK_SUBMODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_parties/duckdb-quack")

# The submodule must be the commit DuckDB itself pins for this release.
FILE(STRINGS "${DUCKDB_SUBMODULE_DIR}/.github/config/extensions/quack.cmake"
     _QUACK_PIN_LINE REGEX "GIT_TAG")
STRING(REGEX MATCH "[0-9a-f]+$" _QUACK_PIN "${_QUACK_PIN_LINE}")
EXECUTE_PROCESS(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY "${_QUACK_SUBMODULE_DIR}"
  OUTPUT_VARIABLE _QUACK_HEAD OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _rc ERROR_QUIET)
IF(_rc EQUAL 0 AND NOT _QUACK_HEAD STREQUAL _QUACK_PIN)
  MESSAGE(FATAL_ERROR "third_parties/duckdb-quack is at ${_QUACK_HEAD}, "
          "but DuckDB pins Quack ${_QUACK_PIN}")
ENDIF()

# Quack is built from a patched copy of the submodule, so the submodule itself
# stays clean. Patches are applied in this order; each is security-relevant,
# see Quack.MD.
SET(_QUACK_PATCHES
  0001-random-token-from-openssl.patch
  0002-disable-quack-serve.patch
  0003-client-loopback-only.patch
)
LIST(TRANSFORM _QUACK_PATCHES
     PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/patches/duckdb-quack/")
SET(_QUACK_STAGING_DIR "${CMAKE_CURRENT_BINARY_DIR}/duckdb-quack-staging")
SET(QUACK_SOURCE_DIR   "${CMAKE_CURRENT_BINARY_DIR}/duckdb-quack-src")
FILE(REMOVE_RECURSE "${_QUACK_STAGING_DIR}")
FILE(COPY "${_QUACK_SUBMODULE_DIR}/CMakeLists.txt" "${_QUACK_SUBMODULE_DIR}/src"
     DESTINATION "${_QUACK_STAGING_DIR}")
FIND_PROGRAM(PATCH_EXECUTABLE patch)
IF(NOT PATCH_EXECUTABLE)
  MESSAGE(FATAL_ERROR "'patch' is needed to build Quack")
ENDIF()
FOREACH(_patch IN LISTS _QUACK_PATCHES)
  EXECUTE_PROCESS(
    # Not git apply: inside the repo's build dir it silently skips the paths.
    COMMAND ${PATCH_EXECUTABLE} -p1 --forward --batch --no-backup-if-mismatch
            --fuzz=0 --input "${_patch}"
    WORKING_DIRECTORY "${_QUACK_STAGING_DIR}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  IF(NOT _rc EQUAL 0)
    MESSAGE(FATAL_ERROR "Cannot apply ${_patch} to Quack: ${_out}${_err}")
  ENDIF()
ENDFOREACH()

# Mirror staging into QUACK_SOURCE_DIR. CONFIGURE_FILE(COPYONLY) rewrites a
# file only when its content changes, so incremental builds stay incremental;
# files no longer in staging are removed.
FILE(GLOB_RECURSE _QUACK_FILES RELATIVE "${_QUACK_STAGING_DIR}"
     "${_QUACK_STAGING_DIR}/*")
FILE(GLOB_RECURSE _QUACK_OLD_FILES RELATIVE "${QUACK_SOURCE_DIR}"
     "${QUACK_SOURCE_DIR}/*")
FOREACH(_f IN LISTS _QUACK_OLD_FILES)
  IF(NOT _f IN_LIST _QUACK_FILES)
    FILE(REMOVE "${QUACK_SOURCE_DIR}/${_f}")
  ENDIF()
ENDFOREACH()
FOREACH(_f IN LISTS _QUACK_FILES)
  CONFIGURE_FILE("${_QUACK_STAGING_DIR}/${_f}" "${QUACK_SOURCE_DIR}/${_f}" COPYONLY)
ENDFOREACH()
# Re-stage when a submodule file or a patch changes.
FILE(GLOB_RECURSE _QUACK_SUBMODULE_FILES CONFIGURE_DEPENDS
     "${_QUACK_SUBMODULE_DIR}/CMakeLists.txt" "${_QUACK_SUBMODULE_DIR}/src/*")
SET_PROPERTY(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${_QUACK_SUBMODULE_FILES} ${_QUACK_PATCHES})

SET(_DUCKDB_GIT_DESCRIBE "v2.0.0")

# DuckDB v2 no longer builds an extension loader archive. Render its
# static_extension_loader.c.in template (as duckdb_write_static_extension_loader()
# in extension/extension_build_tools.cmake does) into a source defining
# duckdb_register_static_extensions(), which DuckdbManager calls before
# opening the database.
SET(LINK_EXTENSION_LIST "")
SET(DESCRIBE_DECLARATIONS "")
SET(DESCRIBE_REGISTRATIONS "")
FOREACH(_ext IN LISTS _DUCKDB_LINKED_EXTENSIONS)
  STRING(APPEND LINK_EXTENSION_LIST " ${_ext}")
  STRING(APPEND DESCRIBE_DECLARATIONS
    "int32_t duckdb_extension_${_ext}_describe(duckdb_extension_descriptor *descriptor);\n")
  STRING(APPEND DESCRIBE_REGISTRATIONS
    "\tif (duckdb_register_static_extension(duckdb_extension_${_ext}_describe) != 0) {\n\t\tresult = 1;\n\t}\n")
ENDFOREACH()
STRING(STRIP "${LINK_EXTENSION_LIST}" LINK_EXTENSION_LIST)
SET(DUCKDB_STATIC_EXTENSION_LOADER
    "${CMAKE_CURRENT_BINARY_DIR}/duckdb_static_extension_loader.c")
CONFIGURE_FILE(
  "${DUCKDB_SUBMODULE_DIR}/extension/loader/static_extension_loader.c.in"
  "${DUCKDB_STATIC_EXTENSION_LOADER}" @ONLY)

SET(_DUCKDB_EXTRA_CXX_FLAGS "")
# GCC 16+ warns about DuckDB classes in SFINAE contexts (upstream issue, not ours).
# Silence it for DuckDB's own ExternalProject build too.
CHECK_CXX_COMPILER_FLAG("-Wsfinae-incomplete" have_CXX__Wsfinae_incomplete)
IF(have_CXX__Wsfinae_incomplete)
  STRING(APPEND _DUCKDB_EXTRA_CXX_FLAGS " -Wno-sfinae-incomplete")
ENDIF()

IF(WITH_ASAN)
  SET(ADDRESS_SANITIZER TRUE)
ELSE()
  SET(ADDRESS_SANITIZER FALSE)
ENDIF()

IF(WITH_UBSAN)
  SET(UB_SANITIZER TRUE)
ELSE()
  SET(UB_SANITIZER FALSE)
ENDIF()

ExternalProject_Add(duckdb_build
  PREFIX          "${_DUCKDB_BUILD_DIR}"
  SOURCE_DIR      "${DUCKDB_SUBMODULE_DIR}"
  BINARY_DIR      "${_DUCKDB_BUILD_DIR}"
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${_DUCKDB_BUILD_TYPE}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DBUILD_SHELL=OFF
    -DBUILD_UNITTESTS=OFF
    -DENABLE_UNITTEST_CPP_TESTS=OFF
    -DBUILD_PYTHON=OFF
    -DBUILD_BENCHMARKS=OFF
    -DBUILD_TPCE=OFF
    -DEXTENSION_STATIC_BUILD=1
    "-DDUCKDB_EXTENSION_CONFIGS=${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb_extensions.cmake"
    "-DMARIADB_QUACK_SOURCE_DIR=${QUACK_SOURCE_DIR}"
    "-DOPENSSL_INCLUDE_DIR=${OPENSSL_INCLUDE_DIR}"
    "-DOPENSSL_SSL_LIBRARY=${OPENSSL_SSL_LIBRARY}"
    "-DOPENSSL_CRYPTO_LIBRARY=${OPENSSL_CRYPTO_LIBRARY}"
    "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS} ${_DUCKDB_EXTRA_CXX_FLAGS}"
    "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}"
    "-DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
    "-DCMAKE_MODULE_LINKER_FLAGS=${CMAKE_MODULE_LINKER_FLAGS}"
    "-DCMAKE_C_LINKER_LAUNCHER=${CMAKE_C_LINKER_LAUNCHER}"
    "-DCMAKE_CXX_LINKER_LAUNCHER=${CMAKE_CXX_LINKER_LAUNCHER}"
    -DENABLE_SANITIZER=${ADDRESS_SANITIZER}
    -DENABLE_UBSAN=${UB_SANITIZER}
    -DOVERRIDE_GIT_DESCRIBE=${_DUCKDB_GIT_DESCRIBE}
  INSTALL_COMMAND  ""
  BUILD_BYPRODUCTS ${_DUCKDB_STATIC_LIBS}
  USES_TERMINAL_BUILD ON
)

# The build step is not BUILD_ALWAYS; re-run it when the staged Quack sources
# (submodule + patches) change.
FILE(GLOB_RECURSE _QUACK_STAGED_FILES "${QUACK_SOURCE_DIR}/*")
ExternalProject_Add_StepDependencies(duckdb_build build ${_QUACK_STAGED_FILES})

# Expose all DuckDB archives as a single INTERFACE target so the rest of the
# cmake tree links against "libduckdb" unchanged.
ADD_LIBRARY(libduckdb INTERFACE)
# OpenSSL after the archives: libquack_extension.a uses it.
TARGET_LINK_LIBRARIES(libduckdb INTERFACE -Wl,--start-group ${_DUCKDB_STATIC_LIBS} -Wl,--end-group
  OpenSSL::SSL OpenSSL::Crypto)
ADD_DEPENDENCIES(libduckdb duckdb_build)

MESSAGE(STATUS "DuckDB include: ${DUCKDB_INCLUDE_DIR}")
MESSAGE(STATUS "DuckDB libs:    ${_DUCKDB_STATIC_LIBS}")

INCLUDE_DIRECTORIES(BEFORE SYSTEM "${DUCKDB_INCLUDE_DIR}")
INCLUDE_DIRECTORIES(BEFORE SYSTEM "${DUCKDB_SUBMODULE_DIR}/third_party/re2")
SET(DUCKDB_LIBRARY libduckdb)
