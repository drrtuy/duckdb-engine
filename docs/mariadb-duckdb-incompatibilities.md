# MariaDB - DuckDB Incompatibilities

Discovered during porting the DuckDB storage engine plugin to MariaDB 12.

---

## 1. Identifier Quoting

| MariaDB | DuckDB |
|---|---|
| `` `backticks` `` | `"double quotes"` (SQL standard) |

MariaDB uses backticks by default for quoting identifiers. DuckDB follows the SQL standard and uses double quotes. All SQL generation code in the plugin must use double quotes.

**Affected files**: `ddl_convertor.cc`, `dml_convertor.cc`, `ha_duckdb.cc`, `delta_appender.cc`, `ha_duckdb_pushdown.cc`

**Fix applied**: replaced all backtick literals with double quotes in codegen; added `std::replace` in `ha_duckdb_pushdown.cc` for `SELECT_LEX::print()` output.

---

## 2. Function Name Rewriting by SELECT_LEX::print()

MariaDB's `SELECT_LEX::print()` rewrites user-facing function names to internal canonical names. Some of these canonical names are not supported by DuckDB.

### Confirmed incompatible

| User writes | print() outputs | DuckDB status | Fix |
|---|---|---|---|
| `LENGTH()` | `octet_length()` | NO - No VARCHAR overload (only BLOB/BIT) | Patch: added VARCHAR overload via `patches/0001-octet_length-varchar.patch` |

### Confirmed compatible (aliases already exist in DuckDB)

| User writes | print() outputs | DuckDB alias |
|---|---|---|
| `LOWER()` | `lcase()` | `LcaseFun -> LowerFun` |
| `UPPER()` | `ucase()` | `UcaseFun -> UpperFun` |
| `IFNULL()` / `NVL()` | `ifnull()` | Parser rewrites to `COALESCE` |
| `CEIL()` | `ceiling()` | Supported natively |
| `POWER()` | `pow()` | Supported natively |
| `SUBSTRING()` | `substr()` | Supported natively |

### Potentially incompatible (not yet triggered)

| User writes | print() outputs | DuckDB status |
|---|---|---|
| `SCHEMA()` | `database()` | NO - No `database()` function in DuckDB |
| `ATAN2(x,y)` | `atan(x,y)` | MAYBE - Arity may differ |

---

## 3. Data Type Handling

### DECIMAL Appender

DuckDB upstream Appender API does not accept raw `Append<intN_t>()` for DECIMAL columns -- it interprets them as plain integers and fails the type cast. The fix is to use `duckdb::Value::DECIMAL(value, width, scale)` which tells DuckDB the value is already scaled.

**Affected file**: `delta_appender.cc`

### Text types

MariaDB `MEDIUMTEXT`, `LONGTEXT`, `TEXT`, `TINYTEXT` all map to DuckDB `VARCHAR`. This works, but functions operating on these columns may hit function signature mismatches (e.g., the `octet_length` issue above).

---

## 4. DuckDB API Differences (AliSQL fork vs upstream 1.2.1)

The AliSQL fork of DuckDB has custom extensions to the API that do not exist in upstream DuckDB:

| Feature | AliSQL fork | Upstream 1.2.1 |
|---|---|---|
| `scheduler_process_partial` config option | YES | NO - Does not exist |
| `appender_allocator_flush_threshold` config option | YES | NO - Does not exist |
| `Appender(conn, schema, table, AppenderType)` constructor | YES | NO - No `AppenderType` parameter |
| `LengthFun` for VARCHAR | Uses `StrLenOperator` (byte count) | Uses `StringLengthOperator` (codepoint count) |

---

## 5. DuckDB Extensions

| Extension | Required for | Default in upstream? | Action needed |
|---|---|---|---|
| **ICU** | `SET TimeZone = ...` | NO | Added via `cmake/duckdb_extensions.cmake` |
| **JSON** | JSON functions | NO | Added via `cmake/duckdb_extensions.cmake` |
| `core_functions` | Basic functions | YES | -- |
| `parquet` | Parquet I/O | YES | -- |
| `jemalloc` | Memory allocator (Linux x86_64) | YES | -- |

Build flags: `EXTENSION_STATIC_BUILD=1` required for static linking. `DISABLE_BUILTIN_EXTENSIONS=TRUE` must **not** be set.

---

## 6. Potential Future Incompatibilities

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

---

## 7. Patch Management

DuckDB patches are stored in `patches/` and applied via `PATCH_COMMAND` in `cmake/duckdb.cmake`:

```cmake
PATCH_COMMAND   git -C "${DUCKDB_SUBMODULE_DIR}" checkout -- . &&
                git -C "${DUCKDB_SUBMODULE_DIR}" apply --whitespace=nowarn
                  "${CMAKE_CURRENT_SOURCE_DIR}/patches/0001-octet_length-varchar.patch"
```

To add a new patch:
1. Create a `git diff` patch file in `patches/`
2. Chain it in `PATCH_COMMAND` with `&&`
