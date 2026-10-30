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
