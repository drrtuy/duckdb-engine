# Disabled Tests Analysis & Work Plan

Status as of 2026-04-14. **Enabled: 17/47 tests. Disabled: 30.**

### Done this session (12 → 17)

| Test | Fix |
|------|-----|
| `duckdb_set_operation` | Already passing after charset fix |
| `truncate_and_maintenance_duckdb_table` | Re-record for MariaDB ANALYZE output |
| `duckdb_db_table_strconvert` | Re-record for UDF API |
| `duckdb_monitor` | Fix `direct_delete/update_rows` counters + rewrite test |
| `charset_and_collation` | Fix collation in master.opt + error codes |

### Engine fixes done

- `ER_DUCKDB_*` error codes (4206–4213) with codegen from `duckdb_errors.txt`
- `report_duckdb_table_struct_error`: `ER_DUCKDB_TABLE_STRUCT_INVALID` for ALTER, `ER_ILLEGAL_HA_CREATE_OPTION` for CREATE
- `execute_dml`: `ER_DUCKDB_QUERY_ERROR` for row-at-a-time DML failures
- `direct_delete_rows` / `direct_update_rows`: increment `Duckdb_rows_delete` / `Duckdb_rows_update`
- `external_lock`: reject DML on DuckDB tables inside XA transactions (`ER_XAER_RMFAIL`)
- `have_duckdb.inc`: engine check + utf8mb4 charset setup
- `my.cnf`: `character-set-server=utf8mb4`
- Build fixes: `CREATE_TYPELIB_FOR`, `HA_EXTRA_*_ALTER_COPY`

## Group A: Missing SQL functions in DuckDB pushdown (6 tests)

MariaDB pushes SELECT into DuckDB, but DuckDB lacks these functions.

| Test | Line | Error | Missing function |
|------|------|-------|------------------|
| `duckdb_time_func` | 24 | `Scalar Function with name adddate does not exist!` | `ADDDATE()` |
| `duckdb_string_func` | 124 | `Scalar Function with name insert does not exist!` | `INSERT(str,pos,len,new)` |
| `duckdb_fix_sql` | 26 | `Scalar Function with name oct does not exist!` | `oct()` |
| `duckdb_sql_syntax` | 5 | `syntax error at or near "WITH"` | `WITH ROLLUP` |
| `duckdb_numeric_func` | 27 | `ACOS is undefined outside [-1,1]` | Not a missing function — DuckDB is stricter on domain checks |
| `duckdb_agg_func` | 53 | `No function matches 'avg(VARCHAR)'` | `AVG()` on string types not supported |

**Fix:** Implement SQL rewrite in `ha_duckdb_pushdown.cc` — intercept `SELECT_LEX::print()` output and rewrite:
- `adddate(x, interval)` → `x + interval` or `date_add(x, interval)`
- `insert(s,p,l,n)` → `overlay(s placing n from p for l)`
- `oct(x)` → implement via `printf('%o', x)` or refuse pushdown
- `WITH ROLLUP` → refuse pushdown (return NULL from `create_duckdb_select_handler`)
- `ACOS` domain error — wrap in `CASE WHEN ... BETWEEN -1 AND 1` or refuse pushdown
- `AVG(VARCHAR)` — refuse pushdown when argument is non-numeric

## Group B: Decimal precision >38 (3 tests)

DuckDB max: DECIMAL(38,x). MariaDB supports up to DECIMAL(65,30).

| Test | Line | Error |
|------|------|-------|
| `decimal_high_precision` | 35 | `Could not cast value ... to DECIMAL(38,30)` |
| `decimal_precision_all_possibilities` | 48 | `Could not cast value ... to DECIMAL(38,5)` |
| `feature_duckdb_data_type` | 74 | Same — extreme decimal insert |

**Fix:** In `ddl_convertor.cc` map `DECIMAL(>38, scale)` to `DOUBLE` (when `duckdb_use_double_for_decimal=ON`) or `DECIMAL(38, min(scale, 38-intg))` with truncation. In `delta_appender.cc` — analogous fallback on append. Test `decimal_high_precision` also needs PK (already added in working tree).

## Group C: Wrong error code (3 remaining tests)

Error code fixes done: `ER_DUCKDB_TABLE_STRUCT_INVALID` for ALTER structural errors, `ER_DUCKDB_QUERY_ERROR` for DML, XA DML rejection. `charset_and_collation` **DONE** (enabled).

| Test | Status | Remaining issue |
|------|--------|-----------------|
| ~~`charset_and_collation`~~ | **DONE** | — |
| `rename_duckdb_table` | Error codes fixed, result updated | Server log warnings during cross-schema rename test |
| `bugfix_temp_and_system_database` | Error code `ER_DUCKDB_QUERY_ERROR` fixed | `DROP TABLE t1` in DuckDB "temp" schema fails — DuckDB internal schema conflict |
| `duckdb_refuse_xa` | XA DML rejection implemented | INSERT after `XA COMMIT` in PREPARED state fails — need to handle XA lifecycle correctly |

## Group D: Engine features not implemented (5 tests)

| Test | Line | Error | What's needed |
|------|------|-------|---------------|
| `alter_default_debug` | 23 | INSERT after `ALTER COLUMN DROP DEFAULT` succeeds — should fail with `ER_NO_DEFAULT_FOR_FIELD` | Implement `ALTER COLUMN DROP DEFAULT` in `ChangeColumnDefaultConvertor` |
| `duckdb_ddl_during_transaction` | 43 | INSERT after DDL in transaction succeeds — should fail with `ER_DUCKDB_APPENDER_ERROR` | Invalidate appender after DDL within a transaction |
| `create_table_constraint` | 11 | `CREATE TABLE ... index idx_id(name)` fails | Ignore non-unique index in CREATE TABLE (as already done for ALTER) |
| `duckdb_require_primary_key` | 24 | `ALTER TABLE t ADD COLUMN` succeeds without PK — should fail with `ER_REQUIRES_PRIMARY_KEY` | Check PK presence on ALTER TABLE when `duckdb_require_primary_key=ON` |
| `supported_copy_ddl` | 10 | `cross-schema rename is not supported` | Implement cross-schema rename via COPY (CREATE + INSERT + DROP) |

