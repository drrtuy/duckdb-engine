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
  The MariaDB server functions the bridge calls differ between the versions
  the engine builds against (11.4, 12.x, 13.x): string arguments became
  LEX_CSTRING in 12, privilege sets became access_t (allow and deny bits) in
  13. Each wrapper calls the server function itself; the bridge never
  combines privilege bits, so DENY keeps working.
*/

#pragma once

#include <my_global.h>
#include <mysql_version.h>
#include "sql_class.h"
#include "sql_acl.h"
#include "sql_parse.h"
#include "handler.h"

namespace quack_bridge
{

/* The privilege-set type of this server */
using privileges_t= decltype(Security_context::master_access);

inline bool compat_acl_getroot(Security_context *sctx, const LEX_CSTRING &user,
                               const LEX_CSTRING &host, const LEX_CSTRING &ip,
                               const LEX_CSTRING &db)
{
#if MYSQL_VERSION_ID < 110500
  return acl_getroot(sctx, user.str, host.str, ip.str, db.str);
#else
  return acl_getroot(sctx, user, host, ip, db);
#endif
}

/* 0 on success (role granted to the current account); fills `access` */
inline int compat_acl_check_setrole(THD *thd, const LEX_CSTRING &role,
                                    privileges_t *access)
{
#if MYSQL_VERSION_ID < 110500
  return acl_check_setrole(thd, role.str, access);
#else
  return acl_check_setrole(thd, role, access);
#endif
}

inline int compat_acl_setrole(THD *thd, const LEX_CSTRING &role,
                              const privileges_t &access)
{
#if MYSQL_VERSION_ID < 110500
  return acl_setrole(thd, role.str, access);
#else
  return acl_setrole(thd, role, access);
#endif
}

/* false if the column is granted; raises an error otherwise (clear it) */
inline bool compat_check_grant_column(THD *thd, GRANT_INFO *grant,
                                      const char *db, const char *table,
                                      const LEX_CSTRING &column)
{
#if MYSQL_VERSION_ID < 110500
  return check_grant_column(thd, grant, db, table, column.str, column.length,
                            thd->security_ctx);
#else
  return check_grant_column(thd, grant, db, table, Lex_ident_column(column),
                            thd->security_ctx);
#endif
}

/* Any privilege at the database level, or on something in it */
inline bool compat_db_visible(THD *thd, const char *db)
{
  Security_context *sctx= thd->security_ctx;
#if MYSQL_VERSION_ID < 130000
  privilege_t db_access= acl_get_all3(sctx, db, false) | sctx->master_access;
  return (db_access & (DB_ACLS & ~GRANT_ACL)) || !check_grant_db(thd, db);
#else
  access_t db_access= acl_get_all3(sctx, db, false);
  db_access.merge_with_parent(sctx->master_access);
  return (db_access & (DB_ACLS & ~GRANT_ACL)) ||
         !check_grant_db(thd, db_access, db);
#endif
}

/* The engine of an existing table, or null */
inline handlerton *compat_table_engine(THD *thd, const LEX_CSTRING &db,
                                       const LEX_CSTRING &table)
{
  handlerton *hton= nullptr;
#if MYSQL_VERSION_ID < 110700
  if (!ha_table_exists(thd, &db, &table, nullptr, nullptr, &hton, nullptr))
    return nullptr;
#elif MYSQL_VERSION_ID < 130000
  if (!ha_table_exists(thd, &db, &table, nullptr, &hton, nullptr))
    return nullptr;
#else
  if (!ha_table_exists(thd, &db, &table, nullptr, &hton, nullptr, 0))
    return nullptr;
#endif
  return hton;
}

} /* namespace quack_bridge */
