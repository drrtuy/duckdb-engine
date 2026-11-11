# The MariaDB side of the Quack bridge (MDEV-41383, see Quack.MD): implements
# quack_guard's QuackAuthorizer on MariaDB accounts and grants. Sees only the
# bridge interface of quack_guard (quack_guard_api), never its internals.
INCLUDE(${PROJECT_SOURCE_DIR}/cmake/duckdb_target_setup.cmake)

ADD_LIBRARY(quack_bridge STATIC
  acl_worker.cc
  mariadb_authorizer.cc
  bridge.cc
  account_row.cc
)
duckdb_setup_target(quack_bridge)
TARGET_INCLUDE_DIRECTORIES(quack_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
TARGET_LINK_LIBRARIES(quack_bridge PUBLIC quack_guard_api)
