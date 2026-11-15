#include "mariadb_lexical_guard.h"

#include <string>

#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parser.hpp"

namespace myduck
{

bool mariadb_query_has_lexical_mismatch(std::string_view query,
                                      bool backslash_escapes, bool ansi_quotes)
{
  bool non_ascii= false;
  for (unsigned char c : query)
  {
    if (c == 0)
      return true;
    non_ascii|= c >= 0x80;
  }

  auto quote_end= [query](size_t start, bool escapes) {
    for (size_t i= start + 1; i < query.size(); i++)
    {
      if (query[i] == '\\' && escapes && i + 1 < query.size())
        i++;
      else if (query[i] == query[start])
      {
        if (i + 1 < query.size() && query[i + 1] == query[start])
          i++;
        else
          return i + 1;
      }
    }
    return std::string_view::npos;
  };
  auto identifier_start= [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };

  for (size_t i= 0; i < query.size();)
  {
    unsigned char c= query[i];
    if (c == '#' || c == '$' || c == '\\' || c >= 0x80 ||
        (c == '"' && !ansi_quotes))
      return true;
    if (c == '\'' || c == '"' || c == '`')
    {
      size_t end= quote_end(i, false);
      if (end == std::string_view::npos ||
          (c == '\'' && backslash_escapes && quote_end(i, true) != end))
        return true;
      i= end;
      continue;
    }
    if (c == '-' && i + 1 < query.size() && query[i + 1] == '-')
    {
      if (i + 2 < query.size() &&
          static_cast<unsigned char>(query[i + 2]) > ' ')
        return true;
      size_t end= query.find('\n', i + 2);
      if (end == std::string_view::npos)
        end= query.size();
      auto comment= query.substr(i + 2, end - i - 2);
      size_t cr= comment.find('\r');
      if (cr != std::string_view::npos &&
          (end == query.size() || cr + 1 != comment.size()))
        return true;
      i= end;
      continue;
    }
    if (c == '/' && i + 1 < query.size() && query[i + 1] == '*')
    {
      size_t end= query.find("*/", i + 2);
      if (end == std::string_view::npos)
        return true;
      auto comment= query.substr(i + 2, end - i - 2);
      if ((!comment.empty() && comment[0] == '!') ||
          (comment.size() >= 2 && (comment[0] == 'M' || comment[0] == 'm') &&
           comment[1] == '!') || comment.find("/*") != std::string_view::npos)
        return true;
      i= end + 2;
      continue;
    }
    if (c == '*' && i + 1 < query.size() && query[i + 1] == '/')
      return true;
    if (identifier_start(c))
    {
      size_t start= i++;
      while (i < query.size() &&
             (identifier_start(query[i]) ||
              (query[i] >= '0' && query[i] <= '9') || query[i] == '$'))
        i++;
      if (i < query.size() && query[i] == '\'' &&
          (query[start] == '_' ||
           (i == start + 1 && (c == 'E' || c == 'e'))))
        return true;
      continue;
    }
    i++;
  }
  if (non_ascii)
  {
    try
    {
      std::string original(query);
      return duckdb::Parser::NormalizeSQLString(original) != original;
    }
    catch (const duckdb::Exception &)
    {
      return true;
    }
  }
  return false;
}

}
