#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/carnot/compiler/compiler_state.h"
#include "src/carnot/compiler/ir_nodes.h"
#include "src/carnot/compiler/registry_info.h"
#include "src/table_store/table_store.h"

namespace pl {
namespace carnot {
namespace compiler {

class IRRelationHandler {
 public:
  IRRelationHandler() = delete;
  explicit IRRelationHandler(CompilerState* compiler_state);
  /**
   * @brief Wrapper for all of the update functionality of the IRRelationHandler.

   * @param ir_graph -> ptr to ir memorythat is managed by the caller of the IRRelationHandler. Will
   * be updat by this constructor.
   * @return Status
   */
  Status UpdateRelationsAndCheckFunctions(IR* ir_graph);

 private:
  /**
   * @brief Finds the sources in the graph, then gets the relation from the appropriate places.
   *
   * @param ir_graph
   * @param compiler_state
   * @return Status
   */
  Status UpdateSourceRelations(IR* ir_graph, CompilerState* compiler_state);

  /**
   * @brief Iterates through all of the IR columns and makes sure
   * that they are read to be transposed into the logical plan nodes.
   *
   * @param ir_graph
   * @return Status
   */
  std::vector<Status> VerifyIRColumnsReady(IR* ir_graph);

  Status RelationUpdate(OperatorIR* node);

  /**
   * @brief Handle sinks. Just copies the parent_relation.
   *
   * @param the node operating on.
   * @param parent_rel - the parent relation of the node.
   * @return StatusOr <table_store::schema::Relation> the resultant relation.
   */
  StatusOr<table_store::schema::Relation> SinkHandler(OperatorIR* node,
                                                      table_store::schema::Relation parent_rel);

  /**
   * @brief Handle Agg Operator.
   * Creates a new relation based on the expressions of the Agg.
   * Returns an error if it can't find expected columns in the parent_relation.
   *
   * @param the node operating on.
   * @param parent_rel - the parent relation of the node.
   * @return StatusOr <table_store::schema::Relation> the resultant relation.
   */
  StatusOr<table_store::schema::Relation> BlockingAggHandler(
      OperatorIR* node, table_store::schema::Relation parent_rel);

  /**
   * @brief Handle Map operator.
   * Adds columns to the parent relation according to each expression.
   * Returns an error if it can't find expected columns in the parent_relation.
   *
   * @param the node operating on.
   * @param parent_rel - the parent relation of the node.
   * @return StatusOr <table_store::schema::Relation> the resultant relation.
   */
  StatusOr<table_store::schema::Relation> MapHandler(OperatorIR* node,
                                                     table_store::schema::Relation parent_rel);

  /**
   * @brief Handle Range Operator. Just copies the parent_relation.
   *
   * @param the node operating on.
   * @param parent_rel - the parent relation of the node.
   * @return StatusOr <table_store::schema::Relation> the resultant relation.
   */
  StatusOr<table_store::schema::Relation> RangeHandler(OperatorIR* node,
                                                       table_store::schema::Relation parent_rel);

  /**
   * @brief Handle Filter operator.
   * Really should just copy the parent relation for now.
   *
   * @param the node operating on.
   * @param parent_rel - the parent relation of the node.
   * @return StatusOr <table_store::schema::Relation> the resultant relation.
   */
  StatusOr<table_store::schema::Relation> FilterHandler(OperatorIR* node,
                                                        table_store::schema::Relation parent_rel);

  Status HasExpectedColumns(const std::unordered_set<std::string>& expected_columns,
                            const table_store::schema::Relation& parent_relation);
  /**
   * @brief Evaluates the expression to get the data.
   *
   * @param expr -> the expression to evaluate on
   * @param parent_rel -> the parent relation to use for evaluation.
   * @param is_map -> true if this is for a map, false if this is for agg. Used to select UDF vs UDA
   * @return StatusOr<types::DataType> The datatype output by this expression.
   */
  StatusOr<types::DataType> EvaluateExpression(IRNode* expr,
                                               const table_store::schema::Relation& parent_rel,
                                               bool is_map);
  StatusOr<types::DataType> EvaluateFuncExpr(FuncIR* expr,
                                             const table_store::schema::Relation& parent_rel,
                                             bool is_map);
  StatusOr<types::DataType> EvaluateColExpr(ColumnIR* expr,
                                            const table_store::schema::Relation& parent_rel);
  Status SetSourceRelation(IRNode* node);
  Status SetAllSourceRelations(IR* ir_graph);
  StatusOr<std::vector<ColumnIR*>> GetColumnsFromRelation(
      IRNode* node, std::vector<std::string> col_names,
      const table_store::schema::Relation& relation);

  StatusOr<IntIR*> EvaluateCompilerExpression(IRNode* node);
  StatusOr<IntIR*> EvaluateCompilerFunction(const std::string& name,
                                            std::vector<IntIR*> evaled_args, IRNode* parent_node);
  /** Variables **/
  CompilerState* compiler_state_;
};
}  // namespace compiler
}  // namespace carnot
}  // namespace pl
