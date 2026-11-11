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

Verdict MariaDBAuthorizer::authenticate(const quack_guard::ClientClaims &,
                                        const quack_guard::TransportFacts &,
                                        std::shared_ptr<const quack_guard::Identity> &)
{
  /* Credentials are verified from the next stage on; until then nobody */
  return Verdict::Deny(Reason::Unsupported);
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

void MariaDBAuthorizer::sessionClosed(const quack_guard::Identity &) {}

} /* namespace quack_bridge */
