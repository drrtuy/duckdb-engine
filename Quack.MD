# Quack protocol in the DuckDB engine (MDEV-41383)

[Quack](https://github.com/duckdb/duckdb-quack) is DuckDB's client/server
protocol: a DuckDB client attaches a remote DuckDB over HTTP(S) and runs SQL
there. This document describes how the DuckDB storage engine exposes its
DuckDB instance over Quack without bypassing MariaDB authentication, roles
and grants. It grows with the implementation; sections marked *planned* are
not implemented yet.

## Principles

- **MariaDB objects are the only source of truth.** Accounts (`user@host`),
  their authentication plugins, roles, `PUBLIC` and grants decide who connects
  and what they may read. DuckDB holds no users, tokens, secrets or copies of
  grants.
- **Deny by default.** Quack is off unless configured. Anything not explicitly
  allowed is denied, and every error path denies.
- **Regular MariaDB users are not affected.** Quack's restrictions apply per
  connection; the only global settings the plugin sets are Quack's own
  (`quack_*`), which do nothing for SQL that does not use Quack.
- **DuckDB-side code stays separate** from the handler: the self-contained
  module `quack_guard/` (no MariaDB headers) talks to MariaDB only through the
  bridge interface `quack_guard/include/quack_guard/authorizer.hpp`; the
  MariaDB side, `bridge/`, implements it.

## Build and linking

Quack is not part of the DuckDB tree. It is a second submodule,
`third_parties/duckdb-quack`, pinned to the commit DuckDB v2.0.0 itself pins
(`third_parties/duckdb/.github/config/extensions/quack.cmake`; configure fails
if the two differ), and built as a statically linked DuckDB extension:

- `cmake/duckdb.cmake` stages a copy of the submodule (`CMakeLists.txt`,
  `src/`) into `<build>/duckdb-quack-src` and applies the patches listed there
  (`patches/duckdb-quack/`, explicit ordered list, `patch -p1 --fuzz=0`), so the
  submodule stays clean. Files are rewritten only when their content changes,
  files that disappeared are removed, and DuckDB's build step re-runs when a
  staged file changes.
- `cmake/duckdb_extensions.cmake` loads `quack` from that staged copy;
  `cmake/duckdb.cmake` adds it to the static extension loader and links
  `libquack_extension.a`.
- Quack's HTTPS (httplib) uses OpenSSL. `ha_duckdb.so` links the **system**
  `libssl`/`libcrypto` dynamically; on distribution builds these are the same
  libraries `mariadbd` loads. OpenSSL development files and `patch` are build
  requirements (`build.sh -D` installs them; CI through `extra_packages_deb` /
  `extra_packages_rpm`).

Only Quack's own sources are needed, not its nested submodules:
`git submodule update --init third_parties/duckdb third_parties/duckdb-quack`.

### Patches

Each patch is security-relevant and a candidate for upstream.

| Patch | What and why |
|---|---|
| `0001-random-token-from-openssl.patch` | Session ids, the HMAC key and generated tokens come from OpenSSL's CSPRNG (`RAND_bytes`). Stock Quack takes them from DuckDB's writable crypto module, which exists only when the `httpfs` extension is loaded; the plugin deliberately does not link `httpfs` (network access from SQL, see `docs/security-model.md`). |
| `0002-disable-quack-serve.patch` | `quack_serve()` always fails. The stock server would listen inside `mariadbd` and parse unauthenticated requests; the plugin runs its own authenticated listener. |
| `0003-client-loopback-only.patch` | The Quack client (`quack_query`, `ATTACH 'quack:...'`) only reaches `localhost`, `127.0.0.1` or `::1`. With a pinned certificate fingerprint it needs no `httpfs`, so without this patch `run_in_duckdb` would gain network egress. Loopback stays open for the in-server test client. |
| `0004-connection-hooks.patch` | Virtual hooks in `QuackServer` so the plugin's listener overrides Quack's login and statement decisions instead of copying Quack's code (a Quack update that changes them then breaks the patch, not silently the listener): `AuthenticateConnection()` (a connection request, after Quack's version and heartbeat checks), `AuthorizeStatement()` (the SQL of a `PREPARE_REQUEST`), `OnConnectionCreated()` (a new session before it becomes visible), `OnConnectionClosed()` (every session removed by disconnect or lease expiry); `CleanupExpiredConnection()` made reachable for subclasses; `ConnectionRequestMessage::WipeAuthString()`. Each default keeps Quack's behaviour. |

