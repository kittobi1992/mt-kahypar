/*******************************************************************************
 * This file is part of MT-KaHyPar.
 *
 * Copyright (C) 2020 Tobias Heuer <tobias.heuer@kit.edu>
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

#pragma once

#include "kahypar/datastructure/fast_reset_flag_array.h"

#include "mt-kahypar/definitions.h"
#include "mt-kahypar/datastructures/sparse_map.h"
#include "mt-kahypar/parallel/stl/scalable_vector.h"
#include "mt-kahypar/utils/range.h"

namespace mt_kahypar {

/**
 * Hypergraph that takes a small subset of the nodes and conceptually contracts
 * the vertices that are not part of the subset and belongs to the same block of
 * the partition into one supervertex.
 */
class ILPHypergraph {

  template <typename IDType>
  class SimpleIterator :
    public std::iterator<std::forward_iterator_tag,  // iterator_category
                         IDType,            // value_type
                         std::ptrdiff_t,             // difference_type
                         const IDType*,     // pointer
                         IDType> {          // reference
   public:

    SimpleIterator(IDType id, IDType max_id) :
      _id(id),
      _max_id(max_id) { }

    // ! Returns the id of the element the iterator currently points to.
    IDType operator* () const {
      return _id;
    }

    // ! Prefix increment. The iterator advances to the next valid element.
    SimpleIterator & operator++ () {
      ++_id;
      return *this;
    }

    // ! Postfix increment. The iterator advances to the next valid element.
    SimpleIterator operator++ (int) {
      SimpleIterator copy = *this;
      operator++ ();
      return copy;
    }

    bool operator!= (const SimpleIterator& rhs) {
      return _id != rhs._id;
    }

    bool operator== (const SimpleIterator& rhs) {
      return _id == rhs._id;
    }

   private:
    IDType _id = 0;
    IDType _max_id = 0;
  };

  class PinIterator :
    public std::iterator<std::forward_iterator_tag,  // iterator_category
                         HypernodeID,            // value_type
                         std::ptrdiff_t,             // difference_type
                         const HypernodeID*,     // pointer
                         HypernodeID> {          // reference

    using IncidenceIterator = typename Hypergraph::IncidenceArray::iterator;

   public:

    PinIterator(IncidenceIterator begin,
                IncidenceIterator end,
                const PartitionedHypergraph& phg,
                const HypernodeID num_hg_nodes,
                const ds::DynamicSparseMap<HypernodeID, HypernodeID>& hns_to_ilp_hg,
                kahypar::ds::FastResetFlagArray<>& marked_blocks) :
      _it(begin),
      _end(end),
      _current_pin(kInvalidHypernode),
      _num_hg_nodes(num_hg_nodes),
      _phg(phg),
      _hns_to_ilp_hg(hns_to_ilp_hg),
      _marked_blocks(marked_blocks) {
      _current_pin = next_pin();
    }

    // ! Returns the id of the element the iterator currently points to.
    HypernodeID operator* () const {
      return _current_pin;
    }

    // ! Prefix increment. The iterator advances to the next valid element.
    PinIterator & operator++ () {
      ++_it;
      _current_pin = next_pin();
      return *this;
    }

    bool operator!= (const PinIterator& rhs) {
      return _it != rhs._it;
    }

    bool operator== (const PinIterator& rhs) {
      return _it == rhs._it;
    }

   private:
    HypernodeID next_pin() {
      while ( _it != _end ) {
        const HypernodeID* ilp_hn = _hns_to_ilp_hg.get_if_contained(*_it);
        if ( ilp_hn ) {
          // If vertex is contained in the hash map it is part of the ILP hypergraph
          return *ilp_hn;
        } else {
          // If not than it represents a supervertex corresponding to a block of the partition
          const PartitionID block = _phg.partID(*_it);
          if ( !_marked_blocks[block] ) {
            _marked_blocks.set(block, true);
            return _num_hg_nodes + block;
          }
        }
        ++_it;
      }
      return _current_pin;
    }

    IncidenceIterator _it;
    IncidenceIterator _end;
    HypernodeID _current_pin;
    const HypernodeID _num_hg_nodes;

    const PartitionedHypergraph& _phg;
    // ! Maps a vertex from the original hypergraph to its corresponding
    // ! vertex in the ILP hypergraph
    const ds::DynamicSparseMap<HypernodeID, HypernodeID>& _hns_to_ilp_hg;
    // ! Data structure required by the pin iterator
    kahypar::ds::FastResetFlagArray<>& _marked_blocks;
  };

