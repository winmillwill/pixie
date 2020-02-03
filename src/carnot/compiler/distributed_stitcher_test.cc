#include <gmock/gmock.h>
#include <google/protobuf/text_format.h>
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include <pypa/parser/parser.hh>

#include "src/carnot/compiler/distributed_coordinator.h"
#include "src/carnot/compiler/distributed_plan.h"
#include "src/carnot/compiler/distributed_planner.h"
#include "src/carnot/compiler/distributed_stitcher.h"
#include "src/carnot/compiler/grpc_source_conversion.h"
#include "src/carnot/compiler/ir/ir_nodes.h"
#include "src/carnot/compiler/logical_planner/test_utils.h"
#include "src/carnot/compiler/metadata_handler.h"
#include "src/carnot/compiler/rule_mock.h"
#include "src/carnot/compiler/rules.h"
#include "src/carnot/compiler/test_utils.h"
#include "src/carnot/udf_exporter/udf_exporter.h"

namespace pl {
namespace carnot {
namespace compiler {
namespace distributed {
using logical_planner::testutils::kOneAgentOneKelvinDistributedState;
using logical_planner::testutils::kOneAgentThreeKelvinsDistributedState;
using logical_planner::testutils::kThreeAgentsOneKelvinDistributedState;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::UnorderedElementsAre;
using ::testing::UnorderedElementsAreArray;

class StitcherTest : public OperatorTests {
 protected:
  void SetUpImpl() override {
    auto rel_map = std::make_unique<RelationMap>();
    auto cpu_relation = table_store::schema::Relation(
        std::vector<types::DataType>({types::DataType::INT64, types::DataType::FLOAT64,
                                      types::DataType::FLOAT64, types::DataType::FLOAT64}),
        std::vector<std::string>({"count", "cpu0", "cpu1", "cpu2"}));
    rel_map->emplace("cpu", cpu_relation);

    info_ = std::make_unique<compiler::RegistryInfo>();
    compiler_state_ = std::make_unique<CompilerState>(std::move(rel_map), info_.get(), time_now);
  }
  distributedpb::DistributedState LoadDistributedStatePb(const std::string& physical_state_txt) {
    distributedpb::DistributedState physical_state_pb;
    CHECK(google::protobuf::TextFormat::MergeFromString(physical_state_txt, &physical_state_pb));
    return physical_state_pb;
  }

  void MakeSourceSinkGraph() {
    auto mem_source = MakeMemSource(MakeRelation());
    auto mem_sink = MakeMemSink(mem_source, "out");
    PL_CHECK_OK(mem_sink->SetRelation(MakeRelation()));
  }

  std::unique_ptr<DistributedPlan> MakeDistributedPlan(const distributedpb::DistributedState& ps) {
    auto coordinator = Coordinator::Create(ps).ConsumeValueOrDie();

    MakeSourceSinkGraph();

    auto physical_plan = coordinator->Coordinate(graph.get()).ConsumeValueOrDie();
    return physical_plan;
  }

  void TestBeforeSetSourceGroupGRPCAddress(std::vector<CarnotInstance*> data_stores,
                                           std::vector<CarnotInstance*> mergers) {
    for (auto data_store_agent : data_stores) {
      // Make sure that the grpc_sink has not yet been set to avoid a false positive.
      for (auto ir_node : data_store_agent->plan()->FindNodesOfType(IRNodeType::kGRPCSink)) {
        SCOPED_TRACE(data_store_agent->carnot_info().query_broker_address());
        auto grpc_sink = static_cast<GRPCSinkIR*>(ir_node);
        EXPECT_FALSE(grpc_sink->DestinationAddressSet());
      }
    }

    for (auto merger_agent : mergers) {
      // Make sure that the grpc_source_group addresses have not yet been set to avoid a false
      // positive.
      for (auto ir_node : merger_agent->plan()->FindNodesOfType(IRNodeType::kGRPCSourceGroup)) {
        SCOPED_TRACE(absl::Substitute("agent : $0, node :$1",
                                      merger_agent->carnot_info().query_broker_address(),
                                      ir_node->DebugString()));
        auto grpc_source_group = static_cast<GRPCSourceGroupIR*>(ir_node);
        EXPECT_FALSE(grpc_source_group->GRPCAddressSet());
        // EXPECT_FALSE(grpc_source_group->GRPCAddressSet());
        EXPECT_EQ(grpc_source_group->dependent_sinks().size(), 0);
      }
    }
  }

