/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov <leontodys@gmail.com>

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

#define MYSQL_SERVER 1
#include "mariadb_authorizer.h"
#include "acl_compat.h"
#include "account_row.h"

#include "password.h"

#include "mysqld.h"
#include "table.h"

#include <cctype>

namespace quack_bridge
{

using quack_guard::AccessRequest;
using quack_guard::ObjectKind;
using quack_guard::ObjectName;
using quack_guard::Reason;
using quack_guard::Verdict;

static bool loopback(const std::string &ip)
{
  return ip == "127.0.0.1" || ip == "::1";
}

std::string HostForPeer(const std::string &ip)
{
  /* As the server does at login: loopback is "localhost" unless name
     resolution is off, otherwise the address itself */
  if (loopback(ip) && !opt_skip_name_resolve)
    return my_localhost;
  return ip;
}

static LEX_CSTRING lex(const std::string &s) { return {s.c_str(), s.size()}; }

static std::string lower_ascii(std::string s)
{
  for (auto &c : s)
    c= (char) std::tolower((unsigned char) c);
  return s;
}

/*
  Make `sctx` the identity's current security context on `thd`: the account
  is matched again from the live ACL cache (it may have been dropped or
  renamed), must still be the account pinned at login, and the role must
  still be granted to it. The strings in `id` must outlive the use of sctx.
*/
static Reason become(THD *thd, const MariaDBIdentity &id, Security_context &sctx)
{
  if (opt_noacl)
    return Reason::Denied;  /* --skip-grant-tables: no grants to check */
  if (id.user.empty() || id.host.empty() || id.ip.empty())
    return Reason::NoSuchAccount;  /* anonymous accounts never match; an
                                      empty host would select a role */
  /* With name resolution on, a login from another host is matched by its
     host name, which the bridge does not resolve: an account defined by
     name could be passed over for a broader one. Not supported. */
  if (!opt_skip_name_resolve && !loopback(id.ip))
    return Reason::Unsupported;
  static const std::string no_db;
  /* acl_getroot() never picks an anonymous account; a login would, if one
     matches this peer more specifically. Refuse whenever one matches. */
  {
    static const std::string anonymous;
    Security_context probe;
    if (!compat_acl_getroot(&probe, lex(anonymous), lex(id.host), lex(id.ip),
                            lex(no_db)))
      return Reason::NoSuchAccount;
  }
  if (compat_acl_getroot(&sctx, lex(id.user), lex(id.host), lex(id.ip),
                         lex(no_db)))
    return Reason::NoSuchAccount;
  if (id.priv_user.empty() ? false
                           : (id.priv_user != sctx.priv_user ||
                              id.priv_host != sctx.priv_host))
    return Reason::NoSuchAccount;  /* now matches another account */
  thd->security_ctx= &sctx;
  if (!id.role.empty())
  {
    privileges_t access(NO_ACL);
    if (compat_acl_check_setrole(thd, lex(id.role), &access))
      return Reason::Denied;
    compat_acl_setrole(thd, lex(id.role), access);
  }
  return Reason::Allowed;
}

/* Is this MariaDB table an ENGINE=DuckDB table, unambiguously named? */
static bool is_engine_table(THD *thd, handlerton *engine, const std::string &db,
                            const std::string &table)
{
  if (compat_table_engine(thd, lex(db), lex(table)) != engine)
    return false;
  if (lower_case_table_names == 0)
  {
    /* DuckDB's catalog is case-insensitive: refuse when MariaDB has another
       table that DuckDB would not tell apart */
    auto ldb= lower_ascii(db), ltable= lower_ascii(table);
    if ((ldb != db || ltable != table) &&
        compat_table_engine(thd, lex(ldb), lex(ltable)))
      return false;
  }
  return true;
}

/* SELECT on the table, or on each listed column; with any_column also some
   privilege on the table or one of its columns */
static bool may_read(THD *thd, const AccessRequest &request)
{
  LEX_CSTRING db= lex(request.schema), table= lex(request.table);
  TABLE_LIST tl;
  tl.reset();
  tl.init_one_table(&db, &table, nullptr, TL_READ);
  if (check_table_access(thd, SELECT_ACL, &tl, false, 1, true))
    return false;
  if (!tl.grant.want_privilege)
    return true;  /* table-level (or higher) SELECT */
  for (auto &column : request.columns)
  {
    if (compat_check_grant_column(thd, &tl.grant, db.str, table.str,
                                  lex(column)))
    {
      thd->clear_error();
      return false;
    }
  }
  if (request.columns.empty())
  {
    /* No column named: any privilege on the table or a column will do */
    if (check_grant(thd, SELECT_ACL, &tl, true, 1, true))
      return false;
  }
  return true;
}

/* Table-level (or higher) SELECT: the catalog shows every column */
static bool table_visible(THD *thd, const ObjectName &object)
{
  LEX_CSTRING db= lex(object.schema), table= lex(object.table);
  TABLE_LIST tl;
  tl.reset();
  tl.init_one_table(&db, &table, nullptr, TL_READ);
  return !check_table_access(thd, SELECT_ACL, &tl, false, 1, true) &&
         !tl.grant.want_privilege;
}

MariaDBAuthorizer::MariaDBAuthorizer(handlerton *engine_p,
                                     BridgeSettings settings_p)
    : engine(engine_p), settings(settings_p)
{
}

MariaDBAuthorizer::~MariaDBAuthorizer() { Stop(); }

Verdict MariaDBAuthorizer::run(AclWorker::Job job)
{
  size_t queue_size= settings.queue_size ? settings.queue_size->load() : 0;
  uint64_t timeout= settings.timeout_ms ? settings.timeout_ms->load() : 0;
  return worker.Run(std::move(job), queue_size, timeout);
}

/* What the worker found out about a login; the throttle and the session
   limits are applied by the caller, which owns that state */
struct LoginFacts
{
  Reason reason= Reason::Internal;  /* Allowed: all checks but the password passed */
  bool password_ok= false;
  std::string priv_user, priv_host, role;
  int64_t max_user_connections= 0;
  /* Changes when the account is altered (password, lock, ...) */
  std::string row_version;
};

static void wipe(std::string &secret)
{
  volatile char *p= secret.empty() ? nullptr : &secret[0];
  for (size_t i= 0; i < secret.size(); i++)
    p[i]= 0;
  secret.clear();
}

static bool same(const std::string &a, const std::string &b)
{
  /* constant time for equal lengths */
  if (a.size() != b.size())
    return false;
  unsigned char diff= 0;
  for (size_t i= 0; i < a.size(); i++)
    diff|= (unsigned char) (std::toupper((unsigned char) a[i]) ^
                            std::toupper((unsigned char) b[i]));
  return diff == 0;
}

/* The account's own method: milestone 1 knows mysql_native_password */
static bool password_matches(const AccountRow &row, const std::string &proof)
{
  if (row.authentication_string.empty())
    return proof.empty();
  if (row.authentication_string.size() != SCRAMBLED_PASSWORD_CHAR_LENGTH)
    return false;  /* pre-4.1 hashes are not accepted */
  char computed[SCRAMBLED_PASSWORD_CHAR_LENGTH + 1];
  my_make_scrambled_password(computed, proof.c_str(), proof.size());
  std::string ours(computed, SCRAMBLED_PASSWORD_CHAR_LENGTH);
  bool match= same(ours, row.authentication_string);
  wipe(ours);
  return match;
}

static LoginFacts login_facts(THD *thd, const MariaDBIdentity &claimed,
                              const std::string &proof,
                              const quack_guard::TransportFacts &transport)
{
  LoginFacts facts;
  Security_context sctx;
  MariaDBIdentity base= claimed;
  base.role.clear();
  auto reason= become(thd, base, sctx);
  if (reason != Reason::Allowed)
  {
    facts.reason= reason;
    return facts;
  }
  facts.priv_user= sctx.priv_user;
  facts.priv_host= sctx.priv_host;
  thd->security_ctx= &thd->main_security_ctx;

  AccountRow row;
  if (!ReadAccount(facts.priv_user, facts.priv_host, row))
    return facts;  /* Internal */
  if (row.is_role)
  {
    facts.reason= Reason::NoSuchAccount;
    return facts;
  }
  if (row.multiple_auth ||
      !(row.plugin.empty() || row.plugin == "mysql_native_password"))
  {
    facts.reason= Reason::Unsupported;
    return facts;
  }
  if (row.locked)
  {
    facts.reason= Reason::Locked;
    return facts;
  }
  /* As the server: PASSWORD EXPIRE stores 0; for the lifetime a missing
     date counts as 0, and whole days are compared with >= */
  int64_t lifetime= row.password_lifetime < 0
                        ? (int64_t) default_password_lifetime
                        : row.password_lifetime;
  int64_t changed= row.password_last_changed < 0 ? 0 : row.password_last_changed;
  if (row.password_last_changed == 0 ||
      (lifetime > 0 &&
       ((int64_t) time(nullptr) - changed) / 3600 / 24 >= lifetime))
  {
    facts.reason= Reason::Expired;
    return facts;
  }
  /* REQUIRE SSL is met by the listener's TLS; REQUIRE X509 / ISSUER /
     SUBJECT / CIPHER need client certificates, which are not requested */
  if (row.ssl_type > 1 || (row.ssl_type == 1 && !transport.tls))
  {
    facts.reason= Reason::RequirementUnmet;
    return facts;
  }
  facts.password_ok= password_matches(row, proof);
  /* The account's own limit if set, else the server's; 0 is none and a
     negative one refuses (as the server does for such accounts) */
  facts.max_user_connections= row.max_user_connections != 0
                                  ? row.max_user_connections
                                  : row.global_max_user_connections;
  facts.row_version= row.authentication_string + "|" +
                     std::to_string(row.password_last_changed) + "|" +
                     (row.locked ? "L" : "U") + "|" +
                     std::to_string(row.ssl_type) + "|" + row.plugin;

  /* Role: the requested one must be granted; the default one is used when
     none is requested and it is still granted */
  std::string role= claimed.role.empty() ? row.default_role : claimed.role;
  if (!role.empty())
  {
    MariaDBIdentity with_role= claimed;
    with_role.role= role;
    with_role.priv_user= facts.priv_user;
    with_role.priv_host= facts.priv_host;
    Security_context role_sctx;
    if (become(thd, with_role, role_sctx) == Reason::Allowed)
      facts.role= role;
    else if (!claimed.role.empty())
    {
      facts.reason= Reason::Denied;
      return facts;
    }
  }
  facts.reason= Reason::Allowed;
  return facts;
}

static const char *reason_text(Reason reason)
{
  switch (reason)
  {
  case Reason::Denied: return "wrong password or role not granted";
  case Reason::NoSuchAccount: return "no such account";
  case Reason::Locked: return "account locked or blocked after wrong passwords";
  case Reason::Expired: return "password expired";
  case Reason::RequirementUnmet: return "REQUIRE not met";
  case Reason::Unsupported: return "authentication method or host not supported";
  case Reason::Timeout: return "ACL worker timeout";
  case Reason::Saturated: return "connection limit or ACL queue full";
  case Reason::Shutdown: return "shutting down";
  default: return "internal error";
  }
}

/* As the server logs refused logins: with log_warnings > 1 */
Verdict MariaDBAuthorizer::deny_login(const std::string &account,
                                      const quack_guard::TransportFacts &transport,
                                      Reason reason)
{
  if (global_system_variables.log_warnings > 1)
    sql_print_warning("DuckDB: Quack login refused for '%s' from %s: %s",
                      account.c_str(), transport.peer_ip.c_str(),
                      reason_text(reason));
  return Verdict::Deny(reason);
}

Verdict MariaDBAuthorizer::authenticate(
    const quack_guard::ClientClaims &claims,
    const quack_guard::TransportFacts &transport,
    std::shared_ptr<const quack_guard::Identity> &out)
{
  try
  {
    return authenticate_unsafe(claims, transport, out);
  }
  catch (...)
  {
    return Verdict::Deny(Reason::Internal);
  }
}

Verdict MariaDBAuthorizer::authenticate_unsafe(
    const quack_guard::ClientClaims &claims,
    const quack_guard::TransportFacts &transport,
    std::shared_ptr<const quack_guard::Identity> &out)
{
  auto claimed= std::make_shared<MariaDBIdentity>();
  claimed->user= claims.user;
  claimed->ip= transport.peer_ip;
  claimed->host= HostForPeer(transport.peer_ip);
  claimed->role= claims.role;
  /* The job owns a copy of the password and wipes it when done */
  auto proof= std::make_shared<std::string>(claims.proof);
  auto facts= std::make_shared<LoginFacts>();
  auto transport_copy= std::make_shared<quack_guard::TransportFacts>(transport);
  auto verdict= run([claimed, proof, facts, transport_copy](
                        THD *thd, const std::atomic<bool> &) {
    *facts= login_facts(thd, *claimed, *proof, *transport_copy);
    wipe(*proof);
    return facts->reason == Reason::Allowed ? Verdict::Allow()
                                            : Verdict::Deny(facts->reason);
  });
  if (!verdict.allow)
    return deny_login(claims.user, transport, verdict.reason);

  std::string account= facts->priv_user + "@" + facts->priv_host;
  auto identity= std::make_shared<MariaDBIdentity>(*claimed);
  identity->priv_user= facts->priv_user;
  identity->priv_host= facts->priv_host;
  identity->role= facts->role;
  {
    std::lock_guard<std::mutex> guard(state_lock);
    /* As max_password_errors: after that many wrong passwords in a row the
       account is refused over Quack, even with the right password, until
       the account is altered (ALTER USER: new password, lock/unlock, ...),
       re-created, or the server restarts. Kept apart from the server's own
       counter. */
    auto &failed= failures[account];
    if (failed.row_version != facts->row_version)
      failed= {facts->row_version, 0};
    if (max_password_errors && (uint) failed.count >= max_password_errors)
      return deny_login(account, transport, Reason::Locked);
    if (!facts->password_ok)
    {
      failed.count++;
      return deny_login(account, transport, Reason::Denied);
    }
    failures.erase(account);
    int64_t limit= facts->max_user_connections;
    auto &open= sessions[account];
    if (limit < 0 || (limit > 0 && open >= limit))
      return deny_login(account, transport, Reason::Saturated);
    open++;
  }
  out= identity;
  return Verdict::Allow();
}

Verdict MariaDBAuthorizer::resolve(const std::string &user,
                                   const std::string &ip,
                                   const std::string &role,
                                   std::shared_ptr<const MariaDBIdentity> &out)
{
  try
  {
    return resolve_unsafe(user, ip, role, out);
  }
  catch (...)
  {
    return Verdict::Deny(Reason::Internal);
  }
}

Verdict MariaDBAuthorizer::resolve_unsafe(
    const std::string &user, const std::string &ip, const std::string &role,
    std::shared_ptr<const MariaDBIdentity> &out)
{
  auto identity= std::make_shared<MariaDBIdentity>();
  identity->user= user;
  identity->ip= ip;
  identity->host= HostForPeer(ip);
  identity->role= role;
  auto verdict= run([identity](THD *thd, const std::atomic<bool> &) {
    Security_context sctx;
    auto reason= become(thd, *identity, sctx);
    if (reason != Reason::Allowed)
      return Verdict::Deny(reason);
    identity->priv_user= sctx.priv_user;
    identity->priv_host= sctx.priv_host;
    return Verdict::Allow();
  });
  if (verdict.allow)
    out= identity;
  return verdict;
}

Verdict MariaDBAuthorizer::check(const quack_guard::Identity &identity_p,
                                 const std::vector<AccessRequest> &requests_p)
{
  try
  {
    return check_unsafe(identity_p, requests_p);
  }
  catch (...)
  {
    return Verdict::Deny(Reason::Internal);
  }
}

Verdict MariaDBAuthorizer::check_unsafe(const quack_guard::Identity &identity_p,
                                        const std::vector<AccessRequest> &requests_p)
{
  auto *identity= dynamic_cast<const MariaDBIdentity *>(&identity_p);
  if (!identity || requests_p.empty())
    return Verdict::Deny(Reason::Denied);
  for (auto &r : requests_p)
    if (r.columns.empty() && !r.any_column)
      return Verdict::Deny(Reason::Denied);
  /* Copies: the job may outlive this call after a timeout */
  auto id= std::make_shared<MariaDBIdentity>(*identity);
  auto requests= std::make_shared<std::vector<AccessRequest>>(requests_p);
  auto *engine_p= engine;
  return run([id, requests, engine_p](THD *thd,
                                      const std::atomic<bool> &abandoned) {
    Security_context sctx;
    auto reason= become(thd, *id, sctx);
    if (reason != Reason::Allowed)
      return Verdict::Deny(reason);
    for (auto &request : *requests)
    {
      if (abandoned)
        return Verdict::Deny(Reason::Timeout);
      if (!is_engine_table(thd, engine_p, request.schema, request.table) ||
          !may_read(thd, request))
        return Verdict::Deny(Reason::Denied);
    }
    return Verdict::Allow();
  });
}

std::vector<ObjectName>
MariaDBAuthorizer::filterVisible(const quack_guard::Identity &identity_p,
                                 const std::vector<ObjectName> &objects_p)
{
  try
  {
    return filter_unsafe(identity_p, objects_p);
  }
  catch (...)
  {
    return {};
  }
}

std::vector<ObjectName>
MariaDBAuthorizer::filter_unsafe(const quack_guard::Identity &identity_p,
                                 const std::vector<ObjectName> &objects_p)
{
  auto *identity= dynamic_cast<const MariaDBIdentity *>(&identity_p);
  if (!identity || objects_p.empty())
    return {};
  auto id= std::make_shared<MariaDBIdentity>(*identity);
  auto objects= std::make_shared<std::vector<ObjectName>>(objects_p);
  auto result= std::make_shared<std::vector<ObjectName>>();
  auto *engine_p= engine;
  auto verdict= run([id, objects, result, engine_p](
                         THD *thd, const std::atomic<bool> &abandoned) {
    Security_context sctx;
    auto reason= become(thd, *id, sctx);
    if (reason != Reason::Allowed)
      return Verdict::Deny(reason);
    for (auto &object : *objects)
    {
      if (abandoned)
        return Verdict::Deny(Reason::Timeout);
      bool visible= false;
      if (object.kind == ObjectKind::Schema)
        visible= compat_db_visible(thd, object.schema.c_str());
      else if (object.kind == ObjectKind::Table)
        visible= is_engine_table(thd, engine_p, object.schema, object.table) &&
                 table_visible(thd, object);
      if (visible)
        result->push_back(object);
    }
    return Verdict::Allow();
  });
  /* The job filled `result` only if it ran to the end in time */
  return verdict.allow ? *result : std::vector<ObjectName>();
}

void MariaDBAuthorizer::sessionClosed(const quack_guard::Identity &identity_p)
{
  auto *identity= dynamic_cast<const MariaDBIdentity *>(&identity_p);
  if (!identity)
    return;
  std::lock_guard<std::mutex> guard(state_lock);
  auto it= sessions.find(identity->priv_user + "@" + identity->priv_host);
  if (it != sessions.end() && it->second > 0 && --it->second == 0)
    sessions.erase(it);
}

} /* namespace quack_bridge */
