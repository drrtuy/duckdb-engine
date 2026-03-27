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

The engine is built as part of the MariaDB server tree. It lives under `storage/duckdb/` and uses `ExternalProject_Add` to build DuckDB from source (submodule at `third_parties/duckdb/`).

```bash
# Clone MariaDB Server
git clone https://github.com/MariaDB/server.git mariadb-server
cd mariadb-server

# Clone the DuckDB engine into the storage directory (with submodules)
git clone --recurse-submodules https://github.com/drrtuy/duckdb-engine.git storage/duckdb

# Build
./storage/duckdb/build.sh
```

## License

This project is licensed under the GNU General Public License v2. See [COPYING](COPYING) for details.

DuckDB itself is licensed under the MIT License.

## Acknowledgments

**Alibaba and the AliSQL Project** — A special thank you to Alibaba and their engineering team for open-sourcing [AliSQL](https://github.com/alibaba/AliSQL). AliSQL is a MySQL branch developed at Alibaba Group and extensively used in their production infrastructure. The December 2025 open-source release of AliSQL 8.0 included integration of DuckDB as a native storage engine, providing a valuable reference implementation and validating the viability of embedding DuckDB inside a MySQL-compatible server. The DuckDB Engine for MariaDB draws heavily on this experience.

**The DuckDB Project** — None of this would be possible without the remarkable work of the [DuckDB team](https://github.com/duckdb/duckdb). DuckDB has grown from an academic research project into one of the most impressive analytical engines available today — fast, embeddable, dependency-free, and released under the permissive MIT License. Its clean C++ codebase and well-defined embedding API are what make integrations like this one feasible.
