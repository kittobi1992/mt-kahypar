/*******************************************************************************
 * This file is part of KaHyPar.
 *
 * Copyright (C) 2019 Tobias Heuer <tobias.heuer@kit.edu>
 * Copyright (C) 2021 Nikolai Maas <nikolai.maas@student.kit.edu>
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
#include "mt-kahypar/datastructures/static_graph.h"
#include "mt-kahypar/datastructures/static_graph_factory.h"

using ::testing::Test;

namespace mt_kahypar {
namespace ds {

using AStaticGraph = HypergraphFixture<StaticGraph, StaticGraphFactory, true>;

TEST_F(AStaticGraph, HasCorrectStats) {
  ASSERT_EQ(7,  hypergraph.initialNumNodes());
  ASSERT_EQ(6,  hypergraph.initialNumEdges());
  ASSERT_EQ(12, hypergraph.initialNumPins());
  ASSERT_EQ(12, hypergraph.initialTotalVertexDegree());
  ASSERT_EQ(7,  hypergraph.totalWeight());
  ASSERT_EQ(2,  hypergraph.maxEdgeSize());
}

TEST_F(AStaticGraph, HasCorrectInitialNodeIterator) {
  HypernodeID expected_hn = 0;
  for ( const HypernodeID& hn : hypergraph.nodes() ) {
    ASSERT_EQ(expected_hn++, hn);
  }
  ASSERT_EQ(7, expected_hn);
}

TEST_F(AStaticGraph, HasCorrectNodeIteratorIfVerticesAreDisabled) {
  hypergraph.removeDegreeZeroHypernode(0);
  const std::vector<HypernodeID> expected_iter =
    { 1, 2, 3, 4, 5, 6 };
  HypernodeID pos = 0;
  for ( const HypernodeID& hn : hypergraph.nodes() ) {
    ASSERT_EQ(expected_iter[pos++], hn);
  }
  ASSERT_EQ(expected_iter.size(), pos);
}

TEST_F(AStaticGraph, HasCorrectInitialEdgeIterator) {
  // Note that each edge appears twice in the adjacency array
  const std::vector<HypernodeID> expected_iter =
    { 0, 1, 3, 6, 7, 9 };
  HyperedgeID pos = 0;
  for ( const HyperedgeID& he : hypergraph.edges() ) {
    ASSERT_EQ(expected_iter[pos++], he);
  }
  ASSERT_EQ(expected_iter.size(), pos);
}

TEST_F(AStaticGraph, IteratesParallelOverAllNodes) {
  std::vector<uint8_t> visited(7, false);
  hypergraph.doParallelForAllNodes([&](const HypernodeID hn) {
      visited[hn] = true;
    });

  for ( size_t i = 0; i < visited.size(); ++i ) {
    ASSERT_TRUE(visited[i]) << i;
  }
}

TEST_F(AStaticGraph, VerifiesIncidentNets1) {
  verifyIncidentNets(0, { });
}

TEST_F(AStaticGraph, VerifiesIncidentNets2) {
  verifyIncidentNets(1, { 0, 1 });
}

TEST_F(AStaticGraph, VerifiesIncidentNets3) {
  verifyIncidentNets(2, { 2, 3 });
}

TEST_F(AStaticGraph, VerifiesIncidentNets4) {
  verifyIncidentNets(6, { 10, 11 });
}

TEST_F(AStaticGraph, VerifiesPinsOfHyperedges) {
  verifyPins({ 0, 1, 3, 6, 7, 9 },
    { {1, 2}, {1, 4}, {2, 3}, {4, 5}, {4, 6}, {5, 6} });
}

TEST_F(AStaticGraph, VerifiesVertexWeights) {
  for ( const HypernodeID& hn : hypergraph.nodes() ) {
    ASSERT_EQ(1, hypergraph.nodeWeight(hn));
  }
}

TEST_F(AStaticGraph, ModifiesNodeWeight) {
  hypergraph.setNodeWeight(0, 2);
  hypergraph.setNodeWeight(6, 2);
  ASSERT_EQ(2, hypergraph.nodeWeight(0));
  ASSERT_EQ(2, hypergraph.nodeWeight(6));
    hypergraph.computeAndSetTotalNodeWeight(TBBNumaArena::GLOBAL_TASK_GROUP);
  ASSERT_EQ(9, hypergraph.totalWeight());
}

TEST_F(AStaticGraph, VerifiesVertexDegrees) {
  ASSERT_EQ(0, hypergraph.nodeDegree(0));
  ASSERT_EQ(2, hypergraph.nodeDegree(1));
  ASSERT_EQ(2, hypergraph.nodeDegree(2));
  ASSERT_EQ(1, hypergraph.nodeDegree(3));
  ASSERT_EQ(3, hypergraph.nodeDegree(4));
  ASSERT_EQ(2, hypergraph.nodeDegree(5));
  ASSERT_EQ(2, hypergraph.nodeDegree(6));
}

TEST_F(AStaticGraph, RemovesVertices) {
  hypergraph.removeDegreeZeroHypernode(0);
  ASSERT_EQ(1, hypergraph.numRemovedHypernodes());
}

TEST_F(AStaticGraph, VerifiesEdgeWeights) {
  for ( const HyperedgeID& he : hypergraph.edges() ) {
    ASSERT_EQ(1, hypergraph.edgeWeight(he));
  }
}

TEST_F(AStaticGraph, ModifiesEdgeWeight) {
  hypergraph.setEdgeWeight(0, 2);
  hypergraph.setEdgeWeight(2, 2);
  ASSERT_EQ(2, hypergraph.edgeWeight(0));
  ASSERT_EQ(2, hypergraph.edgeWeight(2));
}

TEST_F(AStaticGraph, VerifiesEdgeSizes) {
  for ( const HyperedgeID& he : hypergraph.edges() ) {
    ASSERT_EQ(2, hypergraph.edgeSize(he));
  }
}

TEST_F(AStaticGraph, SetsCommunityIDsForEachVertex) {
  hypergraph.setCommunityID(0, 1);
  hypergraph.setCommunityID(1, 1);
  hypergraph.setCommunityID(2, 1);
  hypergraph.setCommunityID(3, 2);
  hypergraph.setCommunityID(4, 2);
  hypergraph.setCommunityID(5, 3);
  hypergraph.setCommunityID(6, 3);

  ASSERT_EQ(1, hypergraph.communityID(0));
  ASSERT_EQ(1, hypergraph.communityID(1));
  ASSERT_EQ(1, hypergraph.communityID(2));
  ASSERT_EQ(2, hypergraph.communityID(3));
  ASSERT_EQ(2, hypergraph.communityID(4));
  ASSERT_EQ(3, hypergraph.communityID(5));
  ASSERT_EQ(3, hypergraph.communityID(6));
}

TEST_F(AStaticGraph, ComparesStatsIfCopiedParallel) {
  StaticGraph copy_hg = hypergraph.copy(TBBNumaArena::GLOBAL_TASK_GROUP);
  ASSERT_EQ(hypergraph.initialNumNodes(), copy_hg.initialNumNodes());
  ASSERT_EQ(hypergraph.initialNumEdges(), copy_hg.initialNumEdges());
  ASSERT_EQ(hypergraph.initialNumPins(), copy_hg.initialNumPins());
  ASSERT_EQ(hypergraph.initialTotalVertexDegree(), copy_hg.initialTotalVertexDegree());
  ASSERT_EQ(hypergraph.totalWeight(), copy_hg.totalWeight());
  ASSERT_EQ(hypergraph.maxEdgeSize(), copy_hg.maxEdgeSize());
}

TEST_F(AStaticGraph, ComparesStatsIfCopiedSequential) {
  StaticGraph copy_hg = hypergraph.copy();
  ASSERT_EQ(hypergraph.initialNumNodes(), copy_hg.initialNumNodes());
  ASSERT_EQ(hypergraph.initialNumEdges(), copy_hg.initialNumEdges());
  ASSERT_EQ(hypergraph.initialNumPins(), copy_hg.initialNumPins());
  ASSERT_EQ(hypergraph.initialTotalVertexDegree(), copy_hg.initialTotalVertexDegree());
  ASSERT_EQ(hypergraph.totalWeight(), copy_hg.totalWeight());
  ASSERT_EQ(hypergraph.maxEdgeSize(), copy_hg.maxEdgeSize());
}

TEST_F(AStaticGraph, ComparesIncidentNetsIfCopiedParallel) {
  StaticGraph copy_hg = hypergraph.copy(TBBNumaArena::GLOBAL_TASK_GROUP);
  verifyIncidentNets(copy_hg, 0, { });
  verifyIncidentNets(copy_hg, 1, { 0, 1 });
  verifyIncidentNets(copy_hg, 2, { 2, 3 });
  verifyIncidentNets(copy_hg, 3, { 4 });
  verifyIncidentNets(copy_hg, 4, { 5, 6, 7 });
  verifyIncidentNets(copy_hg, 5, { 8, 9 });
  verifyIncidentNets(copy_hg, 6, { 10, 11 });
}

TEST_F(AStaticGraph, ComparesIncidentNetsIfCopiedSequential) {
  StaticGraph copy_hg = hypergraph.copy();
  verifyIncidentNets(copy_hg, 0, { });
  verifyIncidentNets(copy_hg, 1, { 0, 1 });
  verifyIncidentNets(copy_hg, 2, { 2, 3 });
  verifyIncidentNets(copy_hg, 3, { 4 });
  verifyIncidentNets(copy_hg, 4, { 5, 6, 7 });
  verifyIncidentNets(copy_hg, 5, { 8, 9 });
  verifyIncidentNets(copy_hg, 6, { 10, 11 });
}

TEST_F(AStaticGraph, ComparesPinsOfHyperedgesIfCopiedParallel) {
  StaticGraph copy_hg = hypergraph.copy(TBBNumaArena::GLOBAL_TASK_GROUP);
  verifyPins(copy_hg, { 0, 1, 3, 6, 7, 9 },
    { {1, 2}, {1, 4}, {2, 3}, {4, 5}, {4, 6}, {5, 6} });
}

TEST_F(AStaticGraph, ComparesPinsOfHyperedgesIfCopiedSequential) {
  StaticGraph copy_hg = hypergraph.copy();
  verifyPins(copy_hg, { 0, 1, 3, 6, 7, 9 },
    { {1, 2}, {1, 4}, {2, 3}, {4, 5}, {4, 6}, {5, 6} });
}

TEST_F(AStaticGraph, ComparesCommunityIdsIfCopiedParallel) {
  assignCommunityIds();
  StaticGraph copy_hg = hypergraph.copy(TBBNumaArena::GLOBAL_TASK_GROUP);
  ASSERT_EQ(hypergraph.communityID(0), copy_hg.communityID(0));
  ASSERT_EQ(hypergraph.communityID(1), copy_hg.communityID(1));
  ASSERT_EQ(hypergraph.communityID(2), copy_hg.communityID(2));
  ASSERT_EQ(hypergraph.communityID(3), copy_hg.communityID(3));
  ASSERT_EQ(hypergraph.communityID(4), copy_hg.communityID(4));
  ASSERT_EQ(hypergraph.communityID(5), copy_hg.communityID(5));
  ASSERT_EQ(hypergraph.communityID(6), copy_hg.communityID(6));
}

TEST_F(AStaticGraph, ComparesCommunityIdsIfCopiedSequential) {
  assignCommunityIds();
  StaticGraph copy_hg = hypergraph.copy();
  ASSERT_EQ(hypergraph.communityID(0), copy_hg.communityID(0));
  ASSERT_EQ(hypergraph.communityID(1), copy_hg.communityID(1));
  ASSERT_EQ(hypergraph.communityID(2), copy_hg.communityID(2));
  ASSERT_EQ(hypergraph.communityID(3), copy_hg.communityID(3));
  ASSERT_EQ(hypergraph.communityID(4), copy_hg.communityID(4));
  ASSERT_EQ(hypergraph.communityID(5), copy_hg.communityID(5));
  ASSERT_EQ(hypergraph.communityID(6), copy_hg.communityID(6));
}

}
} // namespace mt_kahypar