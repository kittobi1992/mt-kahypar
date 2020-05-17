/*******************************************************************************
 * This file is part of KaHyPar.
 *
 * Copyright (C) 2020 Lars Gottesbüren <lars.gottesbueren@kit.edu>
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

#include <functional>
#include <random>

#include "gmock/gmock.h"

#include "mt-kahypar/macros.h"

#include <mt-kahypar/partition/refinement/fm/localized_kway_fm_core.h>
#include <mt-kahypar/io/hypergraph_io.h>
#include <mt-kahypar/partition/metrics.h>

namespace mt_kahypar {

using ::testing::Test;
class FMCoreTest : public Test {
public:
  FMCoreTest() {
    // hypergraph construction in parallel does some reordering of incident edges depending on scheduling --> results not reproducible
    // --> sort incident edges --> constructed as in sequential
    hg = io::readHypergraphFile("../test_instances/ibm01.hgr", 0, true /* enable stable construction */);
    phg = PartitionedHypergraph(k, hg);
    HypernodeID nodes_per_part = hg.initialNumNodes() / k;
    for (PartitionID i = 0; i < k; ++i) {
      for (HypernodeID u = i * nodes_per_part; u < (i+1) * nodes_per_part; ++u) {
        phg.setOnlyNodePart(u, i);
      }
    }
    phg.initializePartition(TBBNumaArena::GLOBAL_TASK_GROUP);
    phg.initializeGainInformation();

    context.partition.k = k;
    context.partition.epsilon = 0.03;
    context.setupPartWeights(hg.totalWeight());

    sharedData = FMSharedData(hg.initialNumNodes(), context);
  }

  Hypergraph hg;
  PartitionID k = 8;
  PartitionedHypergraph phg;
  FMSharedData sharedData;
  Context context;
};

void printGains(PartitionedHypergraph& phg, PartitionID k) {
  for (HypernodeID u = 0; u < phg.initialNumNodes(); ++u) {
    std::cout << "u=" << u << "p=" << phg.partID(u) << ". gains=";
    for (PartitionID i = 0; i < k; ++i) {
      if (i != phg.partID(u)) {
        std::cout << phg.km1Gain(u, phg.partID(u), i) << " ";
      }
    }
    std::cout << std::endl;
  }
}

void printHypergraph(PartitionedHypergraph& phg) {
  std::cout << "Vertices\n";
  for (HypernodeID u = 0; u < phg.initialNumNodes(); ++u) {
    std::cout << "u=" << u << " -> ";
    for (HyperedgeID he : phg.incidentEdges(u)) {
      std::cout << he << " ";
    }
    std::cout << "\n";
  }
  std::cout << "Hyperedges\n";
  for (HyperedgeID e = 0; e < phg.initialNumEdges(); ++e) {
    std::cout << "e=" << e << " -> ";
    for (HypernodeID v : phg.pins(e)) {
      std::cout << v << " ";
    }
    std::cout << "\n";
  }
  std::cout << std::flush;
}

TEST_F(FMCoreTest, PQInsertAndUpdate) {
  LocalizedKWayFM fm(context, hg.initialNumNodes(), sharedData.vertexPQHandles.data());
  HyperedgeWeight initial_km1 = metrics::km1(phg, false);
  HypernodeID initialNode = 23;
  fm.findMoves(phg, sharedData, initialNode);

  HyperedgeWeight accumulated_gain = 0;
  for (MoveID move_id = 0; move_id < sharedData.moveTracker.numPerformedMoves(); ++move_id) {
    Move& m = sharedData.moveTracker.moveOrder[move_id];
    if (m.gain != invalidGain) {  // skip reverted moves
      accumulated_gain += m.gain;
    }
  }

  HyperedgeWeight km1_after_fm = metrics::km1(phg, false);
  ASSERT_EQ(km1_after_fm, initial_km1 - accumulated_gain);
}

}