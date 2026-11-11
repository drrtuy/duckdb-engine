# MariaDB - DuckDB Incompatibilities

Discovered during porting the DuckDB storage engine plugin to MariaDB 12.
Current DuckDB pin: **`5406f514e6aa23a43ec4413cc89ebe3e73dd4cd4`** (`v2.0.0-3524-g5406f514e6a`, post-tag 2.0 development revision) in `third_parties/duckdb/`. SELECT/source execution uses the fixed-profile `mariadb` PEG dialect; this is not full MariaDB syntax or `sql_mode` compatibility.

---

## 1. Identifier Quoting

| MariaDB | DuckDB |
|---|---|
| `` `backticks` `` | `"double quotes"` (SQL standard) |

MariaDB uses backticks by default for quoting identifiers. DuckDB follows the SQL standard and uses double quotes.

**Forwarded SELECT/source:** `ha_duckdb_select_handler::init_scan()` (`ha_duckdb_pushdown.cc`) passes the original source to `duckdb_stream_mariadb_query()` (`runtime/duckdb_query.cc`). `parse_mariadb_statement()` (`runtime/mariadb_dialect.cc`) extracts one SELECT AST under the `mariadb` dialect, whose base tokenizer accepts backticks and double quotes as identifier delimiters. The base dialect is restored before AST submission. No top-level identifier requoting occurs.

**Generated SQL and direct DML:** `backticks_to_double_quotes()` in `runtime/duckdb_query.cc` remains in generic text wrappers. It respects quoted/comment regions and doubles embedded double quotes in converted identifiers; it is not an unconditional character replacement. DDL/DML builders in `convertor/ddl_convertor.cc`, `convertor/dml_convertor.cc`, `ha_duckdb.cc`, and `runtime/delta_appender.cc` also construct quoted identifiers directly.

Without `ANSI_QUOTES`, a double-quoted code token is a MariaDB string, not a DuckDB identifier. SELECT/source factories therefore decline pushdown for such text; direct UPDATE/DELETE report a lexical-mismatch error. The guard ignores inert double-quote characters inside validated single-quoted strings or backtick identifiers.

---

## 2. Function Compatibility

MariaDB function semantics differ from DuckDB in several areas. These are handled by **runtime function overrides** registered at startup via `register_mariadb_compat_functions()` in `duckdb_mysql_compat.cc`. No DuckDB source patches are used.

### Overridden functions (registered in `duckdb_mysql_compat.cc`)

