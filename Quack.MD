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
  MariaDB side (*planned*: `bridge/`) implements it.

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
| `0002-disable-quack-serve.patch` | `quack_serve()` always fails. The stock server would listen inside `mariadbd` and parse unauthenticated requests; the plugin runs its own authenticated listener (*planned*). |
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
plugin's own listener (*planned*) authenticates against MariaDB and does not
use these callbacks.

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
the statement gate (*planned*), because bind-time code runs before the plan
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
  are kept 5 s. Request bodies are capped by `max_request_bytes`; an
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
  handles it, including its own statement callback, which is still the
  deny-all function (*planned*: the statement gate).
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

### Plan gate

`plan_gate.cpp` registers a DuckDB `PlannerExtension` whose `post_bind`
function runs for every bound statement (every path that plans goes through
`Planner::CreatePlan`, including `Prepare` and rebinds). For other contexts it
returns immediately. For Quack sessions it currently rejects every statement
(*planned*: allowlist and grant checks). `PrincipalState` forces a rebind on
every `EXECUTE`, so prepared statements are re-authorized each time.

`post_bind` runs after binding, and binding has effects: table-function
binds open files (`read_csv`), constant arguments are evaluated, pragmas are
expanded, binder errors name existing objects. These are closed by the
statement gate on the parsed statement (*planned*, required before any
listener is enabled), not by the plan gate.

### MariaDB tables are out of reach

The engine's cross-engine path (`_mdb_scan` and the MariaDB replacement scan
in `runtime/cross_engine_scan.cc`) refuses Quack sessions explicitly, in
addition to its thread-local table registry being empty on Quack threads.

### Quack client without `httpfs`

Quack's client needs `httpfs` for plain HTTP(S), except when the server
certificate is pinned by fingerprint (`ssl_fingerprint`): then it uses its own
TLS client. The plugin's listener (*planned*) is TLS-only and publishes its
fingerprint, which is how the in-server loopback client used by MTR tests
connects.

### Tests

- `quack_guard/test/guard_test.cpp` (Layer A, plain DuckDB, mock authorizer;
  build with `-DDUCKDB_UNIT_TESTS=ON`, run `build/quack_guard/quack_guard_test`):
  contract defaults deny; regular connections unaffected; stock callbacks
  pinned; a Quack session is denied every planned statement kind (queries,
  DML, DDL, SET, EXPLAIN, CALL, transactions, ATTACH, LOAD, PREPARE,
  multi-statement strings, the `Prepare` API) without side effects; a
  session Quack created without a principal is denied too; a principal
  cannot be replaced or be null; the guard's authorizer can be set and
  removed; `ParseAuthString` (escapes, roles, limits, anonymous) and
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
