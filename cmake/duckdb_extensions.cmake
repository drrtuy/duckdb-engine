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

# httpfs gives read_parquet() and friends https://, http:// and s3:// paths.
# It is out of tree since DuckDB 1.3; the pin is DuckDB v2.0.0's own
# (.github/config/extensions/httpfs.cmake). Linked statically, like the
# extensions above, because mysqld never installs extensions at runtime.
if(WITH_DUCKDB_HTTPFS)
  duckdb_extension_load(httpfs
      GIT_URL https://github.com/duckdb/duckdb-httpfs
      GIT_TAG 77392a72077a85897db2519fae11e2c0da972b8a)
  duckdb_extension_statically_link(httpfs)
endif()