## Stock Quack entry points are closed

Linking Quack makes its SQL functions (`quack_serve`, `quack_query`, `ATTACH
'quack:...'`) available to every DuckDB connection; from MariaDB they are
reachable only through `run_in_duckdb` (SUPER and
`duckdb_allow_run_in_duckdb`). Besides the patches above, Quack's two SQL
callbacks are pinned. Quack names them in global settings:
`quack_authentication_function` (login, default: a shared-token check) and
`quack_authorization_function` (every statement, default: allow all). Before
the plugin locks DuckDB's configuration, `DuckdbManager::Initialize()` calls
`quack_guard::Install()`, whose `Lockdown()` (`quack_guard/src/lockdown.cpp`):

1. registers two internal functions in the system catalog:
   `mariadb_quack_deny_all(connection_id, auth_string, token)` returns `false`,
   `mariadb_quack_deny_query(connection_id, query)` returns `NULL` (statement
   rejected);
2. runs `LOAD quack`, so the settings exist;
3. sets both settings to the fully qualified `system.main.*` names, so a
   same-named macro in another schema cannot shadow them.

After `lock_configuration` neither setting can be changed in any scope, and
internal system functions cannot be replaced or dropped. If any step fails,
DuckDB initialization fails and the engine does not load (fail closed). The
plugin's own listener authenticates through the `QuackAuthorizer` and decides
statements itself (hooks of patch 0004); it does not use these callbacks.

Known gaps for SUPER users with `run_in_duckdb`: `CREATE SECRET (TYPE quack)`
can hold a client token in memory, and `quack_identify()` writes the cosmetic
`whoami_*` settings past the configuration lock. Neither opens access.

## The quack_guard module

```
quack_guard/
  include/quack_guard/authorizer.hpp   bridge interface (both sides include it)
  include/quack_guard/quack_guard.hpp  Install(), Guard, Listener, IsQuackSession()
  src/lockdown.cpp                     stock Quack entry points closed (above)
  src/guard.cpp                        principal state
  src/session_registry.{hpp,cpp}       open sessions, exactly-once close
  src/login.{hpp,cpp}                  auth string and peer address parsing
  src/plan_gate.cpp                    post-bind hook
  src/guarded_server.{hpp,cpp}         the plugin's Quack listener
  test/                                Layer A tests, mock authorizer
```

`quack_guard` is a static library built with DuckDB and Quack headers only; it
is added before the plugin's `LINK_LIBRARIES(mariadb_private ...)`, so it cannot
include MariaDB headers by accident. The MariaDB side links only the
`quack_guard_api` interface target (the `include/` directory).

### Bridge interface

`QuackAuthorizer` (`authorizer.hpp`) is the only way `quack_guard` learns
anything about MariaDB:

| Call | Purpose |
|---|---|
| `authenticate(credentials, out)` | map the client's credentials and the transport facts (peer address, TLS, taken from the socket) to a MariaDB principal |
| `check(identity, requests)` | all table/column accesses of one statement; allowed only if every one is |
| `filterVisible(identity, objects)` | which catalog objects the principal may see |
| `sessionClosed(identity)` | bookkeeping when a session ends |

The contract is fail closed: a default `Verdict` denies, an empty request list
denies, a request without columns denies unless it is explicitly
"any column" (`count(*)`), `filterVisible` returns an empty list on error.
`Identity` is opaque to `quack_guard`; its `audit()` text ("user@host/role") is
for logs and errors only.

### Which contexts are guarded

A Quack session is a DuckDB `ClientContext` that carries a principal
(`PrincipalState` in `registered_state`, attached once by the plugin's
listener right after it creates the session, before the client learns the
session id; it cannot be replaced) or that Quack's server created (it always
registers `quack_session`); a Quack session without a principal gets nothing.
Everything else - MariaDB-side connections, `run_in_duckdb`, DuckDB's
internal connections (WAL replay, checkpoints) - is not affected.

