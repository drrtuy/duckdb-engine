--disable_query_log
--disable_warnings
CREATE FUNCTION IF NOT EXISTS duckdb_query_udf RETURNS STRING SONAME 'ha_duckdb.so';
--enable_warnings
--enable_query_log
