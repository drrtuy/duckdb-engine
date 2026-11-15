#pragma once

#include <string_view>

namespace myduck
{

/*
  Lexical safety preflight. Returns true when MariaDB and DuckDB would lex the
  same text differently, so the text must not be forwarded to DuckDB (it would
  parse into a different AST / reference different objects).

  Lives in the common layer so both the runtime (SELECT/source pushdown, direct
  UPDATE/DELETE) and the convertor (DDL expression-default forwarding) can call
  it without a convertor->runtime reverse dependency.
*/
bool mariadb_query_has_lexical_mismatch(std::string_view query,
                                        bool backslash_escapes, bool ansi_quotes);

}