`Install()` returns the database's `Guard`, which holds the
`QuackAuthorizer`. Without an authorizer every Quack session is denied. `Guard::StartListener()`
returns a caller-owned `Listener` (see below).

The planning team had proposed the opposite (every context untrusted unless
the plugin marks it). That breaks DuckDB's own internal connections, which
bind SQL during WAL replay and checkpoints, and would change behaviour for
regular users. Contexts a Quack session might open indirectly (for example
`json_execute_serialized_sql`) must instead be prevented before binding, by
the statement gate, because bind-time code runs before the plan
gate (see below).

### Listener

`GuardedQuackServer` (`src/guarded_server.cpp`) is Quack's `QuackServer`
with the plugin's own transport, and with login, session creation and
session end handled in the hooks of patch 0004. `Guard::StartListener()`
returns a `Listener` handle owned by the caller (the plugin keeps it next to
the database and stops it before the database is destroyed). One listener
per guard.

- **Transport, TLS only.** Certificate and key are read once into memory
  (unencrypted key; a passphrase is never prompted for) and the published
  fingerprint is computed from the same bytes. One httplib `SSLServer` per
  address; IPv6 addresses are written `[::1]` in endpoints, sockets are
  `IPV6_V6ONLY` and use `SO_REUSEADDR` (not `SO_REUSEPORT`, so a port in use
  is an error); a host name that does not resolve is reported as such. With
  port 0 the first address picks a port and the others reuse it. If anything
  fails while starting, everything already bound is closed again. Client
  connections over all addresses are capped by `max_connections`; beyond the
  cap a connection is closed at once, not queued. Idle client connections
  are kept 30 s. Request bodies are capped by `max_request_bytes`; an
  incomplete body is answered 400 and never parsed. A listener thread that
  stops on its own is reported in `ListenerStatus::failure` (and its socket
  is not closed a second time).
- **Peer address.** Taken from the socket (`X-Forwarded-For` is never
  trusted) and canonicalized (`CanonicalPeer`): IPv4-mapped IPv6 becomes
  IPv4, the IPv6 zone is dropped, anything else is refused.
- **Login** (`AuthenticateConnection`). The auth string is
  `user[/role]:password`; `%XX` escapes any byte but NUL in user and role
  (for `:`, `/`, `%` in names); user and role up to 512 bytes after
  unescaping (MariaDB's 128 characters in utf8mb4; the authorizer applies the
  exact limit), password 1024; an empty user is passed on (the authorizer
  decides about anonymous accounts).
  Refused before the authorizer is asked: listener stopping, heartbeat lease
  longer than `max_heartbeat_seconds`, `max_sessions` reached, no authorizer,
  malformed auth string. Then `QuackAuthorizer::authenticate()` gets the
  claims and the transport facts; the password is wiped afterwards in our
  copies, in Quack's decoded message and in the request buffer (httplib's
  socket buffers are not under our control). Every refusal and every authorizer error answers the same
  "Authentication failed" (except the heartbeat and session-cap limits).
- **Session creation** (`OnConnectionCreated`, inside Quack before the
  session is visible): the principal is attached to the session's context
  and the session is registered in the guard's `SessionRegistry` (id, peer,
  identity, the authorizer that admitted it). If that fails the session is
  not created and the identity is released.
- **Messages.** Every other message must name a registered session and come
  from its peer address; otherwise "Invalid connection id". Quack then
  handles it; its SQL goes through the statement gate (the
  `AuthorizeStatement` hook) and the plan gate (below).
- **Session end** (`OnConnectionClosed`): disconnect, an expired lease
  noticed by Quack, or the listener's reaper, which every `reap_interval_ms`
  interrupts and removes sessions whose heartbeat lease ran out (Quack itself
  only notices expiry when the client comes back). The registry then tells
  the admitting authorizer `sessionClosed()` exactly once, even if the
  guard's authorizer has been replaced meanwhile.
