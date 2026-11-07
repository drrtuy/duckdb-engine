/*
  Copyright (c) 2026, MariaDB Foundation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

/*
  Purpose-built function plugins that wrap DuckDB statements, so that
  operators do not need run_in_duckdb().  Privileges, path and URL rules and
  known limitations (no binlog, presigned URLs in logs) are described
  in docs/security-model.md, section 7.

    duckdb_load_{parquet,csv,json}(table, path)    -> rows loaded
    duckdb_export_{parquet,csv,json}(table, path)  -> rows written
    duckdb_export_database(dir)
    duckdb_checkpoint([force]), duckdb_analyze([table]), duckdb_file_size(),
    duckdb_setting(name), duckdb_loaded_extensions()

  There is deliberately no import counterpart (IMPORT DATABASE would create
  tables that exist only in DuckDB, without MariaDB table definitions) and
  no VACUUM wrapper (it never shrinks the file; see docs/space-reclaim.md).

  The wrappers run on their own DuckDB connection, which is why all but the
  read-only introspection functions are refused inside an open transaction
  and why loaded rows are not binlogged.
*/

#define MYSQL_SERVER
#include "mariadb.h"
#include "item.h"
#include <mysql/plugin_function.h>

#undef UNKNOWN

#include "sql_acl.h"
#include "privilege.h"
#include "mysqld.h"
#include "sql_class.h"
#include "sql_parse.h"
#include "handler.h"
#include "ha_duckdb.h"

#include <sys/stat.h>
#include <string>
#include <vector>

#include "duckdb_query.h"
#include "duckdb_manager.h"
#include "duckdb_types.h"

extern my_bool allow_duckdb_file_io;
extern char *duckdb_remote_url_prefixes;

namespace
{

typedef std::vector<std::string> Args;

struct FnResult
{
  std::string str;
  longlong num= 0;
  bool is_null= false;
};

typedef bool (*FnImpl)(THD *thd, const Args &args, FnResult &out);

/* ----- errors, quoting and validation helpers ----- */

/* Raise a DuckDB error; returns true so callers can `return fail(..)`. */
bool fail(const char *msg)
{
  my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_GENERIC, msg, "DuckDB");
  return true;
}

/*
  Backticks are refused because the engine's own query path,
  backticks_to_double_quotes(), rewrites them even inside string literals;
  keeping them out means a wrapper's SQL means the same on either path.
*/
bool has_bad_chars(const std::string &s)
{
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7f || c == '`')
      return true;
  return false;
}

/* A DuckDB string literal; identifiers use quote_duckdb_identifier(). */
std::string literal(const std::string &s)
{
  std::string r(1, '\'');
  for (char c : s)
  {
    if (c == '\'')
      r+= c;
    r+= c;
  }
  return r + '\'';
}

std::string qualified(const std::string &db, const std::string &tbl)
{
  return quote_duckdb_identifier(db) + "." + quote_duckdb_identifier(tbl);
}

/*
  The wrappers run on their own DuckDB connection, so they cannot see rows
  written earlier in the caller's open transaction and cannot be rolled back.
*/
bool check_no_open_trx(THD *thd)
{
  return thd->in_active_multi_stmt_transaction() &&
         fail("not allowed inside an open transaction");
}

std::string casedn_name(const std::string &s)
{
  std::string r(s.size() * files_charset_info->casedn_multiply(), '\0');
  r.resize(files_charset_info->casedn(s.data(), s.size(), &r[0], r.size()));
  return r;
}

/* Split "db.tbl" or "tbl" (current database) into db and table. */
bool parse_table_name(THD *thd, const std::string &arg, std::string &db,
                      std::string &tbl)
{
  if (arg.empty() || has_bad_chars(arg))
    return fail("invalid table name");
  size_t dot= arg.find('.');
  if (dot == std::string::npos)
  {
    if (!thd->db.str)
    {
      my_error(ER_NO_DB_ERROR, MYF(0));
      return true;
    }
    db.assign(thd->db.str, thd->db.length);
    tbl= arg;
  }
  else
  {
    db= arg.substr(0, dot);
    tbl= arg.substr(dot + 1);
  }
  if (db.empty() || tbl.empty() || tbl.find('.') != std::string::npos)
    return fail("table name must be 'table' or 'database.table'");
  /* As the parser does, so that MDL keys match other statements' locks. */
  if (lower_case_table_names)
  {
    db= casedn_name(db);
    tbl= casedn_name(tbl);
  }
  return false;
}

