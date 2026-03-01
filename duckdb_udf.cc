/*
  Copyright (c) 2025, Alibaba and/or its affiliates. All Rights Reserved.
  Ported to MariaDB.

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

#include <my_global.h>
#include <mysql/plugin.h>

#undef UNKNOWN

#include "duckdb_query.h"
#include "duckdb_manager.h"

/*
  UDF: duckdb_query(sql_string)
  Executes a SQL query directly in DuckDB and returns the result as a string.
  This is the MariaDB equivalent of AliSQL's CALL dbms_duckdb.query(...).
*/

extern "C"
{

  my_bool duckdb_query_udf_init(UDF_INIT *initid, UDF_ARGS *args,
                                char *message)
  {
    if (args->arg_count != 1)
    {
      strncpy(message, "duckdb_query() requires exactly one string argument",
              MYSQL_ERRMSG_SIZE - 1);
      return 1;
    }

    if (args->arg_type[0] != STRING_RESULT)
      args->arg_type[0]= STRING_RESULT;

    initid->maybe_null= 1;
    initid->max_length= 65535;
    initid->ptr= NULL;
    return 0;
  }

  void duckdb_query_udf_deinit(UDF_INIT *initid)
  {
    if (initid->ptr)
    {
      delete[] initid->ptr;
      initid->ptr= NULL;
    }
  }

  char *duckdb_query_udf(UDF_INIT *initid, UDF_ARGS *args, char *result,
                         unsigned long *length, char *is_null, char *error)
  {
    if (!args->args[0])
    {
      *is_null= 1;
      return NULL;
    }

    /* Free previous result if any */
    if (initid->ptr)
    {
      delete[] initid->ptr;
      initid->ptr= NULL;
    }

    std::string sql(args->args[0], args->lengths[0]);

    auto conn= myduck::DuckdbManager::CreateConnection();
    if (!conn)
    {
      *error= 1;
      return NULL;
    }

    auto res= myduck::duckdb_query(*conn, sql);

    if (res->HasError())
    {
      std::string err_msg= res->GetError();
      *length= (unsigned long) err_msg.size();
      if (*length < 255)
      {
        memcpy(result, err_msg.c_str(), *length);
        return result;
      }
      initid->ptr= new char[*length + 1];
      memcpy(initid->ptr, err_msg.c_str(), *length);
      initid->ptr[*length]= '\0';
      return initid->ptr;
    }

    if (res->type == duckdb::QueryResultType::STREAM_RESULT)
    {
      auto &stream_result= res->Cast<duckdb::StreamQueryResult>();
      res= stream_result.Materialize();
    }

    std::string output= res->ToString();

    *length= (unsigned long) output.size();
    if (*length < 255)
    {
      memcpy(result, output.c_str(), *length);
      return result;
    }

    initid->ptr= new char[*length + 1];
    memcpy(initid->ptr, output.c_str(), *length);
    initid->ptr[*length]= '\0';
    return initid->ptr;
  }

} /* extern "C" */