- **Stop.** Refuse new logins and mark every answer `Connection: close`,
  stop the reaper, close the listening sockets, then until no client
  connection is left: remove every session and abort what it runs (also a
  statement that started after an earlier abort), and shut down every
  accepted client socket on the listener's ports (found via
  `/proc/self/fd`; httplib offers no accept hook), so neither a long
  statement nor a slow client, logged in or not, can hold the stop. Then
  join the listener threads. Idempotent; also runs when the handle is
  destroyed. Must not run on a listener thread.

### What a Quack session may run (milestone 1)

Read-only queries over base tables, decided twice, from
allowlists (anything not listed is refused, so a new DuckDB statement,
operator or function is refused until reviewed):

- **Statement gate** (`src/statement_gate.cpp`, the listener's
  `AuthorizeStatement` hook, before anything is bound; binding has effects:
  table-function binds open files, unknown names reach file replacement
  scans and extension autoloading, constant arguments are evaluated, binder
  errors name objects).
  - Exactly one statement: `SELECT` (plain, set operations, non-recursive
    CTEs, `FROM`-first), `EXPLAIN` of one (not `ANALYZE`), or `BEGIN` /
    `COMMIT` / `ROLLBACK` (Quack's client wraps work on an attached
    database in them). A transaction keeps one snapshot of the shared
    database open, so a session whose transaction stays open longer than
    `max_transaction_seconds` (1 s .. 7 days) is closed by the reaper,
    which observes each session's real transaction state rather than
    predicting it from statements; grants are checked on every statement
    inside a transaction too.
  - Table references: base tables, joins (not `NEAREST BY`), subqueries,
    `VALUES`; no table functions, `PIVOT`, `SHOW`/`DESCRIBE`, `SAMPLE`,
    `AT`, no nested schema paths. Once the whole form has been checked (so
    a form refusal always wins and reveals nothing about objects), every
    base table name except CTE names in scope is looked up, all in one
    transaction of a separate connection: it must be a table (not a view)
    in the engine's own database, outside the schemas `main`, `pg_catalog`,
    `information_schema`, `temp` (any case) and not a MariaDB `#sql...`
    name. A CTE is in scope for the CTEs after it and the body only, as
    DuckDB binds it, so self and forward references are looked up too.
  - Expressions: columns, constants, comparisons, `AND`/`OR`, `CASE`,
    `BETWEEN`, `NOT`, `IS [NOT] NULL`, `[NOT] IN`, `NULLIF`, `COALESCE`,
    plain `*` and `t.*` (no `COLUMNS()`, `EXCLUDE`, `REPLACE`, `RENAME`,
    which reveal column names through what they match), casts to built-in
    types with constant parameters, subqueries, and functions and window
    functions only by unqualified name from the function list
    (`src/allowlist.cpp`: deterministic, no I/O, no settings, no nested
    SQL). No `COLLATE`, array or struct access, parameters, lambdas,
    `UNNEST`.
- **Plan gate** (`src/plan_gate.cpp`, DuckDB `post_bind` hook, every
  statement any path plans): the bound operators (projection, filter,
  aggregate, window, order/limit, distinct, joins, set operations, CTEs,
  dummy/expression scans, explain), the bound expressions (again the
  function list, also inside macro bodies, so a macro shadowing an allowed
  name is caught, and inside subquery plans), and table scans only of base
  tables of the engine's own database outside the reserved schemas and
  names. The prepared-statement API is refused (Quack runs every request as
  a new query); a prepared statement would also be re-bound on every
  `EXECUTE`.