## Group E: Result mismatch / UDF issues (3 tests)

| Test | Problem |
|------|---------|
| `duckdb_add_backticks` | UDF `duckdb_query_udf` returns `[Rows: 0]` for digit-name schemas (`09898141`) — DuckDB information_schema can't find them because schema names starting with digits need quoting |
| `duckdb_appender_allocator_flush_threshold` | `appender_allocator_flush_threshold` setting doesn't exist in upstream DuckDB v1.3.2 (AliSQL fork only) |
| `duckdb_bit_string` | `WHERE col = x'41'` returns empty result — hex/binary literal comparison via pushdown is broken, likely `SELECT_LEX::print()` outputs `x'41'` which DuckDB doesn't understand |

**Fix:**
- `duckdb_add_backticks`: quote schema/table names in DuckDB DDL queries (e.g. `CREATE SCHEMA IF NOT EXISTS "09898141"`)
- `duckdb_appender_allocator_flush_threshold`: mark N/A or remove checks for this AliSQL-only setting
- `duckdb_bit_string`: rewrite hex literals in pushdown SQL or handle in `SELECT_LEX::print()` post-processing

## Group F: Server / external issues (5 tests)

| Test | Problem | Complexity |
|------|---------|------------|
| `duckdb_allow_encryption` | MySQL `keyring_file` plugin, `default-table-encryption` don't exist in MariaDB | Rewrite for MariaDB encryption API or N/A |
| `system_timezone` | Hangs on `mariadbd-safe` restart | Adapt restart mechanism for MariaDB |
| `duckdb_alter_table_engine` | Server crash: `Assertion 'len > alloc_length' failed` on 64MB JSON | MariaDB server bug, not duck |
| `duckdb_kill` | Timeout 900s | `simulate_interrupt_duckdb_row/chunk` DEBUG sync points not implemented |
| `bugfix_crash_after_commit_error` | `--skip TODO` in the test itself | Test needs to be written |

## Group G: SQL mode / index handling (2 tests)

| Test | Line | Error |
|------|------|-------|
| `duckdb_sql_mode` | 13 | `column "id" must appear in the GROUP BY clause` — DuckDB is stricter than MariaDB without `ONLY_FULL_GROUP_BY` |
| `alter_duckdb_index` | 18 | `Duplicate key name 'uk_b'` — DuckDB ignores indexes but MariaDB remembers their names |

**Fix:**
- `duckdb_sql_mode`: pass permissive GROUP BY setting to DuckDB on pushdown, or refuse pushdown when `ONLY_FULL_GROUP_BY` is off
- `alter_duckdb_index`: don't register ignored index names in MariaDB metadata

## Group H: Timestamp/timezone (1 test)

| Test | Problem |
|------|---------|
| `create_table_column_timestamp` | Checksums InnoDB vs DuckDB don't match — timezone offset applied incorrectly |

**Fix:** Fix timezone propagation in `config_duckdb_session` — DuckDB sees wrong timezone on INSERT/SELECT timestamps.

---

## Priority work plan

| # | Task | Tests unblocked | Complexity | Status |
|---|------|-----------------|------------|--------|
| ~~3~~ | ~~**Error code fixes**~~ | ~~4~~ | ~~low–medium~~ | **DONE** (1 enabled, 3 partially fixed) |
| 1 | **SQL function rewrite** in pushdown: `adddate→+interval`, `insert→overlay`, `oct`, `WITH ROLLUP→no pushdown` | 4 | medium | TODO |
| 2 | **Decimal >38 fallback** to DOUBLE or truncated DECIMAL(38) | 3 | medium | TODO |
| 4 | **ALTER COLUMN DROP DEFAULT** propagate to DuckDB | 1 | medium | TODO |
| 5 | **Appender invalidation** after DDL in transaction | 1 | medium | TODO |
| 6 | **Index handling**: ignore non-unique index in CREATE TABLE, don't register ignored index names | 2 | medium | TODO |
| 7 | **UDF digit-name schemas** — quote schema/table names in DuckDB queries | 1 | medium | TODO |
| 8 | **Hex/binary literal** in WHERE via pushdown | 1 | medium | TODO |
| 9 | **AVG(VARCHAR)** / strict GROUP BY — refuse pushdown | 2 | medium | TODO |
| 10 | **Numeric function domain** (ACOS outside [-1,1]) | 1 | medium | TODO |
| 11 | **require_primary_key on ALTER** | 1 | low | TODO |
| 12 | **Timezone propagation** | 1 | medium | TODO |
| 13 | **Cross-schema rename via COPY** | 1 | high | TODO |
| 14 | **appender_allocator_flush_threshold** — AliSQL-only setting | 1 | low | TODO (N/A or adapt) |
| 15 | **KILL/interrupt** — DEBUG sync points | 1 | high | TODO |
| 16 | **bugfix_crash_after_commit_error** — test marked TODO | 1 | unknown | TODO |
| 17 | **Encryption** — MySQL→MariaDB | 1 | high | TODO |
| 18 | **system_timezone** — mariadbd-safe restart | 1 | high | TODO |
| 19 | **Server crash** on 64MB JSON | 1 | out of scope | MariaDB server bug |

Items 1–2 unblock **7 tests**. Items 4–12 add **11 more**. Items 13–19 are complex or external.
