# DuckDB Storage Engine for MariaDB

A pluggable storage engine that brings [DuckDB](https://github.com/duckdb/duckdb)'s columnar analytical engine inside [MariaDB Server](https://github.com/MariaDB/server).

Create a table with `ENGINE=DuckDB` and analytical queries against it are executed through DuckDB's columnar vectorized engine — no ETL pipelines, no separate cluster, no additional protocols. One server, one SQL interface, the familiar `mariadb` client.

## Use Cases

- **HTAP (Hybrid Transactional/Analytical Processing)** — InnoDB handles OLTP, DuckDB handles analytics, both in the same database.
- **Ad-hoc analytical queries** — complex joins, aggregations, subqueries, and window functions over large datasets without exporting data to a separate system.
- **Eliminating ETL complexity** — no need for a dedicated analytical cluster or data movement pipelines; the analytical engine runs in-process.

## Performance

TPC-H Scale Factor 10 (~10 GB raw data, ~60M rows in `lineitem`):

| Metric | Result |
|---|---|
| Data loading | 250 seconds |
| All 22 TPC-H queries | **3.7 seconds** total |

## How It Works

DuckDB is an in-process analytical database. Its performance rests on three pillars:

- **Columnar storage** — minimizes unnecessary data reads.
- **Vectorized execution** — processes data in batches for maximum CPU cache efficiency.
- **Parallelism** — leverages all available processor cores.

Tables created with `ENGINE=DuckDB` store data in DuckDB's native format. Queries are translated and executed by the DuckDB engine. InnoDB and DuckDB tables coexist in the same database.

## Building

The engine is built as part of the MariaDB server tree. It lives under `storage/duckdb/` and uses `ExternalProject_Add` to build upstream DuckDB v1.5.2 from source (git submodule at `third_parties/duckdb/`).

Until the patch is accepted into MariaDB upstream, clone one of the prepared branches from the server fork:

```bash
# Pick the branch matching your target MariaDB version
git clone --recurse-submodules -b bb-11.4-duckdb https://github.com/drrtuy/mdb-server.git mariadb-server
# or: -b 11.8-duckdb
# or: -b 12.3-duckdb
cd mariadb-server

# Install build dependencies (requires root)
./storage/duckdb/duckdb/build.sh -D

# Build and install
./storage/duckdb/duckdb/build.sh
```

### Build packages

**RPM** (Rocky/Fedora/Amazon Linux):

```bash
./storage/duckdb/duckdb/build.sh -p
```

**DEB** (Debian/Ubuntu):

```bash
./storage/duckdb/duckdb/build.sh -p
```

## Cross-Engine Queries

The engine supports cross-engine joins — a single `SELECT` can combine DuckDB tables with tables from other engines (e.g. InnoDB). When the query planner detects a mix of engines, it:

1. Opens the non-DuckDB tables via MariaDB's handler API.
2. Registers them in a thread-local table registry.
3. Pushes the **entire query** — including all `WHERE`, `JOIN`, `GROUP BY`, and `ORDER BY` clauses — down to DuckDB as a single SQL statement.
4. DuckDB's replacement scan callback transparently redirects references to non-DuckDB tables to the `_mdb_scan` table function, which streams rows from the MariaDB handler into DuckDB's vectorized pipeline.

Because the full query text (with filters) is pushed down, DuckDB's optimizer can apply predicates, reorder joins, and build hash tables over the streamed InnoDB rows — the non-DuckDB side is a full table scan, but DuckDB handles all the filtering and aggregation internally.

This means queries like the following just work:

```sql
SELECT d.id, d.amount, i.name
  FROM analytics.orders d          -- ENGINE=DuckDB
  JOIN inventory.products i        -- ENGINE=InnoDB
    ON d.product_id = i.id
 WHERE d.amount > 1000;
```

DuckDB handles the join, aggregation, and sorting; InnoDB rows are streamed in on demand. No data copying or ETL is required.
InnoDB and other engines tables are scanned via `ha_rnd_next` and predicate pushdown functionality is WIP.

## Current Limitations

- **DECIMAL precision > 38** — DuckDB supports up to 38 digits; wider MariaDB DECIMALs will fail on DDL conversion.
- **Some MariaDB functions are yet not pushdown-compatible** — `GROUP_CONCAT()`, `DATE_FORMAT()`, `JSON_CONTAINS()`, `FOUND_ROWS()`, `LAST_INSERT_ID()`, and a few others have no DuckDB equivalent or differ in syntax. Such queries fall back to MariaDB execution.
- **Strict GROUP BY** — DuckDB rejects `SELECT` columns not in `GROUP BY` and not aggregated, even when MariaDB's `sql_mode` allows it.
- **XA transactions** — `XA PREPARE` is not supported by the engine.
- **Collations** — MariaDB UCA-based collation rules are approximated via DuckDB's built-in `NOCASE`/`NOACCENT` collations for UTF-8 charsets; non-UTF8 charsets fall back to binary comparison. See [`docs/collation-mapping.md`](docs/collation-mapping.md) for the full mapping and known gaps.
- **Cross-engine scan is yet single-threaded** — external (non-DuckDB) tables are scanned via `ha_rnd_next` on a single thread; only the DuckDB side of the query is parallelized.
- **ALTER COLUMN DROP DEFAULT** — not propagated to DuckDB catalog.
- **Timezone propagation** — `TIMESTAMP` columns are stored as `TIMESTAMPTZ`; timezone must be set consistently between MariaDB and DuckDB contexts to avoid shifts.

See [`docs/mariadb-duckdb-incompatibilities.md`](docs/mariadb-duckdb-incompatibilities.md) for a detailed compatibility matrix.

## License

This project is licensed under the GNU General Public License v2. See [COPYING](COPYING) for details.

DuckDB itself is licensed under the MIT License.

## Acknowledgments

**Alibaba and the AliSQL Project** — A special thank you to Alibaba and their engineering team for open-sourcing [AliSQL](https://github.com/alibaba/AliSQL). AliSQL is a MySQL branch developed at Alibaba Group and extensively used in their production infrastructure. The December 2025 open-source release of AliSQL 8.0 included integration of DuckDB as a native storage engine, providing a valuable reference implementation and validating the viability of embedding DuckDB inside a MySQL-compatible server. The DuckDB Engine for MariaDB draws heavily on this experience.

**The DuckDB Project** — None of this would be possible without the remarkable work of the [DuckDB team](https://github.com/duckdb/duckdb). DuckDB has grown from an academic research project into one of the most impressive analytical engines available today — fast, embeddable, dependency-free, and released under the permissive MIT License. Its clean C++ codebase and well-defined embedding API are what make integrations like this one feasible.
