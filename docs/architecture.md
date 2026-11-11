# MariaDB ↔ DuckDB Storage Engine: Architecture

## Overview

DuckDB is embedded into MariaDB as a **storage engine plugin** (`ha_duckdb`). MariaDB owns metadata (`.frm` files), client SQL parsing/resolution/authorization, the row-interface plan, and the client protocol. DuckDB parses eligible forwarded source through the `mariadb` PEG dialect, then owns its binding/optimization, columnar execution, data storage, and MVCC transactions. The two systems communicate through MariaDB's handler API and the `select_handler` pushdown interface.

```
┌───────────────────────────────────────────────────────────┐
│                     MariaDB Server                        │
│                                                           │
│  SQL Parser → Optimizer → Executor                        │
│       │            │          │                            │
│       │     ┌──────┘          │                            │
│       │     │  select_handler │  handler API               │
│       │     │  (pushdown)     │  (row-by-row)              │
│       ▼     ▼                 ▼                            │
│  ┌─────────────────────────────────────────┐              │
│  │        ha_duckdb  (storage engine)      │              │
│  │                                         │              │
│  │  DDL convertors ──┐                     │              │
│  │  DML convertors ──┴── SQL generation    │              │
│  │  select_handler ── dialect → AST       │              │
│  │                                         │              │
│  │  DuckdbThdContext (per THD)             │              │
│  │   └─ duckdb::Connection                 │              │
│  │       └─ transaction, session state     │              │
│  │                                         │              │
│  │  DuckdbManager (singleton)              │              │
│  │   └─ duckdb::DuckDB instance            │              │
│  │       └─ catalog, buffer pool, WAL      │              │
│  └─────────────────────────────────────────┘              │
└───────────────────────────────────────────────────────────┘
```

## Plugin Lifecycle

### Initialization (`duckdb_init_func`)

1. Registers handlerton callbacks: `create`, `commit`, `rollback`, `close_connection`, `drop_database`, `create_select`, `create_unit`.
2. `DuckdbManager::CreateInstance()` / `Initialize()` (`runtime/duckdb_manager.cc`) register the statically linked extensions and the `mariadb` dialect, then open `duckdb.db` in the MariaDB datadir.
3. A temporary connection warms the dialect by calling `parse_mariadb_statement(..., "SELECT 1")` (`runtime/mariadb_dialect.cc`). It restores the base dialect before generated initialization SQL. All per-THD connections reuse the compiled grammar; compilation failure fails initialization.
4. Registers SQL macros for MySQL compatibility (`adddate`, `datediff`, `curdate`, `convert_tz`, `substring_index`, `strcmp`, etc.) and C++ UDFs via `register_mariadb_compat_functions()` (`runtime/duckdb_mysql_compat.cc`).
5. Registers cross-engine scan infrastructure: `_mdb_scan` table function + replacement scan callback, then locks the DuckDB configuration with an explicit changeable-setting allowlist. This does not disable external file access; see [security-model.md](security-model.md).

### Shutdown (`duckdb_deinit_func`)

`DuckdbManager::Cleanup()` destroys the DuckDB instance and closes the database file.

---

## Query Processing Paths

### Path 1: SELECT Pushdown (`select_handler`)

Primary path for analytical queries. MariaDB parses, resolves, and authorizes the query; DuckDB receives the **entire eligible SELECT** as a native AST, not rewritten SQL.

```text
MariaDB parse / resolution / authorization
  → hton->create_select / hton->create_unit
    → eligibility + lexical preflight in ha_duckdb_pushdown.cc
      → retain original SELECT/WITH text (or INSERT ... SELECT source tail)
      → create ha_duckdb_select_handler
        → init_scan():
           1. Register external tables and per-table WHERE fragments, if any
           2. Configure the per-THD connection under the base DuckDB dialect
           3. Open the interruptible window
           4. Select the warmed mariadb dialect in process
           5. ExtractStatements(source) once; require exactly one SELECT AST
           6. Restore the base dialect, including on exceptions
           7. Submit(move(AST)); bind / optimize / execute
        → next_row(): Fetch() chunks, convert DuckDB Value → MariaDB Field
        → end_scan(): release stream, clear external registry and interrupt state
```

