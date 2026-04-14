if (`SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.ENGINES WHERE engine = 'DuckDB' AND support IN ('YES', 'DEFAULT', 'ENABLED')`)
{
  --skip Test requires engine DuckDB.
}

# DuckDB only supports UTF-8. The test database is bootstrapped with latin1,
# so override it here to avoid 'non-utf8 charset' errors on CREATE TABLE.
--disable_query_log
ALTER DATABASE test CHARACTER SET utf8mb4;
SET NAMES utf8mb4;
--enable_query_log
