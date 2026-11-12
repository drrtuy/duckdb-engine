# Security Model: DuckDB and the `run_in_duckdb` Function

This document describes the security characteristics of the embedded DuckDB
instance used by the engine, the limitations of DuckDB's security model, the
normal forwarded-SQL boundary, and the security side effects of the
`run_in_duckdb()` SQL function. It is intended for operators deciding whether
and where to enable the engine, especially in
shared or multi-tenant deployments.

> **Summary:** `run_in_duckdb()` executes arbitrary DuckDB SQL, in-process,
> as the server OS user, on a single shared DuckDB instance. It is gated by two
> coarse checks: the `duckdb_allow_run_in_duckdb` system variable must be ON
> (it is **OFF by default**), and the caller must hold the `SUPER` privilege.
> DuckDB itself applies no access control. Treat the ability to call it as
> equivalent to granting `FILE` plus arbitrary code-adjacent access on the
> server host, and do not enable it in untrusted or multi-tenant environments.

## 1. DuckDB's security model and its limitations

DuckDB is an *embedded* analytical database. Its threat model assumes the
embedding application is trusted and is responsible for access control. As a
result:

- **No users, roles, or object privileges.** DuckDB has no `GRANT`/`REVOKE`
  system comparable to a server DBMS. There is no per-table, per-column, or
  per-row authorization inside DuckDB. Any SQL that reaches the DuckDB
  connection runs with full rights over the whole DuckDB catalog.
- **Filesystem and network access are SQL-level features.** DuckDB SQL can read
  and write local files (`COPY ... TO`, `read_csv`, `read_parquet`,
  `read_text`, `ATTACH`) and — once the relevant extensions are present — reach
  the network (e.g. `httpfs`). These are normal SQL functions, not privileged
  operations.
- **Security is configuration-driven and must be locked.** DuckDB's intended
  hardening mechanism is instance-creation configuration that is then frozen:
  `enable_external_access`, `allowed_directories` / `allowed_paths`,
  `autoinstall_known_extensions`, `autoload_known_extensions`,
  `allow_community_extensions`, `allow_unsigned_extensions`, and
  `lock_configuration`. If these are not set (and locked), the full SQL surface
  is available.

See the upstream guidance: <https://duckdb.org/docs/operations_manual/securing_duckdb/overview>.

## 2. Normal forwarded SQL: the parser boundary

Normal engine statements are authorized by MariaDB before forwarding. This is a
separate boundary from the administrator-only `run_in_duckdb()` function: a
MariaDB string or comment must not become executable DuckDB syntax or an
unauthorized column reference merely because the parsers disagree.

### SELECT/source pushdown

`create_duckdb_select_handler()` and `create_duckdb_unit_handler()` in
`ha_duckdb_pushdown.cc` call `extract_source_query()` to guard the **original
source**, retaining SELECT text or extracting an INSERT ... SELECT tail.
`ha_duckdb_select_handler::init_scan()` then calls
`duckdb_stream_mariadb_query()` (`runtime/duckdb_query.cc`). Session setup runs
under the base dialect before `parse_mariadb_statement()`
(`runtime/mariadb_dialect.cc`) parses once through `ExtractStatements()` and
requires exactly one SELECT. `MariadbDialectScope` in that file uses
`CurrentDialectSetting::SetLocal()` / `ResetLocal()` to restore the base dialect,
including on parse failure, before the runtime wrapper calls `Submit(AST)`.
There are no top-level SQL rewrites, identifier requoting, or text-submission
retries on this path. Dialect selection uses no SQL `SET`/`RESET` statements or
per-query grammar compilation. `DuckdbManager::Initialize()`
(`runtime/duckdb_manager.cc`) registers and warms the dialect once.

A factory refusal leaves the query on MariaDB's normal row interface; it does
**not** guarantee that execution there succeeds. A late parse, bind, or execution
error after pushdown acceptance is returned as an error, with **no fallback**.
The dialect has a fixed grammar profile, not full MariaDB `sql_mode` semantics.

The THD/charset wrapper `mariadb_query_has_lexical_mismatch()` in
`runtime/duckdb_query.cc` and its bounded preflight in
`runtime/mariadb_dialect.cc` reject unsupported lexical interpretations before
PEG parsing. Refusals include `#` and dollar-quote openers outside protected
regions, incompatible `--` boundaries, nested or executable comments, malformed
regions, NULs, unsupported literal prefixes, and non-UTF8 encoding cases.
Double quotes outside protected regions are declined unless
`ANSI_QUOTES` is active: a default-mode MariaDB string must not become a DuckDB
identifier. For eligible non-ASCII text, the preflight calls
`Parser::NormalizeSQLString()` in `runtime/mariadb_dialect.cc` and refuses changed
or invalid text. This is conservative eligibility checking, not complete
cross-engine semantic equivalence; non-boundary backslash value differences
retain the existing parity policy.

### Other SQL paths are not the SELECT AST handoff