| Function | Issue | Fix |
|---|---|---|
| `octet_length(VARCHAR)` | DuckDB builtin only has BLOB overload | Added VARCHAR→BIGINT overload (byte count) |
| `length(VARCHAR)` | DuckDB returns character count; MariaDB returns byte count | Overridden to return byte count |
| `length(BLOB)` | DuckDB builtin `length()` only works on VARCHAR | Added BLOB→BIGINT overload |
| `ascii(VARCHAR)` | DuckDB returns Unicode codepoint; MariaDB returns first byte value | Overridden to return first byte |
| `ord(VARCHAR)` | DuckDB returns Unicode codepoint; MariaDB returns multibyte byte-value | Overridden for MariaDB semantics |
| `hex()` | DuckDB builtin missing several type overloads | Full overload set (VARCHAR, BLOB, BIGINT, UBIGINT, HUGEINT, UHUGEINT, DOUBLE, FLOAT) |
| `oct()` | Not in DuckDB | Full implementation with all numeric + string overloads |
| `bin()` | Not in DuckDB | Full implementation with all numeric + string overloads |
| `locate(substr, str [, pos])` | DuckDB has `instr(str, substr)` with reversed arg order | Custom 2-arg and 3-arg implementations |
| `mid(s, p [, n])` | Not in DuckDB | SQL macro delegating to `substr()` |
| `addtime(TIMESTAMP/TIME, VARCHAR)` | DuckDB INTERVAL doesn't parse MariaDB `'D H:M:S.us'` format | Custom implementation parsing MariaDB interval strings |
| `subtime(TIMESTAMP/TIME, VARCHAR)` | Same as addtime | Custom implementation |
| `rtrim(VARCHAR, VARCHAR)` | DuckDB removes individual chars from set; MariaDB removes substring | Overridden for substring semantics |
| `ltrim(VARCHAR, VARCHAR)` | Same as rtrim | Overridden for substring semantics |
| `regexp_instr(VARCHAR, VARCHAR)` | Not in DuckDB | Custom implementation using RE2 |
| `regexp_replace(VARCHAR, VARCHAR, VARCHAR)` | DuckDB has it but with different overload set | Custom 3-arg implementation using RE2 |
| `regexp_substr(VARCHAR, VARCHAR)` | Not in DuckDB | Custom implementation using RE2 |
| `json_unquote(VARCHAR)` | Not in DuckDB | Custom implementation (strip quotes + unescape) |
| `json_contains(VARCHAR, VARCHAR, VARCHAR)` | DuckDB only has 2-arg form | 3-arg placeholder (returns false — needs proper implementation) |
| `week(DATE/TIMESTAMP, INTEGER)` | DuckDB `week()` has no MariaDB mode argument | Added 2-arg overloads honoring the MariaDB week `mode` |
| `yearweek(DATE/TIMESTAMP, INTEGER)` | DuckDB `yearweek()` has no MariaDB mode argument | Added 2-arg overloads returning `year*100 + week` per MariaDB mode |
| `to_days(DATE/VARCHAR)` | DuckDB `to_days(BIGINT)` builds an INTERVAL; no date/string overload | Added DATE/VARCHAR→BIGINT overloads returning MariaDB day number (epoch_days + 719528) |
| `dayofweek(DATE/TIMESTAMP)` | DuckDB is 0=Sunday..6=Saturday | Overloads returning MariaDB 1=Sunday..7=Saturday |
| `weekday(DATE/TIMESTAMP)` | Not in DuckDB with MariaDB semantics | Overloads returning MariaDB 0=Monday..6=Sunday |

### Compatible aliases (already work in DuckDB)

| User writes | MariaDB canonical | DuckDB status |
|---|---|---|
| `LOWER()` | `lcase()` | Alias exists |
| `UPPER()` | `ucase()` | Alias exists |
| `IFNULL()` / `NVL()` | `ifnull()` | Rewritten to `COALESCE` |
| `CEIL()` | `ceiling()` | Supported natively |
| `POWER()` | `pow()` | Supported natively |
| `SUBSTRING()` | `substr()` | Supported natively |

### Not translated by the pushdown layer

| Function | Issue | Workaround |
|---|---|---|
| `TIMESTAMPDIFF(unit, dt1, dt2)` | The `unit` keyword is not bindable, so the call reaches DuckDB as an unknown `timestampdiff` function and the query fails. DuckDB's own `date_diff()` takes the unit as a string and is not a drop-in match | Use `UNIX_TIMESTAMP()` arithmetic, e.g. `FLOOR((UNIX_TIMESTAMP(dt2) - UNIX_TIMESTAMP(dt1)) / 600) * 10` for minute buckets |

### Potentially incompatible (not yet triggered)

| User writes | MariaDB canonical | DuckDB status |
|---|---|---|
| `SCHEMA()` | `database()` | No `database()` function in DuckDB |
| `ATAN2(x,y)` | `atan(x,y)` | Arity may differ |

---

## 3. MariaDB PEG Syntax (SELECT/source pushdown)

`extract_source_query()` in `ha_duckdb_pushdown.cc` retains the eligible original source. The grammar deltas in `runtime/mariadb_dialect_expression.cc` and `runtime/mariadb_dialect_select.cc` construct standard DuckDB AST nodes. There is no SQL rendering between parsing and submission, and the former `init_scan()` text-rewrite block is deleted.

