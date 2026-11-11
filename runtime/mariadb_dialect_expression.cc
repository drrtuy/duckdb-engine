#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"

namespace myduck
{
namespace
{

void initialize_convert(duckdb::PEGTransformer &,
                        duckdb::GeneratedTransformProcess &process)
{
  auto &list= process.parse_result.Cast<duckdb::ListParseResult>();
  process.ReserveChildSlots(2);
  process.PushChild(list.GetChild(4), 1);
  process.PushChild(list.GetChild(2), 0);
}

duckdb::arena_ptr<duckdb::TransformResultValue> finalize_convert(
    duckdb::PEGTransformer &transformer,
    duckdb::GeneratedTransformProcess &process)
{
  auto expression=
      process.TakeResult<duckdb::unique_ptr<duckdb::ParsedExpression>>(0);
  auto type= process.TakeResult<duckdb::LogicalType>(1);
  auto result= duckdb::make_uniq<duckdb::CastExpression>(
      type, std::move(expression));
  return transformer.MakeResult<duckdb::unique_ptr<duckdb::ParsedExpression>>(
      std::move(result));
}

duckdb::arena_ptr<duckdb::TransformProcess> start_convert(
    duckdb::PEGTransformer &transformer, duckdb::ParseResult &parse_result)
{
  static const duckdb::TransformFrameOps operations{
      "MariadbConvertExpression", initialize_convert, finalize_convert, nullptr};
  return transformer.MakeProcess<duckdb::GeneratedTransformProcess>(
      transformer, duckdb::TransformInput(parse_result), operations);
}

duckdb::arena_ptr<duckdb::TransformProcess> start_invalid_convert(
    duckdb::PEGTransformer &transformer, duckdb::ParseResult &parse_result)
{
  return transformer.MakeProcess<duckdb::FinalizeTransformProcess>(
      transformer, parse_result,
      [](duckdb::PEGTransformer &, duckdb::ParseResult &invalid_result)
          -> duckdb::arena_ptr<duckdb::TransformResultValue> {
        throw duckdb::ParserException(invalid_result.GetLocation(),
                                      "CONVERT requires an expression and a type");
      });
}

duckdb::arena_ptr<duckdb::TransformProcess> start_invalid_regex(
    duckdb::PEGTransformer &transformer, duckdb::ParseResult &parse_result)
{
  return transformer.MakeProcess<duckdb::FinalizeTransformProcess>(
      transformer, parse_result,
      [](duckdb::PEGTransformer &, duckdb::ParseResult &invalid_result)
          -> duckdb::arena_ptr<duckdb::TransformResultValue> {
        throw duckdb::ParserException(invalid_result.GetLocation(),
                                      "REGEXP/RLIKE requires a pattern expression");
      });
}

duckdb::arena_ptr<duckdb::TransformResultValue> finalize_current_time(
    duckdb::PEGTransformer &transformer, duckdb::ParseResult &parse_result)
{
  auto &list= parse_result.Cast<duckdb::ListParseResult>();
  auto &keyword= list.Child<duckdb::ListParseResult>(0)
                    .Child<duckdb::ChoiceParseResult>(0)
                    .GetResult().Cast<duckdb::KeywordParseResult>();
  auto &precision= list.Child<duckdb::OptionalParseResult>(2);
  if (precision.HasResult())
  {
    auto &number= precision.GetResult().Cast<duckdb::NumberParseResult>();
    if (duckdb::Identifier(keyword.keyword) == "CURRENT_DATE")
      throw duckdb::ParserException(number.GetLocation(),
                                    "CURRENT_DATE does not accept a precision");
    uint32_t value= 0;
    bool valid= true;
    for (char digit : number.number)
    {
      if (digit < '0' || digit > '9')
      {
        valid= false;
        break;
      }
      value= value * 10 + static_cast<uint32_t>(digit - '0');
      if (value > 6)
      {
        valid= false;
        break;
      }
    }
    if (!valid)
      throw duckdb::ParserException(number.GetLocation(),
                                    "Fractional seconds precision must be an integer from 0 to 6");
  }
  auto result= duckdb::make_uniq<duckdb::ColumnRefExpression>(
      duckdb::Identifier(duckdb::StringUtil::Lower(keyword.keyword)));
  return transformer.MakeResult<duckdb::unique_ptr<duckdb::ParsedExpression>>(
      std::move(result));
}

duckdb::arena_ptr<duckdb::TransformProcess> start_current_time(
    duckdb::PEGTransformer &transformer, duckdb::ParseResult &parse_result)
{
  return transformer.MakeProcess<duckdb::FinalizeTransformProcess>(
      transformer, parse_result, finalize_current_time);
}

}

void add_mariadb_expression_grammar(duckdb::ParsedGrammar &grammar)
{
  grammar.AddRule(
      "MariadbInvalidConvertExpression <- 'CONVERT' FunctionExpressionArguments",
      start_invalid_convert);
  grammar.PrependChoice("SingleExpression", "MariadbInvalidConvertExpression");
  grammar.AddRule(
      "MariadbConvertExpression <- 'CONVERT' '(' Expression ',' Type ')'",
      start_convert);
  grammar.SetExpressionDepth("MariadbConvertExpression",
                             duckdb::ExpressionDepthKind::NESTING);
  grammar.PrependChoice("SingleExpression", "MariadbConvertExpression");
  grammar.AddChoice("RegexMatchToken", "'REGEXP'");
  grammar.AddChoice("RegexMatchToken", "'RLIKE'");
  grammar.AddRule("MariadbInvalidRegexExpression <- 'REGEXP' / 'RLIKE'",
                  start_invalid_regex);
  grammar.AddChoice("BetweenLikeOpExpression", "MariadbInvalidRegexExpression");
  grammar.AddRule(
      "MariadbCurrentTimeExpression <- ('CURRENT_DATE' / 'CURRENT_TIME' / 'CURRENT_TIMESTAMP') "
      "'(' NumberLiteral? ')'",
      start_current_time);
  grammar.PrependChoice("SingleExpression", "MariadbCurrentTimeExpression");
}

}