- **Authorization.** The plan gate collects every base table read with the
  exact columns read anywhere (`count(*)`, `rowid` and the like add "any
  column": the privilege on the table or on one of its columns), and asks
  `QuackAuthorizer::check()` once per statement with all of them; a
  statement that reads no table is not checked. Deny, an authorizer error
  or no authorizer refuses. `EXPLAIN` needs the same access as the query.
- **Errors.** A refused statement form answers "Statement not allowed over
  Quack". Everything about objects answers "Access denied, or the object
  does not exist": denied access, missing, reserved or non-table objects,
  plan-gate refusals (the plan exists only if every name resolved), and
  every other error, except execution errors (conversion, out of range,
  division by zero, invalid input, interrupt, out of memory) of a statement
  the plan gate has granted: some of those types are also thrown while
  binding, so `PrincipalState` carries an "authorized" flag, reset when a
  query begins and set by the plan gate. A client learns nothing about
  objects it cannot read.
- Each session has `use_replacement_scans` off (`HardenSession`), the
  second line behind the table lookup.
- Unreachable by design in milestone 1: DuckDB views (a SUPER user could
  create one with `run_in_duckdb`; MariaDB views are not DuckDB objects),
  and MariaDB databases named `main`, `pg_catalog`, `information_schema` or
  `temp`.

### Catalog discovery

A client that attaches the server (`ATTACH 'quack:host:port' AS r (TOKEN
'user:password', SSL_FINGERPRINT '...')`) loads the remote catalog with two
fixed queries, `QuackSchemaSet::GetLoadQuery()` and
`QuackTableSet::GetLoadQuery()`. The listener recognizes exactly these
strings (calling Quack's own functions, so a Quack update cannot drift
silently) and runs `mariadb_quack_catalog('schemas' | 'tables')`
(`src/catalog_filter.cpp`) instead. That table function runs the original
query (for tables, the same columns plus the names it needs) on a separate
connection and returns, with the original columns and types, only:

- top-level schemas of the engine's database: `main` always (every DuckDB
  has it; its tables are never listed), other non-reserved schemas only if
  `QuackAuthorizer::filterVisible()` keeps them;
- tables of listed schemas (so never of a hidden schema), with
  non-reserved names, that `filterVisible()` keeps. The listed DDL shows
  every column, so the bridge keeps a table only for a table-level (or
  higher) privilege, not for column grants alone;
- no views (unreachable in milestone 1), no other catalogs, no nested
  schemas.

Only objects that were asked about count, whatever `filterVisible()`
returns. Schema oids are replaced by dense ordinals (`main` is 1), so the
gaps between real oids cannot reveal how many objects others created.
Without a principal or authorizer, or if `filterVisible()` fails or returns
nothing, only `main` is listed. The function itself is reachable only
through this rewrite (the statement gate refuses table functions); called
from any other context it lists only `main`, and in a plan it does not
lift the check of the other tables. Queries through the attached database
(`SELECT ... FROM r.shop.items`, sent by the client as `SELECT #1, #2 FROM
shop.items`) go through both gates like any other.

### MariaDB tables are out of reach

The engine's cross-engine path (`_mdb_scan` and the MariaDB replacement scan
in `runtime/cross_engine_scan.cc`) refuses Quack sessions explicitly, in
addition to its thread-local table registry being empty on Quack threads.

## The MariaDB bridge

`bridge/` implements `QuackAuthorizer` on MariaDB accounts, roles, `PUBLIC`
and grants. It links only `quack_guard_api` (the interface), never
`quack_guard`'s internals, and calls only exported server functions.

```
bridge/
  acl_compat.h            11.4 / 12.x / 13.x signatures of the ACL calls
  acl_worker.{h,cc}       the ACL worker thread and its background THD
  mariadb_authorizer.{h,cc}  QuackAuthorizer, MariaDBIdentity
  bridge.{h,cc}           start/stop inside the plugin
  bridge_test_udf.cc      QUACK_BRIDGE_TEST() for MTR
```

- **ACL worker.** MariaDB's ACL functions need a THD, so every bridge call
  runs on one worker thread that owns a background THD
  (`create_background_thd()`: invisible in the process list, not counted
  at shutdown). Calls wait in a queue of at most
  `duckdb_quack_acl_queue_size` requests and at most
  `duckdb_quack_acl_timeout_ms`; a full queue, a timeout or a stopped worker
  deny (`saturated`, `timeout`, `shutdown`). 0 is legal for both and denies
  everything. Requests carry copies; a request whose caller stopped waiting
  is skipped if still queued and gives up between objects if running. Each
  request runs as a statement of its own (`lex_start` / `lex_end`, which
  also releases the plugin locks `ha_table_exists` takes); afterwards the
  THD's security context, diagnostics and memory are reset. `Start()`
  returns only once the worker has its THD. The worker stops in the
  handlerton's `pre_shutdown` (after client connections are closed) and in
  plugin deinit. Every public call turns an exception into a denial.
