#pragma once

#include <stdexcept>
#include <string>

namespace myduck::cdc
{

enum class DdlRecoveryAction { Execute, Confirm };

inline DdlRecoveryAction RecoveryAction(const std::string &operation, bool done,
    const std::string &before, const std::string &before_new,
    const std::string &after, const std::string &after_new,
    const std::string &current, const std::string &current_new)
{
  if (done && current == after && current_new == after_new)
    return DdlRecoveryAction::Confirm;
  if (!done)
  {
    if (operation == "DROP" && current.empty() && current_new.empty())
      return DdlRecoveryAction::Confirm;
    if (operation == "RENAME" && current.empty() && !before.empty() &&
        before_new.empty() && current_new == before)
      return DdlRecoveryAction::Confirm;
    if (current == before && current_new == before_new)
      return DdlRecoveryAction::Execute;
  }
  throw std::runtime_error("CDC applier: ambiguous DDL recovery; manual catalog reconciliation required");
}

inline std::string SchemaDefinition(const std::string &sql, const std::string &table)
{
  std::string quoted= "`";
  for (char c : table) { quoted+= c; if (c == '`') quoted+= c; }
  quoted+= '`';
  const std::string prefix= "CREATE TABLE " + quoted;
  if (sql.compare(0, prefix.size(), prefix) != 0)
    throw std::runtime_error("CDC applier: unexpected SHOW CREATE format");
  std::string out;
  unsigned depth= 0;
  char quote= 0;
  for (size_t i= prefix.size(); i < sql.size(); ++i)
  {
    char c= sql[i];
    if (c == '\\')
      throw std::runtime_error("CDC applier: ambiguous schema quoting");
    if (quote)
    {
      out+= c;
      if (c == quote)
      {
        if (i + 1 < sql.size() && sql[i + 1] == quote) out+= sql[++i];
        else quote= 0;
      }
      continue;
    }
    if (c == '\'' || c == '`' || c == '"') quote= c;
    if (c == '(') ++depth;
    if (c == ')' && depth) --depth;
    if (!quote && !depth && sql.compare(i, 16, " AUTO_INCREMENT=") == 0)
    {
      i+= 16;
      size_t first= i;
      while (i < sql.size() && sql[i] >= '0' && sql[i] <= '9') ++i;
      if (i == first) throw std::runtime_error("CDC applier: invalid AUTO_INCREMENT metadata");
      --i;
      continue;
    }
    out+= c;
  }
  if (quote || depth)
    throw std::runtime_error("CDC applier: malformed schema definition");
  return out;
}

}