/*
  Loads are writes: honour read_only and a read-only transaction like any
  other data change, and refuse while this connection holds FLUSH TABLES
  WITH READ LOCK or a backup lock.
*/
bool check_writable(THD *thd)
{
  if (thd->tx_read_only)
  {
    my_error(ER_CANT_EXECUTE_IN_READ_ONLY_TRANSACTION, MYF(0));
    return true;
  }
  bool admin=
      (thd->security_ctx->master_access & PRIV_IGNORE_READ_ONLY) != NO_ACL;
#if MYSQL_VERSION_ID >= 120000
  /* 12.0 made read_only a level; the top one binds admins too. */
  if (opt_readonly >= READONLY_NO_LOCK_NO_ADMIN)
    admin= false;
#endif
  if (opt_readonly && !admin && !thd->slave_thread)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--read-only");
    return true;
  }
  return thd->has_read_only_protection();
}

/*
  Prepare db.tbl for a wrapper that reads it (export, analyze) or writes it
  (load): check the table privileges, take the metadata locks a normal
  statement would take, then check that it is an ENGINE=DuckDB table.

  DuckDB works on the table through its own connection, so without the
  locks a load or export would ignore LOCK TABLES, ALTER/DROP TABLE and (for
  loads) FLUSH TABLES WITH READ LOCK or BACKUP STAGE in other sessions.
  The locks last until the end of the statement.
*/
bool prepare_table(THD *thd, privilege_t want, bool write,
                   const std::string &db, const std::string &tbl)
{
  LEX_CSTRING db_l= {db.c_str(), db.length()};
  LEX_CSTRING tbl_l= {tbl.c_str(), tbl.length()};
  TABLE_LIST tl;
  tl.init_one_table(&db_l, &tbl_l, 0, TL_READ);
  if (check_table_access(thd, want, &tl, false, 1, false))
    return true;

  /* Waiting for a lock while holding LOCK TABLES locks could deadlock. */
  if (thd->locked_tables_mode)
    return fail("not allowed under LOCK TABLES");
  if (write && check_writable(thd))
    return true;
  MDL_request backup, table;
  MDL_REQUEST_INIT(&backup, MDL_key::BACKUP, "", "", MDL_BACKUP_DML,
                   MDL_STATEMENT);
  MDL_REQUEST_INIT(&table, MDL_key::TABLE, db.c_str(), tbl.c_str(),
                   write ? MDL_SHARED_WRITE : MDL_SHARED_READ, MDL_STATEMENT);
  double timeout= thd->variables.lock_wait_timeout;
  if ((write && thd->mdl_context.acquire_lock(&backup, timeout)) ||
      thd->mdl_context.acquire_lock(&table, timeout))
    return true;

  /*
    Privileges are checked against the MariaDB table, but the statement runs
    against the DuckDB catalog by name.  Make sure the name really is an
    ENGINE=DuckDB table, so a broad grant cannot reach other DuckDB schemas
    or internal objects.
  */
  handlerton *hton= NULL;
#if MYSQL_VERSION_ID >= 110800
  bool exists= ha_table_exists(thd, &db_l, &tbl_l, NULL, &hton);
#else
  bool exists= ha_table_exists(thd, &db_l, &tbl_l, NULL, NULL, &hton);
#endif
  if (!exists || hton != duckdb_hton)
    return fail("not an existing ENGINE=DuckDB table");
  return false;
}

/* Global gate for every function that touches files or the network. */
bool check_file_io_allowed(THD *thd)
{
  if (!allow_duckdb_file_io)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0),
             "--duckdb-allow-file-io=OFF");
    return true;
  }
  return check_global_access(thd, FILE_ACL);
}

