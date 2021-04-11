
#include "natural_cut_detection.h"
#include "hyper_flow_instance.h"
#include "external_tools/kahypar/kahypar/datastructure/fast_reset_flag_array.h"
#include "external_tools/kahypar/external_tools/WHFC/datastructure/queue.h"
#include "mt-kahypar/utils/timer.h"
#include "stack"
#include "map"

namespace mt_kahypar::community_detection {

  using Queue = LayeredQueue<HypernodeID>;
  static constexpr HypernodeID invalid_node = std::numeric_limits<HypernodeID>::max();

  void depthFirstSearch(HypernodeID start, HypernodeID d, Hypergraph& hypergraph,
                        kahypar::ds::FastResetFlagArray<>& visitedHypernode, std::vector<HypernodeID>& depth,
                        std::vector<HypernodeID>& lowPoint, std::vector<HypernodeID>& parent,
                        parallel::scalable_vector<HypernodeID>& components) {
    std::stack<HypernodeID> s;
    s.push(start);

    kahypar::ds::FastResetFlagArray<> pushedChildren(hypergraph.initialNumNodes());
    HypernodeID previous;
    while (!s.empty()) {
      HypernodeID v = s.top();
      if (!pushedChildren[v]) {
        pushedChildren.set(v);
        if (v != start) {
          depth[v] = depth[parent[v]] + 1;
        } else {
          depth[v] = 0;
        }
        lowPoint[v] = depth[v];
        for (const HyperedgeID e : hypergraph.incidentEdges(v)) {
          for (const HypernodeID u : hypergraph.pins(e)) {
            if (!visitedHypernode[u]) {
              visitedHypernode.set(u);
              parent[u] = v;
              s.push(u);
            }
          }
        }
      } else {
        s.pop();
        int children = 0;
        bool isArticulationPoint = false;
        for (const HyperedgeID e : hypergraph.incidentEdges(v)) {
          for (const HypernodeID u : hypergraph.pins(e)) {
            if (parent[u] = v) {
              children++;
              if (lowPoint[u] >= depth[v]) {
                isArticulationPoint = true;
              }
              lowPoint[v] = std::min(lowPoint[v], lowPoint[u]);
            } else if (parent[v] != u) {
              lowPoint[v] = std::min(lowPoint[v], lowPoint[u]);
            }
          }
        }
        if (!(((parent[v] != invalid_node) && isArticulationPoint) || ((parent[v] = invalid_node) && (children > 1)))) {
          if (lowPoint[previous] == lowPoint[v]) {
            components[v] = previous;
          }
        }
        previous = v;
      }
    }
  }