`ha_duckdb_select_handler::init_scan()` calls `duckdb_stream_mariadb_query()` in `runtime/duckdb_query.cc`. That wrapper calls `parse_mariadb_statement()` in `runtime/mariadb_dialect.cc`; its `MariadbDialectScope` uses `CurrentDialectSetting::SetLocal/ResetLocal`, not SQL SET/RESET. The connection-level wrapper then calls `Connection::Submit(unique_ptr<SQLStatement>)` without rendering or reparsing the AST. The destructor also clears the registry and interrupt state if the MariaDB framework skips `end_scan()` after a metadata-send failure.

The fixed-profile dialect extends the base PEG grammar with `CONVERT`, REGEXP/RLIKE, parenthesized current-time forms, comma LIMIT, postfix ROLLUP, conditionless JOIN, and parsed-and-ignored SELECT/index hints. Its rules live in `runtime/mariadb_dialect_expression.cc` and `runtime/mariadb_dialect_select.cc`; no top-level text rewrites or identifier requoting remain in this path. Hint syntax acceptance does not imply enforcement of hints or STRAIGHT_JOIN ordering.

An eligibility refusal returns `nullptr` from the factory and leaves execution to MariaDB. A later dialect parse, bind, or execution error is returned to the client: there is no retry with another SQL interpretation. "One parse" means one successful DuckDB parse of the forwarded top-level source; MariaDB parsing, generated internal SQL, and explicitly enabled debug reparse verification are separate.

#### Pushdown eligibility

Both the single-SELECT and whole-unit factories in `ha_duckdb_pushdown.cc`
decline pushdown for:

- statement kinds other than SELECT or INSERT ... SELECT, no participating DuckDB table, or an INSERT ... SELECT with a DuckDB target, ON DUPLICATE KEY UPDATE, or RETURNING;
- a source that fails lexical preflight or cannot be retained as a SELECT/WITH source tail;
- an explicit `FOR SYSTEM_TIME` clause, because DuckDB does not implement
  MariaDB system-versioning syntax;
- an explicit reference to an invisible column, including references in the
  select list, `WHERE`, `HAVING`, `GROUP BY`, `ORDER BY`, join conditions, and
  window `PARTITION BY` / `ORDER BY` specifications;
- an external table where a visible column follows an invisible column, because
  `_mdb_scan` can expose only a visible prefix while preserving MariaDB field
  indexes;
- any invisible column on an external table when
  `duckdb_cross_engine_ryow=ON`; the direct handler scan bypasses the synthetic
  MariaDB SELECT that handles hidden metadata and implicit conditions.

With `duckdb_cross_engine_ryow=OFF`, trailing invisible columns are supported:
`_mdb_scan` exposes only the visible prefix, while MariaDB applies implicit
system-versioning conditions inside the synthetic external-table SELECT.
Automatically generated system-versioning field references do not count as
explicit references.

Returning no select handler makes MariaDB use its regular execution path; that
fallback can itself reject a mixed-engine plan if it requests an unsupported
DuckDB handler operation.

The single-SELECT factory additionally declines pushdown when MariaDB marks the
top-level `SELECT_LEX` with `UNCACHEABLE_SIDEEFFECT`. MariaDB sets this flag for:

- reads and assignments of user variables (`@var`, `@var := expr`);
- reads of session or global system variables (`@@session.var`, `@@global.var`);
- `LAST_INSERT_ID()`, `BENCHMARK()`, `SLEEP()`, and `LOAD_FILE()`;
- named-lock functions (`GET_LOCK()`, `IS_FREE_LOCK()`, `IS_USED_LOCK()`,
  `RELEASE_LOCK()`, and `RELEASE_ALL_LOCKS()`);
- `SELECT ... INTO OUTFILE`, `INTO DUMPFILE`, or `INTO` variables;
- the `PROCEDURE` clause.

`UNCACHEABLE_RAND` is a separate flag and does not by itself disable DuckDB
pushdown. This eligibility check currently applies to the single-SELECT factory;
the whole-unit `UNION`/`EXCEPT`/`INTERSECT` factory does not perform the same
side-effect check. The whole-unit factory also declines at PREPARE time; this migration does not add new prepared-statement eligibility.