/*
  Remote reads are enabled only by listing URL prefixes in
  duckdb_remote_url_prefixes.  A prefix matches only if it ends with '/' (or
  equals the whole URL), so each entry pins a host: https://host would
  otherwise also accept https://host.evil.com/ or https://host@evil/.  An
  allow-list, rather than an on/off switch, keeps the server from being used
  to reach internal endpoints (SSRF), for example a cloud metadata service.
  Redirects followed by the HTTP client are not covered, so list only hosts
  you trust.  Raises the error itself and returns false when not allowed.
*/
bool url_allowed(const std::string &url)
{
  std::string list;
  mysql_mutex_lock(&LOCK_global_system_variables);
  if (duckdb_remote_url_prefixes)
    list= duckdb_remote_url_prefixes;
  mysql_mutex_unlock(&LOCK_global_system_variables);

  bool any= false;
  for (size_t pos= 0; pos <= list.size();)
  {
    size_t comma= list.find(',', pos);
    if (comma == std::string::npos)
      comma= list.size();
    size_t b= pos, e= comma;
    while (b < e && list[b] == ' ')
      b++;
    while (e > b && list[e - 1] == ' ')
      e--;
    pos= comma + 1;
    if (e == b)
      continue;
    any= true;
    size_t n= e - b;
    if (url.compare(0, n, list, b, n) == 0 &&
        (list[e - 1] == '/' || url.size() == n))
      return true;
  }
  if (any)
    fail("URL is not in duckdb_remote_url_prefixes");
  else
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0),
             "--duckdb-remote-url-prefixes=''");
  return false;
}

bool has_dotdot_component(const std::string &path)
{
  for (size_t pos= 0; pos <= path.size();)
  {
    size_t next= path.find_first_of("/\\", pos);
    if (next == std::string::npos)
      next= path.size();
    if (path.compare(pos, next - pos, "..") == 0)
      return true;
    pos= next + 1;
  }
  return false;
}

/*
  A remote server may normalize "/a/../b" (also percent-encoded) to "/b",
  which would escape the path of a duckdb_remote_url_prefixes entry.
*/
bool url_has_dotdot(const std::string &url)
{
  std::string s;
  for (size_t i= 0; i < url.size(); i++)
  {
    if (url[i] == '%' && i + 2 < url.size())
    {
      std::string hex= url.substr(i + 1, 2);
      for (char &c : hex)
        c= (char) tolower((unsigned char) c);
      if (hex == "2e" || hex == "2f" || hex == "5c")
      {
        s+= hex == "2e" ? '.' : '/';
        i+= 2;
        continue;
      }
    }
    s+= url[i];
  }
  return has_dotdot_component(s);
}

/* Validate a source or destination path (local file or allowed URL). */
bool check_io_path(const std::string &path, bool for_write)
{
  if (path.empty() || has_bad_chars(path))
    return fail("invalid path");

  size_t sep= path.find("://");
  if (sep != std::string::npos)
  {
    std::string scheme= path.substr(0, sep);
    for (char &c : scheme)
      c= (char) tolower((unsigned char) c);
    if (scheme != "https" && scheme != "http" && scheme != "s3")
      return fail("URL scheme not allowed (https, http, s3 only)");
    if (for_write)
      return fail("writing to remote locations is not supported");
    if (url_has_dotdot(path))
      return fail("URL must not contain '..'");
    return !url_allowed(path);
  }

  if (path[0] == '~' || !test_if_hard_path(path.c_str()))
    return fail("path must be absolute");
  if (has_dotdot_component(path))
    return fail("path must not contain '..'");
  std::string copy= path;
  if (!is_secure_file_path(&copy[0]))
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--secure-file-priv");
    return true;
  }
  /*
    Like SELECT ... INTO OUTFILE, never overwrite an existing file: with an
    empty secure_file_priv that could clobber any file the server can write,
    including the DuckDB database file itself.  The check cannot be atomic
    with DuckDB's own open() of the file, so it does not stop a local user
    who can create files in the target directory from racing the export.
  */
  struct stat st;
  if (for_write && stat(path.c_str(), &st) == 0)
    return fail("target already exists");
  return false;
}

/* ----- running SQL in DuckDB ----- */

