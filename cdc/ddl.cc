#include "ddl.h"

#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace myduck::cdc
{
namespace
{
[[noreturn]] void Reject() { throw std::runtime_error("unsupported CDC DDL"); }

bool Letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool Digit(char c) { return c >= '0' && c <= '9'; }
bool Word(char c) { return Letter(c) || Digit(c) || c == '_' || c == '$'; }
std::string Upper(std::string s)
{
  for (char &c : s) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
  return s;
}
std::string Quote(const std::string &s, char quote = '`')
{
  std::string out(1, quote);
  for (char c : s) { out += c; if (c == quote) out += c; }
  return out + quote;
}
void ValidName(const std::string &s)
{
  if (s.empty() || s.size() > 64 || s.back() == ' ') Reject();
  for (unsigned char c : s)
    if (c < 32 || c > 126 || c == '\\' || c == '/' || c == '.' || c == '#') Reject();
}
void ValidDatabase(const std::string &s)
{
  ValidName(s);
  const auto name = Upper(s);
  if (name == "MYSQL" || name == "INFORMATION_SCHEMA" || name == "PERFORMANCE_SCHEMA" ||
      name == "SYS") Reject();
}

struct Token
{
  char kind;
  std::string text;
};
std::vector<Token> Lex(const std::string &s)
{
  if (s.empty() || s.size() > 1024 * 1024) Reject();
  std::vector<Token> tokens;
  for (size_t i = 0; i < s.size();)
  {
    const unsigned char c = s[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
    if (c < 32 || c > 126) Reject();
    if (c == '`' || c == '\'')
    {
      const char quote = s[i++];
      std::string text;
      bool closed = false;
      while (i < s.size())
      {
        const unsigned char value = s[i++];
        if (value < 32 || value > 126 || value == '\\') Reject();
        if (value == quote)
        {
          if (i < s.size() && s[i] == quote) { ++i; text += quote; }
          else { closed = true; break; }
        }
        else text += value;
      }
      if (!closed) Reject();
      tokens.push_back({quote, std::move(text)});
    }
    else if (Letter(c) || c == '_' || c == '$')
    {
      const size_t start = i++;
      while (i < s.size() && Word(s[i])) ++i;
      tokens.push_back({'w', s.substr(start, i - start)});
    }
    else if (Digit(c))
    {
      const size_t start = i++;
      while (i < s.size() && Digit(s[i])) ++i;
      tokens.push_back({'n', s.substr(start, i - start)});
    }
    else
    {
      if (c != '(' && c != ')' && c != ',' && c != '.' && c != '=' && c != '+' && c != '-') Reject();
      if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') Reject();
      tokens.push_back({static_cast<char>(c), std::string(1, c)});
      ++i;
    }
    if (tokens.size() > 32768) Reject();
  }
  tokens.push_back({'e', ""});
  return tokens;
}

class Parser
{
  std::vector<Token> tokens;
  size_t pos = 0;
  const std::string &database;
  const std::string &context;
  std::set<std::string> columns, indexes;
  std::vector<std::string> references;
  bool primary = false;
  bool auto_increment = false;

  bool Is(const std::string &word) const
  {
    return tokens[pos].kind == 'w' && Upper(tokens[pos].text) == word;
  }
  bool Take(const std::string &word)
  {
    if (!Is(word)) return false;
    ++pos;
    return true;
  }
  bool Take(char c)
  {
    if (tokens[pos].kind != c) return false;
    ++pos;
    return true;
  }
  void Need(const std::string &s) { if (!Take(s)) Reject(); }
  void Need(char c) { if (!Take(c)) Reject(); }
  std::string Identifier()
  {
    if (tokens[pos].kind != 'w' && tokens[pos].kind != '`') Reject();
    std::string s = tokens[pos++].text;
    ValidName(s);
    return s;
  }
  std::string Table()
  {
    std::string first = Identifier();
    if (Take('.'))
    {
      ValidDatabase(first);
      if (first != database) Reject();
      return Identifier();
    }
    if (context != database) Reject();
    return first;
  }
  std::string Target(const std::string &table) const { return Quote(database) + "." + Quote(table); }
  unsigned Number(unsigned max)
  {
    if (tokens[pos].kind != 'n' || tokens[pos].text.size() > 6) Reject();
    unsigned n = 0;
    for (char c : tokens[pos++].text) { n = n * 10 + c - '0'; if (n > max) Reject(); }
    return n;
  }
  std::string Type(bool &integer)
  {
    if (tokens[pos].kind != 'w') Reject();
    std::string type = Upper(tokens[pos++].text), out = type;
    integer = type == "TINYINT" || type == "SMALLINT" || type == "MEDIUMINT" ||
              type == "INT" || type == "INTEGER" || type == "BIGINT";
    const bool decimal = type == "DECIMAL" || type == "NUMERIC";
    const bool floating = type == "FLOAT" || type == "DOUBLE";
    const bool chars = type == "CHAR" || type == "VARCHAR" || type == "BINARY" || type == "VARBINARY";
    const bool temporal = type == "DATETIME" || type == "TIME";
    const bool plain = type == "DATE" || type == "YEAR" || type == "BOOL" || type == "BOOLEAN" ||
        type == "TINYBLOB" || type == "BLOB" || type == "MEDIUMBLOB" || type == "LONGBLOB" ||
        type == "TINYTEXT" || type == "TEXT" || type == "MEDIUMTEXT" || type == "LONGTEXT";
    if (!integer && !decimal && !floating && !chars && !temporal && !plain) Reject();
    const bool params = Take('(');
    if (params)
    {
      const unsigned max = integer ? 255 : decimal ? 65 : temporal ? 6 :
          (type == "CHAR" || type == "BINARY") ? 255 : 65535;
      if (!integer && !decimal && !chars && !temporal) Reject();
      const unsigned n = Number(max);
      if (!temporal && n == 0) Reject();
      out += "(" + std::to_string(n);
      if (decimal && Take(','))
      {
        const unsigned scale = Number(30);
        if (scale > n) Reject();
        out += "," + std::to_string(scale);
      }
      Need(')');
      out += ")";
    }
    if (!params && (type == "VARCHAR" || type == "VARBINARY")) Reject();
    if (Take("UNSIGNED"))
    {
      if (!integer && !decimal && !floating) Reject();
      out += " UNSIGNED";
    }
    return out;
  }
  std::string Literal(bool &null)
  {
    null = Take("NULL");
    if (null) return "NULL";
    if (Take("TRUE")) return "TRUE";
    if (Take("FALSE")) return "FALSE";
    if (tokens[pos].kind == '\'') return Quote(tokens[pos++].text, '\'');
    std::string out;
    if (Take('-')) out = "-";
    else Take('+');
    if (tokens[pos].kind != 'n' || tokens[pos].text.size() > 65) Reject();
    out += tokens[pos++].text;
    if (Take('.'))
    {
      if (tokens[pos].kind != 'n' || tokens[pos].text.size() > 30) Reject();
      out += "." + tokens[pos++].text;
    }
    return out;
  }
  std::string Column(bool add)
  {
    const std::string name = Identifier();
    if (!columns.insert(Upper(name)).second || columns.size() > 1024) Reject();
    bool integer = false;
    std::string out = Quote(name) + " " + Type(integer);
    bool not_null = false, null_seen = false, default_seen = false, default_null = false;
    bool inline_primary = false, increment_seen = false;
    for (;;)
    {
      if (Is("NOT") || Is("NULL"))
      {
        if (null_seen) Reject();
        null_seen = true;
        not_null = Take("NOT");
        Need("NULL");
        out += not_null ? " NOT NULL" : " NULL";
      }
      else if (Take("DEFAULT"))
      {
        if (default_seen) Reject();
        default_seen = true;
        out += " DEFAULT " + Literal(default_null);
      }
      else if (Take("AUTO_INCREMENT"))
      {
        if (add || auto_increment || !integer) Reject();
        auto_increment = increment_seen = true;
        out += " AUTO_INCREMENT";
      }
      else if (Take("PRIMARY"))
      {
        if (add || primary) Reject();
        Need("KEY");
        primary = inline_primary = true;
        out += " PRIMARY KEY";
      }
      else break;
    }
    if ((not_null && default_seen && default_null) ||
        (increment_seen && default_seen) || (inline_primary && null_seen && !not_null) ||
        (add && not_null && (!default_seen || default_null))) Reject();
    return out;
  }
  std::string Key(bool pk)
  {
    std::string out;
    if (pk)
    {
      if (primary) Reject();
      primary = true;
      Need("KEY");
      out = "PRIMARY KEY";
    }
    else
    {
      const auto name = Identifier();
      if (Upper(name) == "PRIMARY" || !indexes.insert(Upper(name)).second) Reject();
      out = "KEY " + Quote(name);
    }
    Need('(');
    out += " (";
    std::set<std::string> seen;
    do
    {
      const auto name = Identifier();
      if (!seen.insert(Upper(name)).second || seen.size() > 64) Reject();
      references.push_back(Upper(name));
      if (seen.size() > 1) out += ", ";
      out += Quote(name);
    } while (Take(','));
    Need(')');
    return out + ")";
  }
  std::string Create()
  {
    std::string out = " (";
    Need('(');
    size_t count = 0;
    do
    {
      if (++count > 2048) Reject();
      if (count > 1) out += ", ";
      if (Take("PRIMARY")) out += Key(true);
      else if (Take("KEY") || Take("INDEX")) out += Key(false);
      else if (Is("CONSTRAINT") || Is("FOREIGN") || Is("CHECK") || Is("UNIQUE")) Reject();
      else out += Column(false);
    } while (Take(','));
    Need(')');
    if (columns.empty()) Reject();
    for (const auto &ref : references) if (!columns.count(ref)) Reject();
    out += ") ENGINE=DuckDB";
    bool engine = false, charset = false, collate = false;
    while (tokens[pos].kind != 'e')
    {
      if (Take("ENGINE"))
      {
        if (engine) Reject();
        engine = true;
        Need('=');
        if (!Take("INNODB") && !Take("DUCKDB")) Reject();
      }
      else if (Take("DEFAULT"))
      {
        if (charset) Reject();
        charset = true;
        Need("CHARSET");
        Need('=');
        Need("UTF8MB4");
        out += " DEFAULT CHARSET=utf8mb4";
      }
      else if (Take("COLLATE"))
      {
        if (collate) Reject();
        collate = true;
        Take('=');
        out += " COLLATE=" + Quote(Identifier());
      }
      else Reject();
    }
    return out;
  }
public:
  Parser(const std::string &query, const std::string &def, const std::string &allowed)
      : tokens(Lex(query)), database(allowed), context(def) {}
  DdlPlan Run()
  {
    ValidDatabase(database);
    DdlPlan plan;
    std::string prefix;
    if (Take("CREATE"))
    {
      plan.operation = "CREATE";
      Need("TABLE");
      prefix = "CREATE TABLE ";
      if (Take("IF")) { Need("NOT"); Need("EXISTS"); prefix += "IF NOT EXISTS "; }
      plan.table = Table();
      plan.sql = prefix + Target(plan.table) + Create();
    }
    else if (Take("DROP"))
    {
      plan.operation = "DROP";
      Need("TABLE");
      prefix = "DROP TABLE ";
      if (Take("IF")) { Need("EXISTS"); prefix += "IF EXISTS "; }
      plan.table = Table();
      plan.sql = prefix + Target(plan.table);
    }
    else if (Take("RENAME"))
    {
      plan.operation = "RENAME";
      Need("TABLE");
      plan.table = Table();
      Need("TO");
      plan.new_table = Table();
      plan.sql = "RENAME TABLE " + Target(plan.table) + " TO " + Target(plan.new_table);
    }
    else if (Take("TRUNCATE"))
    {
      plan.operation = "TRUNCATE";
      Take("TABLE");
      plan.table = Table();
      plan.sql = "TRUNCATE TABLE " + Target(plan.table);
    }
    else if (Take("ALTER"))
    {
      plan.operation = "ALTER";
      Need("TABLE");
      plan.table = Table();
      plan.sql = "ALTER TABLE " + Target(plan.table);
      if (Take("ADD")) { Take("COLUMN"); plan.sql += " ADD COLUMN " + Column(true); }
      else if (Take("DROP"))
      {
        Take("COLUMN");
        if (Is("PRIMARY") || Is("KEY") || Is("INDEX") || Is("FOREIGN") || Is("CONSTRAINT") ||
            Is("CHECK") || Is("PARTITION")) Reject();
        plan.sql += " DROP COLUMN " + Quote(Identifier());
      }
      else Reject();
    }
    else Reject();
    if (tokens[pos].kind != 'e') Reject();
    return plan;
  }
};
}

DdlPlan PlanDdl(const std::string &query, const std::string &default_database,
                const std::string &allowed_database)
{
  return Parser(query, default_database, allowed_database).Run();
}
}