Lexical preflight is `mariadb_query_has_lexical_mismatch()` (`runtime/mariadb_dialect.cc` with the THD/charset wrapper in `runtime/duckdb_query.cc`). It uses the actual ANSI_QUOTES/NO_BACKSLASH_ESCAPES flags without compiling per-mode grammars. Refusals include hash/dollar openers, executable/nested or boundary-mismatched comments, unsafe quote boundaries, malformed/NUL input, E-prefixed/charset-introduced literals, and unsupported encoding cases. Double-quoted code tokens require ANSI_QUOTES; eligible quoted UTF-8 text must remain byte-identical under DuckDB normalization. This is not another PEG parse. ASCII scanning is allocation-free; non-ASCII normalization uses an owned string. It does not correct remaining value-semantic differences such as boundary-preserving backslash escapes.

Generated DDL/DML, configuration, appender flushes, internal scans, and `run_in_duckdb()` remain under the base DuckDB dialect. Direct whole-statement UPDATE/DELETE remain text paths with original-source lexical checks before transaction registration/appender flush. Their errors and text execution are not a fallback for SELECT parsing.

### Path 1a: Cross-Engine Queries

When a SELECT mixes tables from **different engines** (DuckDB + InnoDB, etc.):

```
ha_duckdb_select_handler::init_scan()
  → registers non-DuckDB tables in a thread-local registry
  → parses the source under mariadb, restores base dialect, submits the AST
    → DuckDB cannot find the table in its catalog
      → replacement scan callback redirects to _mdb_scan('table_name')
        → mdb_scan_function():
           • creates FiberScanState
           • in fiber: new THD, synthetic SELECT * FROM table WHERE …,
             mysql_execute_command()
           • MariaDB result rows → DataChunk via select_to_duckdb interceptor
           • yield() when DataChunk is full, continue() for next chunk
```

The fiber runs on the same OS thread as DuckDB. TLS (`current_thd`, `my_thread_var`) is swapped around fiber spawn/continue calls. The per-table condition is printed by `COND::print()` in `ha_duckdb_select_handler::init_scan()` (`ha_duckdb_pushdown.cc`); `build_synthetic_select()` / `fiber_scan_func()` (`runtime/fiber_scan.cc`) build and execute the synthetic query through MariaDB. This fragment channel is distinct from the top-level DuckDB AST handoff.

#### Pushdown modes: `duckdb_cross_engine_ryow`

For an eligible query, the session variable `duckdb_cross_engine_ryow`
(default `OFF`) selects how external (non-DuckDB) tables are read. The flag is
captured by `ha_duckdb_select_handler::init_scan()` in
`ha_duckdb_pushdown.cc`, which calls `register_cross_engine_ryow()`; the
replacement scan then redirects to one of two table functions. Eligibility is
checked first: with RYOW enabled, an external table containing any invisible
column makes the factory decline pushdown rather than select the direct table
function.

| Aspect | `duckdb_cross_engine_ryow = OFF` (default) | `duckdb_cross_engine_ryow = ON` |
|---|---|---|
| Table function | `_mdb_scan` | `_mdb_scan_direct` |
| Scan mechanism | fiber runs a synthetic `SELECT … WHERE …` via `mysql_execute_command()` | direct `ha_rnd_init` / `ha_rnd_next` on the parent handler |
| Transaction | separate background-THD transaction | parent THD's transaction |
| Visibility | committed data only — the fiber runs in a *separate transaction* (background THD), so the parent's uncommitted writes are **not** visible (isolation follows the server default) | **Read-Your-Own-Writes**: the parent's uncommitted writes are visible |
| MariaDB optimizer for the external table | yes — range/index access, ICP (`idx_cond_push`), range-filter pushdown, index choice | no — plain full table scan |
| DuckDB `filter_pushdown` | `true` — DuckDB removes the predicates from the plan; the fiber applies them via the WHERE registered by `make_cond_for_table()` | `false` — DuckDB keeps the `Filter` operator above the scan and applies predicates itself |
| DuckDB `projection_pushdown` | `true` | `true` |

Why `filter_pushdown` differs: `PhysicalTableScan` returns the table function's
chunk **without re-applying** `table_filters`. When `filter_pushdown = true`
the optimizer strips those predicates and the scan is solely responsible for
them. The direct `ha_rnd_next` path does not evaluate DuckDB `input.filters`,
so `_mdb_scan_direct` sets `filter_pushdown = false` to keep correctness via
DuckDB's own `Filter` operator. Projection pushdown stays enabled in both modes
(the direct path honors `column_ids` through the read set).