  ds::Clustering run_natural_cut_detection(Hypergraph& originalHypergraph, const Context& context, bool disable_randomization) {

    parallel::scalable_vector<HypernodeID> components(originalHypergraph.initialNumNodes());
    kahypar::ds::FastResetFlagArray<> visitedHypernode(originalHypergraph.initialNumNodes());
    std::vector<HypernodeID> depth(originalHypergraph.initialNumNodes(), invalid_node);
    std::vector<HypernodeID> lowPoint(originalHypergraph.initialNumNodes(), invalid_node);
    std::vector<HypernodeID> parent(originalHypergraph.initialNumNodes(), invalid_node);
    for (HypernodeID id = 0; id < originalHypergraph.initialNumNodes(); id++) {
      if (!visitedHypernode[id]) {
        depthFirstSearch(id, 0, originalHypergraph, visitedHypernode, depth, lowPoint, parent, components);
      }
    }

    std::map<HypernodeID,HypernodeID> m;
    for (HypernodeID id = 0; id < originalHypergraph.initialNumNodes(); id++) {
      if (m.find(components[id]) == m.end()) {
        m.insert(std::make_pair(components[id],0));
      }
      m[components[id]]++;
    }

    for (HypernodeID id = 0; id < originalHypergraph.initialNumNodes(); id++) {
      if (m[components[id]] >= (originalHypergraph.initialNumNodes()/(2*10))) {
        components[id] = id;
      }
    }

    Hypergraph hypergraph = originalHypergraph.contract(components, TBBNumaArena::GLOBAL_TASK_GROUP);



    kahypar::ds::FastResetFlagArray<> hypernodeProcessed(hypergraph.initialNumNodes());
    kahypar::ds::FastResetFlagArray<> visitedHyperedge(hypergraph.initialNumEdges());
    ds::Clustering communities(hypergraph.initialNumNodes());
    parallel::scalable_vector<HypernodeID> vertices;
    vertices.resize(hypergraph.initialNumNodes());
    tbb::parallel_for(ID(0), hypergraph.initialNumNodes(), [&](const HypernodeID hn) {
      ASSERT(hn < vertices.size());
      vertices[hn] = hn;
    });
    //TODO make deterministic
    if ( !disable_randomization ) {
      utils::Randomize::instance().parallelShuffleVector(
        vertices, 0UL, vertices.size());
    }

    /*tbb::parallel_for(ID(0), hypergraph.initialNumNodes(), [&](const HypernodeID id) {
      bool foundEdge = false;
      for (HyperedgeID e : hypergraph.incidentEdges(id)) {
        if (hypergraph.edgeSize(e) < 1000) {
          foundEdge = true;
          break;
        }
      }
      if (!foundEdge) {
        hypernodeProcessed.set(id);
      }
    });*/

    tbb::atomic<size_t> progress = 0;
    tbb::atomic<size_t> num = 0;
    // Do flow calculations from every Hypernode
    //tbb::enumerable_thread_specific <std::vector<HyperedgeID>> cut_edges_local;
    //for (HypernodeID id = 0; id < hypergraph.initialNumNodes(); id++) {
    tbb::parallel_for(ID(0), hypergraph.initialNumNodes(), [&](const HypernodeID id) {    // REVIEW no control how often a vertex appears in a core. potentially quadratic running  time in allocations
      ASSERT(id < vertices.size());
      HypernodeID v = vertices[id];
      if (!hypernodeProcessed[v]) {
        std::cout << "Starting Flow Iteration" << std::endl;
        auto t = tbb::tick_count::now();
        HyperFlowInstance hfib(hypergraph, context, v, hypernodeProcessed);
        auto t2 = tbb::tick_count::now();
        std::cout << "Starting Flow Computation" << std::endl;
        std::vector<HyperedgeID> cut = hfib.computeCut();
        auto t3 = tbb::tick_count::now();
        std::cout << "Found cut with " << cut.size() << " edges" << std::endl;
        std::cout << "Time building Flowgraph " << (t2-t).seconds() << std::endl;
        std::cout << "Time calculating Cut " << (t3-t2).seconds() << std::endl;
        //cut_edges_local.local().insert(cut_edges_local.local().end(), cut.begin(), cut.end());
        for (HyperedgeID he : cut) {
          visitedHyperedge.set(he);
        }
        size_t temp = 0;
        for (HypernodeID hn : hfib._core) {
          if (!hypernodeProcessed[hn]) {
            //TODO make atomic
            temp++;
          }
          //TODO do this in processing
          hypernodeProcessed.set(hn);
        }
        progress += temp;
        auto t4 = tbb::tick_count::now();
        std::cout << "Time marking cut and core " << (t4-t3).seconds() << std::endl;
        std::cout << "Progress: " << progress << "/" << hypergraph.initialNumNodes() << std::endl;
        num++;
      }
    //}
    });

    std::cout << "Num Flow calcs: " << num << std::endl;
    auto t5 = tbb::tick_count::now();

    // Compute Connected Components
    hypernodeProcessed.reset();
    int current_community = 0;
    int one = 0;
    for (HypernodeID v = 0; v < hypergraph.initialNumNodes(); v++) {
      int size = 0;
      if (!hypernodeProcessed[v]) {
        Queue queue(hypergraph.initialNumNodes());
        queue.push(v);
        hypernodeProcessed.set(v);
        communities[v] = current_community;
        size++;
        while (!queue.empty()) {
          HypernodeID u = queue.pop();
          for (const HyperedgeID e : hypergraph.incidentEdges(u)) {
            if (!visitedHyperedge[e]) {
              visitedHyperedge.set(e);
              for (const HypernodeID u : hypergraph.pins(e)) {
                if (!hypernodeProcessed[u]) {
                  queue.push(u);
                  hypernodeProcessed.set(u);
                  communities[u] = current_community;
                  size++;
                }
              }
            }
          }
        }
        current_community++;
        if (size == 1) {
          one++;
        }
      }
    }


    ds::Clustering uncontractedCommunities(originalHypergraph.initialNumNodes());
    tbb::parallel_for(ID(0), originalHypergraph.initialNumNodes(), [&](const HypernodeID hn) {
      uncontractedCommunities[hn] = communities[components[hn]];
    });

    std::cout << "Communities found: " << current_community << std::endl;
    std::cout << "Communities Size 1: " << one << std::endl;
    auto t6 = tbb::tick_count::now();
    std::cout << "Time cc Computation " << (t6-t5).seconds() << std::endl;
    return uncontractedCommunities;
  }


}