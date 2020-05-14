/*******************************************************************************
 * This file is part of KaHyPar.
 *
 * Copyright (C) 2019 Tobias Heuer <tobias.heuer@kit.edu>
 *
 * KaHyPar is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * KaHyPar is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with KaHyPar.  If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/

#include "gmock/gmock.h"

#include "tests/datastructures/hypergraph_fixtures.h"
#include "mt-kahypar/definitions.h"
#include "mt-kahypar/io/hypergraph_io.h"
#include "mt-kahypar/partition/context.h"
#include "mt-kahypar/partition/initial_partitioning/flat/bfs_initial_partitioner.h"
#include "mt-kahypar/partition/refinement/flow/flow_refiner.h"
#include "mt-kahypar/partition/refinement/policies/gain_policy.h"
#include "mt-kahypar/utils/randomize.h"

using ::testing::Test;

namespace mt_kahypar {
template <PartitionID k, typename FlowTypeTraits>
struct TestConfig { };

template <PartitionID k>
struct TestConfig<k, FlowOptTypeTraits> {
  using Refiner = FlowRefiner<FlowOptTypeTraits>;
  static constexpr PartitionID K = k;
  static constexpr FlowAlgorithm FLOW_ALGO = FlowAlgorithm::flow_opt;
};

template <PartitionID k>
struct TestConfig<k, FlowMatchingTypeTraits> {
  using Refiner = FlowRefiner<FlowOptTypeTraits>;
  static constexpr PartitionID K = k;
  static constexpr FlowAlgorithm FLOW_ALGO = FlowAlgorithm::flow_match;
};

template <typename Config>
class AFlowRefiner : public Test {
  using Refiner = typename Config::Refiner;

  static size_t num_threads;

 public:
  AFlowRefiner() :
    hypergraph(),
    partitioned_hypergraph(),
    context(),
    refiner(nullptr),
    metrics() {
    context.partition.graph_filename = "test_instances/ibm01.hgr";
    context.partition.graph_community_filename = "test_instances/ibm01.hgr.community";
    context.partition.mode = kahypar::Mode::direct_kway;
    context.partition.objective = kahypar::Objective::km1;
    context.partition.epsilon = 0.25;
    context.partition.k = Config::K;
    context.partition.verbose_output = false;

    // Shared Memory
    context.shared_memory.num_threads = num_threads;

    // Initial Partitioning
    context.initial_partitioning.mode = InitialPartitioningMode::recursive;
    context.initial_partitioning.runs = 1;

    //Flow
    context.refinement.flow.algorithm = Config::FLOW_ALGO;

    // Label Propagation
    context.refinement.label_propagation.algorithm = LabelPropagationAlgorithm::do_nothing;
    context.initial_partitioning.refinement.label_propagation.algorithm = LabelPropagationAlgorithm::do_nothing;

    // Read hypergraph
    hypergraph = io::readHypergraphFile(
      "../test_instances/unweighted_ibm01.hgr", TBBNumaArena::GLOBAL_TASK_GROUP);
    partitioned_hypergraph = PartitionedHypergraph<>(
      context.partition.k, TBBNumaArena::GLOBAL_TASK_GROUP, hypergraph);
    context.setupPartWeights(hypergraph.totalWeight());
    initialPartition();

    refiner = std::make_unique<Refiner>(partitioned_hypergraph, context, TBBNumaArena::GLOBAL_TASK_GROUP);
    refiner->initialize(partitioned_hypergraph);
  }

  static void SetUpTestSuite() {
    TBBNumaArena::instance(num_threads);
  }

  void initialPartition() {
    Context ip_context(context);
    ip_context.refinement.label_propagation.algorithm = LabelPropagationAlgorithm::do_nothing;
    InitialPartitioningDataContainer ip_data(partitioned_hypergraph, ip_context, TBBNumaArena::GLOBAL_TASK_GROUP);
    BFSInitialPartitioner& initial_partitioner = *new(tbb::task::allocate_root())
      BFSInitialPartitioner(InitialPartitioningAlgorithm::bfs, ip_data, ip_context);
    tbb::task::spawn_root_and_wait(initial_partitioner);
    ip_data.apply();
    metrics.km1 = metrics::km1(partitioned_hypergraph);
    metrics.cut = metrics::hyperedgeCut(partitioned_hypergraph);
    metrics.imbalance = metrics::imbalance(partitioned_hypergraph, context);
  }

  Hypergraph hypergraph;
  PartitionedHypergraph<> partitioned_hypergraph;
  Context context;
  std::unique_ptr<Refiner> refiner;
  kahypar::Metrics metrics;
};

template <typename Config>
size_t AFlowRefiner<Config>::num_threads = HardwareTopology::instance().num_cpus();

static constexpr double EPS = 0.05;

typedef ::testing::Types<TestConfig<2, FlowOptTypeTraits>,
                         TestConfig<4, FlowOptTypeTraits>,
                         TestConfig<8, FlowOptTypeTraits>,
                         TestConfig<2, FlowMatchingTypeTraits>,
                         TestConfig<4, FlowMatchingTypeTraits>,
                         TestConfig<8, FlowMatchingTypeTraits> > TestConfigs;

TYPED_TEST_CASE(AFlowRefiner, TestConfigs);

TYPED_TEST(AFlowRefiner, UpdatesImbalanceCorrectly) {
  this->refiner->refine(this->partitioned_hypergraph, this->metrics);
  ASSERT_DOUBLE_EQ(metrics::imbalance(this->partitioned_hypergraph, this->context), this->metrics.imbalance);
}

TYPED_TEST(AFlowRefiner, DoesNotViolateBalanceConstraint) {
  this->refiner->refine(this->partitioned_hypergraph, this->metrics);
  ASSERT_LE(this->metrics.imbalance, this->context.partition.epsilon + EPS);
}

TYPED_TEST(AFlowRefiner, UpdatesMetricsCorrectly) {
  this->refiner->refine(this->partitioned_hypergraph, this->metrics);
  ASSERT_EQ(metrics::objective(this->partitioned_hypergraph, this->context.partition.objective),
            this->metrics.getMetric(kahypar::Mode::direct_kway, this->context.partition.objective));
}

TYPED_TEST(AFlowRefiner, DoesNotWorsenSolutionQuality) {
  HyperedgeWeight objective_before = metrics::objective(this->partitioned_hypergraph, this->context.partition.objective);
  this->refiner->refine(this->partitioned_hypergraph, this->metrics);
  ASSERT_LE(this->metrics.getMetric(kahypar::Mode::direct_kway, this->context.partition.objective), objective_before);
}
}  // namespace mt_kahypar