/*
  Run SQL in DuckDB and store its first cell in *first (NULL when there are
  no rows).  On failure raises an error and returns true.
*/
bool run_sql(THD *thd, const std::string &sql, duckdb::Value *first= nullptr)
{
  auto res= myduck::duckdb_query_interruptible(thd, sql);
  if (res->HasError())
  {
    report_duckdb_error(thd, res->GetError());
    return true;
  }
  if (first)
  {
    auto chunk= res->Fetch();
    *first= chunk && chunk->size() ? chunk->GetValue(0, 0) : duckdb::Value();
  }
  return false;
}

bool duckdb_file_bytes(longlong &size)
{
  std::string path= std::string(mysql_real_data_home) +
                    myduck::DUCKDB_FILE_NAME;
  struct stat st;
  if (stat(path.c_str(), &st) != 0)
    return fail("cannot stat the DuckDB database file");
  size= (longlong) st.st_size;
  return false;
}

/* ----- ingest / export ----- */

/*
  WRITE=false: INSERT INTO table SELECT * FROM <SQL>(path), SQL being the
  DuckDB reader function.  WRITE=true: COPY table TO path (<SQL>), SQL being
  the format clause.  Both return the row count DuckDB reports.
*/
template <bool WRITE, const char *SQL>
bool fn_transfer(THD *thd, const Args &a, FnResult &out)
{
  std::string db, tbl;
  if (check_no_open_trx(thd) || check_file_io_allowed(thd) ||
      parse_table_name(thd, a[0], db, tbl) || check_io_path(a[1], WRITE) ||
      prepare_table(thd, WRITE ? SELECT_ACL : INSERT_ACL, !WRITE, db, tbl))
    return true;

  std::string table= qualified(db, tbl), path= literal(a[1]);
  std::string sql;
  if (WRITE)
    sql= "COPY " + table + " TO " + path + " (" + SQL + ")";
  else
    sql= "INSERT INTO " + table + " SELECT * FROM " + SQL + "(" + path + ")";
  duckdb::Value count;
  if (run_sql(thd, sql, &count))
    return true;
  out.num= count.IsNull() ? 0 : count.GetValue<int64_t>();
  return false;
}

constexpr char READ_PARQUET[]= "read_parquet";
constexpr char READ_CSV[]= "read_csv";
constexpr char READ_JSON[]= "read_json";
constexpr char FORMAT_PARQUET[]= "FORMAT PARQUET";
constexpr char FORMAT_CSV[]= "FORMAT CSV, HEADER";
constexpr char FORMAT_JSON[]= "FORMAT JSON";

bool fn_export_database(THD *thd, const Args &a, FnResult &out)
{
  if (check_no_open_trx(thd) || check_global_access(thd, SUPER_ACL) ||
      check_file_io_allowed(thd) || check_io_path(a[0], true))
    return true;
  if (run_sql(thd, "EXPORT DATABASE " + literal(a[0]) +
                       " (FORMAT PARQUET)"))
    return true;
  out.str= "OK";
  return false;
}

/* ----- maintenance / introspection ----- */

bool fn_checkpoint(THD *thd, const Args &a, FnResult &out)
{
  if (check_no_open_trx(thd) || check_global_access(thd, SUPER_ACL))
    return true;
  /*
    CHECKPOINT fails cleanly while other transactions are running.
    FORCE CHECKPOINT aborts them instead, so it is only used when the caller
    asks for it with duckdb_checkpoint(1).  See docs/space-reclaim.md.
  */
  bool force= !a.empty() && a[0] == "1";
  if (!a.empty() && !force && a[0] != "0")
    return fail("duckdb_checkpoint() argument must be 0 or 1");

  longlong before, after;
  if (duckdb_file_bytes(before) ||
      run_sql(thd, force ? "FORCE CHECKPOINT" : "CHECKPOINT") ||
      duckdb_file_bytes(after))
    return true;
  out.str= "size_before=" + std::to_string(before) +
           " size_after=" + std::to_string(after);
  return false;
}

bool fn_analyze(THD *thd, const Args &a, FnResult &out)
{
  std::string sql= "ANALYZE";
  if (check_no_open_trx(thd))
    return true;
  if (a.empty() || a[0].empty())
  {
    if (check_global_access(thd, SUPER_ACL))
      return true;
  }
  else
  {
    std::string db, tbl;
    if (parse_table_name(thd, a[0], db, tbl) ||
        prepare_table(thd, SELECT_ACL | INSERT_ACL, false, db, tbl))
      return true;
    sql+= " " + qualified(db, tbl);
  }
  if (run_sql(thd, sql))
    return true;
  out.str= "OK";
  return false;
}