- **Identity.** `MariaDBIdentity` holds the login user, the peer address,
  the account host derived from it as a MariaDB login does (`localhost`
  for loopback unless `skip_name_resolve`, otherwise the address itself),
  the active role, and the account MariaDB matched at login
  (`priv_user@priv_host`). The bridge does no name resolution: with
  `skip_name_resolve` off, only loopback peers are supported (a remote
  login would be matched by its host name, and an account defined by name
  could be passed over for a broader one); remote clients need
  `skip_name_resolve` (`unsupported` otherwise).
- **Every call re-resolves the account** with `acl_getroot()` from the live
  ACL cache: an unknown or dropped user, an anonymous account, or a peer
  that now matches a different account than at login denies; so does any
  anonymous account matching the peer (`acl_getroot` never picks one, a
  login might); `--skip-grant-tables` denies everything. A role is applied with `acl_check_setrole()` /
  `acl_setrole()` only if still granted. Nothing is cached, so `GRANT`,
  `REVOKE`, `DROP USER` and role changes act on the next statement. The
  bridge never combines privilege bits itself (13.x `DENY` keeps working).
- **`check()`.** Each table must exist in MariaDB with the DuckDB engine
  (`ha_table_exists`), and with `lower_case_table_names=0` must not have a
  case-variant twin DuckDB could not tell apart. Then
  `check_table_access(SELECT)`; with only column grants, each requested
  column through `check_grant_column()`, and for "any column" a
  `check_grant()` that accepts any privilege on the table or a column. Any
  failure denies the whole statement.
- **`filterVisible()`.** A schema is visible with any privilege in it
  (as `SHOW DATABASES`: database-level, or on something in it; on 11.4 a
  table granted only to `PUBLIC` does not make its database visible); a
  table only with table-level (or higher) `SELECT`, since the catalog
  shows its DDL with every column, and only if it is a DuckDB table.
- **Names.** Only an all-lowercase case-variant twin is detected with
  `lower_case_table_names=0` (`Sales` next to `sales`); other twins
  (`Sales` next to `SALES`) are not, a limitation the engine already has
  for regular users, since DuckDB's catalog does not tell them apart.
- **`authenticate()`** logs a Quack client in as a MariaDB account, on the
  ACL worker:
  1. The account is matched as above (`acl_getroot`, the peer's host and
     address); its row is read from `mysql.global_priv` through the
     server's SQL service on the worker's THD (`bridge/account_row.cc`; the
     server extracts the JSON fields, names go in as hex literals). One
     internal query per login; nothing is cached or copied.
  2. Refused: roles, accounts with more than one authentication method,
     methods other than `mysql_native_password` (milestone 1; `ed25519`,
     PARSEC and others follow, see "Follow-ups"), `ACCOUNT LOCK`, an expired
     password (`PASSWORD EXPIRE`, or older than its lifetime or
     `default_password_lifetime`; Quack has no sandbox mode), `REQUIRE X509`
     / `ISSUER` / `SUBJECT` / `CIPHER` (client certificates are not
     requested yet). `REQUIRE SSL` is met by the listener's TLS.
  3. The password is checked with the server's own
     `my_make_scrambled_password()` against the stored hash (constant-time
     compare); pre-4.1 hashes are refused. The copies are wiped.
  4. Role: the requested one (`user/role:password`) must be granted; with
     none requested, the account's default role applies while granted.
  5. `max_password_errors`: after that many wrong passwords in a row, the
     account is refused over Quack (also with the right password) until
     the account row changes (`ALTER USER`, lock/unlock, re-create) or the
     server restarts; a successful login resets the count. Kept apart from
     the server's own counter.
  6. Connection limit: the account's `MAX_USER_CONNECTIONS`, else
     `@@max_user_connections` (a negative global value refuses, as in the
     server), counted over the account's open Quack sessions (MariaDB
     connections are not counted together).
  Every refusal reaches the client as the same "Authentication failed".
  With `log_warnings > 1` the server log gets `DuckDB: Quack login refused
  for '<user>' from <address>: <reason>`, as for refused MariaDB logins
  (the account is `user@host` once matched).
