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
- **DuckDB-side code stays separate** from the handler: a self-contained
  module (*planned*: `quack_guard/`) talks to MariaDB through a narrow bridge
  interface (*planned*: `bridge/`).

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

## Stock Quack entry points are closed

Linking Quack makes its SQL functions (`quack_serve`, `quack_query`, `ATTACH
'quack:...'`) available to every DuckDB connection; from MariaDB they are
reachable only through `run_in_duckdb` (SUPER and
`duckdb_allow_run_in_duckdb`). Besides the patches above, Quack's two SQL
callbacks are pinned. Quack names them in global settings:
`quack_authentication_function` (login, default: a shared-token check) and
`quack_authorization_function` (every statement, default: allow all). Before
the plugin locks DuckDB's configuration, `DuckdbManager::Initialize()` calls
`quack_lockdown()` (`runtime/quack_lockdown.cc`), which:

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

### Quack client without `httpfs`

Quack's client needs `httpfs` for plain HTTP(S), except when the server
certificate is pinned by fingerprint (`ssl_fingerprint`): then it uses its own
TLS client. The plugin's listener (*planned*) is TLS-only and publishes its
fingerprint, which is how the in-server loopback client used by MTR tests
connects.

### Tests

- `mysql-test/duckdb/t/quack_lockdown.test`: both callbacks are pinned, cannot
  be set (any scope), reset, replaced or dropped, and deny; `quack_serve()`
  fails; the client refuses non-loopback hosts and still tries loopback. The
  test does not use `have_duckdb.inc`, so a lockdown failure that keeps the
  engine from loading fails it instead of skipping it.
