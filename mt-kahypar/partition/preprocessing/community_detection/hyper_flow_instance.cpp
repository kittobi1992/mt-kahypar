
#include "hyper_flow_instance.h"
#include <external_tools/WHFC/algorithm/cutter_state.h>
#include <external_tools/WHFC/algorithm/dinic.h>
#include <external_tools/WHFC/algorithm/grow_assimilated.h>
#include "external_tools/kahypar/external_tools/WHFC/datastructure/flow_hypergraph_builder.h"
#include "external_tools/kahypar/external_tools/WHFC/datastructure/node_border.h"
#include "external_tools/kahypar/external_tools/WHFC/datastructure/flow_hypergraph.h"
#include "external_tools/kahypar/external_tools/WHFC/datastructure/queue.h"


namespace mt_kahypar::community_detection {

  using Queue = LayeredQueue<HypernodeID>;

  std::vector<HyperedgeID> HyperFlowInstance::computeCut() {
    std::vector<HyperedgeID> cut(0);
    if (shouldBeComputed) {
      whfc::TimeReporter timer("HyperFlowCommunityDetection");
      whfc::CutterState<whfc::Dinic> cs(_flow_hg_builder, timer);
      cs.setMaxBlockWeight(0, _flow_hg_builder.totalNodeWeight() / 2);
      cs.setMaxBlockWeight(1, _flow_hg_builder.totalNodeWeight() / 2);
      cs.initialize(_nodeIDMap[_globalSourceID], _nodeIDMap[_globalTargetID]);
      whfc::Dinic flow_algo(_flow_hg_builder);
      flow_algo.upperFlowBound = std::numeric_limits<whfc::Flow>::max();
      cs.borderNodes.enterMostBalancedCutMode();
      cs.hasCut = flow_algo.exhaustFlow(cs);
      whfc::GrowAssimilated<whfc::Dinic>::grow(cs, flow_algo.getScanList());
      std::cout << "Flow: " << cs.flowValue << std::endl;
      cs.verifyCutPostConditions();
      for (whfc::Hyperedge e : cs.cuts.sourceSide.entries()) {
        cut.push_back(_edgeIDMap[e]);
      }
    }
    return cut;
  }

  void HyperFlowInstance::BreathFirstSearch(const Hypergraph &hg, const HypernodeID start, size_t coreSize, size_t U, kahypar::ds::FastResetFlagArray<>& hypernodeProcessed) {
    //TODO move out
    Queue queue(hg.initialNumNodes()*2);
    // Add Target Node to Flow Hypergraph
    _globalTargetID = hg.initialNumNodes();
    _nodeIDMap[_globalTargetID] = whfc::Node::fromOtherValueType(queue.queueEnd());
    _flow_hg_builder.addNode(0);
    // BFS
    queue.push(start);
    _nodeIDMap[start] = whfc::Node::fromOtherValueType(queue.queueEnd());
    _flow_hg_builder.addNode(whfc::NodeWeight(hg.nodeWeight(start)));
    _visitedNode.set(start);
    size_t numVisited = 0;
    size_t newNodes = 0;
    size_t numPushed = 1;
    bool sourceConnectedToTarget = false;
    while (!queue.empty() && numVisited < U) {
      HypernodeID v = queue.pop();
      if (numVisited < coreSize) {
        //_core[numVisited] = v;
        _core.push_back(v);
        if (!hypernodeProcessed[v]) {
          newNodes++;
        }
      }
      if (numVisited == coreSize) {
        double newNodeRatio = ((double) newNodes) / ((double) numVisited);
        if (newNodeRatio < 0.1) {
          shouldBeComputed = false;
          return;
        }
      }
      for (const HyperedgeID e : hg.incidentEdges(v)) {
        if (!_visitedHyperedge[e]) {
          _visitedHyperedge.set(e);
          _flow_hg_builder.startHyperedge(hg.edgeWeight(e));
          _edgeIDMap.push_back(e);
          std::vector<HypernodeID> sampledNodes;
          auto pins = hg.pins(e);
          std::sample(pins.begin(), pins.end(), std::back_inserter(sampledNodes), 1000, std::mt19937{std::random_device{}()});
          for (const HypernodeID u : sampledNodes) {
            if (!_visitedNode[u]) {
              if (numPushed < U) {
                queue.push(u);
                _nodeIDMap[u] = whfc::Node::fromOtherValueType(queue.queueEnd());
                _flow_hg_builder.addNode(whfc::NodeWeight(hg.nodeWeight(u)));
                _visitedNode.set(u);
                numPushed++;
              } else {
                _flow_hg_builder.addPin(_nodeIDMap[_globalTargetID]);
                sourceConnectedToTarget = true;
                break;
              }
            }
            _flow_hg_builder.addPin(_nodeIDMap[u]);
          }

        }
      }
      numVisited++;
    }

    if (!sourceConnectedToTarget) {
      shouldBeComputed = false;
      return;
    }

    _globalSourceID = hg.initialNumNodes() + 1;
    _nodeIDMap[_globalSourceID] = whfc::Node::fromOtherValueType(queue.queueEnd()+1);
    _flow_hg_builder.addNode(0);
    _flow_hg_builder.startHyperedge(std::numeric_limits<whfc::Flow>::max());
    _flow_hg_builder.addPin(_nodeIDMap[_globalSourceID]);
    for (HypernodeID v : _core) {
      _flow_hg_builder.addPin(_nodeIDMap[v]);
    }
    _flow_hg_builder.finalize();
  }

}