  using HypernodeIterator = SimpleIterator<HypernodeID>;
  using HyperedgeIterator = SimpleIterator<HyperedgeID>;

 public:
  explicit ILPHypergraph(const PartitionedHypergraph& phg,
                         const vec<HypernodeID>& nodes) :
    _num_nodes(nodes.size() + phg.k()),
    _num_hg_nodes(nodes.size()),
    _num_edges(0),
    _k(kInvalidPartition),
    _phg(phg),
    _contained_blocks(),
    _to_ilp_block(phg.k(), kInvalidPartition),
    _ilp_hns_to_hg(nodes),
    _hns_to_ilp_hg(6 * nodes.size()),
    _ilp_hes_to_hg(),
    _super_vertex_weights(phg.k(), 0),
    _marked_blocks(phg.k()) {
    initialize();
  }

  // ####################### General Hypergraph Stats ######################

  // ! Number of hypernodes
  HypernodeID numNodes() const {
    return _num_nodes;
  }

  // ! Number of hyperedges
  HyperedgeID numEdges() const {
    return _num_edges;
  }

  // ! Total weight of hypergraph
  HypernodeWeight totalWeight() const {
    return _phg.totalWeight();
  }

  // ! Number of blocks this hypergraph is partitioned into
  PartitionID k() const {
    return _k;
  }

  // ####################### Iterators #######################

  // ! Iterator over the nodes of the hypergraph
  IteratorRange<HypernodeIterator> nodes() const {
    return IteratorRange<HyperedgeIterator>(
      HypernodeIterator(0, _num_nodes), HypernodeIterator(_num_nodes, _num_nodes));
  }

  // ! Iterator over the nodes representing the supervertices of each block
  IteratorRange<HypernodeIterator> block_nodes() const {
    return IteratorRange<HyperedgeIterator>(
      HypernodeIterator(_num_hg_nodes, _num_nodes), HypernodeIterator(_num_nodes, _num_nodes));
  }

  // ! Iterator over the edges of the hypergraph
  IteratorRange<HyperedgeIterator> edges() const {
    return IteratorRange<HyperedgeIterator>(
      HyperedgeIterator(0, _num_edges), HyperedgeIterator(_num_edges, _num_edges));
  }

  // ! Iterator over the pins of a hyperedge
  IteratorRange<PinIterator> pins(const HyperedgeID e) {
    ASSERT(e < _num_edges);
    const HyperedgeID original_he = _ilp_hes_to_hg[e];
    auto it = _phg.pins(original_he);
    _marked_blocks.reset();
    return IteratorRange<PinIterator>(
      PinIterator(it.begin(), it.end(), _phg, _num_hg_nodes, _hns_to_ilp_hg, _marked_blocks),
      PinIterator(it.end(), it.end(), _phg, _num_hg_nodes, _hns_to_ilp_hg, _marked_blocks));
  }

  // ####################### Hypernode Information #######################

  // ! Weight of a vertex
  HypernodeWeight nodeWeight(const HypernodeID u) const {
    ASSERT(u < _num_nodes);
    if ( u < _num_hg_nodes ) {
      return _phg.nodeWeight(_ilp_hns_to_hg[u]);
    } else {
      const PartitionID block = u - _num_hg_nodes;
      return _super_vertex_weights[block];
    }
  }

  // ! Block that vertex u belongs to
  PartitionID partID(const HypernodeID u) const {
    ASSERT(u < _num_nodes);
    if ( u < _num_hg_nodes ) {
      const PartitionID block = _phg.partID(_ilp_hns_to_hg[u]);
      ASSERT(_to_ilp_block[block] != kInvalidPartition);
      return _to_ilp_block[block];
    } else {
      return u - _num_hg_nodes;
    }
  }

