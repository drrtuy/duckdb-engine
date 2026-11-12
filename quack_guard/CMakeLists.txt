# quack_guard: DuckDB-side Quack access control (MDEV-41383, see Quack.MD).
# Must not depend on MariaDB headers; add it before the plugin's
# LINK_LIBRARIES(mariadb_private ...).
INCLUDE(${PROJECT_SOURCE_DIR}/cmake/duckdb_target_setup.cmake)

ADD_LIBRARY(quack_guard STATIC
  src/guard.cpp
  src/lockdown.cpp
  src/plan_gate.cpp
  src/guarded_server.cpp
  src/login.cpp
  src/session_registry.cpp
  src/transport.cpp
  src/allowlist.cpp
  src/statement_gate.cpp
  src/catalog_filter.cpp
)
duckdb_setup_target(quack_guard)

TARGET_INCLUDE_DIRECTORIES(quack_guard
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Quack's private headers, as its own build sees them.
TARGET_INCLUDE_DIRECTORIES(quack_guard SYSTEM PRIVATE
  ${QUACK_SOURCE_DIR}/src/include
  ${DUCKDB_SUBMODULE_DIR}/third_party/httplib
  ${DUCKDB_SUBMODULE_DIR}/third_party/mbedtls/include
  ${DUCKDB_SUBMODULE_DIR}/third_party/fmt/include
  ${OPENSSL_INCLUDE_DIR})
TARGET_COMPILE_DEFINITIONS(quack_guard PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

IF(DUCKDB_UNIT_TESTS)
  # Layer A: quack_guard on plain DuckDB with a mock authorizer, no MariaDB.
  ADD_EXECUTABLE(quack_guard_test test/guard_test.cpp
                 ${DUCKDB_STATIC_EXTENSION_LOADER})
  duckdb_setup_target(quack_guard_test)
  TARGET_INCLUDE_DIRECTORIES(quack_guard_test PRIVATE
                             ${CMAKE_CURRENT_SOURCE_DIR}/src)
  TARGET_INCLUDE_DIRECTORIES(quack_guard_test SYSTEM PRIVATE
    ${QUACK_SOURCE_DIR}/src/include
    ${DUCKDB_SUBMODULE_DIR}/third_party/httplib
    ${DUCKDB_SUBMODULE_DIR}/third_party/mbedtls/include
    ${DUCKDB_SUBMODULE_DIR}/third_party/fmt/include
    ${OPENSSL_INCLUDE_DIR})
  TARGET_COMPILE_DEFINITIONS(quack_guard_test PRIVATE
    CPPHTTPLIB_OPENSSL_SUPPORT
    QUACK_TEST_CERT_DIR="${PROJECT_SOURCE_DIR}/third_parties/duckdb-quack/test/certs")
  TARGET_LINK_LIBRARIES(quack_guard_test PRIVATE quack_guard ${DUCKDB_LIBRARY}
                        atomic pthread dl)
ENDIF()

# The bridge interface only, for code that implements QuackAuthorizer
# (the MariaDB side) without seeing quack_guard's internals.
ADD_LIBRARY(quack_guard_api INTERFACE)
TARGET_INCLUDE_DIRECTORIES(quack_guard_api INTERFACE
                           ${CMAKE_CURRENT_SOURCE_DIR}/include)