- **Worker transactions.** The worker THD runs with autocommit forced;
  after each request it commits and releases its transactional locks, so
  a login's read of `mysql.global_priv` holds nothing afterwards.
- **`QUACK_BRIDGE_TEST(spec)`** drives `check()` and `filterVisible()` from
  SQL for MTR, also with an identity kept across account changes (`pin`). It exists only with `--duckdb-quack-bridge-test` (read-only,
  default OFF), needs SUPER, and never handles credentials.

Residual: a listed table's DDL is passed through as DuckDB stores it. Only
tables the principal may read entirely are listed, so its columns reveal
nothing new; its defaults name the table's own autoincrement sequence.

## In the server

`ha_duckdb.cc` wires it up; `DuckdbManager` owns the database's `Guard` and
the running `Listener`, and stops the listener before the database is
destroyed.

| System variable | Default | |
|---|---|---|
| `duckdb_quack_enabled` | OFF | serve Quack (read-only, startup) |
| `duckdb_quack_bind_address` | `127.0.0.1` | comma-separated, one socket each |
| `duckdb_quack_port` | 0 | 0 = any free port |
| `duckdb_quack_ssl_cert`, `duckdb_quack_ssl_key` | none | PEM; required |
| `duckdb_quack_max_connections` | 64 | client connections over all addresses |
| `duckdb_quack_max_sessions` | 256 | open sessions |
| `duckdb_quack_max_transaction_seconds` | 300 | longest open transaction |
| `duckdb_quack_max_heartbeat_seconds` | 300 | longest heartbeat lease |
| `duckdb_quack_acl_queue_size` | 64 | dynamic; 0 denies all |
| `duckdb_quack_acl_timeout_ms` | 5000 | dynamic; 0 denies all |
| `duckdb_quack_bridge_test` | OFF | enables `QUACK_BRIDGE_TEST()` (read-only) |

All listener settings are read-only (startup only). Status:
`Duckdb_quack_running`, `Duckdb_quack_endpoints` (`host:port`, comma
separated), `Duckdb_quack_ssl_fingerprint` (what clients pin with
`ssl_fingerprint`), `Duckdb_quack_sessions`.

- **Start.** At plugin init the ACL worker starts; the listener starts only
  once the server is fully up (`mysqld_server_started`: grant tables
  loaded), from a thread that waits for it. With `--skip-grant-tables`, no
  certificate or key, or any other failure it does not start (logged);
  the engine works as before either way.
- **Stop.** In the handlerton's `pre_shutdown` and in plugin deinit: the
  start thread is joined, the listener stopped (sessions closed), then the
  ACL worker. Quack sessions are not MariaDB connections: they are not in
  the process list and cannot be killed with `KILL`; stopping the plugin
  or the server closes them.
- **Clients** connect with
  `ATTACH 'quack:<endpoint>' AS r (TOKEN 'user:password', SSL_FINGERPRINT
  '<Duckdb_quack_ssl_fingerprint>')` or
  `quack_query('quack:<endpoint>', '<sql>', token => ..., ssl_fingerprint
  => ...)`.

### Quack client without `httpfs`

Quack's client needs `httpfs` for plain HTTP(S), except when the server
certificate is pinned by fingerprint (`ssl_fingerprint`): then it uses its own
TLS client. The plugin's listener is TLS-only and publishes its
fingerprint, which is how the in-server loopback client used by MTR tests
connects.

### Tests