| MariaDB syntax | AST / execution policy |
|---|---|
| `GROUP BY ... WITH ROLLUP` | Native grouping-set expansion, matching `GROUP BY ROLLUP(...)` |
| `CONVERT(expr, TYPE)` | Native `CastExpression`; SIGNED remains INTEGER, unsupported UNSIGNED retains the baseline error; no added CONVERT USING support |
| `CURRENT_TIME()` / `CURRENT_DATE()` / `CURRENT_TIMESTAMP()` | Bare keyword-equivalent column-reference AST with canonical lowercase metadata names; TIME/TIMESTAMP precision 0–6 accepted but ignored |
| `STRAIGHT_JOIN` | Regular qualified join or conditionless CROSS JOIN; SELECT modifier ignored, join ordering not enforced |
| Conditionless plain `JOIN` (no ON/USING) | Native CROSS JOIN; not a general extension to conditionless outer/qualified joins |
| `REGEXP` / `NOT REGEXP` / `RLIKE` / `NOT RLIKE` | Native regex operator AST and NOT handling, preserving the baseline engine behavior rather than promising full MariaDB regex equivalence |
| `LIMIT offset, count` | Native limit/offset modifier; parameter numbering follows source order |
| `DISTINCTROW` | Native DISTINCT |
| `HIGH_PRIORITY`, `SQL_NO_CACHE`, `SQL_CACHE`, `SQL_BUFFER_RESULT`, `SQL_SMALL_RESULT`, `SQL_BIG_RESULT`, `SQL_CALC_FOUND_ROWS` | Parsed and ignored; no new execution semantics |
| `FORCE INDEX(...)`, `USE INDEX(...)`, `IGNORE INDEX(...)` | Parsed and ignored on base tables, including supported INDEX/KEY names and FOR JOIN/ORDER BY/GROUP BY scopes; not supported on derived tables/table functions |

### Deliberate refusals and remaining gaps

| Input / feature | Current behavior |
|---|---|
| Double-quoted string with `ANSI_QUOTES` off, e.g. `WHERE product = "MariaDB mug"` | SELECT/source pushdown is declined before DuckDB parsing. MariaDB evaluates its string semantics; the dialect does not reinterpret the token. Direct UPDATE/DELETE reject the lexical mismatch. |
| `#` comments, potential dollar-quote openers, mismatched `--` or nested comments | Pushdown declined; inert occurrences inside validated quoted regions and dollar signs within ordinary ASCII identifiers remain allowed. |
| Executable/versioned comments (`/*!...*/`, `/*M!...*/`) | Pushdown declined, including version-gated forms. No silent treatment as an ordinary DuckDB block comment. |
| Quote-boundary backslash mismatches, malformed regions, NUL, E-prefixed or charset-introduced literals | Pushdown declined according to the session's actual quote flags. `NO_BACKSLASH_ESCAPES` does not bypass the other safety checks. |
| Non-ASCII text | Bare non-ASCII tokens and non-ASCII in non-UTF8 client charsets are declined. Quoted UTF-8 text must also survive DuckDB normalization byte-for-byte, so an authorized identifier cannot change during parsing. |
| Reserved-word aliases | No blanket keyword unreservation or text-based alias repair; use an explicit backtick-quoted alias. A dialect parse failure is an error, not a late fallback. |
| `sql_mode` operators, literal values, regex/coercion/collation semantics | Fixed-profile parity only; no general mode propagation. `\|\|` keeps the baseline DuckDB AST interpretation, not a new logical-OR transform. N/X/B literals and boundary-preserving backslashes retain existing value-semantic gaps. |

A factory refusal permits MariaDB's normal row-interface path; it does not guarantee that every fallback plan is supported. Once `init_scan()` starts dialect parsing, parse/bind/execute failures are reported without retrying either the old text executor or MariaDB execution. Cross-engine WHERE fragments and generated DDL/DML remain separate text channels, not reasons to restore top-level rewrites.

Regression coverage: `duckdb_lexical_guard`, `duckdb_peg_pushdown`, `duckdb_identifier_escaping`, `pushdown_rewrite`, and the standalone AST/result/metadata/error corpora. See [mariadb-peg-dialect-migration-design.md](mariadb-peg-dialect-migration-design.md) for scope, parser-count evidence, measurements, and open release gates.

---

## 4. Data Type Handling

### DECIMAL Appender

DuckDB upstream Appender API does not accept raw `Append<intN_t>()` for DECIMAL columns -- it interprets them as plain integers and fails the type cast. The fix is to use `duckdb::Value::DECIMAL(value, width, scale)` which tells DuckDB the value is already scaled.

