#pragma once

#include <memory>
#include <string>
#include <vector>

#include "src/carnot/exec/exec_node.h"
#include "src/carnot/exec/exec_state.h"
#include "src/carnot/exec/expression_evaluator.h"
#include "src/table_store/table_store.h"

namespace pl {
namespace carnot {
namespace exec {

class FilterNode : public ProcessingNode {
 public:
  FilterNode() = default;

 protected:
  std::string DebugStringImpl() override;
  Status InitImpl(
      const plan::Operator& plan_node, const table_store::schema::RowDescriptor& output_descriptor,
      const std::vector<table_store::schema::RowDescriptor>& input_descriptors) override;
  Status PrepareImpl(ExecState* exec_state) override;
  Status OpenImpl(ExecState* exec_state) override;
  Status CloseImpl(ExecState* exec_state) override;
  Status ConsumeNextImpl(ExecState* exec_state, const table_store::schema::RowBatch& rb) override;

 private:
  std::unique_ptr<VectorNativeScalarExpressionEvaluator> evaluator_;
  std::unique_ptr<plan::FilterOperator> plan_node_;
  std::unique_ptr<table_store::schema::RowDescriptor> output_descriptor_;
};

}  // namespace exec
}  // namespace carnot
}  // namespace pl
