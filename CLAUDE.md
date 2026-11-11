# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

DuckDB storage engine plugin for MariaDB. Creates tables with `ENGINE=DuckDB` that store data in DuckDB's columnar format and execute analytical queries through DuckDB's vectorized engine. The server-tree location is `storage/duckdb/`; this repository builds out of tree against an installed MariaDB plugin SDK.

## Build

```bash
# Build + install + start MariaDB, preserving the existing datadir
./build.sh -n -S -t Debug

# CI mode (build only, no install)
./build.sh -c -t Debug

# Build a DEB package (rpm and tgz are also supported)
./build.sh -p deb -t RelWithDebInfo
```

The out-of-tree build uses the installed MariaDB plugin SDK and defaults to `./build` (`build.sh`, override with `--build-dir`). Use `-n` when installing to preserve the existing datadir; without it the script deletes and reinitializes that directory. DuckDB is built from `third_parties/duckdb/` via `ExternalProject_Add` and merged into `libduckdb_bundle.a`.

## Tests (MTR)

Tests use MariaDB's MTR (MySQL Test Runner) framework. Test files live in `mysql-test/duckdb/t/` with expected results in `mysql-test/duckdb/r/`.

```bash
# Run a single test
./run_mtr.sh create_table_column

# Run and record new expected output
./run_mtr.sh -r create_table_column

# Run all tests
./run_mtr.sh -a

# Run against an externally running MariaDB
./run_mtr.sh -e create_table_column

# Run all against extern server
./run_mtr.sh -a -e
```

The standalone PEG dialect test covers Phases 0–3 and is excluded from the default build. Build and run it without installing the plugin or starting MariaDB:

```bash
cmake -S . -B build
cmake --build build --target mariadb_dialect_test -j 4
./build/runtime/mariadb_dialect_test
```

The test wraps the pinned DuckDB PEG transformer entry with the linker to count actual parses, including a positive control using debug reparse verification. Its mangled symbol in `runtime/CMakeLists.txt` must be rechecked on DuckDB API bumps. Production code has no parser counters. Phase 1 adds a table-driven lexical corpus and asserts that preflight itself never invokes the PEG parser. The integration regression is `mysql-test/duckdb/t/duckdb_lexical_guard.test`. The `mariadb` dialect is registered and warmed in `DuckdbManager::Initialize()` (`runtime/duckdb_manager.cc`), and Phase 3 SELECT/source pushdown uses it through `duckdb_stream_mariadb_query()` (`runtime/duckdb_query.cc`). The new integration regression is `mysql-test/duckdb/t/duckdb_peg_pushdown.test` (including column-grant and Unicode-normalization safety cases). Phase 2 grammar deltas live in `runtime/mariadb_dialect_expression.cc` and `runtime/mariadb_dialect_select.cc`, applied by `MariadbDialect::ApplyGrammarChanges()` in `runtime/mariadb_dialect.cc`. Paired tests compare ASTs, result rows/types, byte-exact column names, error categories, and parameter order against normalized base SQL. Public PEG transformer headers require the submodule's `third_party/utf8proc/include` as a private include path. Run `./build/runtime/mariadb_dialect_test --benchmark` for warmed parser-only p50/p95/p99 measurements; this does not measure MariaDB end-to-end latency or old text-rewrite cost.

`run_mtr.sh` tests the installed plugin, not automatically `build/ha_duckdb.so`; it copies source tests into the installed suite before each run and copies results back only with `-r`. After building/installing the matching plugin, run `./run_mtr.sh -a -j 4`. Local RelWithDebInfo verification on 2026-11-10 passed all 57 runs after updating expected type/error rendering for the DuckDB pin; 1 test requires a debug build and 24 remain disabled. This is not coverage of the disabled scenarios or a full release/security gate.

Installed MTR can also test a source suite and the freshly built plugin without installation: run `mtr` from the installed `mariadb-test` directory with an absolute `--suite` path, a fresh `--vardir` under `build/`, and `--mysqld=--plugin-dir=<source>/build`. The installed runner splits suite names at `-`, so source paths containing dashes need a dash-free symlink for `--suite`. Preserve the suite's utf8mb4 server charset/collation settings when running this way.

## Architecture

All engine code is in the `myduck` namespace (except `ha_duckdb` which is in global scope per MariaDB handler convention).

> **Doc convention:** when this file (or a code comment) names a specific API to describe control flow (e.g. `SELECT_LEX::print()`), it must also name the file/function where that API is actually called. If no call site can be pointed to, describe the behavior instead of naming the API — do not imply a code path that isn't there.