bool fn_file_size(THD *thd, const Args &, FnResult &out)
{
  return check_global_access(thd, SUPER_ACL) || duckdb_file_bytes(out.num);
}

bool fn_setting(THD *thd, const Args &a, FnResult &out)
{
  if (check_global_access(thd, SUPER_ACL))
    return true;
  if (a[0].empty() || has_bad_chars(a[0]))
    return fail("invalid setting name");
  /* Older DuckDB versions expose S3 credentials as plain settings. */
  std::string lower= a[0];
  for (char &c : lower)
    c= (char) tolower((unsigned char) c);
  for (const char *word : {"secret", "token", "password", "_key",
                           "credential", "connection_string"})
    if (lower.find(word) != std::string::npos)
      return fail("this setting cannot be read");

  duckdb::Value v;
  if (run_sql(thd, "SELECT CAST(value AS VARCHAR) FROM duckdb_settings() "
                   "WHERE name = " + literal(a[0]), &v))
    return true;
  if (v.IsNull())
    out.is_null= true;
  else
    out.str= v.ToString();
  return false;
}

bool fn_loaded_extensions(THD *thd, const Args &, FnResult &out)
{
  if (check_global_access(thd, SUPER_ACL))
    return true;
  duckdb::Value v;
  if (run_sql(thd, "SELECT string_agg(extension_name, ',' ORDER BY "
                   "extension_name) FROM duckdb_extensions() WHERE loaded",
              &v))
    return true;
  if (!v.IsNull())
    out.str= v.ToString();
  return false;
}

/* ----- generic Item / Create_func glue ----- */

bool eval_args(Item **args, uint n, Args &vals)
{
  for (uint i= 0; i < n; i++)
  {
    String buf, conv;
    String *s= args[i]->val_str(&buf);
    if (args[i]->null_value || !s)
      return true;
    uint errors= 0;
    if (conv.copy(s->ptr(), s->length(), s->charset(),
                  &my_charset_utf8mb4_general_ci, &errors) ||
        errors)
      return fail("argument is not valid utf8mb4");
    vals.emplace_back(conv.ptr(), conv.length());
  }
  return false;
}

/*
  DuckDB reports many failures as C++ exceptions (type conversions, out of
  memory, connection setup).  An exception escaping into the server would
  terminate it, so turn every one into a normal MariaDB error.
*/
template <FnImpl IMPL>
bool call_impl(THD *thd, const Args &vals, FnResult &r)
{
  try
  {
    return IMPL(thd, vals, r);
  }
  catch (const std::exception &e)
  {
    return fail(e.what());
  }
  catch (...)
  {
    return fail("unknown exception");
  }
}

/*
  Common part of the string- and integer-valued items.  These functions have
  side effects (file I/O, DDL-like maintenance) and must run exactly once per
  row: never constant-fold, never allow them in generated columns or CHECK
  constraints.
*/
template <class BASE, FnImpl IMPL> class Item_duckdb : public BASE
{
  const char *m_name;

public:
  Item_duckdb(THD *thd, List<Item> &list, const char *name)
      : BASE(thd, list), m_name(name)
  {}

  LEX_CSTRING func_name_cstring() const override
  {
    return {m_name, strlen(m_name)};
  }

  bool const_item() const override { return false; }
  table_map used_tables() const override
  {
    return this->used_tables_cache | RAND_TABLE_BIT;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(m_name, arg, VCOL_IMPOSSIBLE);
  }

protected:
  /* Evaluate the arguments and run IMPL; true (and NULL) on NULL or error. */
  bool run_fn(FnResult &r)
  {
    DBUG_ASSERT(this->fixed());
    Args vals;
    this->null_value= eval_args(this->args, this->arg_count, vals) ||
                      call_impl<IMPL>(current_thd, vals, r) || r.is_null;
    return this->null_value;
  }
};

