/*******************************************************************************
 * This file is part of KaHyPar.
 *
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

#include "static_graph_factory.h"

#include <tbb/parallel_for.h>
#include <tbb/parallel_invoke.h>

#include "mt-kahypar/parallel/parallel_prefix_sum.h"
#include "mt-kahypar/utils/timer.h"

namespace mt_kahypar::ds {

  // TODO: more efficient construction (not using a vector of vectors)?
  StaticGraph StaticGraphFactory::construct(
          const TaskGroupID task_group_id,
          const HypernodeID num_nodes,
          const HyperedgeID num_edges,
          const HyperedgeVector& edge_vector,
          const HyperedgeWeight* edge_weight,
          const HypernodeWeight* node_weight,
          const bool stable_construction_of_degree) {
    StaticGraph graph;
    graph._num_nodes = num_nodes;
    graph._num_edges = num_edges;
    graph._nodes.resize(num_nodes + 1);
    graph._edges.resize(2 * num_edges);

    ASSERT(edge_vector.size() == num_edges);

    // Compute degree for each vertex
    utils::Timer::instance().start_timer("compute_ds_sizes", "Precompute DS Size", true);
    ThreadLocalCounter local_degree_per_vertex(num_nodes, 0);
    tbb::parallel_for(ID(0), num_edges, [&](const size_t pos) {
      Counter& num_degree_per_vertex = local_degree_per_vertex.local();
      if (edge_vector[pos].size() != 2) {
        ERROR("Using graph data structure; but the input hypergraph is not a graph.");
      }
      for ( const HypernodeID& pin : edge_vector[pos] ) {
        ASSERT(pin < num_nodes, V(pin) << V(num_nodes));
        ++num_degree_per_vertex[pin];
      }
    });

    // We sum up the degree per vertex only thread local. To obtain the
    // global degree, we iterate over each thread local counter and sum it up.
    Counter num_degree_per_vertex(num_nodes, 0);
    for ( Counter& c : local_degree_per_vertex ) {
      tbb::parallel_for(ID(0), num_nodes, [&](const size_t pos) {
        num_degree_per_vertex[pos] += c[pos];
      });
    }
    utils::Timer::instance().stop_timer("compute_ds_sizes");

    // Compute prefix sum over the degrees. The prefix sum is used than
    // as start position for each node in the edge array.
    utils::Timer::instance().start_timer("compute_prefix_sums", "Compute Prefix Sums", true);
    parallel::TBBPrefixSum<size_t> degree_prefix_sum(num_degree_per_vertex);
    tbb::parallel_scan(tbb::blocked_range<size_t>( 0UL, UI64(num_nodes)), degree_prefix_sum);
    utils::Timer::instance().stop_timer("compute_prefix_sums");

    utils::Timer::instance().start_timer("setup_hypergraph", "Setup hypergraph", true);
    ASSERT(degree_prefix_sum.total_sum() == 2 * num_edges);

    AtomicCounter incident_edges_position(num_nodes,
                                         parallel::IntegralAtomicWrapper<size_t>(0));

    auto setup_edges = [&] {
      tbb::parallel_for(ID(0), num_edges, [&](const size_t pos) {
        const HypernodeID pin0 = edge_vector[pos][0];
        const HyperedgeID incident_edges_pos0 = degree_prefix_sum[pin0] + incident_edges_position[pin0]++;
        ASSERT(incident_edges_pos0 < graph._edges.size());
        StaticGraph::Edge& edge0 = graph._edges[incident_edges_pos0];
        const HypernodeID pin1 = edge_vector[pos][1];
        const HyperedgeID incident_edges_pos1 = degree_prefix_sum[pin1] + incident_edges_position[pin1]++;
        ASSERT(incident_edges_pos1 < graph._edges.size());
        StaticGraph::Edge& edge1 = graph._edges[incident_edges_pos1];

        edge0.setTarget(pin1);
        edge0.setBackwardsEdge(incident_edges_pos1);
        edge1.setTarget(pin0);
        edge1.setBackwardsEdge(incident_edges_pos0);

        if ( edge_weight ) {
          edge0.setWeight(edge_weight[pos]);
          edge1.setWeight(edge_weight[pos]);
        }
      });
    };

    auto setup_nodes = [&] {
      tbb::parallel_for(ID(0), num_nodes, [&](const size_t pos) {
        StaticGraph::Node& node = graph._nodes[pos];
        node.enable();
        node.setFirstEntry(degree_prefix_sum[pos]);
        if ( node_weight ) {
          node.setWeight(node_weight[pos]);
        }
      });
    };

    auto init_communities = [&] {
      graph._community_ids.resize(num_nodes, 0);
    };

    tbb::parallel_invoke(setup_edges, setup_nodes, init_communities);

    // Add Sentinel
    graph._nodes.back() = StaticGraph::Node(graph._edges.size());

    if (stable_construction_of_degree) {
      // sort incident edges of each node, so their ordering is independent of scheduling (and the same as a typical sequential implementation)
      tbb::parallel_for(ID(0), num_nodes, [&](HypernodeID u) {
        auto b = graph._edges.begin() + graph.node(u).firstEntry();
        auto e = graph._edges.begin() + graph.node(u + 1).firstEntry();
        std::sort(b, e, [](StaticGraph::Edge& a, StaticGraph::Edge& b) {
          return a.target() < b.target();
        });
      });
    }

    graph.computeAndSetTotalNodeWeight(task_group_id);

    utils::Timer::instance().stop_timer("setup_hypergraph");
    return graph;
  }

}