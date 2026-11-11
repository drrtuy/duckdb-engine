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

#include "quack_lockdown.h"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

namespace myduck
{

static const char *const DENY_AUTH_FUNCTION= "mariadb_quack_deny_all";

static const char *const DENY_QUERY_FUNCTION= "mariadb_quack_deny_query";

static void deny_all(duckdb::DataChunk &args, duckdb::ExpressionState &,
                     duckdb::Vector &result)
{
  result.SetVectorType(duckdb::VectorType::CONSTANT_VECTOR);
  duckdb::ConstantVector::GetData<bool>(result)[0]= false;
}

/* Quack runs the statement this returns; NULL rejects it. */
static void deny_query(duckdb::DataChunk &args, duckdb::ExpressionState &,
                       duckdb::Vector &result)
{
  result.SetVectorType(duckdb::VectorType::CONSTANT_VECTOR);
  duckdb::ConstantVector::SetNull(result, true);
}

static void register_system_function(duckdb::DatabaseInstance &db,
                                     duckdb::ScalarFunction fn)
{
  fn.SetVolatile();
  duckdb::CreateScalarFunctionInfo info(fn);
  info.on_conflict= duckdb::OnCreateConflict::ERROR_ON_CONFLICT;
  auto &catalog= duckdb::Catalog::GetSystemCatalog(db);
  auto transaction= duckdb::CatalogTransaction::GetSystemTransaction(db);
  catalog.CreateFunction(transaction, info);
}

void quack_lockdown(duckdb::DatabaseInstance &db, duckdb::Connection &con)
{
  using duckdb::LogicalType;
  /* (connection id, auth string, server token) -> admitted */
  register_system_function(
      db, duckdb::ScalarFunction(DENY_AUTH_FUNCTION,
                                 {LogicalType::VARCHAR, LogicalType::VARCHAR,
                                  LogicalType::VARCHAR},
                                 LogicalType::BOOLEAN, deny_all));
  /* (connection id, query) -> query to run, NULL = rejected */
  register_system_function(
      db, duckdb::ScalarFunction(DENY_QUERY_FUNCTION,
                                 {LogicalType::VARCHAR, LogicalType::VARCHAR},
                                 LogicalType::VARCHAR, deny_query));

  /* Fully qualified, so a same-named macro elsewhere cannot shadow them */
  for (const char *sql :
       {"LOAD quack",
        "SET GLOBAL quack_authentication_function="
        "'system.main.mariadb_quack_deny_all'",
        "SET GLOBAL quack_authorization_function="
        "'system.main.mariadb_quack_deny_query'"})
  {
    auto result= con.Query(sql);
    if (!result || result->HasError())
      throw duckdb::InvalidInputException(
          "Quack lockdown failed at '%s': %s", sql,
          result ? result->GetError() : std::string("no result"));
  }
}

} /* namespace myduck */
