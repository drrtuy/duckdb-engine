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

#include "allowlist.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace quack_guard
{

const char *const STATEMENT_DENIED= "Statement not allowed over Quack";
const char *const ACCESS_DENIED=
    "Access denied, or the object does not exist";

static const std::unordered_set<std::string> &functions()
{
  static const std::unordered_set<std::string> list= {
      /* comparisons and casts, as the binder names them */
      "=", "==", "!=", "<>", "<", ">", "<=", ">=", "__cast", "__try_cast",
      /* operators as the parser and binder name them */
      "+", "-", "*", "/", "//", "%", "**", "^", "||", "&", "|", "<<", ">>",
      "~~", "!~~", "~~*", "!~~*", "~~~", "!~~~", "like_escape",
      "not_like_escape", "ilike_escape", "not_ilike_escape",
      "add", "subtract", "multiply", "divide", "mod", "fdiv", "fmod",
      /* numeric */
      "abs", "sign", "round", "floor", "ceil", "ceiling", "trunc", "sqrt",
      "cbrt", "power", "pow", "exp", "ln", "log", "log2", "log10", "pi",
      "degrees", "radians", "sin", "cos", "tan", "asin", "acos", "atan",
      "atan2", "greatest", "least", "isnan", "isinf", "isfinite", "gcd",
      "lcm", "even", "factorial", "bit_count",
      /* conditional */
      "coalesce", "ifnull", "nullif", "if",
      /* strings */
      "lower", "upper", "lcase", "ucase", "length", "len", "char_length",
      "character_length", "octet_length", "strlen", "substring", "substr",
      "trim", "ltrim", "rtrim", "concat", "concat_ws", "replace", "left",
      "right", "starts_with", "ends_with", "prefix", "suffix", "contains",
      "strpos", "instr", "position", "lpad", "rpad", "reverse", "repeat",
      "ascii", "chr", "split_part", "string_split", "regexp_matches",
      "regexp_replace", "regexp_extract", "md5", "sha256", "hash",
      "format", "printf", "translate", "initcap", "unicode", "bit_length",
      /* dates and times */
      "date_part", "datepart", "extract", "date_trunc", "datetrunc",
      "date_diff", "datediff", "date_sub", "datesub", "date_add",
      "year", "month", "day", "hour", "minute", "second", "millisecond",
      "microsecond", "dayofweek", "dayofmonth", "dayofyear", "weekday",
      "week", "weekofyear", "yearweek", "quarter", "epoch", "epoch_ms",
      "make_date", "make_time", "make_timestamp", "strftime", "strptime",
      "to_timestamp", "last_day", "monthname", "dayname", "age",
      "to_days", "to_years", "to_months", "to_hours", "to_minutes",
      "to_seconds", "isodow", "isoyear", "era", "century", "decade",
      "millennium", "timezone_hour", "timezone_minute", "time_bucket",
      /* aggregates */
      "count", "count_star", "sum", "avg", "mean", "min", "max",
      "any_value", "first", "last", "arbitrary", "stddev", "stddev_samp",
      "stddev_pop", "variance", "var_samp", "var_pop", "median", "mode",
      "quantile", "quantile_cont", "quantile_disc", "approx_quantile",
      "approx_count_distinct", "string_agg", "group_concat", "listagg",
      "bool_and", "bool_or", "bit_and", "bit_or", "bit_xor", "product",
      "corr", "covar_pop", "covar_samp", "arg_min", "arg_max", "argmin",
      "argmax", "min_by", "max_by", "sumkahan", "fsum", "kahan_sum",
      "entropy", "kurtosis", "skewness", "histogram", "list", "array_agg",
      /* window */
      "row_number", "rank", "dense_rank", "rank_dense", "percent_rank",
      "cume_dist", "ntile", "lag", "lead", "first_value", "last_value",
      "nth_value",
  };
  return list;
}

bool FunctionAllowed(const std::string &name)
{
  std::string lower(name);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return (char) std::tolower(c); });
  return functions().count(lower) > 0;
}

bool SchemaReserved(const std::string &schema)
{
  return schema.empty() || schema == "main" || schema == "pg_catalog" ||
         schema == "information_schema" || schema == "temp";
}

bool TableReserved(const std::string &table)
{
  /* MariaDB's temporary names during ALTER TABLE */
  return table.empty() || table.rfind("#sql", 0) == 0;
}

} /* namespace quack_guard */