Tradeoffs of `ON`: RYOW at the cost of losing the external engine's range/index
access and `idx_cond_push` for that table (it becomes a full scan). Note that
`cond_push` (engine condition pushdown) is unavailable for InnoDB/Aria/MyISAM/
RocksDB regardless of mode — only ICP is, and only in the `OFF` (fiber) path.

#### Future: index access / `idx_cond_push` in the RYOW path

The RYOW path currently loses row-count reduction because it does a full
`ha_rnd_next` scan. This can be improved without giving up RYOW or correctness:

1. `input.filters` (a DuckDB `TableFilterSet`) is already available in
   `mdb_scan_init_global`. Inspect it for sargable predicates
   (`CONSTANT_COMPARISON`, `IS NULL`, `IN`) on a key prefix of some index of
   the external `TABLE`.
2. Replace `ha_rnd_init` with `ha_index_init(keyno)` + `ha_index_read_map()`
   and an `end_range`, iterating with `ha_index_next()`. This recovers range
   access — the larger win — and needs no server `Item` tree.
3. Optionally, for residual index-column predicates, synthesize an `Item`
   expression from the `TableFilter`s over the table's `Field*` and call
   `tbl->file->idx_cond_push(keyno, item)`, so the engine checks them at the
   index level.

Key constraint: keep `filter_pushdown = false` even after this work. In DuckDB's
model the function must apply pushed filters *entirely* (no re-check), so a
partial/best-effort translation would be unsafe. Treating `input.filters` purely
as a seek/ICP *hint* — while DuckDB's `Filter` stays the source of truth — keeps
correctness independent of translation quality.

Caveats: `Item` synthesis needs a THD + memory root (use `tbl->in_use`);
constant/collation coercion between the DuckDB value and the MariaDB `Field`;
only AND-of-per-column predicates are sargable (OR is not); the parent handler
is mid-statement (opened and RDLCK-locked), so switch cleanly between rnd/index
scans and keep a single active scan; InnoDB refuses ICP on indexes with virtual
columns (`idx_cond_push` returns the condition), leaving a residual that must
fall back to DuckDB's `Filter`.

### Path 2: DDL via Handler API

```
MariaDB executor calls handler method
  → ha_duckdb::create() / delete_table() / rename_table() / truncate()
    → DDL convertor generates SQL for DuckDB
      (CreateTableConvertor, RenameTableConvertor, etc.)
    → SQL sent to DuckDB Connection
```

ALTER TABLE uses inplace ALTER:
```
check_if_supported_inplace_alter() → HA_ALTER_INPLACE_NO_LOCK
commit_inplace_alter_table()
  → AddColumnConvertor / DropColumnConvertor / ChangeColumnConvertor /
    ChangeColumnDefaultConvertor / ChangeColumnForPrimaryKeyConvertor
  → all generated operations execute in one explicit DuckDB transaction
```

Table partitioning is disabled with `HTON_NO_PARTITION`; both
`CREATE TABLE ... PARTITION BY` and `ALTER TABLE ... PARTITION BY` are rejected
before MariaDB creates a partition handler or starts the table-copy protocol.

DROP DATABASE: `duckdb_drop_database()` → `DROP SCHEMA IF EXISTS "db"`.

### Path 3: Row-by-Row DML

```
ha_duckdb::write_row() / update_row() / delete_row()
  → duckdb_register_trx(thd)
  → if BatchState == NOT_IN_BATCH:
      InsertConvertor / UpdateConvertor / DeleteConvertor
        → generates SQL: INSERT INTO … VALUES (…) / UPDATE … / DELETE …
        → duckdb_query(connection, sql)
  → if BatchState == IN_INSERT_ONLY_BATCH or IN_MIX_BATCH:
      DeltaAppender::append_row_insert/update/delete()
        → DuckDB Appender API → temp table → flushed at commit
```

`UpdateConvertor` / `DeleteConvertor` identify the row by the primary key
(`DMLConvertor::fill_index_fields_for_where()` in `convertor/dml_convertor.cc`).
**Tables without a primary key are not reliably supported on this path**
(reached e.g. when the table has triggers, or when a replica applies row
events): the WHERE clause then lists every column, and