  // Test to make sure that GRPCAddresses are set properly and that nothing else is set in the grpc
  // source group.
  void TestGRPCAddressSet(std::vector<CarnotInstance*> mergers) {
    for (auto merger_agent : mergers) {
      // Make sure GRPCAddress is now set.
      for (auto ir_node : merger_agent->plan()->FindNodesOfType(IRNodeType::kGRPCSourceGroup)) {
        SCOPED_TRACE(absl::Substitute("agent : $0, node :$1",
                                      merger_agent->carnot_info().query_broker_address(),
                                      ir_node->DebugString()));
        auto grpc_source_group = static_cast<GRPCSourceGroupIR*>(ir_node);
        EXPECT_TRUE(grpc_source_group->GRPCAddressSet());
        // We have yet to set the dependent sinks.
        EXPECT_EQ(grpc_source_group->dependent_sinks().size(), 0);
        EXPECT_EQ(grpc_source_group->grpc_address(), merger_agent->carnot_info().grpc_address());
      }
    }
  }

  // Test to make sure that GRPC sinks are all connected to GRPC sources in some plan in the graph.
  void TestGRPCBridgesWiring(std::vector<CarnotInstance*> data_stores,
                             std::vector<CarnotInstance*> mergers) {
    absl::flat_hash_set<GRPCSinkIR*> sinks_referenced_by_sources;
    absl::flat_hash_set<GRPCSinkIR*> sinks_found_in_plans;
    for (auto merger_agent : mergers) {
      // Load the sinks refernenced by the GRPC source group and makes sure they are in the agent
      // plan.
      for (auto ir_node : merger_agent->plan()->FindNodesOfType(IRNodeType::kGRPCSourceGroup)) {
        auto grpc_source_group = static_cast<GRPCSourceGroupIR*>(ir_node);
        for (auto sink : grpc_source_group->dependent_sinks()) {
          sinks_referenced_by_sources.insert(sink);
        }
      }
    }

    for (auto data_store_agent : data_stores) {
      // Now check that the sink exists for the grpc sources.
      for (auto ir_node : data_store_agent->plan()->FindNodesOfType(IRNodeType::kGRPCSink)) {
        SCOPED_TRACE(absl::Substitute("agent : $0, node :$1",
                                      data_store_agent->carnot_info().query_broker_address(),
                                      ir_node->DebugString()));
        auto grpc_sink = static_cast<GRPCSinkIR*>(ir_node);
        EXPECT_TRUE(sinks_referenced_by_sources.contains(grpc_sink));
        sinks_found_in_plans.insert(grpc_sink);
      }
    }
    EXPECT_THAT(sinks_found_in_plans, UnorderedElementsAreArray(sinks_referenced_by_sources));
  }

  // Test to make sure that GRPCSourceGroups become GRPCSources that are referenced by all sinks.
  void TestGRPCBridgesExpandedCorrectly(std::vector<CarnotInstance*> data_stores,
                                        std::vector<CarnotInstance*> mergers) {
    // Kelvin plan should no longer have grpc source groups.
    std::vector<int64_t> source_ids;
    std::vector<int64_t> destination_ids;

    for (auto merger_agent : mergers) {
      SCOPED_TRACE(
          absl::Substitute("agent : $0", merger_agent->carnot_info().query_broker_address()));
      EXPECT_EQ(merger_agent->plan()->FindNodesOfType(IRNodeType::kGRPCSourceGroup).size(), 0);
      // Node ids of GRPCSources should be the same as the destination ids.
      for (auto ir_node : merger_agent->plan()->FindNodesOfType(IRNodeType::kGRPCSource)) {
        auto grpc_source = static_cast<GRPCSourceIR*>(ir_node);
        source_ids.push_back(grpc_source->id());
      }
    }

    for (auto data_store_agent : data_stores) {
      // Now check that the sink exists for the grpc sources.
      for (auto ir_node : data_store_agent->plan()->FindNodesOfType(IRNodeType::kGRPCSink)) {
        SCOPED_TRACE(absl::Substitute("agent : $0, node :$1",
                                      data_store_agent->carnot_info().query_broker_address(),
                                      ir_node->DebugString()));
        auto sink = static_cast<GRPCSinkIR*>(ir_node);
        // Test GRPCSinks for expected GRPC destination address, as well as the proper physical id
        // being set, as well as being set to the correct value.
        EXPECT_TRUE(sink->DestinationAddressSet());
        destination_ids.emplace_back(sink->destination_id());
      }
    }

    EXPECT_THAT(source_ids, UnorderedElementsAreArray(destination_ids));
  }