- `quack_server.test` (Quack on, TLS from MTR's `std_data`,
  `--max-password-errors=2`): a real listener in `mariadbd`, driven by
  DuckDB's own Quack client through `run_in_duckdb`: login with the right
  and wrong password and unknown users; locked, expired, `REQUIRE X509`
  accounts refused and accepted once altered (and the refusal logged);
  `REQUIRE SSL` and an empty password accepted; several authentication
  methods, roles as logins and a password past its lifetime refused;
  table and column grants; default role; an ungranted explicit role
  refused; live GRANT/REVOKE, also on an open `ATTACH`; the password-error
  throttle, its reset by a success and by `ALTER USER`; logins refused with
  `duckdb_quack_acl_timeout_ms=0`; a column-only grantee lists nothing;
  `MAX_USER_CONNECTIONS` counted over open sessions; `ATTACH` lists only
  visible tables and reads through them; every session closed at the end.
  `quack_server_off.test`: off by default, settings read-only, status
  empty. `quack_server_noacl.test`: not started with
  `--skip-grant-tables`. `quack_server_nocert.test`: not started without
  TLS material, the engine still works. Both wait for the logged reason
  (`include/quack_wait_for_log.inc`).

- `mysql-test/duckdb/t/quack_bridge.test` (`--duckdb-quack-bridge-test`):
  table, database, global and column grants, "any column" (`count(*)`
  needs SELECT somewhere: INSERT alone is not enough), only DuckDB and only
  existing tables, roles only when asked for and granted, `PUBLIC`,
  host-specific accounts by peer address, anonymous and unknown accounts,
  live GRANT / REVOKE / DROP USER, an anonymous account shadowing a named
  one, tables outside the table cache, a kept identity refused once its
  name matches another account or its role is revoked, remote peers
  refused with name resolution on, catalog visibility, queue and timeout
  limits at 0, SUPER only. `quack_bridge_ip.test` (`--skip-name-resolve`):
  host patterns by peer address, loopback not `localhost`, IPv6.
  `quack_bridge_disabled.test`: the function is refused without the
  option, which is read-only.

- `quack_guard/test/guard_test.cpp` (Layer A, plain DuckDB, mock authorizer;
  build with `-DDUCKDB_UNIT_TESTS=ON`, run `build/quack_guard/quack_guard_test`):
  contract defaults deny; regular connections unaffected; stock callbacks
  pinned; a session Quack created without a principal is denied; a principal
  cannot be replaced or be null; the guard's authorizer can be set and
  removed; the statement gate on a corpus of forbidden and allowed
  statements; the plan gate: every allowed statement passes, the exact
  access requests per query shape (columns, `count(*)`, `rowid`, joins,
  subqueries, CTEs), reserved schemas, refusals without side effects,
  denied/throwing authorizer, prepared-statement API refused, other
  connections untouched; a transaction left open closes its session;
  `ATTACH` lists only visible schemas and tables (not other catalogs,
  reserved names, tables of hidden schemas, objects not asked about; only
  `main` when the filter fails or is empty), listed-but-denied tables are
  still denied through the attached database, the catalog function has
  Quack's column types, is not reachable directly and handles more than
  one chunk; the transaction cap closes only sessions whose transaction is
  really open too long (not after COMMIT/ROLLBACK, not reset by a failing
  second BEGIN) and rejects invalid limits; end to end through the listener: data returned
  when allowed, denied tables, missing objects indistinguishable from
  denied ones, file reads and writes refused before binding;
  `ParseAuthString` (escapes, roles, limits, anonymous) and
  `CanonicalPeer`. Listener, driven by Quack's own client and by a raw
  client: refuses to start without TLS or with an unreadable certificate;
  one listener per guard; a failed second address leaves nothing bound; a
  fixed port in use is an error; no authorizer, wrong password or malformed
  auth string are refused; the right password creates a session (its
  statement is then refused by the closed statement callback); peer
  addresses reach the authorizer canonical, IPv4 and IPv6; a session id is
  refused from another peer address and unknown ids are refused; heartbeat
  and session caps; disconnect, reaping of an abandoned session and stop
  each close sessions exactly once, through the authorizer that admitted
  them; authorizer errors and deny-with-identity leak nothing; concurrent
  logins with stop.

- `mysql-test/duckdb/t/quack_lockdown.test`: both callbacks are pinned, cannot
  be set (any scope), reset, replaced or dropped, and deny; `quack_serve()`
  fails; the client refuses non-loopback hosts and still tries loopback. The
  test does not use `have_duckdb.inc`, so a lockdown failure that keeps the
  engine from loading fails it instead of skipping it.