- a NULL value is emitted as `col = NULL`, which never matches, so the row is
  not updated or deleted although MariaDB counts it as changed;
- the NULL flag is read from the new row while the value comes from the old
  row, so `value → NULL` updates are not applied either;
- exact duplicate rows are all deleted or updated at once.

Keep `duckdb_require_primary_key=ON` (the default) for such workloads.

### Path 3a: Row-Based Replication Apply

A replica applies `Update_rows`/`Delete_rows` events through the same
`update_row()` / `delete_row()` entry points, without reading the row first:

```
Rows_log_event::find_row()                    (sql/log_event_server.cc)
  → unpacks the before-image into record[0]
  → ha_duckdb::rnd_pos_by_record()           // HA_PRIMARY_KEY_REQUIRED_FOR_POSITION
      → returns success without a lookup (only for the replication applier)
Update_rows_log_event::do_exec_row()
  → unpacks the after-image into record[0]
  → ha_duckdb::update_row()
      → rejects the event unless write_set covers all columns
      → batch: DeltaAppender::append_row_update()   (delete marker + full row)
        otherwise: UPDATE … WHERE <before-image PK>
```

Requirements and limitations:

- **The master must log full row images (`binlog_row_image=FULL`, the
  default).** The batch path stores the whole `record[0]` as the new row
  version, so columns absent from a `MINIMAL`, `NOBLOB` or `FULL_NODUP`
  after-image would be replaced by defaults. `ha_duckdb::update_row()` checks
  `write_set`, which `Rows_log_event::do_apply_event()` narrows to the
  after-image columns, and fails the event with an error naming
  `binlog_row_image=FULL` instead. The requirement applies to the master
  table regardless of its engine (InnoDB master → DuckDB replica included).
  The same check fires when the replica table has more columns than the
  master table.
- **Missing or conflicting rows are not detected.** Because no lookup is done,
  a DELETE of a row absent on the replica is a silent no-op; an UPDATE of such
  a row is a no-op without batching and inserts the after-image with batching
  (the flush deletes by key, then inserts). An UPDATE that changes the primary
  key to an existing key replaces that row with batching and leaves a duplicate
  key without batching (DuckDB tables carry no primary key constraint).
  `slave_exec_mode=STRICT` is therefore not enforced for DuckDB tables.
- **Not crash-safe.** The replica position (`mysql.gtid_slave_pos`) and the
  DuckDB data are committed separately.

### Path 4: Direct UPDATE/DELETE (Statement Pushdown)

For single-table statements with simple WHERE (`HA_CAN_DIRECT_UPDATE_AND_DELETE`):
```
ha_duckdb::direct_delete_rows() / direct_update_rows()
  → flush_appenders()          // ensure DuckDB sees consistent data
  → take original SQL from thd_query_string()
  → send entire statement to DuckDB
  → read affected-row count from result
```

### Path 5: Table Scan Fallback (rnd_*)

When `select_handler` is not applicable:
```
rnd_init() → SELECT * FROM "schema"."table" → sent to DuckDB
rnd_next() → reads DataChunk chunk-by-chunk, converts values
rnd_end()  → releases QueryResult
```

---

## Transaction Coordination

```
MariaDB THD
  │
  ├─ external_lock(F_WRLCK/F_RDLCK)
  │    └─ duckdb_register_trx()
  │         ├─ trans_register_ha(stmt)   // register with MariaDB TC
  │         ├─ trans_register_ha(all)    // if explicit transaction
  │         └─ DuckdbThdContext::duckdb_trans_begin()  // BEGIN in DuckDB
  │
  ├─ write_row / update_row / …
  │
  ├─ duckdb_prepare()                   // flush appenders
  │
  ├─ duckdb_commit()
  │    ├─ flush_appenders()             // safety net if prepare was skipped
  │    └─ DuckdbThdContext::duckdb_trans_commit()
  │
  └─ duckdb_rollback()
       └─ DuckdbThdContext::duckdb_trans_rollback()
```

XA transactions are rejected (`reject_xa_if_active`).

---

## Batch DML (DeltaAppender)

For bulk INSERT, the insert-only path appends directly to the target table. For mixed
INSERT/UPDATE/DELETE within a transaction:

