#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/carnot/exec/exec_node.h"
#include "src/carnot/exec/memory_source_node.h"
#include "src/carnot/plan/plan_fragment.h"
#include "src/common/memory/memory.h"
#include "src/shared/types/types.h"

namespace pl {
namespace carnot {
namespace exec {

struct ExecutionStats {
  int64_t bytes_processed;
  int64_t rows_processed;
};

/**
 * An Execution Graph defines the structure of execution nodes for a given plan fragment.
 */
class ExecutionGraph {
 public:
  /**
   * Initializes the Execution Graph by initializing the execution nodes and their children.
   * @param schema The schema of available relations.
   * @param compilerState The compiler state.
   * @param execState The execution state.
   * @param pf The plan fragment to create the execution graph from.
   * @return The status of whether initialization succeeded.
   */
  Status Init(std::shared_ptr<table_store::schema::Schema> schema, plan::PlanState* plan_state,
              ExecState* exec_state, plan::PlanFragment* pf);
  std::vector<int64_t> sources() { return sources_; }
  StatusOr<ExecNode*> node(int64_t id) {
    auto node = nodes_.find(id);
    if (node == nodes_.end()) {
      return error::NotFound("Could not find ExecNode.");
    }
    return node->second;
  }
  Status Execute();
  std::vector<std::string> OutputTables() const;
  ExecutionStats GetStats() const;

 private:
  /**
   * For the given operator type, creates the corresponding execution node and updates the structure
   * of the execution graph.
   * @param node The operator to convert to an execution node.
   * @param nodes A map of node ids to execution nodes in the graph.
   * @param descriptors The descriptors of the execution nodes in the graph.
   * @return A status of whether the initialization of the operator has succeeded.
   */
  template <typename TOp, typename TNode>
  Status OnOperatorImpl(
      TOp node, std::unordered_map<int64_t, table_store::schema::RowDescriptor>* descriptors) {
    std::vector<table_store::schema::RowDescriptor> input_descriptors;

    auto parents = pf_->dag().ParentsOf(node.id());

    // Get input descriptors for this operator.
    for (const int64_t& parent_id : parents) {
      auto input_desc = descriptors->find(parent_id);
      if (input_desc == descriptors->end()) {
        return error::NotFound("Could not find RowDescriptor.");
      }
      input_descriptors.push_back(input_desc->second);
    }
    // Get output descriptor.
    auto output_rel = node.OutputRelation(*schema_, *plan_state_, parents).ConsumeValueOrDie();
    table_store::schema::RowDescriptor output_descriptor(output_rel.col_types());
    // TODO(michelle) (PL-400) causes excessive warnings.
    schema_->AddRelation(node.id(), output_rel);
    descriptors->insert({node.id(), output_descriptor});

    // Create ExecNode.
    auto execNode = pool_.Add(new TNode());
    auto s = execNode->Init(node, output_descriptor, input_descriptors);
    nodes_.insert({node.id(), execNode});

    // Update parents' children.
    for (const int64_t& parent_id : parents) {
      auto parent = nodes_.find(parent_id);
      // Error if can't find parent in nodes.
      if (parent == nodes_.end()) {
        return error::NotFound("Could not find parent ExecNode.");
      }
      parent->second->AddChild(execNode);
    }
    return Status::OK();
  }

  ExecState* exec_state_;
  ObjectPool pool_;
  std::shared_ptr<table_store::schema::Schema> schema_;
  plan::PlanState* plan_state_;
  plan::PlanFragment* pf_;
  std::vector<int64_t> sources_;
  std::vector<int64_t> sinks_;
  std::unordered_map<int64_t, ExecNode*> nodes_;
};

}  // namespace exec
}  // namespace carnot
}  // namespace pl
