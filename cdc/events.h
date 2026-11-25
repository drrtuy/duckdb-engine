#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace myduck
{
namespace cdc
{

uint32_t EventLength(const char *p);
std::vector<std::string> Events(const std::string &payload);
std::string FormatKey(const std::string &format);
std::string Base64(const std::string &bytes);

struct QueryEvent
{
  std::string database;
  std::string query;
};

struct Effects
{
  std::string format;
  std::vector<std::string> statements;
  std::vector<std::string> tables;
  QueryEvent ddl;
  bool rollback= false;
  bool transaction_end= false;
};

Effects DecodeEffects(const std::string &payload, const std::string &database);

}
}