  // ####################### Hyperedge Information #######################

  // ! Weight of a hyperedge
  HypernodeWeight edgeWeight(const HyperedgeID e) const {
    ASSERT(e < _num_edges);
    return _phg.edgeWeight(_ilp_hes_to_hg[e]);
  }

  // ! Returns weather hyperedge contains pin in block p or not
  bool containsPinInPart(const HyperedgeID e, const PartitionID p) const {
    ASSERT(e < _num_edges && p < _k);
    return _phg.pinCountInPart(e, _contained_blocks[p]) > 0;
  }

 private:
  void initialize() {
    // Initialize weight of each block
    for ( PartitionID k = 0; k < _phg.k(); ++k ) {
      _super_vertex_weights[k] = _phg.partWeight(k);
    }

    // Construct mapping from hypernode id to id in ILP hypergraph
    for ( HypernodeID hn = 0; hn < ID(_ilp_hns_to_hg.size()); ++hn ) {
      const HypernodeID original_hn = _ilp_hns_to_hg[hn];
      _hns_to_ilp_hg[original_hn] = hn;
      _super_vertex_weights[_phg.partID(original_hn)] -= _phg.nodeWeight(original_hn);
    }

    // Extract hyperedges contained in ILP
    for ( const HypernodeID& hn : _ilp_hns_to_hg ) {
      for ( const HyperedgeID& he : _phg.incidentEdges(hn) ) {
        _ilp_hes_to_hg.push_back(he);
      }
    }
    // Remove duplicates
    std::sort(_ilp_hes_to_hg.begin(), _ilp_hes_to_hg.end());
    _ilp_hes_to_hg.erase(std::unique(_ilp_hes_to_hg.begin(), _ilp_hes_to_hg.end()), _ilp_hes_to_hg.end());
    _num_edges = _ilp_hes_to_hg.size();

    // Determine number of blocks contained in ILP
    for ( const HyperedgeID& he : _ilp_hes_to_hg ) {
      for ( const HypernodeID& pin : _phg.pins(he) ) {
        const PartitionID block = _phg.partID(pin);
        if ( !_marked_blocks[block] ) {
          _contained_blocks.push_back(block);
          _marked_blocks.set(block, true);
        }
      }
    }
    std::sort(_contained_blocks.begin(), _contained_blocks.end());
    _k = _contained_blocks.size();
    _num_nodes = _num_hg_nodes + _k;
    for ( PartitionID i = 0; i < _k; ++i ) {
      _to_ilp_block[_contained_blocks[i]] = i;
    }
  }

  // ! Number of nodes
  HypernodeID _num_nodes;
  // ! Number of nodes minus k
  HypernodeID _num_hg_nodes;
  // ! Number of edges
  HyperedgeID _num_edges;
  // ! Number of blocks contained in ILP
  PartitionID _k;
  // ! Underlying partitioned hypergraph
  const PartitionedHypergraph& _phg;

  // ! Contains the blocks of the partition contained in the ILP problem
  vec<PartitionID> _contained_blocks;
  // ! Mapping from block in the original hypergraph to block in ILP problem
  vec<PartitionID> _to_ilp_block;
  // ! Maps a vertex from the ILP hypergraph to its corresponding
  // ! vertex in the original hypergraph
  const vec<HypernodeID>& _ilp_hns_to_hg;
  // ! Maps a vertex from the original hypergraph to its corresponding
  // ! vertex in the ILP hypergraph
  ds::DynamicSparseMap<HypernodeID, HypernodeID> _hns_to_ilp_hg;
  // ! Maps a hyperedge from the ILP hypergraph to its corresponding
  // ! hyperedge in the original hypergraph
  vec<HyperedgeID> _ilp_hes_to_hg;

  // ! Stores the sum of the weight of the vertices that are not
  // ! contained in the ILP problem for each block
  vec<HypernodeWeight> _super_vertex_weights;

  // ! Data structure required by the pin iterator
  kahypar::ds::FastResetFlagArray<> _marked_blocks;
};

} // namespace mt_kahypar