### Key components

- **`ha_duckdb`** (`ha_duckdb.cc/h`) — MariaDB `handler` subclass. Entry point for all storage engine operations (open, close, read, write, DDL). Implements row-at-a-time interface for MariaDB, translating to DuckDB batch operations.

- **`DuckdbManager`** (`duckdb_manager.cc/h`) — Singleton owning the `duckdb::DuckDB` instance. Created once at plugin init, creates connections for each thread. Database file stored as `duckdb.db` in MariaDB data directory.

- **`DuckdbThdContext`** (`duckdb_context.cc/h`) — Per-thread context holding a `duckdb::Connection`, transaction state, and appenders. Attached to MariaDB's THD. Manages BEGIN/COMMIT/ROLLBACK lifecycle and session variable propagation (timezone, optimizer flags, collation).

- **DDL/DML Convertors** (`ddl_convertor.cc/h`, `dml_convertor.cc/h`) — Translate MariaDB SQL structures (TABLE, Field, Alter_info) into DuckDB-compatible SQL strings. Handle identifier quoting differences (MariaDB backticks → DuckDB double quotes) and type mapping.

- **`DeltaAppender`** (`delta_appender.cc/h`) — Batched write path. Accumulates INSERT/UPDATE/DELETE rows using DuckDB's Appender API into a temporary buffer table, then flushes as a single DuckDB DML statement at commit time. `DeltaAppenders` manages per-table appender instances.

- **Select handler / Query pushdown** (`ha_duckdb_pushdown.cc/h`) — Implements MariaDB's `select_handler` interface for eligible SELECT, whole set-operation units, and INSERT ... SELECT source tails with a non-DuckDB target. Registered via `hton->create_select` and `hton->create_unit`. Supports pure-DuckDB queries and cross-engine joins; passes one parsed AST to the streaming executor.

- **Cross-engine scan** (`cross_engine_scan.cc/h`) — Enables DuckDB to read from non-DuckDB tables (e.g. InnoDB) during cross-engine joins. Registers a `_mdb_scan` table function and a replacement scan callback in DuckDB. Uses a thread-local registry of external tables.

- **Type mapping** (`duckdb_types.cc/h`) — Converts between MariaDB field types and DuckDB types. `store_duckdb_field_in_mysql_format()` reads DuckDB values back into MariaDB row format.

### SQL generation conventions

DuckDB follows the SQL standard and delimits identifiers with **double quotes**, not backticks.

SQL reaches DuckDB by these routes:

- **SELECT/source pushdown is an AST handoff.** `extract_source_query()` and the select/unit factories in `ha_duckdb_pushdown.cc` apply lexical eligibility and retain SELECT text or extract an INSERT ... SELECT tail. `init_scan()` calls `duckdb_stream_mariadb_query()` in `runtime/duckdb_query.cc`: configure under the base dialect, parse once through `parse_mariadb_statement()`, restore the base dialect, then `Submit(unique_ptr<SQLStatement>)`. There are no top-level syntax rewrites, requoting, or text-submission retries on this path. The dedicated streaming helpers assume the factories have preflighted the source; the parser/AST helper alone is not a lexical or authorization boundary.
- **Direct UPDATE/DELETE still use SQL text.** Their original-source preflight runs before transaction registration/appender flush; the generic `duckdb_query()` wrappers retain identifier requoting and their execution checks.
- **Generated SQL and fragments remain separate.** Generated DDL/DML/configuration, appender flushes, and administrative `run_in_duckdb()` SQL use the base DuckDB dialect and retain `backticks_to_double_quotes()` where required. DDL default-expression text is read by `get_default_expr_from_vcol_defs()` in `convertor/ddl_convertor.cc` from `TABLE_SHARE::vcol_defs`, not printed locally. Cross-engine per-table conditions use `COND::print()` in `ha_duckdb_pushdown.cc`; `build_synthetic_select()` / `fiber_scan_func()` in `runtime/fiber_scan.cc` embed those fragments in a query parsed and executed by **MariaDB**, not by the DuckDB dialect.

Factory eligibility refusal returns `nullptr` and permits MariaDB's normal row interface, which may itself reject an unsupported plan. A late dialect parse/bind/execute error is an error, never a retry through text execution. `next_row()` fetches streaming chunks; `end_scan()` releases the stream and clears cross-engine/interrupt state. The select-handler destructor also clears that state if metadata sending fails before the framework reaches `end_scan()` (`ha_duckdb_pushdown.cc`). There is no source-based rescan cache; any future rescan support must retain/copy the AST rather than parse again.

