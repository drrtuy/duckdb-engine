#include "duckdb/parser/peg/parsed_grammar.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/result_modifier.hpp"

namespace myduck
{
namespace
{

using namespace duckdb;

void replace_select_rule(ParsedGrammar &grammar, const char *name,
                         const char *definition)
{
  const auto &rule= *grammar.GetRule(name);
  auto transform= rule.transform_process;
  auto collapsible= rule.collapsible;
  auto suppress_suggestions= rule.suppress_suggestions;
  auto depth= rule.expression_depth;
  grammar.ReplaceRule(definition, transform);
  grammar.SetTransformProcess(name, std::move(transform), collapsible,
                              suppress_suggestions);
  grammar.SetExpressionDepth(name, depth);
}

void initialize_comma_limit(PEGTransformer &, GeneratedTransformProcess &process)
{
  auto &list= process.parse_result.Cast<ListParseResult>();
  process.ReserveChildSlots(2);
  process.PushChild({list.GetChild(3)}, 1);
  process.PushChild({list.GetChild(1)}, 0);
}

arena_ptr<TransformResultValue>
finalize_comma_limit(PEGTransformer &transformer, GeneratedTransformProcess &process)
{
  auto result= make_uniq<LimitModifier>();
  result->offset= process.TakeResult<unique_ptr<ParsedExpression>>(0);
  result->limit= process.TakeResult<unique_ptr<ParsedExpression>>(1);
  return transformer.MakeResult<unique_ptr<ResultModifier>>(std::move(result));
}

arena_ptr<TransformProcess> transform_comma_limit(PEGTransformer &transformer,
                                                 ParseResult &result)
{
  static const TransformFrameOps ops{"MariadbCommaLimit", initialize_comma_limit,
                                     finalize_comma_limit, nullptr};
  return transformer.MakeProcess<GeneratedTransformProcess>(transformer,
                                                            TransformInput(result), ops);
}

void initialize_postfix_rollup(PEGTransformer &, GeneratedTransformProcess &process)
{
  auto &list= process.parse_result.Cast<ListParseResult>();
  auto expressions= PEGTransformerFactory::ExtractParseResultsFromList(list.GetChild(0));
  process.ReserveChildSlots(expressions.size());
  for (idx_t i= expressions.size(); i > 0; i--)
    process.PushChild({expressions[i - 1].get()}, i - 1);
}

arena_ptr<TransformResultValue>
finalize_postfix_rollup(PEGTransformer &transformer, GeneratedTransformProcess &process)
{
  vector<unique_ptr<ParsedExpression>> expressions;
  for (idx_t i= 0; i < process.child_results.size(); i++)
    expressions.push_back(process.TakeResult<unique_ptr<ParsedExpression>>(i));
  vector<GroupByExpressionInfo> groups;
  groups.push_back(PEGTransformerFactory::TransformCubeOrRollupClause(
      transformer, "ROLLUP", std::move(expressions)));
  return transformer.MakeResult<GroupByNode>(
      PEGTransformerFactory::TransformGroupByList(transformer, std::move(groups)));
}

arena_ptr<TransformProcess> transform_postfix_rollup(PEGTransformer &transformer,
                                                    ParseResult &result)
{
  static const TransformFrameOps ops{"MariadbPostfixRollup", initialize_postfix_rollup,
                                     finalize_postfix_rollup, nullptr};
  return transformer.MakeProcess<GeneratedTransformProcess>(transformer,
                                                            TransformInput(result), ops);
}

void initialize_conditionless_join(PEGTransformer &transformer,
                                    GeneratedTransformProcess &process)
{
  auto &list= process.parse_result.Cast<ListParseResult>();
  process.ReserveChildSlots(2);
  JoinPrefix prefix;
  prefix.ref_type= JoinRefType::CROSS;
  process.SetChildResult(0, transformer.MakeResult<JoinPrefix>(prefix));
  process.PushChild({list.GetChild(1)}, 1);
}

arena_ptr<TransformProcess> transform_conditionless_join(PEGTransformer &transformer,
                                                        ParseResult &result)
{
  static const TransformFrameOps ops{
      "MariadbConditionlessJoin", initialize_conditionless_join,
      PEGTransformerFactory::FinalizeJoinWithoutOnClauseTrampoline, nullptr};
  return transformer.MakeProcess<GeneratedTransformProcess>(transformer,
                                                            TransformInput(result), ops);
}

void initialize_hinted_table(PEGTransformer &transformer,
                             GeneratedTransformProcess &process)
{
  auto &list= process.parse_result.Cast<ListParseResult>();
  process.ReserveChildSlots(5);
  for (idx_t i : {idx_t(4), idx_t(3)})
  {
    auto &optional= list.Child<OptionalParseResult>(i);
    if (optional.HasResult())
      process.PushChild({optional.GetResult()}, i);
  }
  auto &alias_and_hints= list.Child<OptionalParseResult>(2);
  if (alias_and_hints.HasResult())
  {
    auto &choice_list= alias_and_hints.GetResult().Cast<ListParseResult>();
    auto &choice= choice_list.Child<ChoiceParseResult>(0).GetResult();
    if (choice.Name() == "MariadbAliasedIndexHints")
    {
      auto &alias= choice.Cast<ListParseResult>().GetChild(0);
      process.PushChild({transformer.GetRule(PEGRuleId::TableAlias), alias}, 2);
    }
  }
  process.PushChild({list.GetChild(1)}, 1);
  auto &alias_colon= list.Child<OptionalParseResult>(0);
  if (alias_colon.HasResult())
    process.PushChild({alias_colon.GetResult()}, 0);
}

arena_ptr<TransformProcess> transform_hinted_table(PEGTransformer &transformer,
                                                  ParseResult &result)
{
  static const TransformFrameOps ops{
      "BaseTableRef", initialize_hinted_table,
      PEGTransformerFactory::FinalizeBaseTableRefTrampoline, nullptr};
  return transformer.MakeProcess<GeneratedTransformProcess>(transformer,
                                                            TransformInput(result), ops);
}

}

void add_mariadb_select_grammar(duckdb::ParsedGrammar &grammar)
{
  grammar.AddRule("MariadbCommaLimit <- 'LIMIT' Expression ',' Expression",
                  transform_comma_limit);
  grammar.PrependChoice("LimitOffset", "MariadbCommaLimit");

  grammar.AddRule("MariadbPostfixRollup <- List(Expression) 'WITH' 'ROLLUP'",
                  transform_postfix_rollup);
  grammar.PrependChoice("GroupByExpressions", "MariadbPostfixRollup");

  grammar.AddRule("MariadbIgnoredSelectHint <- 'HIGH_PRIORITY' / 'SQL_NO_CACHE' / "
                  "'SQL_CACHE' / 'SQL_BUFFER_RESULT' / 'SQL_SMALL_RESULT' / "
                  "'SQL_BIG_RESULT' / 'SQL_CALC_FOUND_ROWS' / 'STRAIGHT_JOIN'");
  grammar.AddRule("MariadbSelectKeyword <- 'SELECT' MariadbIgnoredSelectHint*");
  replace_select_rule(grammar, "SelectClause",
      "SelectClause <- MariadbSelectKeyword DistinctClause? TargetList?");
  replace_select_rule(grammar, "DistinctOn",
      "DistinctOn <- ('DISTINCT' / 'DISTINCTROW') DistinctOnTargets? MariadbIgnoredSelectHint*");
  replace_select_rule(grammar, "DistinctAll",
      "DistinctAll <- 'ALL' MariadbIgnoredSelectHint*");
  grammar.AddChoice("ReservedKeyword", "'STRAIGHT_JOIN' / 'DISTINCTROW'");

  grammar.AddRule("MariadbJoinKeyword <- 'JOIN' / 'STRAIGHT_JOIN'");
  replace_select_rule(grammar, "RegularJoinClause",
      "RegularJoinClause <- Asof? JoinType? MariadbJoinKeyword TableRef JoinQualifier");
  grammar.AddRule("MariadbConditionlessJoin <- MariadbJoinKeyword InnerTableRef",
                  transform_conditionless_join);
  grammar.AddChoice("JoinClause", "MariadbConditionlessJoin");

  grammar.AddRule("MariadbIndexHintScope <- 'FOR' ('JOIN' / 'ORDER' 'BY' / 'GROUP' 'BY')");
  grammar.AddRule("MariadbIndexHintName <- IndexName / 'PRIMARY'");
  grammar.AddRule("MariadbIndexHintNames <- MariadbIndexHintName (',' MariadbIndexHintName)*");
  grammar.AddRule("MariadbIgnoredIndexHint <- ('USE' ('INDEX' / 'KEY') MariadbIndexHintScope? "
                  "Parens(MariadbIndexHintNames?)) / "
                  "(('FORCE' / 'IGNORE') ('INDEX' / 'KEY') MariadbIndexHintScope? "
                  "Parens(MariadbIndexHintNames))");
  grammar.AddRule("MariadbAliasedIndexHints <- TableAlias MariadbIgnoredIndexHint*");
  grammar.AddRule("MariadbTableAliasAndHints <- MariadbIgnoredIndexHint+ / MariadbAliasedIndexHints");
  auto depth= grammar.GetRule("BaseTableRef")->expression_depth;
  grammar.ReplaceRule(
      "BaseTableRef <- TableAliasColon? BaseTableName MariadbTableAliasAndHints? AtClause? SampleClause?",
      transform_hinted_table);
  grammar.SetExpressionDepth("BaseTableRef", depth);
}

}