- `ha_duckdb::direct_update_rows()` / `direct_delete_rows()` (`ha_duckdb.cc`)
  reject original-source lexical mismatches **before** transaction registration
  or appender flush. They still use the text `duckdb_query()` wrappers and
  `backticks_to_double_quotes()` (`runtime/duckdb_query.cc`).
- Cross-engine per-table WHERE conditions are printed with `COND::print()` in
  `ha_duckdb_select_handler::init_scan()` (`ha_duckdb_pushdown.cc`).
  `build_synthetic_select()` (`runtime/fiber_scan.cc`) incorporates them into a
  synthetic SELECT executed by `fiber_scan_func()` in that file via MariaDB's
  parser and executor, **not** the DuckDB MariaDB dialect.
- Generated DDL/DML and `run_in_duckdb()` remain base-DuckDB-dialect text paths.
  The AST cutover does not replace their escaping or authorization requirements.

## 3. How `run_in_duckdb()` works

`run_in_duckdb(sql_string)` is registered as a `MariaDB_FUNCTION_PLUGIN`
alongside the storage engine (`duckdb_udf.cc`, `ha_duckdb.cc`). When called:

0. It enforces two access checks before doing anything else:
   - if the `duckdb_allow_run_in_duckdb` system variable is OFF (the default),
     it fails with `ER_OPTION_PREVENTS_STATEMENT`
     (`--duckdb-allow-run-in-duckdb`);
   - otherwise it requires the caller to hold the global `SUPER` privilege
     (`check_global_access(thd, SUPER_ACL)` in
     `Item_func_run_in_duckdb::val_str()`, `duckdb_udf.cc`); non-`SUPER` callers
     get an access-denied error and a `NULL` result.
1. It takes the single string argument **verbatim** (the only transformation is
   that backticks are rewritten to double quotes for identifier quoting).
2. `Item_func_run_in_duckdb::val_str()` (`duckdb_udf.cc`) opens a connection to
   the **single, server-wide** DuckDB instance via
   `DuckdbManager::CreateConnection()`, backed by `duckdb.db` in the data directory.
3. It calls `duckdb_query(Connection &, ...)` (`runtime/duckdb_query.cc`), which
   requotes identifiers and executes base-dialect text with `connection.Query()`.
   The function returns the rendered result (or DuckDB error text) as a binary
   string.

Two properties matter for security:

- **Coarse privilege gating only.** Callers must pass the two checks above
  (`duckdb_allow_run_in_duckdb=ON` plus `SUPER`). There is still no *dedicated*
  privilege: `SUPER` is a broad server-admin capability, so gating is all-or-
  nothing rather than per-function or per-object. Any `SUPER` account can call
  it once the sysvar is enabled.
- **No schema scoping.** Unlike normal engine query paths, the function does
  **not** use `prepare_thd_connection()` (`runtime/duckdb_query.cc`), where
  `config_duckdb_env()` / `config_duckdb_session()` are called, so the statement
  is not confined to the caller's current database — it runs against the entire
  shared DuckDB catalog.

## 4. Current instance configuration

The shared instance is created in `DuckdbManager::Initialize()`
(`runtime/duckdb_manager.cc`) with
performance-oriented settings (memory limit, threads, temp directory,
checkpoint threshold). Relevant to security:

- `enable_external_access` is **left at its default (enabled)** — filesystem
  access is permitted.
- `autoinstall_known_extensions=false` and `allow_community_extensions=false`:
  all required extensions are linked statically; automatic installation of known
  extensions and community extensions are disabled. This is not a blanket ban
  on explicit extension installation/loading. `autoload_known_extensions=true`
  remains, so an extension already installed in the server user's extension
  directory can still be loaded.
- `lock_configuration=true` is set after initialization. Only the settings the
  plugin itself propagates from MariaDB system variables and session state are
  listed in `allowed_configs` (see `DuckdbManager::Initialize()`).
- No `allowed_directories` restrictions are applied.

**Locked does not mean disabled.** At the current DuckDB pin `5406f514e6aa`
(post-v2.0.0 tag), an isolated local root/SUPER probe confirmed
`enable_external_access=true` and read a freshly created harmless canary through
`run_in_duckdb()`. This confirms the residual external-access gap (MDEV-40613),
not an unprivileged SQL-injection exploit. The function still requires both its
OFF-by-default opt-in and `SUPER`; ordinary forwarded SQL has the separate
parser boundary above. `duckdb_v2_migration.test` does not assert that external
access is disabled, so a green suite does not establish that property.

## 5. Security side effects of `run_in_duckdb()`

Because the function runs arbitrary DuckDB SQL, in-process, as the server OS
user, on the shared instance, a caller can:

- **Bypass the MariaDB privilege system.** Reads and writes to any
  `ENGINE=DuckDB` table (and the whole DuckDB catalog) are possible regardless
  of MariaDB table/column/row grants, because authorization is checked by
  MariaDB only for normal statements — not for SQL executed inside DuckDB.