  std::unique_ptr<CompilerState> compiler_state_;
  std::unique_ptr<compiler::RegistryInfo> info_;
  int64_t time_now = 1552607213931245000;
};

TEST_F(StitcherTest, one_pem_one_kelvin) {
  auto ps = LoadDistributedStatePb(kOneAgentOneKelvinDistributedState);
  auto physical_plan = MakeDistributedPlan(ps);

  EXPECT_THAT(physical_plan->dag().TopologicalSort(), ElementsAre(1, 0));
  CarnotInstance* kelvin = physical_plan->Get(0);

  std::string kelvin_qb_address = "kelvin";
  ASSERT_EQ(kelvin->carnot_info().query_broker_address(), kelvin_qb_address);

  CarnotInstance* pem = physical_plan->Get(1);
  {
    SCOPED_TRACE("one_pem_one_kelvin");
    TestBeforeSetSourceGroupGRPCAddress({kelvin, pem}, {kelvin});
  }

  // Execute the address rule.
  DistributedSetSourceGroupGRPCAddressRule rule;
  auto node_changed_or_s = rule.Execute(physical_plan.get());
  ASSERT_OK(node_changed_or_s);
  ASSERT_TRUE(node_changed_or_s.ConsumeValueOrDie());

  {
    SCOPED_TRACE("one_pem_one_kelvin");
    TestGRPCAddressSet({kelvin});
  }

  // Associate the edges of the graph.
  AssociateDistributedPlanEdgesRule distributed_edges_rule;
  node_changed_or_s = distributed_edges_rule.Execute(physical_plan.get());
  ASSERT_OK(node_changed_or_s);
  ASSERT_TRUE(node_changed_or_s.ConsumeValueOrDie());

  {
    SCOPED_TRACE("one_pem_one_kelvin");
    TestGRPCBridgesWiring({kelvin, pem}, {kelvin});
  }

  DistributedIRRule<GRPCSourceGroupConversionRule> distributed_grpc_source_conv_rule;
  node_changed_or_s = distributed_grpc_source_conv_rule.Execute(physical_plan.get());
  ASSERT_OK(node_changed_or_s);
  ASSERT_TRUE(node_changed_or_s.ConsumeValueOrDie());

  {
    SCOPED_TRACE("one_pem_one_kelvin");
    TestGRPCBridgesExpandedCorrectly({kelvin, pem}, {kelvin});
  }
}

TEST_F(StitcherTest, three_pems_one_kelvin) {
  auto ps = LoadDistributedStatePb(kThreeAgentsOneKelvinDistributedState);
  auto physical_plan = MakeDistributedPlan(ps);

  EXPECT_THAT(physical_plan->dag().TopologicalSort(), ElementsAre(3, 2, 1, 0));

  CarnotInstance* kelvin = physical_plan->Get(0);
  std::string kelvin_qb_address = "kelvin";
  ASSERT_EQ(kelvin->carnot_info().query_broker_address(), kelvin_qb_address);

  std::vector<CarnotInstance*> data_sources;
  for (int64_t agent_id = 1; agent_id <= 3; ++agent_id) {
    CarnotInstance* agent = physical_plan->Get(agent_id);
    // Quick check to make sure agents are valid.
    ASSERT_THAT(agent->carnot_info().query_broker_address(), HasSubstr("agent"));
    data_sources.push_back(agent);
  }
  // Kelvin can be a data source sometimes.
  data_sources.push_back(kelvin);
  {
    SCOPED_TRACE("three_pems_one_kelvin");
    TestBeforeSetSourceGroupGRPCAddress(data_sources, {kelvin});
  }

  // Execute the address rule.
  DistributedSetSourceGroupGRPCAddressRule rule;
  auto node_changed_or_s = rule.Execute(physical_plan.get());
  ASSERT_OK(node_changed_or_s);
  ASSERT_TRUE(node_changed_or_s.ConsumeValueOrDie());

  {
    SCOPED_TRACE("three_pems_one_kelvin");
    TestGRPCAddressSet({kelvin});
  }

  // Associate the edges of the graph.
  AssociateDistributedPlanEdgesRule distributed_edges_rule;
  node_changed_or_s = distributed_edges_rule.Execute(physical_plan.get());
  ASSERT_OK(node_changed_or_s);
  ASSERT_TRUE(node_changed_or_s.ConsumeValueOrDie());

  {
    SCOPED_TRACE("three_pems_one_kelvin");
    TestGRPCBridgesWiring(data_sources, {kelvin});
  }

  DistributedIRRule<GRPCSourceGroupConversionRule> distributed_grpc_source_conv_rule;
  node_changed_or_s = distributed_grpc_source_conv_rule.Execute(physical_plan.get());
  ASSERT_OK(node_changed_or_s);
  ASSERT_TRUE(node_changed_or_s.ConsumeValueOrDie());

  {
    SCOPED_TRACE("three_pems_one_kelvin");
    TestGRPCBridgesExpandedCorrectly(data_sources, {kelvin});
  }
}

}  // namespace distributed
}  // namespace compiler
}  // namespace carnot
}  // namespace pl