**Affected file**: `delta_appender.cc`

### Text types

MariaDB `MEDIUMTEXT`, `LONGTEXT`, `TEXT`, `TINYTEXT` all map to DuckDB `VARCHAR`.

---

## 5. Table Partitioning

MariaDB table partitioning is not supported for `ENGINE=DuckDB`. The handlerton declares `HTON_NO_PARTITION`, so the server rejects both partitioned table creation and conversion of an existing DuckDB table:

```sql
CREATE TABLE t (id INT) ENGINE=DuckDB PARTITION BY HASH(id);
ALTER TABLE t PARTITION BY HASH(id);
```

Both statements fail with `ER_PARTITION_MERGE_ERROR` (`Engine cannot be used in partitioned tables`) before MariaDB creates partition child handlers or starts the `ALGORITHM=COPY` protocol.

---

## 6. DuckDB API Differences (AliSQL fork vs upstream v1.5.2)

The AliSQL fork of DuckDB has custom extensions to the API that do not exist in upstream DuckDB:

| Feature | AliSQL fork | Upstream v1.5.2 |
|---|---|---|
| `scheduler_process_partial` config option | YES | NO - Does not exist |
| `appender_allocator_flush_threshold` config option | YES | NO - Does not exist |
| `Appender(conn, schema, table, AppenderType)` constructor | YES | NO - No `AppenderType` parameter |
| `LengthFun` for VARCHAR | Uses `StrLenOperator` (byte count) | Uses `StringLengthOperator` (codepoint count) — overridden at runtime via `duckdb_mysql_compat.cc` |

---

## 7. DuckDB Extensions

Loaded via `cmake/duckdb_extensions.cmake`:

| Extension | Required for | Explicitly loaded? |
|---|---|---|
| **ICU** | `SET TimeZone = ...`, locale-aware collations | YES |
| **JSON** | JSON functions | YES |
| **core_functions** | Basic scalar/aggregate functions | YES |
| **parquet** | Parquet I/O | YES |

`cmake/duckdb_extensions.cmake` statically links these extensions; `DuckdbManager::Initialize()` (`runtime/duckdb_manager.cc`) registers the generated loader before opening the database. Compile-time autoload/autoinstall defaults do not describe the runtime policy: initialization sets `autoinstall_known_extensions=false`, `allow_community_extensions=false`, and `lock_configuration=true`. Autoload stays enabled. External access is still enabled by default; configuration locking does not disable it. See [security-model.md](security-model.md).

---

## 8. ABI Compatibility

DuckDB is built as a static library (`libduckdb_bundle.a`) via `ExternalProject_Add` in `cmake/duckdb.cmake`.

| Concern | Detail |
|---|---|
| **`_GLIBCXX_DEBUG`** | MariaDB debug builds define `_GLIBCXX_DEBUG` which changes `sizeof(std::vector)` etc. The plugin target strips this via `-U_GLIBCXX_DEBUG -U_GLIBCXX_ASSERTIONS` in `CMakeLists.txt`. Mismatch causes SIGSEGV in `~DuckDB()`. |
| **C++ standard** | Plugin is built with C++17 (`CXX_STANDARD 17`), as is the pinned DuckDB build. |
| **`_GLIBCXX_USE_CXX11_ABI`** | Not explicitly set — uses the compiler default (ABI=1). Both DuckDB and the plugin use the same default. |

---

## 9. Potential Future Incompatibilities

These have not been triggered yet but are likely to cause issues when more complex queries are pushed down:

| MariaDB function | DuckDB equivalent | Notes |
|---|---|---|
| `GROUP_CONCAT()` | `string_agg()` / `list_aggr()` | Different syntax and separator handling |
| `DATE_FORMAT()` | `strftime()` | Different format specifiers |
| `UNIX_TIMESTAMP()` | `epoch()` | -- |
| `FORMAT(number, decimals)` | -- | MariaDB: locale-aware number formatting; DuckDB: printf-style |
| `FOUND_ROWS()` | -- | No equivalent |
| `LAST_INSERT_ID()` | -- | No equivalent |
| Collations | -- | MariaDB collation rules do not transfer to DuckDB |
