# TPC-H SF10 — MariaDB DuckDB engine on DuckDB v2.0.0 vs v1.5.2

Per-query latency of the 22 TPC-H queries and Parquet load time after the
migration of the embedded DuckDB from v1.5.2 to v2.0.0.

## Hardware

| Component | Value |
|---|---|
| CPU | 13th Gen Intel Core i7-13700H (14 cores: 6 P + 8 E, 20 threads) |
| RAM | 64 GB |
| Storage | NVMe SSD, ext4; data in `/git/tpch/sf10` |

## Environment

| | v1.5.2 (baseline) | v2.0.0 (current) |
|---|---|---|
| MariaDB | 11.4.13 | 11.4.14, RelWithDebInfo |
| Embedded DuckDB | v1.5.2 | v2.0.0 (storage format v2.0.0) |
| `duckdb_memory_limit` | 8 GiB | 0 (DuckDB default) |
| DuckDB threads | 20 | 20 (`duckdb_max_threads=0`) |
| Source data | CSV, `tpchgen-cli` v2.0.2 | Parquet, `tpchgen-cli` v2.0.2 |
| Load path | in-engine `COPY` | `INSERT ... SELECT * FROM read_parquet()` via `run_in_duckdb` |
| Tables | `ENGINE=DUCKDB`, natural-key PKs | `ENGINE=DUCKDB`, natural-key PKs |
| Queries | MariaDB dialect, `/tpch.sql` | MariaDB dialect, `tpch/tpch.sql` |
| Baseline source | [`tpch_sf10_query_benchmark.md`](tpch_sf10_query_benchmark.md) | `tpch/run_all.sh` |

## Methodology

Both sides use the same per-query measurement: one `mariadb` client invocation
per query, wall-clock time, output discarded. Each number includes the ~40 ms
fixed client-connect and pushdown-setup cost. One run per side.

## Results — per-query latency (seconds)

| query | v1.5.2 | v2.0.0 | Δ |
|---|---:|---:|---:|
| q01 | 0.253 | 0.208 | −17.8% |
| q02 | 0.076 | 0.066 | −13.2% |
| q03 | 0.134 | 0.153 | +14.2% |
| q04 | 0.135 | 0.140 | +3.7% |
| q05 | 0.142 | 0.130 | −8.5% |
| q06 | 0.070 | 0.064 | −8.6% |
| q07 | 0.133 | 0.129 | −3.0% |
| q08 | 0.145 | 0.135 | −6.9% |
| q09 | 0.337 | 0.353 | +4.7% |
| q10 | 0.253 | 0.305 | +20.6% |
| q11 | 0.049 | 0.059 | +20.4% |
| q12 | 0.152 | 0.138 | −9.2% |
| q13 | 0.606 | 0.462 | −23.8% |
| q14 | 0.128 | 0.109 | −14.8% |
| q15 | 0.101 | 0.089 | −11.9% |
| q16 | 0.178 | 0.170 | −4.5% |
| q17 | 0.116 | 0.108 | −6.9% |
| q18 | 0.318 | 0.335 | +5.3% |
| q19 | 0.207 | 0.142 | −31.4% |
| q20 | 0.166 | 0.131 | −21.1% |
| q21 | 0.486 | 0.227 | −53.3% |
| q22 | 0.113 | 0.095 | −15.9% |
| **TOTAL** | **4.298** | **3.748** | **−12.8%** |

## Results — load (seconds, wall clock incl. client)

| Table | Rows | v1.5.2 CSV `COPY` | v2.0.0 Parquet `read_parquet` |
|---|---:|---:|---:|
| lineitem | 59,986,052 | 12.66 | 14.729 |
| orders | 15,000,000 | 9.70 | 8.755 |
| partsupp | 8,000,000 | 5.26 | 5.339 |
| part | 2,000,000 | 2.70 | 1.859 |
| customer | 1,500,000 | 2.16 | 1.941 |
| supplier | 100,000 | 0.13 | 0.102 |
| nation | 25 | 0.04 | 0.064 |
| region | 5 | 0.04 | 0.034 |
| **TOTAL** | **86,586,082** | **32.69** | **32.822** |

Row counts match on both sides.

## Observations

- Total query time over the 22 queries dropped by 12.8% (4.30 s → 3.75 s);
  16 of 22 queries are faster.
- The changes larger than the ~±15% run-to-run variance noted for the baseline
  are speed-ups: q21 (2.1×), q19 (−31%), q13 (−24%), q20 (−21%).
- q10 (+21%) and q11 (+20%) are the largest slowdowns. q11 is +10 ms on a
  ~50 ms query dominated by the fixed client cost. q10 (+52 ms) has not been
  re-measured; its cause is unknown.
- The load totals are not comparable as a DuckDB-version effect: the source
  format (CSV vs Parquet) and the load statement differ.

## Caveats

- Single run per side; differences under ~15% are within noise.
- `duckdb_memory_limit` differs (8 GiB vs the DuckDB default).
- Baseline tables were loaded from CSV into database `tpch`, current ones from
  Parquet into `bench`; physical layout may differ.
