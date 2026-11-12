# DuckDB Storage Engine for MariaDB

A pluggable storage engine that brings [DuckDB](https://github.com/duckdb/duckdb)'s columnar analytical engine inside [MariaDB Server](https://github.com/MariaDB/server).

Create a table with `ENGINE=DuckDB` and analytical queries against it are executed through DuckDB's columnar vectorized engine — no ETL pipelines, no separate cluster, no additional protocols. One server, one SQL interface, the familiar `mariadb` client.

## Use Cases

- **HTAP (Hybrid Transactional/Analytical Processing)** — InnoDB handles OLTP, DuckDB handles analytics, both in the same database.
- **Ad-hoc analytical queries** — complex joins, aggregations, subqueries, and window functions over large datasets without exporting data to a separate system.
- **Eliminating ETL complexity** — no need for a dedicated analytical cluster or data movement pipelines; the analytical engine runs in-process.

## Roadmap

- **Analytical GIS** — deliver geospatial analytics via DuckDB's Spatial extension.
- **Faster HTAP over InnoDB-only data** — run queries against InnoDB-only data through DuckDB's vectorized execution.
- **Partial query pushdown** — implement a derived handler to push parts of complex queries down into DuckDB.
- **Data lake access** — reach data lake protocols and formats via DuckDB extensions.

## Performance

TPC-H Scale Factor 10 (~11 GB raw data, 86.6M rows total, ~60M in `lineitem`).

**Hardware / environment:** Intel Core i7-13700H (14 cores / 20 threads), 64 GB RAM, NVMe SSD (ext4). MariaDB 11.4.14 with embedded DuckDB v2.0.0, default `duckdb_memory_limit`, `threads=20`. Single warm run (`tpch/run_all.sh`); numbers stable to ~±15%.

| Metric | Result |
|---|---|
| Data loading (Parquet via `read_parquet()`) | 33 seconds |
| All 22 TPC-H queries | **3.75 seconds** total |

### Per-query latency (seconds, warm)

| Query | Time | Query | Time | Query | Time |
|---|---:|---|---:|---|---:|
| q01 | 0.208 | q09 | 0.353 | q17 | 0.108 |
| q02 | 0.066 | q10 | 0.305 | q18 | 0.335 |
| q03 | 0.153 | q11 | 0.059 | q19 | 0.142 |
| q04 | 0.140 | q12 | 0.138 | q20 | 0.131 |
| q05 | 0.130 | q13 | 0.462 | q21 | 0.227 |
| q06 | 0.064 | q14 | 0.109 | q22 | 0.095 |
| q07 | 0.129 | q15 | 0.089 | **Total** | **3.75** |
| q08 | 0.135 | q16 | 0.170 | | |

Each query also pays a small fixed cost (~40 ms) for client connect and pushdown setup — noticeable on the cheapest queries, negligible on the heavy analytical ones. See [`docs/tpch_sf10_duckdb_v2_benchmark.md`](docs/tpch_sf10_duckdb_v2_benchmark.md) for the comparison with DuckDB v1.5.2 (4.30 s total), and [`docs/tpch_sf10_query_benchmark.md`](docs/tpch_sf10_query_benchmark.md) and [`docs/tpch_sf10_ingestion_benchmark.md`](docs/tpch_sf10_ingestion_benchmark.md) for the v1.5.2 methodology.

## Tutorials

Step-by-step guides for loading and analyzing open datasets with the DuckDB storage engine:

- [NYC Taxi Trips](docs/tutorials/nyc-taxi-trips.md) — loading Parquet via `run_in_duckdb()`, then cross-engine joins between `ENGINE=DuckDB` and `ENGINE=InnoDB` tables. Follows the ClickHouse taxi tutorial.
- [OWID CO₂ Emissions](docs/tutorials/owid-co2-emissions.md) — loading a CSV dataset and running aggregations and window functions.

## How It Works

DuckDB is an in-process analytical database. Its performance rests on three pillars:

- **Columnar storage** — minimizes unnecessary data reads.
- **Vectorized execution** — processes data in batches for maximum CPU cache efficiency.
- **Parallelism** — leverages all available processor cores.

Tables created with `ENGINE=DuckDB` store data in DuckDB's native format. InnoDB and DuckDB tables coexist in the same database.

MariaDB still parses, resolves, and authorizes the client statement first. SELECT/source pushdown then uses DuckDB's `mariadb` PEG dialect and an AST handoff, not SQL text rewriting. "One parse" means one DuckDB parse of the forwarded source, not removal of MariaDB parsing or of generated internal SQL parsing. The select/unit factories in `ha_duckdb_pushdown.cc` guard the original source via `extract_source_query()`; `ha_duckdb_select_handler::init_scan()` then calls `duckdb_stream_mariadb_query()` in `runtime/duckdb_query.cc`. Its `parse_mariadb_statement()` helper (`runtime/mariadb_dialect.cc`) scopes `CurrentDialectSetting::SetLocal()` / `ResetLocal()`, calls `ExtractStatements()` once, and requires exactly one SELECT. The base dialect is restored before `Submit(AST)`: no top-level rewrites, requoting, or text retries. Dialect selection uses no SQL `SET`/`RESET` or per-query grammar compilation. The dialect is registered and warmed by `DuckdbManager::Initialize()` (`runtime/duckdb_manager.cc`).

```text
Original SELECT/source → lexical guard → DuckDB mariadb PEG → SELECT AST
                                         base restored → Submit(AST) → streamed rows
Factory refusal → normal MariaDB row interface (success is not guaranteed)
Late parse/bind/execute error → error, no fallback
```

This is a fixed-profile dialect, not full MariaDB `sql_mode` semantics. Direct UPDATE/DELETE still use guarded, requoted text; generated DDL/DML and `run_in_duckdb()` remain on the base DuckDB dialect. See [`docs/security-model.md`](docs/security-model.md) for the forwarding boundary and remaining external-access gap.

## Building

The engine is built outside the MariaDB server tree against an installed MariaDB plugin SDK (`find_package(mariadb-plugin)`, MDEV-40608). DuckDB is built from the `third_parties/duckdb` submodule via `ExternalProject_Add`, currently pinned to `5406f514e6aa` (post-v2.0.0 tag).

### Prerequisites

A MariaDB installation that provides both the server and the Development component (`share/cmake/mariadb-plugin/`), of the same version, under one prefix. Either:

- the `MariaDB-devel` (RPM) or `libmariadb-dev` (DEB) package of a MariaDB release that includes MDEV-40608, or
- a server built from source and installed together with the Development component, e.g. `cmake --install <server-build> --component Development`.

The plugin is ABI-bound to the server it was built against: `build.sh` refuses to install it into a server whose version differs from the SDK's.

### Build and install

```bash
git clone --recurse-submodules git@github.com:drrtuy/duckdb-engine.git
cd duckdb-engine

# Install build dependencies (requires root)
./build.sh -D

# Build only (into ./build)
./build.sh -t RelWithDebInfo -c

# Build, install into the server, keep the existing datadir and start the server
./build.sh -t RelWithDebInfo -n -S
```

Without `-n`, `build.sh` deletes and re-initializes the datadir (`/var/lib/mysql` by default).

`cmake --install` puts `ha_duckdb.so` into the server plugin directory, `duckdb.cnf` into the server's additional config directory and the MTR suite into `<mariadb-test>/plugin/duckdb`. If the server does not read that config directory, load the engine with `INSTALL SONAME 'ha_duckdb';`.

Paths and layout:

| Option | Default | Meaning |
|---|---|---|
| `--mariadb-prefix <dir>` | `/usr` | MariaDB installation with the Development component; used as `CMAKE_PREFIX_PATH` and as the server prefix |
| `--build-dir <dir>` | `./build` | Plugin build directory |
| `--datadir <dir>` | `/var/lib/mysql` | Server data directory |
| `--layout DEB\|RPM\|STANDALONE` | `RPM` for `-p rpm`, `STANDALONE` for `-p tgz`, otherwise `DEB` | Install layout of the server; must match it, otherwise the plugin lands where the server does not look for it |

Run `./build.sh -h` for the full list of options.

### Build packages

```bash
./build.sh -t RelWithDebInfo -p rpm   # Rocky/Fedora/Amazon Linux
./build.sh -t RelWithDebInfo -p deb   # Debian/Ubuntu
./build.sh -t RelWithDebInfo -p tgz
```

### Tests (MTR)

`run_mtr.sh` runs the suite with the installed MTR (`<prefix>/share/mariadb/mariadb-test`, override with `--mtr-dir`) against the installed engine, so run `build.sh` without `-c` first. Source tests are copied into the installed suite before each run, and `-r` copies recorded results back to `mysql-test/duckdb/r/`.

```bash
./run_mtr.sh create_table_column      # one test
./run_mtr.sh -r create_table_column   # run and record
./run_mtr.sh -a                       # all tests
./run_mtr.sh -a -e                    # all tests against a running server
```

### Standalone dialect tests

The `mariadb_dialect_test` target is excluded from the default build (`runtime/CMakeLists.txt`). With the build prerequisites above, build and run it without installing the plugin or starting MariaDB:

```bash
cmake -S . -B build
cmake --build build --target mariadb_dialect_test -j 4
./build/runtime/mariadb_dialect_test
./build/runtime/mariadb_dialect_test --benchmark  # warmed parser-only p50/p95/p99
```

The standalone tests cover lexical preflight, single-parse AST handoff, and paired grammar/AST/result checks. Integration regressions include `duckdb_lexical_guard` and `duckdb_peg_pushdown`. Local RelWithDebInfo verification on 2026-11-10 passed all 57 enabled MTR runs (including replication combinations), with one debug-only skip and 24 disabled tests. This working-tree result includes separately updated DuckDB-pin expected results; it is not full security or release-readiness evidence. Release performance work remains: concurrency, end-to-end p95/p99 latency, and a production query corpus. Parser-only timings do not measure those workloads or the old text-rewrite cost.

## Cross-Engine Queries

The engine supports cross-engine joins — a single `SELECT` can combine DuckDB tables with tables from other engines (e.g. InnoDB). When the query planner detects a mix of engines, the `select_handler` does the following:

1. Opens the non-DuckDB tables via MariaDB's handler API and registers them in a thread-local table registry.
2. Derives a per-table predicate from the query's `WHERE` (via `make_cond_for_table` in `ha_duckdb_select_handler::init_scan()`, `ha_duckdb_pushdown.cc`) and registers it alongside each external table.
3. Pushes the **entire query** — including all `WHERE`, `JOIN`, `GROUP BY`, and `ORDER BY` clauses — down to DuckDB through the MariaDB PEG SELECT AST handoff.
4. DuckDB's replacement scan callback transparently redirects references to non-DuckDB tables to the `_mdb_scan` table function.

The `_mdb_scan` table function does **not** loop over the handler directly. On its first call it lazily spawns a **cooperative fiber** (stack-switching runtime borrowed from `libmariadb`) running on a dedicated background `THD`. Inside the fiber it builds a synthetic, projection- and predicate-pushed
`SELECT <needed columns> FROM <db>.<table> [WHERE <pushed predicate>]`
and executes it through the **full MariaDB pipeline** (`lex_start` → `parse_sql` → `mysql_execute_command`, called in `fiber_scan_func()`, `runtime/fiber_scan.cc`). The predicate is printed with `COND::print()` in `ha_duckdb_select_handler::init_scan()` (`ha_duckdb_pushdown.cc`); `build_synthetic_select()` (`runtime/fiber_scan.cc`) builds the synthetic SQL. This SELECT is parsed by MariaDB, not by the DuckDB MariaDB dialect. A custom `select_result_interceptor` (`select_to_duckdb`) converts each result row's `Item`s into a DuckDB `DataChunk`; when a chunk fills (`STANDARD_VECTOR_SIZE`) the fiber **yields** back to DuckDB, which consumes the chunk and resumes the fiber for the next batch.

Because the external scan runs through `mysql_execute_command`, it uses the **MariaDB optimizer and all available access paths** (index range/ref access, etc.), and the pushed `WHERE` predicate is evaluated by MariaDB *before* rows reach DuckDB. On the DuckDB side, the full query AST lets DuckDB's optimizer reorder joins, build hash tables, and run all aggregation/sorting over the streamed rows. A direct `ha_rnd_next` loop is retained only as a fallback when no fiber is used.

This means queries like the following just work:

```sql
SELECT d.id, d.amount, i.name
  FROM analytics.orders d          -- ENGINE=DuckDB
  JOIN inventory.products i        -- ENGINE=InnoDB
    ON d.product_id = i.id
 WHERE d.amount > 1000;
```

DuckDB handles the join, aggregation, and sorting; InnoDB rows are produced on demand by a fiber-driven MariaDB query. No data copying or ETL is required.

## Current Limitations

- **DECIMAL precision > 38** — DuckDB supports up to 38 digits; wider MariaDB DECIMALs will fail on DDL conversion.
- **Some MariaDB functions are yet not pushdown-compatible** — `GROUP_CONCAT()`, `DATE_FORMAT()`, `JSON_CONTAINS()`, `FOUND_ROWS()`, `LAST_INSERT_ID()`, and a few others have no DuckDB equivalent or differ in syntax. Factory refusal leaves execution to MariaDB's normal row interface, which is not guaranteed to succeed. Once pushdown is accepted, a late parse, bind, or execution error is returned to the caller; incompatible functions do not trigger an error-time fallback.
- **Strict GROUP BY** — DuckDB rejects `SELECT` columns not in `GROUP BY` and not aggregated, even when MariaDB's `sql_mode` allows it.
- **XA transactions** — `XA PREPARE` is not supported by the engine.
- **Table partitioning** — `CREATE TABLE ... PARTITION BY` and converting a DuckDB table with `ALTER TABLE ... PARTITION BY` are not supported; the engine declares `HTON_NO_PARTITION`.
- **Collations** — MariaDB UCA-based collation rules are approximated via DuckDB's built-in `NOCASE`/`NOACCENT` collations for UTF-8 charsets; non-UTF8 charsets fall back to binary comparison. See [`docs/collation-mapping.md`](docs/collation-mapping.md) for the full mapping and known gaps.
- **Cross-engine scan is yet single-threaded** — each external (non-DuckDB) table is produced by a single fiber-driven MariaDB query (`_mdb_scan` reports `MaxThreads() == 1`); only the DuckDB side of the query is parallelized.
- **ALTER COLUMN DROP DEFAULT** — not propagated to DuckDB catalog.
- **Tables without a primary key** (allowed only with `duckdb_require_primary_key=OFF`) — row-by-row `UPDATE`/`DELETE` (used e.g. with triggers or on a replica) silently skips rows containing NULL values and changes all copies of duplicate rows. See [`docs/architecture.md`](docs/architecture.md#path-3-row-by-row-dml).
- **Row-based replication into DuckDB tables requires `binlog_row_image=FULL` on the master** (the default). A replica rejects `UPDATE` events with a partial after-image (`MINIMAL`, `NOBLOB`, `FULL_NODUP`) instead of filling the missing columns with defaults. The replica does not detect missing or conflicting rows (`slave_exec_mode=STRICT` is not enforced) and is not crash-safe. See [`docs/architecture.md`](docs/architecture.md#path-3a-row-based-replication-apply).
- **Timezone propagation** — `TIMESTAMP` columns are stored as `TIMESTAMPTZ`; timezone must be set consistently between MariaDB and DuckDB contexts to avoid shifts.

See [`docs/mariadb-duckdb-incompatibilities.md`](docs/mariadb-duckdb-incompatibilities.md) for a detailed compatibility matrix.

## License

This project is licensed under the GNU General Public License v2. See [COPYING](COPYING) for details.

DuckDB itself is licensed under the MIT License.

## Acknowledgments

**Alibaba and the AliSQL Project** — A special thank you to Alibaba and their engineering team for open-sourcing [AliSQL](https://github.com/alibaba/AliSQL). AliSQL is a MySQL branch developed at Alibaba Group and extensively used in their production infrastructure. The December 2025 open-source release of AliSQL 8.0 included integration of DuckDB as a native storage engine, providing a valuable reference implementation and validating the viability of embedding DuckDB inside a MySQL-compatible server. The DuckDB Engine for MariaDB draws heavily on this experience.

**The DuckDB Project** — None of this would be possible without the remarkable work of the [DuckDB team](https://github.com/duckdb/duckdb). DuckDB has grown from an academic research project into one of the most impressive analytical engines available today — fast, embeddable, dependency-free, and released under the permissive MIT License. Its clean C++ codebase and well-defined embedding API are what make integrations like this one feasible.