`mariadb_query_has_lexical_mismatch()` has a THD/charset wrapper in `runtime/duckdb_query.cc` and a bounded preflight in `runtime/mariadb_dialect.cc`. It refuses unsupported comment/dollar-quote forms, quote boundaries/classes, literal prefixes, malformed regions, and encoding cases even under `NO_BACKSLASH_ESCAPES`. Double-quoted code tokens require `ANSI_QUOTES`; otherwise SELECT declines pushdown, preventing a MariaDB string from becoming an unauthorized DuckDB column reference. Non-ASCII eligible text is also checked with `Parser::NormalizeSQLString()` before handoff; changed or invalid text is refused without a PEG parse. The ASCII preflight remains allocation-free. Non-boundary backslash value differences retain the existing parity policy.

Security audit at DuckDB pin `5406f514e6a` (2026-11-10): `DuckdbManager::Initialize()` (`runtime/duckdb_manager.cc`) locks configuration but does not disable `enable_external_access`. An isolated server reported it as `true`; a SUPER-only `run_in_duckdb()` call read a freshly created harmless canary file. This is the remaining MDEV-40613 hardening/coverage gap, not proof of unprivileged SQL injection. `duckdb_v2_migration.test` does not assert that setting, so a green MTR suite does not establish that external I/O is disabled. `duckdb_identifier_escaping.test` covers MDEV-40653 including a double-quoted schema name; `duckdb_peg_pushdown.test` covers the MDEV-41018 double-quoted WHERE fallback and low-privilege hash/dollar smuggling in all four ANSI_QUOTES/NO_BACKSLASH_ESCAPES combinations. `duckdb_default_value_injection.test` matches the MDEV-40610 Jira testcase: a CREATE-only user, without FILE/SUPER, supplies the crafted literal DEFAULT; the test asserts marker-file absence and exact default-value round-trip. That covers literal defaults, not all expression-default or generated-fragment cases. See `docs/security-model.md` for the authorization boundary and residual risks.

Confirmed release blocker: `finalize_current_time()` in `runtime/mariadb_dialect_expression.cc` constructs a `ColumnRefExpression` for CURRENT_DATE/TIME/TIMESTAMP calls. A local isolated probe with SELECT(id) only on a DuckDB table containing a `current_date` DATE column returned that ungranted column's sentinel through `SELECT CURRENT_DATE()`; the InnoDB control returned the real date and explicit column access was denied. This is an AST semantic/authorization defect, not a demonstrated bypass of the hash/dollar lexical guard. Passing the legacy-normalized parity fixtures or the 57 enabled MTR runs does not establish security acceptance. Preserve function semantics and add column-grant/shadowing regressions before considering the cutover security-complete; a custom tokenizer alone would not fix this transformer defect.

See `docs/mariadb-duckdb-incompatibilities.md` for known function name rewrites and type mapping issues.

Phase 3 finalizes the matcher graph in `MariadbMatcherFactory` (`runtime/mariadb_dialect.cc`) through the matcher-factory API before publication: consume terminal overrides once, install native Identifier memoization, build the root, compute FIRST sets, and initialize list chains. The later root lookup by DuckDB must remain idempotent with the empty base override map. Recheck this lifecycle and native memoized-terminal list on submodule bumps. No DuckDB core patch or post-publication matcher mutation is used.

### DuckDB source and patches

DuckDB source is at `third_parties/duckdb/`, pinned to `5406f514e6aa23a43ec4413cc89ebe3e73dd4cd4` (`v2.0.0-3524-g5406f514e6a`, a post-tag 2.0 revision), built without local patches. New database files use the v2.0.0 storage format; the source pin is not the original v2.0.0 tag. See `docs/mariadb-peg-dialect-migration-design.md` for implementation status, parser-only and limited end-to-end measurements, and the still-open corpus/concurrency/tail-latency/release-margin gates. Statically linked extensions are listed in `cmake/duckdb_extensions.cmake` and in `_DUCKDB_LINKED_EXTENSIONS` in `cmake/duckdb.cmake`, which renders DuckDB's static extension loader template into `duckdb_static_extension_loader.c`; `DuckdbManager::Initialize()` calls `duckdb_register_static_extensions()` before opening the database. Other compatibility is handled at runtime via `duckdb_mysql_compat.cc`. The build produces a static library; `_GLIBCXX_DEBUG` is explicitly undefined in CMakeLists.txt to avoid ABI mismatch with MariaDB's debug build.

### Configuration

`duckdb.cnf` — MariaDB config snippet that loads `ha_duckdb.so`. Installed to `/etc/my.cnf.d/`.
