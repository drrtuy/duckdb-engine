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

/*
  QUACK_BRIDGE_TEST(spec): drives the bridge from SQL, for MTR tests of
  MariaDB grants without a Quack client. Available only when the server was
  started with --duckdb-quack-bridge-test (read-only, default OFF), and only
  to SUPER. It never handles credentials: accounts are matched as at login
  but without a password.

  spec: "check|user|ip|role|db.t:c1,c2;db.t2:*"   -> "allow" / "deny:<reason>"
        "visible|user|ip|role|db;db.t"           -> visible items, comma-separated
        "pin|name|user|ip|role"                  -> resolve once, keep as `name`
        "pinned|name|db.t:c1"                    -> check with the kept identity
  (a pinned identity shows what a session sees after its account changes)
  ("*" asks for any column of the table, as count(*) does)
*/

#define MYSQL_SERVER 1
#include <my_global.h>
#include "item.h"
#include <mysql/plugin_function.h>
#undef UNKNOWN
#include "sql_acl.h"
#include "privilege.h"

#include "bridge.h"

#include <map>
#include <mutex>
#include <sstream>

extern my_bool quack_bridge_test;

namespace
{

using quack_guard::AccessRequest;
using quack_guard::ObjectKind;
using quack_guard::ObjectName;
using quack_guard::Reason;

std::vector<std::string> split(const std::string &s, char sep)
{
  std::vector<std::string> parts;
  std::string part;
  std::istringstream in(s);
  while (std::getline(in, part, sep))
    parts.push_back(part);
  if (!s.empty() && s.back() == sep)
    parts.push_back("");
  return parts;
}

const char *reason_name(Reason reason)
{
  switch (reason)
  {
  case Reason::Allowed: return "allowed";
  case Reason::Denied: return "denied";
  case Reason::NoSuchAccount: return "no_such_account";
  case Reason::Locked: return "locked";
  case Reason::Expired: return "expired";
  case Reason::RequirementUnmet: return "requirement_unmet";
  case Reason::Unsupported: return "unsupported";
  case Reason::Timeout: return "timeout";
  case Reason::Saturated: return "saturated";
  case Reason::Shutdown: return "shutdown";
  case Reason::Internal: return "internal";
  }
  return "internal";
}

std::string check_items(quack_bridge::MariaDBAuthorizer &bridge,
                        const quack_bridge::MariaDBIdentity &identity,
                        const std::string &items)
{
  std::vector<AccessRequest> requests;
  for (auto &item : split(items, ';'))
  {
    auto colon= item.find(':');
    auto dot= item.find('.');
    if (colon == std::string::npos || dot == std::string::npos || dot > colon)
      return "error:spec";
    AccessRequest request;
    request.schema= item.substr(0, dot);
    request.table= item.substr(dot + 1, colon - dot - 1);
    for (auto &column : split(item.substr(colon + 1), ','))
      if (column == "*")
        request.any_column= true;
      else if (!column.empty())
        request.columns.push_back(column);
    requests.push_back(std::move(request));
  }
  auto verdict= bridge.check(identity, requests);
  return verdict.allow ? "allow"
                       : std::string("deny:") + reason_name(verdict.reason);
}

std::string visible_items(quack_bridge::MariaDBAuthorizer &bridge,
                          const quack_bridge::MariaDBIdentity &identity,
                          const std::string &items)
{
  std::vector<ObjectName> objects;
  for (auto &item : split(items, ';'))
  {
    auto dot= item.find('.');
    if (dot == std::string::npos)
      objects.push_back({ObjectKind::Schema, "", item, "", ""});
    else
      objects.push_back({ObjectKind::Table, "", item.substr(0, dot),
                         item.substr(dot + 1), ""});
  }
  std::string out;
  for (auto &o : bridge.filterVisible(identity, objects))
  {
    if (!out.empty())
      out+= ",";
    out+= o.kind == ObjectKind::Schema ? o.schema : o.schema + "." + o.table;
  }
  return out;
}

/* Identities kept by "pin", as a session keeps the one it logged in with */
std::mutex pinned_lock;
std::map<std::string, std::shared_ptr<const quack_bridge::MariaDBIdentity>>
    pinned;

std::string run_spec(const std::string &spec)
{
  auto fields= split(spec, '|');
  auto bridge= quack_bridge::Bridge();
  if (!bridge)
    return "deny:shutdown";
  if (fields.size() == 3 && fields[0] == "pinned")
  {
    std::shared_ptr<const quack_bridge::MariaDBIdentity> identity;
    {
      std::lock_guard<std::mutex> guard(pinned_lock);
      auto it= pinned.find(fields[1]);
      if (it == pinned.end())
        return "error:spec";
      identity= it->second;
    }
    return check_items(*bridge, *identity, fields[2]);
  }
  if (fields.size() != 5)
    return "error:spec";
  std::shared_ptr<const quack_bridge::MariaDBIdentity> identity;
  if (fields[0] == "pin")
  {
    auto login= bridge->resolve(fields[2], fields[3], fields[4], identity);
    if (!login.allow)
      return std::string("deny:") + reason_name(login.reason);
    std::lock_guard<std::mutex> guard(pinned_lock);
    pinned[fields[1]]= identity;
    return "pinned";
  }
  auto login= bridge->resolve(fields[1], fields[2], fields[3], identity);
  if (!login.allow)
    return std::string("deny:") + reason_name(login.reason);
  if (fields[0] == "check")
    return check_items(*bridge, *identity, fields[4]);
  if (fields[0] == "visible")
    return visible_items(*bridge, *identity, fields[4]);
  return "error:spec";
}

class Item_func_quack_bridge_test : public Item_str_func
{
public:
  Item_func_quack_bridge_test(THD *thd, Item *a) : Item_str_func(thd, a) {}

  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("quack_bridge_test")};
    return name;
  }

  bool fix_length_and_dec(THD *) override
  {
    collation.set(&my_charset_bin);
    max_length= 65535;
    set_maybe_null();
    return FALSE;
  }

  String *val_str(String *str) override
  {
    THD *thd= current_thd;
    if (!quack_bridge_test)
    {
      my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0),
               "--skip-duckdb-quack-bridge-test");
      null_value= 1;
      return NULL;
    }
    if (check_global_access(thd, SUPER_ACL))
    {
      null_value= 1;
      return NULL;
    }
    String *arg= args[0]->val_str(str);
    if ((null_value= args[0]->null_value))
      return NULL;
    std::string result;
    if (arg->length() > 65536)
      result= "error:spec";
    else
    {
      try
      {
        result= run_spec(std::string(arg->ptr(), arg->length()));
      }
      catch (...)
      {
        result= "error:internal";
      }
    }
    if (str->copy(result.c_str(), result.length(), &my_charset_bin))
    {
      null_value= 1;
      return NULL;
    }
    return str;
  }

  Item *shallow_copy(THD *thd) const override
  {
    return get_item_copy<Item_func_quack_bridge_test>(thd, this);
  }
};

class Create_func_quack_bridge_test : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    return new (thd->mem_root) Item_func_quack_bridge_test(thd, arg1);
  }
  static Create_func_quack_bridge_test s_singleton;
};

Create_func_quack_bridge_test Create_func_quack_bridge_test::s_singleton;

} /* namespace */

Plugin_function plugin_descriptor_function_quack_bridge_test(
    &Create_func_quack_bridge_test::s_singleton);