1. Creates a **temporary table** cloning the target schema + 3 auxiliary columns (`#mdb_delete_flag`, `#mdb_row_no`, `#mdb_trx_no`).
2. Writes inserted row versions and delete markers via the `duckdb::Appender` API.
3. On `flush()` (at commit/prepare), deletes target rows for every primary key present in the temporary table.
4. For each primary key, selects the temporary row with the greatest `#mdb_row_no` and inserts it unless that latest row is a delete marker.
5. The temp table lives in the connection-local temporary catalog — invisible to other sessions.

---

## SQL Translation Layer

### DDL Convertors (`convertor/ddl_convertor.cc`)
Translate MariaDB DDL → DuckDB DDL:
- Data type mapping (MariaDB types → DuckDB types)
- Expression defaults (read from `TABLE_SHARE::vcol_defs` binary blob, since the Item tree is invalid at `ha_duckdb::create()` time)
- PRIMARY KEY → NOT NULL constraints (DuckDB has no indexes)
- ADD/DROP/CHANGE COLUMN, RENAME TABLE

### DML Convertors (`convertor/dml_convertor.cc`)
Generate INSERT/UPDATE/DELETE SQL from MariaDB row buffers.

### MySQL Compat Functions (`runtime/duckdb_mysql_compat.cc`)
~1000 lines of C++ scalar UDFs registered in DuckDB for MariaDB SQL compatibility: `mid`, `oct`, `bin`, `hex`, `locate`, `addtime`, `subtime`, `regexp_replace`, `field`, `elt`, etc.

### DuckDB Value → MariaDB Field (`convertor/duckdb_select.cc`)
`store_duckdb_field_in_mysql_format()` converts DuckDB `Value` objects to MariaDB `Field::store()` calls when reading query results.

---

## Source Code Map

The map uses the server-tree `storage/duckdb/` prefix; in the out-of-tree checkout these files are at the repository root.

```
storage/duckdb/
├── ha_duckdb.h/cc                — handler class, handlerton, DDL/DML entry points
├── ha_duckdb_pushdown.h/cc       — SELECT/unit eligibility, source extraction, AST streaming
├── duckdb_udf.cc                 — UDF plugin registration
│
├── convertor/
│   ├── ddl_convertor.h/cc        — CREATE/ALTER/DROP/RENAME SQL generation
│   ├── dml_convertor.h/cc        — INSERT/UPDATE/DELETE SQL generation
│   └── duckdb_select.h/cc        — DuckDB Value → MariaDB Field conversion
│
├── runtime/
│   ├── duckdb_manager.h/cc       — DuckdbManager singleton, DuckDB instance lifecycle
│   ├── duckdb_context.h/cc       — DuckdbThdContext: per-THD connection + transactions
│   ├── duckdb_query.h/cc         — base text wrappers and dedicated MariaDB AST stream
│   ├── mariadb_dialect.h/cc      — registration, scope, preflight, SELECT AST extraction
│   ├── mariadb_dialect_expression.cc — expression grammar/transformers
│   ├── mariadb_dialect_select.cc — SELECT grammar/transformers
│   ├── mariadb_dialect*_test.cc  — standalone parser/lifecycle/parity tests
│   ├── duckdb_mysql_compat.h/cc  — C++ UDFs for MySQL function compatibility
│   ├── cross_engine_scan.h/cc    — _mdb_scan table function + replacement scan
│   ├── fiber_context.h/c         — stackful coroutine (fiber) primitives
│   ├── fiber_scan.h/cc           — fiber-based MariaDB scan for cross-engine queries
│   └── delta_appender.h/cc       — batch DML via Appender API + temp tables
│
├── common/
│   ├── duckdb_config.h/cc        — global sysvar definitions
│   ├── duckdb_charset_collation.h/cc
│   ├── duckdb_timezone.h/cc
│   ├── duckdb_types.h/cc         — DatabaseTableNames, path parsing
│   ├── duckdb_log.h/cc           — logging control
│   ├── duckdb_handler_errors.h   — error code constants
│   └── row_helpers.h             — byte-level row reading utilities
│
├── cmake/                        — DuckDB build integration (ExternalProject_Add)
└── third_parties/duckdb/         — upstream DuckDB submodule (v2.0.0)
```
