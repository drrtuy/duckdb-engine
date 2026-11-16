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

#include <my_global.h>
#include "my_time.h" /* TIMESTAMP_MAX_VALUE */

#include "duckdb_mariadb_tz.h"
#include "duckdb_timezone.h"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "timezone.hpp" /* DuckDB ICU extension: datetime::TimeZone */

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace myduck
{

using duckdb::LogicalType;
using duckdb::datetime::LocalOption;
using duckdb::datetime::TimeZone;

/* ----------------------------------------------------------------
   Zones and local time
   ---------------------------------------------------------------- */

/* A zone by ICU id, matched case-insensitively like SET TimeZone */
static duckdb::unique_ptr<TimeZone> find_zone(const std::string &id)
{
  if (id.empty())
    return nullptr;
  if (auto zone= TimeZone::TryCreate(id))
    return zone;
  for (const auto &candidate : TimeZone::GetAvailableIds())
    if (duckdb::StringUtil::CIEquals(candidate, id))
      return TimeZone::TryCreate(candidate);
  return nullptr;
}

static int32_t zone_offset_ms(const TimeZone &zone, int64_t utc_ms)
{
  int32_t raw, dst;
  zone.GetOffset(utc_ms, raw, dst);
  return raw + dst;
}

/* The instants a local time stands for in a zone, in epoch µs */
struct LocalTimeInstants
{
  enum Kind
  {
    SINGLE,   /* i1 */
    REPEATED, /* i1 < i2; the second occurrence starts at t */
    SKIPPED   /* no instant; the clocks jump at t */
  } kind;
  int64_t i1= 0, i2= 0, t= 0;
  /* SKIPPED: the instant MariaDB stores, t plus the fraction of a second */
  int64_t skipped_us= 0;
};

/* First millisecond in (lo, hi] whose offset differs from lo's */
static int64_t find_transition_ms(const TimeZone &zone, int64_t lo,
                                  int64_t hi)
{
  int32_t before= zone_offset_ms(zone, lo);
  while (lo < hi)
  {
    int64_t mid= lo + (hi - lo) / 2;
    if (zone_offset_ms(zone, mid) == before)
      lo= mid + 1;
    else
      hi= mid;
  }
  return lo;
}

static LocalTimeInstants local_time_instants(const TimeZone &zone,
                                             int64_t local_us)
{
  int64_t local_ms= local_us / 1000, rem_us= local_us % 1000;
  if (rem_us < 0)
  {
    local_ms--;
    rem_us+= 1000;
  }
  int64_t frac_us= local_us % 1000000; /* MariaDB keeps second_part */
  if (frac_us < 0)
    frac_us+= 1000000;
  int32_t raw, dst;
  zone.GetOffsetFromLocal(local_ms, LocalOption::FORMER, LocalOption::FORMER,
                          raw, dst);
  int64_t a= local_ms - (raw + dst);
  zone.GetOffsetFromLocal(local_ms, LocalOption::LATTER, LocalOption::LATTER,
                          raw, dst);
  int64_t b= local_ms - (raw + dst);
  bool a_ok= a + zone_offset_ms(zone, a) == local_ms;
  bool b_ok= b + zone_offset_ms(zone, b) == local_ms;
  int64_t lo= std::min(a, b), hi= std::max(a, b);

  LocalTimeInstants r;
  if (a_ok && b_ok && a != b)
  {
    r.kind= LocalTimeInstants::REPEATED;
    r.i1= lo * 1000 + rem_us;
    r.i2= hi * 1000 + rem_us;
    r.t= find_transition_ms(zone, lo, hi) * 1000;
  }
  else if (a_ok || b_ok)
  {
    r.kind= LocalTimeInstants::SINGLE;
    r.i1= (a_ok ? a : b) * 1000 + rem_us;
  }
  else
  {
    r.kind= LocalTimeInstants::SKIPPED;
    r.t= find_transition_ms(zone, lo, hi) * 1000;
    r.skipped_us= r.t + frac_us;
  }
  return r;
}

/* Local times this far from the timestamp range limits are rejected
   rather than shifted by a zone offset, which could overflow */
static const int64_t LOCAL_RANGE_MARGIN_US= 2LL * 86400 * 1000000;

static bool local_in_range(duckdb::timestamp_t local)
{
  return local.value > std::numeric_limits<int64_t>::min() +
                           LOCAL_RANGE_MARGIN_US &&
         local.value < std::numeric_limits<int64_t>::max() -
                           LOCAL_RANGE_MARGIN_US;
}

/*
  Instant (epoch µs) of a naive local time, resolved the MariaDB way: a
  time a DST change skips maps to the transition instant (keeping the
  fraction of a second), one that occurs twice to the earlier instant,
  or to the later one when repeated_later is set (time_zone=SYSTEM,
  which goes through libc). Infinities pass through. The caller checks
  local_in_range().
*/
static int64_t local_to_epoch_us(const TimeZone &zone, bool repeated_later,
                                 duckdb::timestamp_t local)
{
  if (!local.IsFinite())
    return local.value;
  auto in= local_time_instants(zone, local.value);
  switch (in.kind)
  {
  case LocalTimeInstants::REPEATED:
    return repeated_later ? in.i2 : in.i1;
  case LocalTimeInstants::SKIPPED:
    return in.skipped_us;
  default:
    return in.i1;
  }
}

/* Naive local timestamp of an instant (epoch µs) */
static duckdb::timestamp_t epoch_to_local(const TimeZone &zone,
                                          int64_t epoch_us)
{
  if (!duckdb::timestamp_t(epoch_us).IsFinite())
    return duckdb::timestamp_t(epoch_us);
  int64_t epoch_ms= epoch_us / 1000;
  if (epoch_us % 1000 < 0)
    epoch_ms--;
  return duckdb::timestamp_t(epoch_us +
                             (int64_t) zone_offset_ms(zone, epoch_ms) * 1000);
}

/* ----------------------------------------------------------------
   Session time zone, bound per statement
   ---------------------------------------------------------------- */

static bool get_user_variable(duckdb::ClientContext &context,
                              const char *name, duckdb::Value &value)
{
  return duckdb::ClientConfig::GetConfig(context).GetUserVariable(
             std::string(name), value) &&
         !value.IsNull();
}

/* A boolean user variable; anything but a true BOOLEAN reads as false */
static bool get_bool_variable(duckdb::ClientContext &context,
                              const char *name)
{
  duckdb::Value value;
  return get_user_variable(context, name, value) &&
         value.type().id() == duckdb::LogicalTypeId::BOOLEAN &&
         duckdb::BooleanValue::Get(value);
}

struct SessionZoneBindData : public duckdb::FunctionData
{
  std::string zone_name;   /* DuckDB TimeZone setting */
  bool repeated_later;     /* MARIADB_TZ_REPEATED_LATER_VAR */
  std::string system_zone; /* MARIADB_SYSTEM_TZ_VAR */
  duckdb::unique_ptr<TimeZone> zone;

  SessionZoneBindData(std::string name, bool later, std::string system)
      : zone_name(std::move(name)), repeated_later(later),
        system_zone(std::move(system)), zone(find_zone(zone_name))
  {
    if (!zone)
      zone= TimeZone::TryCreate("UTC");
  }
  duckdb::unique_ptr<duckdb::FunctionData> Copy() const override
  {
    return duckdb::make_uniq<SessionZoneBindData>(zone_name, repeated_later,
                                                  system_zone);
  }
  bool Equals(const duckdb::FunctionData &other) const override
  {
    auto &o= other.Cast<SessionZoneBindData>();
    return zone_name == o.zone_name && repeated_later == o.repeated_later &&
           system_zone == o.system_zone;
  }
};

static duckdb::unique_ptr<duckdb::FunctionData>
session_zone_bind(duckdb::BindScalarFunctionInput &input)
{
  auto &context= input.GetClientContext();
  duckdb::Value value;
  std::string zone= "UTC";
  if (context.TryGetCurrentSetting("TimeZone", value) && !value.IsNull())
    zone= value.ToString();
  bool later= get_bool_variable(context, MARIADB_TZ_REPEATED_LATER_VAR);
  std::string system_zone;
  if (get_user_variable(context, MARIADB_SYSTEM_TZ_VAR, value))
    system_zone= value.ToString();
  return duckdb::make_uniq<SessionZoneBindData>(zone, later, system_zone);
}

static const SessionZoneBindData &bind_data(duckdb::ExpressionState &state)
{
  return state.expr.Cast<duckdb::BoundFunctionExpression>()
      .BindInfo()
      ->Cast<SessionZoneBindData>();
}

static void register_function(duckdb::DatabaseInstance &db,
                              duckdb::ScalarFunctionSet set)
{
  auto &catalog= duckdb::Catalog::GetSystemCatalog(db);
  auto transaction= duckdb::CatalogTransaction::GetSystemTransaction(db);
  duckdb::CreateScalarFunctionInfo info(std::move(set));
  info.on_conflict= duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
  catalog.CreateFunction(transaction, info);
}

/* ----------------------------------------------------------------
   MARIADB_UNIX_TIMESTAMP(TIMESTAMP | TIMESTAMPTZ) -> BIGINT
   Epoch seconds as MariaDB's UNIX_TIMESTAMP() computes them: a naive
   TIMESTAMP is local time in the session zone, fractions are truncated,
   and the result is NULL outside MariaDB's TIMESTAMP range (at or
   before the epoch, after TIMESTAMP_MAX_VALUE). The zero TIMESTAMP
   gives 0, as for a MariaDB TIMESTAMP column.
   ---------------------------------------------------------------- */

static duckdb::optional<int64_t> epoch_us_to_unix_timestamp(int64_t us)
{
  if (us <= 0 || us / 1000000 > (int64_t) TIMESTAMP_MAX_VALUE)
    return {};
  return us / 1000000;
}

static void unix_timestamp_tz_func(duckdb::DataChunk &args,
                                   duckdb::ExpressionState &,
                                   duckdb::Vector &result)
{
  duckdb::UnaryExecutor::Execute<duckdb::timestamp_tz_t, int64_t>(
      args.data[0], result, args.size(),
      [](duckdb::timestamp_tz_t ts) -> duckdb::optional<int64_t> {
        if (ts.value == zero_timestamp_epoch_us)
          return 0;
        return epoch_us_to_unix_timestamp(ts.value);
      });
}

static void unix_timestamp_local_func(duckdb::DataChunk &args,
                                      duckdb::ExpressionState &state,
                                      duckdb::Vector &result)
{
  auto &info= bind_data(state);
  duckdb::UnaryExecutor::Execute<duckdb::timestamp_t, int64_t>(
      args.data[0], result, args.size(),
      [&](duckdb::timestamp_t ts) -> duckdb::optional<int64_t> {
        if (!ts.IsFinite() || !local_in_range(ts))
          return {};
        return epoch_us_to_unix_timestamp(
            local_to_epoch_us(*info.zone, info.repeated_later, ts));
      });
}

static const char *const TO_TIMESTAMPTZ= "mariadb_to_timestamptz";
static const char *const TRY_TO_TIMESTAMPTZ= "mariadb_try_to_timestamptz";

/* ----------------------------------------------------------------
   MARIADB_TO_TIMESTAMPTZ(TIMESTAMP | DATE | VARCHAR) -> TIMESTAMPTZ
   The cast to TIMESTAMPTZ with MariaDB's local time rules. VARCHAR is
   parsed like DuckDB's cast: an explicit offset is honored, a zone name
   in the string is used instead of the session zone. The try_ variant
   gives NULL where the cast would fail.
   ---------------------------------------------------------------- */

/* A local time as TIMESTAMPTZ; NULL (TRY) or an error when out of range */
template <bool TRY>
static duckdb::optional<duckdb::timestamp_tz_t>
local_to_timestamptz(const SessionZoneBindData &info, duckdb::timestamp_t ts)
{
  if (!local_in_range(ts))
  {
    if (TRY)
      return {};
    throw duckdb::ConversionException("Timestamp out of range: %s",
                                      duckdb::Timestamp::ToString(ts));
  }
  return duckdb::timestamp_tz_t(
      local_to_epoch_us(*info.zone, info.repeated_later, ts));
}

template <bool TRY>
static void to_timestamptz_local_func(duckdb::DataChunk &args,
                                      duckdb::ExpressionState &state,
                                      duckdb::Vector &result)
{
  auto &info= bind_data(state);
  duckdb::UnaryExecutor::Execute<duckdb::timestamp_t, duckdb::timestamp_tz_t>(
      args.data[0], result, args.size(), [&](duckdb::timestamp_t ts) {
        return local_to_timestamptz<TRY>(info, ts);
      });
}

template <bool TRY>
static void to_timestamptz_date_func(duckdb::DataChunk &args,
                                     duckdb::ExpressionState &state,
                                     duckdb::Vector &result)
{
  auto &info= bind_data(state);
  duckdb::UnaryExecutor::Execute<duckdb::date_t, duckdb::timestamp_tz_t>(
      args.data[0], result, args.size(),
      [&](duckdb::date_t date) -> duckdb::optional<duckdb::timestamp_tz_t> {
        duckdb::timestamp_t ts;
        if (!duckdb::TryCast::Operation(date, ts, false))
        {
          if (TRY)
            return {};
          throw duckdb::ConversionException("Date out of range: %s",
                                            duckdb::Date::ToString(date));
        }
        return local_to_timestamptz<TRY>(info, ts);
      });
}

template <bool TRY>
static void to_timestamptz_varchar_func(duckdb::DataChunk &args,
                                        duckdb::ExpressionState &state,
                                        duckdb::Vector &result)
{
  auto &info= bind_data(state);
  std::unordered_map<std::string, duckdb::unique_ptr<TimeZone>> named;
  duckdb::UnaryExecutor::Execute<duckdb::string_t, duckdb::timestamp_tz_t>(
      args.data[0], result, args.size(),
      [&](duckdb::string_t input) -> duckdb::optional<duckdb::timestamp_tz_t> {
        duckdb::timestamp_t ts;
        duckdb::string_t tz(nullptr, 0);
        bool has_offset= false;
        auto status= duckdb::Timestamp::TryConvertTimestampTZ(
            input.GetData(), input.GetSize(), ts, true, has_offset, tz);
        if (status != duckdb::TimestampCastResult::SUCCESS)
        {
          if (TRY)
            return {};
          throw duckdb::ConversionException(
              status == duckdb::TimestampCastResult::ERROR_RANGE
                  ? duckdb::Timestamp::RangeError(input)
                  : duckdb::Timestamp::FormatError(input));
        }
        if (has_offset)
          return duckdb::timestamp_tz_t(ts);
        const TimeZone *zone= info.zone.get();
        if (tz.GetSize())
        {
          std::string name= tz.GetString();
          auto it= named.find(name);
          if (it == named.end())
            it= named.emplace(name, find_zone(name)).first;
          if (!it->second)
          {
            if (TRY)
              return {};
            throw duckdb::ConversionException("Unknown TimeZone '%s'", name);
          }
          zone= it->second.get();
        }
        if (!local_in_range(ts))
        {
          if (TRY)
            return {};
          throw duckdb::ConversionException(
              duckdb::Timestamp::RangeError(input));
        }
        return duckdb::timestamp_tz_t(
            local_to_epoch_us(*zone, info.repeated_later, ts));
      });
}

static duckdb::ScalarFunctionSet to_timestamptz_functions(bool try_cast)
{
  duckdb::ScalarFunctionSet set(try_cast ? TRY_TO_TIMESTAMPTZ
                                         : TO_TIMESTAMPTZ);
  set.AddFunction(duckdb::ScalarFunction(
      {LogicalType::TIMESTAMP}, LogicalType::TIMESTAMP_TZ,
      try_cast ? to_timestamptz_local_func<true>
               : to_timestamptz_local_func<false>,
      session_zone_bind));
  set.AddFunction(duckdb::ScalarFunction(
      {LogicalType::DATE}, LogicalType::TIMESTAMP_TZ,
      try_cast ? to_timestamptz_date_func<true>
               : to_timestamptz_date_func<false>,
      session_zone_bind));
  set.AddFunction(duckdb::ScalarFunction(
      {LogicalType::VARCHAR}, LogicalType::TIMESTAMP_TZ,
      try_cast ? to_timestamptz_varchar_func<true>
               : to_timestamptz_varchar_func<false>,
      session_zone_bind));
  return set;
}

/* ----------------------------------------------------------------
   MARIADB_CONVERT_TZ(TIMESTAMP | TIMESTAMPTZ, VARCHAR, VARCHAR)
     -> TIMESTAMP
   MariaDB's CONVERT_TZ(): zones are MariaDB names (SYSTEM, "+05:30",
   names in the time zone tables); an unknown zone gives NULL; a value
   outside the TIMESTAMP range is returned unchanged. A TIMESTAMPTZ
   argument (a TIMESTAMP column) is first read in the session zone.
   ---------------------------------------------------------------- */

struct ResolvedZone
{
  duckdb::unique_ptr<TimeZone> zone;
  bool repeated_later= false;
};

static const ResolvedZone &
resolve_mariadb_zone(const SessionZoneBindData &info,
                     std::unordered_map<std::string, ResolvedZone> &cache,
                     const std::string &name)
{
  auto it= cache.find(name);
  if (it != cache.end())
    return it->second;
  ResolvedZone resolved;
  if (duckdb::StringUtil::CIEquals(name, "SYSTEM"))
  {
    resolved.zone= find_zone(info.system_zone);
    resolved.repeated_later= true;
  }
  else
    resolved.zone= find_zone(mariadb_zone_name_to_icu(name));
  return cache.emplace(name, std::move(resolved)).first->second;
}

static duckdb::optional<duckdb::timestamp_t>
convert_tz(const SessionZoneBindData &info,
           std::unordered_map<std::string, ResolvedZone> &cache,
           duckdb::timestamp_t local, duckdb::string_t from,
           duckdb::string_t to)
{
  auto &from_zone= resolve_mariadb_zone(info, cache, from.GetString());
  auto &to_zone= resolve_mariadb_zone(info, cache, to.GetString());
  if (!from_zone.zone || !to_zone.zone)
    return {};
  if (!local.IsFinite() || !local_in_range(local))
    return {};
  int64_t epoch_us=
      local_to_epoch_us(*from_zone.zone, from_zone.repeated_later, local);
  int64_t sec= epoch_us / 1000000;
  if (epoch_us % 1000000 < 0)
    sec--;
  if (sec < 1 || sec > (int64_t) TIMESTAMP_MAX_VALUE)
    return local;
  return epoch_to_local(*to_zone.zone, epoch_us);
}

static void convert_tz_local_func(duckdb::DataChunk &args,
                                  duckdb::ExpressionState &state,
                                  duckdb::Vector &result)
{
  auto &info= bind_data(state);
  std::unordered_map<std::string, ResolvedZone> cache;
  duckdb::TernaryExecutor::Execute<duckdb::timestamp_t, duckdb::string_t,
                                   duckdb::string_t, duckdb::timestamp_t>(
      args.data[0], args.data[1], args.data[2], result,
      [&](duckdb::timestamp_t ts, duckdb::string_t from, duckdb::string_t to)
          -> duckdb::optional<duckdb::timestamp_t> {
        return convert_tz(info, cache, ts, from, to);
      });
}

static void convert_tz_instant_func(duckdb::DataChunk &args,
                                    duckdb::ExpressionState &state,
                                    duckdb::Vector &result)
{
  auto &info= bind_data(state);
  std::unordered_map<std::string, ResolvedZone> cache;
  duckdb::TernaryExecutor::Execute<duckdb::timestamp_tz_t, duckdb::string_t,
                                   duckdb::string_t, duckdb::timestamp_t>(
      args.data[0], args.data[1], args.data[2], result,
      [&](duckdb::timestamp_tz_t ts, duckdb::string_t from,
          duckdb::string_t to) -> duckdb::optional<duckdb::timestamp_t> {
        if (ts.value == zero_timestamp_epoch_us)
          return {};
        return convert_tz(info, cache, epoch_to_local(*info.zone, ts.value),
                          from, to);
      });
}

/* ----------------------------------------------------------------
   Optimizer extension: casts to TIMESTAMPTZ with MariaDB rules
   ---------------------------------------------------------------- */

/* A cast to TIMESTAMPTZ from a DATETIME, DATE or string value */
static bool is_local_to_tz_cast(const duckdb::Expression &expr)
{
  if (expr.GetExpressionClass() != duckdb::ExpressionClass::BOUND_FUNCTION ||
      !duckdb::BoundCastExpression::IsCast(expr))
    return false;
  auto &cast= expr.Cast<duckdb::BoundFunctionExpression>();
  if (duckdb::BoundCastExpression::TargetType(cast).id() !=
      duckdb::LogicalTypeId::TIMESTAMP_TZ)
    return false;
  auto source= duckdb::BoundCastExpression::SourceType(cast).id();
  return source == duckdb::LogicalTypeId::TIMESTAMP ||
         source == duckdb::LogicalTypeId::DATE ||
         source == duckdb::LogicalTypeId::VARCHAR;
}

static const duckdb::Expression &cast_child(const duckdb::Expression &cast)
{
  return duckdb::BoundCastExpression::Child(
      cast.Cast<duckdb::BoundFunctionExpression>());
}

/*
  Replaces casts to TIMESTAMPTZ with MariaDB's rules, and compares a
  TIMESTAMP with a DATETIME, DATE or string the MariaDB way: as DATETIME,
  i.e. on the TIMESTAMP's local time in the session zone. DuckDB casts the
  other side to TIMESTAMPTZ instead, which differs for local times that
  DST changes skip or repeat.

  A constant is turned into the exact condition on instants, so the
  TIMESTAMP column is still compared directly and filters on it can be
  pushed into the scan; usually that is one comparison, two only for a
  constant in a repeated hour. Otherwise the TIMESTAMP side is cast to
  local time.
*/
class MariadbCastRewriter : public duckdb::LogicalOperatorVisitor
{
public:
  explicit MariadbCastRewriter(duckdb::ClientContext &context)
      : context_(context)
  {
    duckdb::Value value;
    std::string name= "UTC";
    if (context.TryGetCurrentSetting("TimeZone", value) && !value.IsNull())
      name= value.ToString();
    zone_= find_zone(name);
    if (!zone_)
      zone_= TimeZone::TryCreate("UTC");
  }

  void VisitOperator(duckdb::LogicalOperator &op) override
  {
    if (op.type == duckdb::LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
        op.type == duckdb::LogicalOperatorType::LOGICAL_DELIM_JOIN ||
        op.type == duckdb::LogicalOperatorType::LOGICAL_ASOF_JOIN)
    {
      for (auto &cond : op.Cast<duckdb::LogicalComparisonJoin>().conditions)
        if (cond.IsComparison())
          compare_as_local({&cond.LeftReference(), &cond.RightReference()});
    }
    duckdb::LogicalOperatorVisitor::VisitOperator(op);
  }

  void VisitExpression(duckdb::unique_ptr<duckdb::Expression> *expression)
      override
  {
    rewrite_comparison(*expression);
    auto &expr= **expression;
    duckdb::ExpressionIterator::EnumerateChildren(
        expr, [&](duckdb::unique_ptr<duckdb::Expression> &child) {
          VisitExpression(&child);
        });
    rewrite_cast(*expression);
  }

private:
  using ExprPtr= duckdb::unique_ptr<duckdb::Expression>;

  /* If any operand is a cast to TIMESTAMPTZ from local time, compare all
     operands as naive TIMESTAMP. */
  void compare_as_local(const std::vector<ExprPtr *> &operands)
  {
    bool local= false;
    for (auto *op : operands)
      local|= is_local_to_tz_cast(**op);
    if (!local)
      return;
    for (auto *op : operands)
    {
      auto &expr= **op;
      if (expr.GetReturnType().id() != duckdb::LogicalTypeId::TIMESTAMP_TZ)
        continue;
      int64_t unused;
      if (is_local_to_tz_cast(expr) &&
          constant_local_time(expr, &unused) != ConstLocal::INSTANT)
      {
        /* Drop the cast to TIMESTAMPTZ, cast its source to TIMESTAMP */
        auto &cast= expr.Cast<duckdb::BoundFunctionExpression>();
        bool try_cast= duckdb::BoundCastExpression::IsTryCast(cast);
        auto child=
            std::move(duckdb::BoundCastExpression::ChildMutable(cast));
        *op= duckdb::BoundCastExpression::AddCastToType(
            context_, std::move(child), LogicalType::TIMESTAMP, try_cast);
      }
      else
        /* An instant, including a string with an offset: its local time */
        *op= duckdb::BoundCastExpression::AddCastToType(
            context_, std::move(*op), LogicalType::TIMESTAMP);
    }
  }

  /* The instant side of a comparison that can be expanded against a
     constant: TIMESTAMPTZ, not itself a local-time cast, not volatile */
  static bool is_instant_operand(const duckdb::Expression &expr)
  {
    return expr.GetReturnType().id() == duckdb::LogicalTypeId::TIMESTAMP_TZ &&
           !is_local_to_tz_cast(expr) && !expr.IsVolatile();
  }

  enum class ConstLocal
  {
    LOCAL,   /* a local time: *local_us is set */
    INSTANT, /* a string with an offset or zone: compare as instant */
    OTHER    /* not a usable constant */
  };

  /* The local time of a cast whose source is a constant */
  ConstLocal constant_local_time(const duckdb::Expression &cast,
                                 int64_t *local_us)
  {
    auto &child= cast_child(cast);
    if (!child.IsFoldable())
      return ConstLocal::OTHER;
    duckdb::Value value;
    if (!duckdb::ExpressionExecutor::TryEvaluateScalar(context_, child,
                                                        value) ||
        value.IsNull())
      return ConstLocal::OTHER;
    duckdb::timestamp_t ts;
    switch (value.type().id())
    {
    case duckdb::LogicalTypeId::TIMESTAMP:
      ts= value.GetValue<duckdb::timestamp_t>();
      break;
    case duckdb::LogicalTypeId::DATE:
      if (!duckdb::TryCast::Operation(value.GetValue<duckdb::date_t>(), ts,
                                      false))
        return ConstLocal::OTHER;
      break;
    case duckdb::LogicalTypeId::VARCHAR: {
      auto str= value.ToString();
      duckdb::string_t tz(nullptr, 0);
      bool has_offset= false;
      if (duckdb::Timestamp::TryConvertTimestampTZ(str.c_str(), str.size(),
                                                   ts, true, has_offset,
                                                   tz) !=
          duckdb::TimestampCastResult::SUCCESS)
        return ConstLocal::OTHER;
      if (has_offset || tz.GetSize())
        return ConstLocal::INSTANT;
      break;
    }
    default:
      return ConstLocal::OTHER;
    }
    if (!ts.IsFinite())
      return ConstLocal::INSTANT;
    *local_us= ts.value;
    return ConstLocal::LOCAL;
  }

  ExprPtr instant_constant(int64_t us)
  {
    return duckdb::make_uniq<duckdb::BoundConstantExpression>(
        duckdb::Value::TIMESTAMPTZ(duckdb::timestamp_tz_t(us)));
  }

  ExprPtr compare(duckdb::ExpressionType type, const duckdb::Expression &x,
                  int64_t us)
  {
    return duckdb::BoundComparisonExpression::Create(type, x.Copy(),
                                                     instant_constant(us));
  }

  static ExprPtr conj(duckdb::ExpressionType type, ExprPtr a, ExprPtr b)
  {
    duckdb::vector<ExprPtr> children;
    children.push_back(std::move(a));
    children.push_back(std::move(b));
    return duckdb::BoundConjunctionExpression::Create(type, std::move(children));
  }

  /* "local time of x <op> L" as a condition on x's instants */
  ExprPtr expand(duckdb::ExpressionType op, const duckdb::Expression &x,
                 int64_t local_us)
  {
    using duckdb::ExpressionType;
    const auto AND= ExpressionType::CONJUNCTION_AND;
    const auto OR= ExpressionType::CONJUNCTION_OR;
    auto in= local_time_instants(*zone_, local_us);
    switch (in.kind)
    {
    case LocalTimeInstants::SINGLE:
      return compare(op, x, in.i1);
    case LocalTimeInstants::SKIPPED:
      switch (op)
      {
      case ExpressionType::COMPARE_EQUAL: /* never; NULL stays NULL */
        return conj(AND, compare(ExpressionType::COMPARE_EQUAL, x, in.t),
                    compare(ExpressionType::COMPARE_NOTEQUAL, x, in.t));
      case ExpressionType::COMPARE_NOTEQUAL:
        return conj(OR, compare(ExpressionType::COMPARE_EQUAL, x, in.t),
                    compare(ExpressionType::COMPARE_NOTEQUAL, x, in.t));
      case ExpressionType::COMPARE_LESSTHAN:
      case ExpressionType::COMPARE_LESSTHANOREQUALTO:
        return compare(ExpressionType::COMPARE_LESSTHAN, x, in.t);
      default:
        return compare(ExpressionType::COMPARE_GREATERTHANOREQUALTO, x, in.t);
      }
    case LocalTimeInstants::REPEATED:
      /* Instants [t0, t) show local times [A, B), then [t, t2) show them
         again; i1 and i2 are L's two instants. */
      switch (op)
      {
      case ExpressionType::COMPARE_EQUAL:
        return conj(OR, compare(op, x, in.i1), compare(op, x, in.i2));
      case ExpressionType::COMPARE_NOTEQUAL:
        return conj(AND, compare(op, x, in.i1), compare(op, x, in.i2));
      case ExpressionType::COMPARE_LESSTHAN:
      case ExpressionType::COMPARE_LESSTHANOREQUALTO:
        return conj(
            OR, compare(op, x, in.i1),
            conj(AND,
                 compare(ExpressionType::COMPARE_GREATERTHANOREQUALTO, x, in.t),
                 compare(op, x, in.i2)));
      default: /* > and >= */
        return conj(
            OR,
            conj(AND, compare(op, x, in.i1),
                 compare(ExpressionType::COMPARE_LESSTHAN, x, in.t)),
            compare(op, x, in.i2));
      }
    }
    return nullptr;
  }

  static bool is_ordered_comparison(duckdb::ExpressionType type)
  {
    switch (type)
    {
    case duckdb::ExpressionType::COMPARE_EQUAL:
    case duckdb::ExpressionType::COMPARE_NOTEQUAL:
    case duckdb::ExpressionType::COMPARE_LESSTHAN:
    case duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
    case duckdb::ExpressionType::COMPARE_GREATERTHAN:
    case duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
      return true;
    default:
      return false;
    }
  }

  /* x <op> cast(constant) on instants; nullptr if it does not apply.
     *instant is set when the constant is itself an instant. */
  ExprPtr try_expand(duckdb::ExpressionType op, const duckdb::Expression &x,
                     const duckdb::Expression &cast, bool *instant)
  {
    *instant= false;
    if (!is_ordered_comparison(op) || !is_instant_operand(x))
      return nullptr;
    int64_t local_us;
    switch (constant_local_time(cast, &local_us))
    {
    case ConstLocal::LOCAL:
      return expand(op, x, local_us);
    case ConstLocal::INSTANT:
      *instant= true;
      return nullptr;
    case ConstLocal::OTHER:
      return nullptr;
    }
    return nullptr;
  }

  void rewrite_comparison(ExprPtr &expression)
  {
    auto &expr= *expression;
    auto type= expr.GetExpressionType();

    if (expr.GetExpressionClass() == duckdb::ExpressionClass::BOUND_OPERATOR &&
        (type == duckdb::ExpressionType::COMPARE_IN ||
         type == duckdb::ExpressionType::COMPARE_NOT_IN))
    {
      rewrite_in(expression);
      return;
    }
    if (expr.GetExpressionClass() != duckdb::ExpressionClass::BOUND_FUNCTION)
      return;
    auto &func= expr.Cast<duckdb::BoundFunctionExpression>();

    if (duckdb::BoundComparisonExpression::IsComparison(expr))
    {
      auto &left= duckdb::BoundComparisonExpression::LeftMutable(func);
      auto &right= duckdb::BoundComparisonExpression::RightMutable(func);
      bool lcast= is_local_to_tz_cast(*left), rcast= is_local_to_tz_cast(*right);
      if (!lcast && !rcast)
        return;
      if (lcast != rcast)
      {
        bool instant;
        auto expanded=
            rcast ? try_expand(type, *left, *right, &instant)
                  : try_expand(duckdb::FlipComparisonExpression(type), *right,
                               *left, &instant);
        if (expanded)
        {
          expression= std::move(expanded);
          return;
        }
        if (instant)
          return; /* rewrite_cast() handles the cast */
      }
      auto l= std::move(left), r= std::move(right);
      compare_as_local({&l, &r});
      expression= duckdb::BoundComparisonExpression::Create(type, std::move(l),
                                                            std::move(r));
    }
    else if (type == duckdb::ExpressionType::COMPARE_BETWEEN)
    {
      auto &input= duckdb::BoundBetweenExpression::InputMutable(func);
      auto &lower= duckdb::BoundBetweenExpression::LowerBoundMutable(func);
      auto &upper= duckdb::BoundBetweenExpression::UpperBoundMutable(func);
      if (!is_local_to_tz_cast(*input) && !is_local_to_tz_cast(*lower) &&
          !is_local_to_tz_cast(*upper))
        return;
      bool lower_inclusive= duckdb::BoundBetweenExpression::LowerInclusive(func);
      bool upper_inclusive= duckdb::BoundBetweenExpression::UpperInclusive(func);
      if (is_instant_operand(*input))
      {
        /* x BETWEEN a AND b as two comparisons, each expanded */
        auto lo_cmp= duckdb::BoundComparisonExpression::Create(
            duckdb::BoundBetweenExpression::LowerComparisonType(lower_inclusive),
            input->Copy(), std::move(lower));
        auto hi_cmp= duckdb::BoundComparisonExpression::Create(
            duckdb::BoundBetweenExpression::UpperComparisonType(upper_inclusive),
            std::move(input), std::move(upper));
        rewrite_comparison(lo_cmp);
        rewrite_comparison(hi_cmp);
        expression= conj(duckdb::ExpressionType::CONJUNCTION_AND,
                         std::move(lo_cmp), std::move(hi_cmp));
        return;
      }
      auto i= std::move(input), lo= std::move(lower), hi= std::move(upper);
      compare_as_local({&i, &lo, &hi});
      expression= duckdb::BoundBetweenExpression::Create(
          std::move(i), std::move(lo), std::move(hi), lower_inclusive,
          upper_inclusive);
    }
  }

  /* x [NOT] IN (items): MariaDB compares a TIMESTAMP with DATETIME and
     string items as DATETIME */
  void rewrite_in(ExprPtr &expression)
  {
    auto &op= expression->Cast<duckdb::BoundOperatorExpression>();
    auto &children= op.GetChildrenMutable();
    bool any_local= is_local_to_tz_cast(*children[0]), all_constant= true;
    for (size_t i= 1; i < children.size(); i++)
    {
      if (is_local_to_tz_cast(*children[i]))
        any_local= true;
      else if (!children[i]->IsFoldable())
        all_constant= false;
    }
    if (!any_local)
      return;

    auto &x= *children[0];
    if (all_constant && is_instant_operand(x))
    {
      /* Every item becomes the instants its local time stands for */
      duckdb::vector<ExprPtr> instants;
      bool ok= true;
      for (size_t i= 1; i < children.size() && ok; i++)
      {
        auto &item= *children[i];
        if (!is_local_to_tz_cast(item))
        {
          instants.push_back(item.Copy());
          continue;
        }
        int64_t local_us;
        if (constant_local_time(item, &local_us) != ConstLocal::LOCAL)
        {
          ok= false;
          break;
        }
        auto in= local_time_instants(*zone_, local_us);
        if (in.kind == LocalTimeInstants::SINGLE ||
            in.kind == LocalTimeInstants::REPEATED)
          instants.push_back(instant_constant(in.i1));
        if (in.kind == LocalTimeInstants::REPEATED)
          instants.push_back(instant_constant(in.i2));
      }
      if (ok)
      {
        bool negated=
            expression->GetExpressionType() == duckdb::ExpressionType::COMPARE_NOT_IN;
        if (instants.empty())
        {
          /* Only skipped times: no match (NOT IN: all), NULL stays NULL */
          int64_t t= 0;
          expression=
              conj(negated ? duckdb::ExpressionType::CONJUNCTION_OR
                           : duckdb::ExpressionType::CONJUNCTION_AND,
                   compare(duckdb::ExpressionType::COMPARE_EQUAL, x, t),
                   compare(duckdb::ExpressionType::COMPARE_NOTEQUAL, x, t));
          return;
        }
        auto result= duckdb::make_uniq<duckdb::BoundOperatorExpression>(
            expression->GetExpressionType(), LogicalType::BOOLEAN);
        result->GetChildrenMutable().push_back(std::move(children[0]));
        for (auto &instant : instants)
          result->GetChildrenMutable().push_back(std::move(instant));
        expression= std::move(result);
        return;
      }
    }
    std::vector<ExprPtr *> operands;
    for (auto &child : children)
      operands.push_back(&child);
    compare_as_local(operands);
  }

  void rewrite_cast(duckdb::unique_ptr<duckdb::Expression> &expression)
  {
    auto &expr= *expression;
    if (expr.GetExpressionClass() != duckdb::ExpressionClass::BOUND_FUNCTION ||
        !duckdb::BoundCastExpression::IsCast(expr))
      return;
    auto &cast= expr.Cast<duckdb::BoundFunctionExpression>();
    if (duckdb::BoundCastExpression::TargetType(cast).id() !=
        duckdb::LogicalTypeId::TIMESTAMP_TZ)
      return;
    switch (duckdb::BoundCastExpression::SourceType(cast).id())
    {
    case duckdb::LogicalTypeId::TIMESTAMP:
    case duckdb::LogicalTypeId::DATE:
    case duckdb::LogicalTypeId::VARCHAR:
      break;
    default:
      return;
    }
    bool try_cast= duckdb::BoundCastExpression::IsTryCast(cast);
    duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> children;
    children.push_back(
        std::move(duckdb::BoundCastExpression::ChildMutable(cast)));
    duckdb::FunctionBinder binder(context_);
    expression= binder.BindScalarFunction(
        duckdb::Identifier(DEFAULT_SCHEMA),
        duckdb::Identifier(try_cast ? TRY_TO_TIMESTAMPTZ : TO_TIMESTAMPTZ),
        std::move(children));
  }

  duckdb::ClientContext &context_;
  duckdb::unique_ptr<TimeZone> zone_; /* session TimeZone */
};

static void rewrite_casts(duckdb::OptimizerExtensionInput &input,
                          duckdb::unique_ptr<duckdb::LogicalOperator> &plan)
{
  if (!get_bool_variable(input.context, MARIADB_TZ_RULES_VAR))
    return;
  MariadbCastRewriter rewriter(input.context);
  rewriter.VisitOperator(*plan);
}

/* ----------------------------------------------------------------
   Registration
   ---------------------------------------------------------------- */

void register_mariadb_time_zone_functions(duckdb::DatabaseInstance &db)
{
  {
    duckdb::ScalarFunctionSet set("mariadb_unix_timestamp");
    set.AddFunction(duckdb::ScalarFunction({LogicalType::TIMESTAMP_TZ},
                                           LogicalType::BIGINT,
                                           unix_timestamp_tz_func));
    set.AddFunction(duckdb::ScalarFunction(
        {LogicalType::TIMESTAMP}, LogicalType::BIGINT,
        unix_timestamp_local_func, session_zone_bind));
    register_function(db, std::move(set));
  }
  {
    duckdb::ScalarFunctionSet set("mariadb_convert_tz");
    set.AddFunction(duckdb::ScalarFunction(
        {LogicalType::TIMESTAMP, LogicalType::VARCHAR, LogicalType::VARCHAR},
        LogicalType::TIMESTAMP, convert_tz_local_func, session_zone_bind));
    set.AddFunction(duckdb::ScalarFunction(
        {LogicalType::TIMESTAMP_TZ, LogicalType::VARCHAR,
         LogicalType::VARCHAR},
        LogicalType::TIMESTAMP, convert_tz_instant_func, session_zone_bind));
    register_function(db, std::move(set));
  }

  register_function(db, to_timestamptz_functions(false));
  register_function(db, to_timestamptz_functions(true));

  duckdb::OptimizerExtension extension;
  extension.pre_optimize_function= rewrite_casts;
  duckdb::OptimizerExtension::Register(duckdb::DBConfig::GetConfig(db),
                                       std::move(extension));
}

} // namespace myduck
