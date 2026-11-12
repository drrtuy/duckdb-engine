# Extensions required by the DuckDB storage engine plugin for MariaDB.
# This config is passed to DuckDB via DUCKDB_EXTENSION_CONFIGS.
# Keep in sync with _DUCKDB_LINKED_EXTENSIONS in cmake/duckdb.cmake.

duckdb_extension_load(core_functions)
duckdb_extension_statically_link(core_functions)
duckdb_extension_load(icu)
duckdb_extension_statically_link(icu)
duckdb_extension_load(json)
duckdb_extension_statically_link(json)
duckdb_extension_load(parquet)
duckdb_extension_statically_link(parquet)
# Quack client/server protocol: third_parties/duckdb-quack (pinned to the
# commit DuckDB itself pins) plus patches/duckdb-quack, staged by
# cmake/duckdb.cmake into MARIADB_QUACK_SOURCE_DIR.
duckdb_extension_load(quack
    SOURCE_DIR ${MARIADB_QUACK_SOURCE_DIR}
    INCLUDE_DIR ${MARIADB_QUACK_SOURCE_DIR}/src/include
)
duckdb_extension_statically_link(quack)