- **Read arbitrary local files** the server process can access
  (e.g. `SELECT * FROM read_text('/etc/passwd')`,
  `read_csv(...)`), enabling data exfiltration.
- **Write arbitrary local files** as the server user
  (e.g. `COPY (...) TO '/path/file'`), which can corrupt data, fill disks, or
  serve as a step toward code execution.
- **Reach the network** via extensions such as `httpfs` if they are already
  installed (auto-install is disabled), enabling SSRF and exfiltration to
  remote endpoints.
- **Attach external databases** (`ATTACH`) and read/modify their contents.
- **Change global DuckDB settings** listed in `allowed_configs` on the shared
  instance (e.g. `SET GLOBAL memory_limit`/`threads`), enabling
  resource-exhaustion / denial of service that affects all sessions. Other
  settings are locked.

Note that MariaDB's usual file-access guards do **not** constrain these paths:
neither the `FILE` privilege nor `secure_file_priv` is consulted for I/O
performed inside DuckDB.

## 6. What is *not* enforced

- No *dedicated* (per-function/per-object) privilege: access is gated only by
  `duckdb_allow_run_in_duckdb` plus the broad `SUPER` privilege.
- No mapping of MariaDB users/grants onto DuckDB objects.
- No `secure_file_priv` / `FILE`-privilege enforcement for DuckDB file I/O.
- No blanket prohibition on extension install/load: known-extension autoinstall
  and community extensions are disabled and configuration is locked, but
  autoload remains enabled. External (file/network) access is not disabled.
- No per-user or per-schema isolation: a single DuckDB instance is shared by the
  whole server.

## 7. Regression coverage and limits

The current regressions cover specific parser and escaping boundaries:

- `mysql-test/duckdb/t/duckdb_lexical_guard.test` exercises lexical refusals;
  `duckdb_peg_pushdown.test` in the same directory covers AST pushdown,
  double-quoted WHERE/column-grant safety, Unicode normalization, and
  low-privilege hash/dollar smuggling under all four combinations of
  `ANSI_QUOTES` and `NO_BACKSLASH_ESCAPES`.
- `mysql-test/duckdb/t/duckdb_default_value_injection.test` now exercises the
  exact MDEV-40610 Jira DEFAULT injection payload using a CREATE-only user
  (no `FILE` or `SUPER`). It checks absence of the injected COPY's marker file
  and a literal roundtrip, rather than only accepting CREATE success.
- `mysql-test/duckdb/t/duckdb_identifier_escaping.test` covers MDEV-40653,
  including a schema name containing a double quote.
- The standalone `mariadb_dialect_test` target (`runtime/CMakeLists.txt`, excluded
  from the default build) checks preflight without PEG parsing, actual single
  parses and AST submission, plus paired AST/result/type/name/error/parameter
  checks. Commands are in the README's standalone dialect test section.

Local RelWithDebInfo verification on 2026-11-10 passed all 57 enabled MTR runs
(including replication combinations), with one debug-only skip and 24 disabled
tests. This working-tree result includes the separately updated DuckDB-pin
expected results. These results and the privileged canary probe are bounded evidence,
**not** proof of full security or release readiness. Concurrency, end-to-end
p95/p99 latency, and a production query corpus remain outstanding release
performance work; standalone warmed parser measurements do not cover them.

## 8. Recommendations

For operators:

- **Do not enable the engine in untrusted or multi-tenant environments** where
  untrusted `SUPER` accounts could call `run_in_duckdb()`. Treat the function as
  a highly privileged, host-level capability.
- **Keep `duckdb_allow_run_in_duckdb=OFF`** unless you actively need the
  function; it is off by default. Enabling it exposes the full DuckDB SQL
  surface to every `SUPER` account.
- **Restrict who holds `SUPER`.** Because gating is coarse (`SUPER`, not a
  per-function privilege), access control must be applied at the account level
  by limiting `SUPER` grants to trusted operators.
- **Run the server as a least-privileged OS user** and confine it (containers,
  systemd sandboxing, restrictive filesystem permissions), since DuckDB file I/O
  inherits the server process's rights.
- **Isolate the data directory** so that files reachable by the server user do
  not include secrets unrelated to the database.

`run_in_duckdb()` is now gated behind the `duckdb_allow_run_in_duckdb` sysvar
(OFF by default) and the `SUPER` privilege. Further engine hardening remains
unimplemented (out of scope for this document but worth tracking):
`enable_external_access=false`, disabling extension autoload, and
`allowed_directories` — and/or replacing the coarse `SUPER` check with a
dedicated per-function privilege.

## References

- DuckDB — Securing DuckDB:
  <https://duckdb.org/docs/operations_manual/securing_duckdb/overview>
- `duckdb_udf.cc` — `run_in_duckdb()` implementation and access checks
- `ha_duckdb.cc` — `duckdb_allow_run_in_duckdb` system variable
- `runtime/duckdb_manager.cc` — shared instance configuration
- `runtime/duckdb_query.cc` — query execution path
