#pragma once

#include <string>

namespace myduck::cdc
{

struct DdlPlan
{
  std::string sql;
  std::string operation;
  std::string table;
  std::string new_table;
};

DdlPlan PlanDdl(const std::string &query, const std::string &default_database,
                const std::string &allowed_database);

}