template <FnImpl IMPL>
class Item_duckdb_str : public Item_duckdb<Item_str_func, IMPL>
{
public:
  using Item_duckdb<Item_str_func, IMPL>::Item_duckdb;

  bool fix_length_and_dec(THD *) override
  {
    this->collation.set(&my_charset_utf8mb4_general_ci);
    this->max_length= 65535;
    this->set_maybe_null();
    return FALSE;
  }

  String *val_str(String *str) override
  {
    FnResult r;
    if (this->run_fn(r))
      return NULL;
    if (str->copy(r.str.c_str(), r.str.length(),
                  &my_charset_utf8mb4_general_ci))
    {
      this->null_value= 1;
      return NULL;
    }
    return str;
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_duckdb_str>(thd, this); }
};

template <FnImpl IMPL>
class Item_duckdb_int : public Item_duckdb<Item_long_func, IMPL>
{
public:
  using Item_duckdb<Item_long_func, IMPL>::Item_duckdb;

  const Type_handler *type_handler() const override
  { return &type_handler_slonglong; }

  bool fix_length_and_dec(THD *) override
  {
    this->max_length= 21;
    this->set_maybe_null();
    return FALSE;
  }

  longlong val_int() override
  {
    FnResult r;
    return this->run_fn(r) ? 0 : r.num;
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_duckdb_int>(thd, this); }
};

template <class ITEM, int MIN_ARGS, int MAX_ARGS>
class Create_duckdb_fn : public Create_native_func
{
  const char *m_name;

public:
  explicit Create_duckdb_fn(const char *name) : m_name(name) {}

  Item *create_native(THD *thd, const LEX_CSTRING *name,
                      List<Item> *item_list) override
  {
    int n= item_list ? item_list->elements : 0;
    if (n < MIN_ARGS || n > MAX_ARGS)
    {
      my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
      return NULL;
    }
    duckdb_mark_side_effect_call(thd);
    List<Item> empty;
    return new (thd->mem_root) ITEM(thd, item_list ? *item_list : empty,
                                    m_name);
  }
};

} // namespace

/*
  Descriptor + factory for one function.  The plugin name (declared in
  ha_duckdb.cc) is what MariaDB registers as the SQL function name.  ITEM is
  Item_duckdb_str or Item_duckdb_int; the rest is the implementation, written
  last so that template arguments may contain commas.
*/
#define DUCKDB_FN(ITEM, SYM, MIN_ARGS, MAX_ARGS, ...)                        \
  static Create_duckdb_fn<ITEM<__VA_ARGS__>, MIN_ARGS, MAX_ARGS>             \
      s_create_##SYM(#SYM);                                                  \
  Plugin_function plugin_descriptor_function_##SYM(&s_create_##SYM)

DUCKDB_FN(Item_duckdb_int, duckdb_load_parquet, 2, 2,
          fn_transfer<false, READ_PARQUET>);
DUCKDB_FN(Item_duckdb_int, duckdb_load_csv, 2, 2,
          fn_transfer<false, READ_CSV>);
DUCKDB_FN(Item_duckdb_int, duckdb_load_json, 2, 2,
          fn_transfer<false, READ_JSON>);
DUCKDB_FN(Item_duckdb_int, duckdb_export_parquet, 2, 2,
          fn_transfer<true, FORMAT_PARQUET>);
DUCKDB_FN(Item_duckdb_int, duckdb_export_csv, 2, 2,
          fn_transfer<true, FORMAT_CSV>);
DUCKDB_FN(Item_duckdb_int, duckdb_export_json, 2, 2,
          fn_transfer<true, FORMAT_JSON>);
DUCKDB_FN(Item_duckdb_str, duckdb_export_database, 1, 1, fn_export_database);
DUCKDB_FN(Item_duckdb_str, duckdb_checkpoint, 0, 1, fn_checkpoint);
DUCKDB_FN(Item_duckdb_str, duckdb_analyze, 0, 1, fn_analyze);
DUCKDB_FN(Item_duckdb_int, duckdb_file_size, 0, 0, fn_file_size);
DUCKDB_FN(Item_duckdb_str, duckdb_setting, 1, 1, fn_setting);
DUCKDB_FN(Item_duckdb_str, duckdb_loaded_extensions, 0, 0,
          fn_loaded_extensions);
