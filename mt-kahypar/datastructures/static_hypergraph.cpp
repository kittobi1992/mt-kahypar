/*******************************************************************************
 * This file is part of KaHyPar.
 *
 * Copyright (C) 2019 Tobias Heuer <tobias.heuer@kit.edu>
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

#include "static_hypergraph.h"

#include "mt-kahypar/parallel/parallel_prefix_sum.h"
#include "mt-kahypar/datastructures/concurrent_bucket_map.h"
#include "mt-kahypar/utils/timer.h"
#include "mt-kahypar/utils/memory_tree.h"

#include <tbb/parallel_reduce.h>
#include <tbb/parallel_sort.h>

#include <boost/dynamic_bitset.hpp>

namespace mt_kahypar::ds {


  /*!
  * This struct is used during multilevel coarsening to efficiently
  * detect parallel hyperedges.
  */
  struct ContractedHyperedgeInformation {
    HyperedgeID he = kInvalidHyperedge;
    size_t hash = kEdgeHashSeed;
    size_t size = std::numeric_limits<size_t>::max();
    bool valid = false;

    bool operator<(const ContractedHyperedgeInformation& o) const {
      return std::tie(hash, size, he) < std::tie(o.hash, o.size, o.he);
    }
  };

  StaticHypergraph StaticHypergraph::contract_v2(vec<HypernodeID>& clusters) {
    auto& timer = utils::Timer::instance();
    timer.start_timer("hypergraph_contraction","Contraction");
    timer.start_timer("compactify","compactify");

    vec<HypernodeID> mapping(initialNumNodes(), 0);
    tbb::parallel_for(0U, initialNumNodes(), [&](HypernodeID u) {
      mapping[clusters[u]] = 1;
    });
    parallel_prefix_sum(mapping.begin(), mapping.begin() + initialNumNodes(), mapping.begin(), std::plus<>(), 0);
    HypernodeID num_coarse_nodes = mapping[initialNumNodes() - 1];
    // apply mapping to cluster IDs. subtract one because prefix sum is inclusive
    tbb::parallel_for(0U, initialNumNodes(), [&](HypernodeID u) {
      clusters[u] = nodeIsEnabled(u) ? mapping[clusters[u]] - 1 : kInvalidHypernode;
    });

    timer.stop_timer("compactify");
    timer.start_timer("generate pinlists","generate pinlists");

    auto get_cluster = [&](HypernodeID u) { assert(u < clusters.size()); return clusters[u]; };
    auto cs2 = [](const size_t x) { return x * x; };

    vec<vec<HypernodeID>> coarse_pin_lists(initialNumEdges());    // can be replaced later by contraction buffers :)
    vec<ContractedHyperedgeInformation> permutation(initialNumEdges());

    tbb::enumerable_thread_specific<boost::dynamic_bitset<>> local_maps(num_coarse_nodes);

    // map coarse pin lists and insert into hash map for work distribution
    doParallelForAllEdges([&](HyperedgeID he) {
      auto& pin_list = coarse_pin_lists[he];
      boost::dynamic_bitset<>& contained = local_maps.local();
      pin_list.reserve(edgeSize(he) / 2);
      for (HypernodeID v : pins(he)) {
        const HypernodeID cv = get_cluster(v);
        if (cv != kInvalidHypernode && !contained[cv]) {
          contained.set(cv);
          pin_list.push_back(cv);
        }
        // pin_list.push_back(cv);
      }
      for (HypernodeID v : pin_list) {
        contained.reset(v);
      }

      // std::sort(pin_list.begin(), pin_list.end());
      // pin_list.erase(std::unique(pin_list.begin(), pin_list.end()), pin_list.end());
      //if (!pin_list.empty() && pin_list.back() == kInvalidHypernode)
      //  pin_list.pop_back();
      if (pin_list.size() > 1) {
        size_t edge_hash = 420; for (const HypernodeID v : pin_list) { edge_hash += cs2(v); }
        permutation[he] = ContractedHyperedgeInformation{ he, edge_hash, pin_list.size(), true };
        // net_map.insert(edge_hash, ContractedHyperedgeInformation{ he, edge_hash, pin_list.size(), true });
      } else {
        pin_list.clear();   // globally mark net as removed
        permutation[he] = ContractedHyperedgeInformation{ he, std::numeric_limits<size_t>::max(), 0, false };
      }
    });

    timer.stop_timer("generate pinlists");
    timer.start_timer("identical net detection","identical net detection");

    tbb::parallel_sort(permutation.begin(), permutation.end());

    vec<HyperedgeWeight> coarse_edge_weights(initialNumEdges());
    HypernodeID num_coarse_nets = 0;
    size_t num_coarse_pins = 0;

    // identical net detection
    doParallelForAllEdges([&](HyperedgeID pos) {
      if ((pos == 0 || permutation[pos].hash != permutation[pos - 1].hash) && permutation[pos].valid ) {
        size_t num_local_nets = 0, num_local_pins = 0;
        size_t hash = permutation[pos].hash;

        for ( ; pos < permutation.size() && hash == permutation[pos].hash; ++pos) {
          const auto& rep = permutation[pos];
          HyperedgeWeight rep_weight = edgeWeight(rep.he);
          if (rep.valid) {
            auto& contained = local_maps.local();
            for (HypernodeID v : coarse_pin_lists[rep.he]) { contained.set(v); }

            for (size_t j = pos + 1; j < permutation.size() && hash == permutation[j].hash &&
                                      rep.size == permutation[j].size; ++j) {
              auto& cand = permutation[j];
              const auto& cand_pins = coarse_pin_lists[cand.he];
              if (cand.valid &&
                  std::all_of(cand_pins.begin(), cand_pins.end(), [&](HypernodeID v) { return contained[v]; })) {
                cand.valid = false;
                rep_weight += edgeWeight(cand.he);
                coarse_pin_lists[cand.he].clear();    // globally mark net as removed
              }
            }
            coarse_edge_weights[rep.he] = rep_weight;
            num_local_nets++;
            num_local_pins += coarse_pin_lists[rep.he].size();
            for (HypernodeID v : coarse_pin_lists[rep.he]) { contained.reset(v); }
          }

        }

        __atomic_fetch_add(&num_coarse_nets, num_local_nets, __ATOMIC_RELAXED);
        __atomic_fetch_add(&num_coarse_pins, num_local_pins, __ATOMIC_RELAXED);
      }
    });

    /*
    // identical net detection
    tbb::parallel_for(0UL, net_map.numBuckets(), [&](const size_t bucket_id) {
      size_t num_local_nets = 0, num_local_pins = 0;
      auto& bucket = net_map.getBucket(bucket_id);
      std::sort(bucket.begin(), bucket.end());
      for (size_t i = 0; i < bucket.size(); ++i) {
        const auto& rep = bucket[i];
        HyperedgeWeight rep_weight = edgeWeight(rep.he);
        if (rep.valid) {
          auto& contained = local_maps.local();
          for (HypernodeID v : coarse_pin_lists[rep.he]) { contained.set(v); }

          for (size_t j = i+1; j < bucket.size(); ++j) {
            auto& cand = bucket[j];
            if (cand.hash != rep.hash) { break; }
            const auto& cand_pins = coarse_pin_lists[cand.he];
            if (cand.valid && coarse_pin_lists[rep.he].size() == cand_pins.size()
                && std::all_of(cand_pins.begin(), cand_pins.end(), [&](HypernodeID v) { return contained[v];})) {
              cand.valid = false;
              rep_weight += edgeWeight(cand.he);
              coarse_pin_lists[cand.he].clear();    // globally mark net as removed
            }
          }
          coarse_edge_weights[rep.he] = rep_weight;
          num_local_nets++;
          num_local_pins += coarse_pin_lists[rep.he].size();
          for (HypernodeID v : coarse_pin_lists[rep.he]) { contained.reset(v); }
        }
      }
      net_map.free(bucket_id);
      __atomic_fetch_add(&num_coarse_nets, num_local_nets, __ATOMIC_RELAXED);
      __atomic_fetch_add(&num_coarse_pins, num_local_pins, __ATOMIC_RELAXED);
    });

    */

    timer.stop_timer("identical net detection");
    timer.start_timer("allocs","allocs");

    vec<size_t> offsets_for_fine_nets;   // TODO consolidate data structures into one vec. potentially extract into reusable memory

    StaticHypergraph chg;
    chg._num_hypernodes = num_coarse_nodes;
    chg._num_hyperedges = num_coarse_nets;
    chg._num_pins = num_coarse_pins;
    chg._total_degree = num_coarse_pins;
    chg._total_weight = _total_weight;   // didn't lose any vertices. or at least we don't want to let the imbalance calculation know about it...

    tbb::parallel_invoke([&] {
      chg._incident_nets.resize(num_coarse_pins);
    }, [&] {
      chg._incidence_array.resize(num_coarse_pins);
    }, [&]{
      chg._community_ids.resize(num_coarse_nodes);
    }, [&] {
      chg._hyperedges.resize(num_coarse_nets);
    }, [&] {
      chg._hypernodes.resize(num_coarse_nodes);
    }, [&] {
      offsets_for_fine_nets.resize(initialNumEdges());
    });

    timer.stop_timer("allocs");
    timer.start_timer("write pin lists", "write pin lists and coutn degrees");

    auto net_size_prefix_sum = [&](const tbb::blocked_range<HyperedgeID>& r,
            std::pair<size_t, HyperedgeID> sums, bool is_final_scan) -> std::pair<size_t,HyperedgeID> {
      size_t net_size_sum = sums.first;
      HyperedgeID coarse_net_id = sums.second;
      for (HyperedgeID he = r.begin(); he < r.end(); ++he) {
        if (!coarse_pin_lists[he].empty()) {
          if (is_final_scan) {
            chg._hyperedges[coarse_net_id].enable();
            chg._hyperedges[coarse_net_id].setSize(coarse_pin_lists[he].size());
            chg._hyperedges[coarse_net_id].setFirstEntry(net_size_sum);
            chg._hyperedges[coarse_net_id].setWeight(coarse_edge_weights[he]);
            offsets_for_fine_nets[he] = net_size_sum;
          }
          net_size_sum += coarse_pin_lists[he].size();
          coarse_net_id++;
        }
      }
      return std::make_pair(net_size_sum, coarse_net_id);
    };
    auto sum_pair = [](std::pair<size_t, HyperedgeID> l, std::pair<size_t, HyperedgeID> r) {
      return std::make_pair(l.first + r.first, l.second + r.second);
    };
    tbb::parallel_scan(tbb::blocked_range<HyperedgeID>(0U, initialNumEdges()), std::make_pair(0UL,0U),
            net_size_prefix_sum, sum_pair);

    // can do this in the is_final_scan in the prefix sum above. may result in bad load balancing. measure!
    doParallelForAllEdges([&](HyperedgeID he) {
      // removed nets are marked via empty pin list
      if (!coarse_pin_lists[he].empty()) {
        size_t pos = offsets_for_fine_nets[he];
        for (HypernodeID v : coarse_pin_lists[he]) {
          chg._incidence_array[pos++] = v;                                        // copy pin list
          __atomic_fetch_add(&chg._hypernodes[v]._size, 1, __ATOMIC_RELAXED);     // increment pin's degree
        }
      }
    });

    timer.stop_timer("write pin lists");
    timer.start_timer("write incident nets", "write incident nets");

    auto degree_prefix_sum = [&](const tbb::blocked_range<HypernodeID>& r, size_t sum, bool is_final_scan) -> size_t {
      for (HypernodeID u = r.begin(); u < r.end(); ++u) {
        if (is_final_scan) {
          chg._hypernodes[u].enable();
          chg._hypernodes[u].setFirstEntry(sum);
        }
        sum += chg._hypernodes[u]._size;
      }
      return sum;
    };
    tbb::parallel_scan(tbb::blocked_range<HypernodeID>(0U, num_coarse_nodes), 0UL, degree_prefix_sum, std::plus<>());

    tbb::parallel_for(0U, num_coarse_nets, [&](HyperedgeID he) {
      // pin lists fully constructed --> safe to use
      for (HypernodeID v : chg.pins(he)) {
        const size_t pos = __atomic_fetch_add(&chg._hypernodes[v]._begin, 1, __ATOMIC_RELAXED);
        chg._incident_nets[pos] = he;
      }
    });

    // reset begin pointers of nodes that we destroyed when writing the incident nets, and make the order deterministic
    tbb::parallel_for(0U, num_coarse_nodes, [&](HypernodeID u) {
      chg._hypernodes[u]._weight = 0;
      chg._hypernodes[u]._begin -= chg._hypernodes[u].size();
      std::sort(chg._incident_nets.begin() + chg._hypernodes[u].firstEntry(),
                chg._incident_nets.begin() + chg._hypernodes[u].firstInvalidEntry());
    });

    timer.stop_timer("write incident nets");
    timer.start_timer("find max edge size", "find max edge size");

    auto find_max_net_size = [&](const tbb::blocked_range<HyperedgeID>& r, HypernodeID max_net_size) {
      for (HyperedgeID e = r.begin(); e < r.end(); ++e) { max_net_size = std::max(max_net_size, chg.edgeSize(e)); }
      return max_net_size;
    };
    auto get_max = [&](HypernodeID lhs, HypernodeID rhs) { return std::max(lhs, rhs); };
    chg._max_edge_size = tbb::parallel_reduce(tbb::blocked_range<HyperedgeID>(0U, num_coarse_nets),
                                      0,find_max_net_size, get_max);

    timer.stop_timer("find max edge size");
    timer.start_timer("aggregate node weights", "aggregate node weights");

    doParallelForAllNodes([&](HypernodeID u) {
      __atomic_fetch_add(&chg._hypernodes[get_cluster(u)]._weight, nodeWeight(u), __ATOMIC_RELAXED);
      chg.setCommunityID(get_cluster(u), communityID(u));
    });

    timer.stop_timer("aggregate node weights");
    timer.stop_timer("hypergraph_contraction");

    return chg;
  }

  /*!
   * Contracts a given community structure. All vertices with the same label
   * are collapsed into the same vertex. The resulting single-pin and parallel
   * hyperedges are removed from the contracted graph. The function returns
   * the contracted hypergraph and a mapping which specifies a mapping from
   * community label (given in 'communities') to a vertex in the coarse hypergraph.
   *
   * \param communities Community structure that should be contracted
   * \param task_group_id Task Group ID
   */

  StaticHypergraph StaticHypergraph::contract(parallel::scalable_vector<HypernodeID>& communities) {
    return contract_v2(communities);

    ASSERT(communities.size() == _num_hypernodes);

    if ( !_tmp_contraction_buffer ) {
      allocateTmpContractionBuffer();
    }

    // Auxiliary buffers - reused during multilevel hierarchy to prevent expensive allocations
    Array<size_t>& mapping = _tmp_contraction_buffer->mapping;
    Array<Hypernode>& tmp_hypernodes = _tmp_contraction_buffer->tmp_hypernodes;
    IncidentNets& tmp_incident_nets = _tmp_contraction_buffer->tmp_incident_nets;
    Array<parallel::IntegralAtomicWrapper<size_t>>& tmp_num_incident_nets =
            _tmp_contraction_buffer->tmp_num_incident_nets;
    Array<parallel::IntegralAtomicWrapper<HypernodeWeight>>& hn_weights =
            _tmp_contraction_buffer->hn_weights;
    Array<Hyperedge>& tmp_hyperedges = _tmp_contraction_buffer->tmp_hyperedges;
    IncidenceArray& tmp_incidence_array = _tmp_contraction_buffer->tmp_incidence_array;
    Array<size_t>& he_sizes = _tmp_contraction_buffer->he_sizes;
    Array<size_t>& valid_hyperedges = _tmp_contraction_buffer->valid_hyperedges;

    ASSERT(static_cast<size_t>(_num_hypernodes) <= mapping.size());
    ASSERT(static_cast<size_t>(_num_hypernodes) <= tmp_hypernodes.size());
    ASSERT(static_cast<size_t>(_total_degree) <= tmp_incident_nets.size());
    ASSERT(static_cast<size_t>(_num_hypernodes) <= tmp_num_incident_nets.size());
    ASSERT(static_cast<size_t>(_num_hypernodes) <= hn_weights.size());
    ASSERT(static_cast<size_t>(_num_hyperedges) <= tmp_hyperedges.size());
    ASSERT(static_cast<size_t>(_num_pins) <= tmp_incidence_array.size());
    ASSERT(static_cast<size_t>(_num_hyperedges) <= he_sizes.size());
    ASSERT(static_cast<size_t>(_num_hyperedges) <= valid_hyperedges.size());


    // #################### STAGE 1 ####################
    // Compute vertex ids of coarse hypergraph with a parallel prefix sum
    utils::Timer::instance().start_timer("preprocess_contractions", "Preprocess Contractions");
    mapping.assign(_num_hypernodes, 0);

    doParallelForAllNodes([&](const HypernodeID& hn) {
      ASSERT(static_cast<size_t>(communities[hn]) < mapping.size());
      mapping[communities[hn]] = 1UL;
    });

    // Prefix sum determines vertex ids in coarse hypergraph
    parallel::TBBPrefixSum<size_t, Array> mapping_prefix_sum(mapping);
    tbb::parallel_scan(tbb::blocked_range<size_t>(0UL, _num_hypernodes), mapping_prefix_sum);
    HypernodeID num_hypernodes = mapping_prefix_sum.total_sum();

    // Remap community ids
    tbb::parallel_for(ID(0), _num_hypernodes, [&](const HypernodeID& hn) {
      if ( nodeIsEnabled(hn) ) {
        communities[hn] = mapping_prefix_sum[communities[hn]];
      } else {
        communities[hn] = kInvalidHypernode;
      }

      // Reset tmp contraction buffer
      if ( hn < num_hypernodes ) {
        hn_weights[hn] = 0;
        tmp_hypernodes[hn] = Hypernode(true);
        tmp_num_incident_nets[hn] = 0;
      }
    });

    // Mapping from a vertex id of the current hypergraph to its
    // id in the coarse hypergraph
    auto map_to_coarse_hypergraph = [&](const HypernodeID hn) {
      ASSERT(hn < communities.size());
      return communities[hn];
    };

    doParallelForAllNodes([&](const HypernodeID& hn) {
      const HypernodeID coarse_hn = map_to_coarse_hypergraph(hn);
      ASSERT(coarse_hn < num_hypernodes, V(coarse_hn) << V(num_hypernodes));
      // Weight vector is atomic => thread-safe
      hn_weights[coarse_hn] += nodeWeight(hn);
      // Aggregate upper bound for number of incident nets of the contracted vertex
      tmp_num_incident_nets[coarse_hn] += nodeDegree(hn);
    });
    utils::Timer::instance().stop_timer("preprocess_contractions");

    // #################### STAGE 2 ####################
    // In this step hyperedges and incident nets of vertices are contracted inside the temporary
    // buffers. The vertex ids of pins are already remapped to the vertex ids in the coarse
    // graph and duplicates are removed. Also nets that become single-pin hyperedges are marked
    // as invalid. All incident nets of vertices that are collapsed into one vertex in the coarse
    // graph are also aggregate in a consecutive memory range and duplicates are removed. Note
    // that parallel and single-pin hyperedges are not removed from the incident nets (will be done
    // in a postprocessing step).
    auto cs2 = [](const HypernodeID x) { return x * x; };
    utils::Timer::instance().start_timer("contract_incidence_structure", "Contract Incidence Structures");
    ConcurrentBucketMap<ContractedHyperedgeInformation> hyperedge_hash_map;
    hyperedge_hash_map.reserve_for_estimated_number_of_insertions(_num_hyperedges);
    tbb::parallel_invoke([&] {
      // Contract Hyperedges
      utils::Timer::instance().start_timer("contract_hyperedges", "Contract Hyperedges", true);
      tbb::parallel_for(ID(0), _num_hyperedges, [&](const HyperedgeID& he) {
        if ( edgeIsEnabled(he) ) {
          // Copy hyperedge and pins to temporary buffer
          const Hyperedge& e = _hyperedges[he];
          ASSERT(static_cast<size_t>(he) < tmp_hyperedges.size());
          ASSERT(e.firstInvalidEntry() <= tmp_incidence_array.size());
          tmp_hyperedges[he] = e;
          valid_hyperedges[he] = 1;

          // Map pins to vertex ids in coarse graph
          const size_t incidence_array_start = tmp_hyperedges[he].firstEntry();
          const size_t incidence_array_end = tmp_hyperedges[he].firstInvalidEntry();
          for ( size_t pos = incidence_array_start; pos < incidence_array_end; ++pos ) {
            const HypernodeID pin = _incidence_array[pos];
            ASSERT(pos < tmp_incidence_array.size());
            tmp_incidence_array[pos] = map_to_coarse_hypergraph(pin);
          }

          // Remove duplicates and disabled vertices
          auto first_entry_it = tmp_incidence_array.begin() + incidence_array_start;
          std::sort(first_entry_it, tmp_incidence_array.begin() + incidence_array_end);
          auto first_invalid_entry_it = std::unique(first_entry_it, tmp_incidence_array.begin() + incidence_array_end);
          while ( first_entry_it != first_invalid_entry_it && *(first_invalid_entry_it - 1) == kInvalidHypernode ) {
            --first_invalid_entry_it;
          }

          // Update size of hyperedge in temporary hyperedge buffer
          const size_t contracted_size = std::distance(
                  tmp_incidence_array.begin() + incidence_array_start, first_invalid_entry_it);
          tmp_hyperedges[he].setSize(contracted_size);


          if ( contracted_size > 1 ) {
            // Compute hash of contracted hyperedge
            size_t footprint = kEdgeHashSeed;
            for ( size_t pos = incidence_array_start; pos < incidence_array_start + contracted_size; ++pos ) {
              footprint += cs2(tmp_incidence_array[pos]);
            }
            hyperedge_hash_map.insert(footprint,
                                      ContractedHyperedgeInformation{ he, footprint, contracted_size, true });
          } else {
            // Hyperedge becomes a single-pin hyperedge
            valid_hyperedges[he] = 0;
            tmp_hyperedges[he].disable();
          }
        } else {
          valid_hyperedges[he] = 0;
        }
      });
      utils::Timer::instance().stop_timer("contract_hyperedges");
    }, [&] {
      // Contract Incident Nets
      utils::Timer::instance().start_timer("tmp_contract_incident_nets", "Tmp Contract Incident Nets", true);

      // Compute start position the incident nets of a coarse vertex in the
      // temporary incident nets array with a parallel prefix sum
      parallel::scalable_vector<parallel::IntegralAtomicWrapper<size_t>> tmp_incident_nets_pos;
      parallel::TBBPrefixSum<parallel::IntegralAtomicWrapper<size_t>, Array>
              tmp_incident_nets_prefix_sum(tmp_num_incident_nets);
      tbb::parallel_invoke([&] {
        tbb::parallel_scan(tbb::blocked_range<size_t>(
                0UL, UI64(num_hypernodes)), tmp_incident_nets_prefix_sum);
      }, [&] {
        tmp_incident_nets_pos.assign(num_hypernodes, parallel::IntegralAtomicWrapper<size_t>(0));
      });

      // Write the incident nets of each contracted vertex to the temporary incident net array
      doParallelForAllNodes([&](const HypernodeID& hn) {
        const HypernodeID coarse_hn = map_to_coarse_hypergraph(hn);
        const HyperedgeID node_degree = nodeDegree(hn);
        size_t incident_nets_pos = tmp_incident_nets_prefix_sum[coarse_hn] +
                                   tmp_incident_nets_pos[coarse_hn].fetch_add(node_degree);
        ASSERT(incident_nets_pos + node_degree <= tmp_incident_nets_prefix_sum[coarse_hn + 1]);
        memcpy(tmp_incident_nets.data() + incident_nets_pos,
               _incident_nets.data() + _hypernodes[hn].firstEntry(),
               sizeof(HyperedgeID) * node_degree);
      });

      // Setup temporary hypernodes
      std::mutex high_degree_vertex_mutex;
      parallel::scalable_vector<HypernodeID> high_degree_vertices;
      tbb::parallel_for(ID(0), num_hypernodes, [&](const HypernodeID& coarse_hn) {
        // Remove duplicates
        const size_t incident_nets_start = tmp_incident_nets_prefix_sum[coarse_hn];
        const size_t incident_nets_end = tmp_incident_nets_prefix_sum[coarse_hn + 1];
        const size_t tmp_degree = incident_nets_end - incident_nets_start;
        if ( tmp_degree <= HIGH_DEGREE_CONTRACTION_THRESHOLD ) {
          std::sort(tmp_incident_nets.begin() + incident_nets_start,
                    tmp_incident_nets.begin() + incident_nets_end);
          auto first_invalid_entry_it = std::unique(tmp_incident_nets.begin() + incident_nets_start,
                                                    tmp_incident_nets.begin() + incident_nets_end);

          // Setup pointers to temporary incident nets
          const size_t contracted_size = std::distance(tmp_incident_nets.begin() + incident_nets_start,
                                                       first_invalid_entry_it);
          tmp_hypernodes[coarse_hn].setSize(contracted_size);
        } else {
          std::lock_guard<std::mutex> lock(high_degree_vertex_mutex);
          high_degree_vertices.push_back(coarse_hn);
        }
        tmp_hypernodes[coarse_hn].setWeight(hn_weights[coarse_hn]);
        tmp_hypernodes[coarse_hn].setFirstEntry(incident_nets_start);
      });

      if ( !high_degree_vertices.empty() ) {
        // High degree vertices are treated special, because sorting and afterwards
        // removing duplicates can become a major sequential bottleneck. Therefore,
        // we distribute the incident nets of a high degree vertex into our concurrent
        // bucket map. As a result all equal incident nets reside in the same bucket
        // afterwards. In a second step, we process each bucket in parallel and apply
        // for each bucket the duplicate removal procedure from above.
        ConcurrentBucketMap<HyperedgeID> duplicate_incident_nets_map;
        for ( const HypernodeID& coarse_hn : high_degree_vertices ) {
          const size_t incident_nets_start = tmp_incident_nets_prefix_sum[coarse_hn];
          const size_t incident_nets_end = tmp_incident_nets_prefix_sum[coarse_hn + 1];
          const size_t tmp_degree = incident_nets_end - incident_nets_start;

          // Insert incident nets into concurrent bucket map
          duplicate_incident_nets_map.reserve_for_estimated_number_of_insertions(tmp_degree);
          tbb::parallel_for(incident_nets_start, incident_nets_end, [&](const size_t pos) {
            HyperedgeID he = tmp_incident_nets[pos];
            duplicate_incident_nets_map.insert(he, std::move(he));
          });

          // Process each bucket in parallel and remove duplicates
          std::atomic<size_t> incident_nets_pos(incident_nets_start);
          tbb::parallel_for(0UL, duplicate_incident_nets_map.numBuckets(), [&](const size_t bucket) {
            auto& incident_net_bucket = duplicate_incident_nets_map.getBucket(bucket);
            std::sort(incident_net_bucket.begin(), incident_net_bucket.end());
            auto first_invalid_entry_it = std::unique(incident_net_bucket.begin(), incident_net_bucket.end());
            const size_t bucket_degree = std::distance(incident_net_bucket.begin(), first_invalid_entry_it);
            const size_t tmp_incident_nets_pos = incident_nets_pos.fetch_add(bucket_degree);
            memcpy(tmp_incident_nets.data() + tmp_incident_nets_pos,
                   incident_net_bucket.data(), sizeof(HyperedgeID) * bucket_degree);
            duplicate_incident_nets_map.clear(bucket);
          });

          // Update number of incident nets of high degree vertex
          const size_t contracted_size = incident_nets_pos.load() - incident_nets_start;
          tmp_hypernodes[coarse_hn].setSize(contracted_size);
        }
        duplicate_incident_nets_map.free();
      }

      utils::Timer::instance().stop_timer("tmp_contract_incident_nets");
    });
    utils::Timer::instance().stop_timer("contract_incidence_structure");

    // #################### STAGE 3 ####################
    // In the step before we aggregated hyperedges within a bucket data structure.
    // Hyperedges with the same hash/footprint are stored inside the same bucket.
    // We iterate now in parallel over each bucket and sort each bucket
    // after its hash. A bucket is processed by one thread and parallel
    // hyperedges are detected by comparing the pins of hyperedges with
    // the same hash.

    utils::Timer::instance().start_timer("remove_parallel_hyperedges", "Remove Parallel Hyperedges");

    // Helper function that checks if two hyperedges are parallel
    // Note, pins inside the hyperedges are sorted.
    auto check_if_hyperedges_are_parallel = [&](const HyperedgeID lhs,
                                                const HyperedgeID rhs) {
      const Hyperedge& lhs_he = tmp_hyperedges[lhs];
      const Hyperedge& rhs_he = tmp_hyperedges[rhs];
      if ( lhs_he.size() == rhs_he.size() ) {
        const size_t lhs_start = lhs_he.firstEntry();
        const size_t rhs_start = rhs_he.firstEntry();
        for ( size_t i = 0; i < lhs_he.size(); ++i ) {
          const size_t lhs_pos = lhs_start + i;
          const size_t rhs_pos = rhs_start + i;
          if ( tmp_incidence_array[lhs_pos] != tmp_incidence_array[rhs_pos] ) {
            return false;
          }
        }
        return true;
      } else {
        return false;
      }
    };

    tbb::parallel_for(0UL, hyperedge_hash_map.numBuckets(), [&](const size_t bucket) {
      auto& hyperedge_bucket = hyperedge_hash_map.getBucket(bucket);
      std::sort(hyperedge_bucket.begin(), hyperedge_bucket.end(),
                [&](const ContractedHyperedgeInformation& lhs, const ContractedHyperedgeInformation& rhs) {
                  return std::tie(lhs.hash, lhs.size, lhs.he) < std::tie(rhs.hash, rhs.size, rhs.he);
                });

      // Parallel Hyperedge Detection
      for ( size_t i = 0; i < hyperedge_bucket.size(); ++i ) {
        ContractedHyperedgeInformation& contracted_he_lhs = hyperedge_bucket[i];
        if ( contracted_he_lhs.valid ) {
          const HyperedgeID lhs_he = contracted_he_lhs.he;
          HyperedgeWeight lhs_weight = tmp_hyperedges[lhs_he].weight();
          for ( size_t j = i + 1; j < hyperedge_bucket.size(); ++j ) {
            ContractedHyperedgeInformation& contracted_he_rhs = hyperedge_bucket[j];
            const HyperedgeID rhs_he = contracted_he_rhs.he;
            if ( contracted_he_rhs.valid &&
                 contracted_he_lhs.hash == contracted_he_rhs.hash &&
                 check_if_hyperedges_are_parallel(lhs_he, rhs_he) ) {
              // Hyperedges are parallel
              lhs_weight += tmp_hyperedges[rhs_he].weight();
              contracted_he_rhs.valid = false;
              valid_hyperedges[rhs_he] = false;
            } else if ( contracted_he_lhs.hash != contracted_he_rhs.hash  ) {
              // In case, hash of both are not equal we go to the next hyperedge
              // because we compared it with all hyperedges that had an equal hash
              break;
            }
          }
          tmp_hyperedges[lhs_he].setWeight(lhs_weight);
        }
      }
      hyperedge_hash_map.free(bucket);
    });
    utils::Timer::instance().stop_timer("remove_parallel_hyperedges");

    // #################### STAGE 4 ####################
    // Coarsened hypergraph is constructed here by writting data from temporary
    // buffers to corresponding members in coarsened hypergraph. For the
    // incidence array, we compute a prefix sum over the hyperedge sizes in
    // the contracted hypergraph which determines the start position of the pins
    // of each net in the incidence array. Furthermore, we postprocess the incident
    // nets of each vertex by removing invalid hyperedges and remapping hyperedge ids.
    // Incident nets are also written to the incident nets array with the help of a prefix
    // sum over the node degrees.
    utils::Timer::instance().start_timer("contract_hypergraph", "Contract Hypergraph");

    StaticHypergraph hypergraph;

    // Compute number of hyperedges in coarse graph (those flagged as valid)
    parallel::TBBPrefixSum<size_t, Array> he_mapping(valid_hyperedges);
    tbb::parallel_invoke([&] {
      tbb::parallel_scan(tbb::blocked_range<size_t>(0UL, UI64(_num_hyperedges)), he_mapping);
    }, [&] {
      hypergraph._hypernodes.resize(num_hypernodes);
    });

    const HyperedgeID num_hyperedges = he_mapping.total_sum();
    hypergraph._num_hypernodes = num_hypernodes;
    hypergraph._num_hyperedges = num_hyperedges;

    auto assign_communities = [&] {
      hypergraph._community_ids.resize(num_hypernodes, 0);
      doParallelForAllNodes([&](HypernodeID fine_hn) {
        hypergraph.setCommunityID(map_to_coarse_hypergraph(fine_hn), communityID(fine_hn));
      });
    };

    auto setup_hyperedges = [&] {
      utils::Timer::instance().start_timer("setup_hyperedges", "Setup Hyperedges", true);
      utils::Timer::instance().start_timer("compute_he_pointer", "Compute HE Pointer", true);
      // Compute start position of each hyperedge in incidence array
      parallel::TBBPrefixSum<size_t, Array> num_pins_prefix_sum(he_sizes);
      tbb::parallel_invoke([&] {
        tbb::parallel_for(ID(0), _num_hyperedges, [&](const HyperedgeID& id) {
          if ( he_mapping.value(id) ) {
            he_sizes[id] = tmp_hyperedges[id].size();
          } else {
            he_sizes[id] = 0;
          }
        });

        tbb::parallel_scan(tbb::blocked_range<size_t>(0UL, UI64(_num_hyperedges)), num_pins_prefix_sum);

        const size_t num_pins = num_pins_prefix_sum.total_sum();
        hypergraph._num_pins = num_pins;
        hypergraph._incidence_array.resize(num_pins);
      }, [&] {
        hypergraph._hyperedges.resize(num_hyperedges);
      });
      utils::Timer::instance().stop_timer("compute_he_pointer");

      utils::Timer::instance().start_timer("setup_incidence_array", "Setup Incidence Array", true);
      // Write hyperedges from temporary buffers to incidence array
      tbb::enumerable_thread_specific<size_t> local_max_edge_size(0UL);
      tbb::parallel_for(ID(0), _num_hyperedges, [&](const HyperedgeID& id) {
        if ( he_mapping.value(id) > 0 /* hyperedge is valid */ ) {
          const size_t he_pos = he_mapping[id];
          const size_t incidence_array_start = num_pins_prefix_sum[id];
          Hyperedge& he = hypergraph._hyperedges[he_pos];
          he = tmp_hyperedges[id];
          const size_t tmp_incidence_array_start = he.firstEntry();
          const size_t edge_size = he.size();
          local_max_edge_size.local() = std::max(local_max_edge_size.local(), edge_size);
          std::memcpy(hypergraph._incidence_array.data() + incidence_array_start,
                      tmp_incidence_array.data() + tmp_incidence_array_start,
                      sizeof(HypernodeID) * edge_size);
          he.setFirstEntry(incidence_array_start);
        }
      });
      hypergraph._max_edge_size = local_max_edge_size.combine(
              [&](const size_t lhs, const size_t rhs) {
                return std::max(lhs, rhs);
              });
      utils::Timer::instance().stop_timer("setup_incidence_array");
      utils::Timer::instance().stop_timer("setup_hyperedges");
    };

    auto setup_hypernodes = [&] {
      utils::Timer::instance().start_timer("setup_hypernodes", "Setup Hypernodes", true);
      utils::Timer::instance().start_timer("compute_num_incident_nets", "Compute Num Incident Nets", true);
      // Remap hyperedge ids in temporary incident nets to hyperedge ids of the
      // coarse hypergraph and remove singple-pin/parallel hyperedges.
      tbb::parallel_for(ID(0), num_hypernodes, [&](const HypernodeID& id) {
        const size_t incident_nets_start =  tmp_hypernodes[id].firstEntry();
        size_t incident_nets_end = tmp_hypernodes[id].firstInvalidEntry();
        for ( size_t pos = incident_nets_start; pos < incident_nets_end; ++pos ) {
          const HyperedgeID he = tmp_incident_nets[pos];
          if ( he_mapping.value(he) > 0 /* hyperedge is valid */ ) {
            tmp_incident_nets[pos] = he_mapping[he];
          } else {
            std::swap(tmp_incident_nets[pos--], tmp_incident_nets[--incident_nets_end]);
          }
        }
        const size_t incident_nets_size = incident_nets_end - incident_nets_start;
        tmp_hypernodes[id].setSize(incident_nets_size);
        tmp_num_incident_nets[id] = incident_nets_size;
      });

      // Compute start position of the incident nets for each vertex inside
      // the coarsened incident net array
      parallel::TBBPrefixSum<parallel::IntegralAtomicWrapper<size_t>, Array>
              num_incident_nets_prefix_sum(tmp_num_incident_nets);
      tbb::parallel_scan(tbb::blocked_range<size_t>(
              0UL, UI64(num_hypernodes)), num_incident_nets_prefix_sum);
      const size_t total_degree = num_incident_nets_prefix_sum.total_sum();
      hypergraph._total_degree = total_degree;
      hypergraph._incident_nets.resize(total_degree);
      utils::Timer::instance().stop_timer("compute_num_incident_nets");

      utils::Timer::instance().start_timer("setup_incident_nets", "Setup Incidenct Nets", true);
      // Write incident nets from temporary buffer to incident nets array
      tbb::parallel_for(ID(0), num_hypernodes, [&](const HypernodeID& id) {
        const size_t incident_nets_start = num_incident_nets_prefix_sum[id];
        Hypernode& hn = hypergraph._hypernodes[id];
        hn = tmp_hypernodes[id];
        const size_t tmp_incident_nets_start = hn.firstEntry();
        std::memcpy(hypergraph._incident_nets.data() + incident_nets_start,
                    tmp_incident_nets.data() + tmp_incident_nets_start,
                    sizeof(HyperedgeID) * hn.size());
        hn.setFirstEntry(incident_nets_start);

        // still need to sort here because high degree vertex handling does not insert in deterministic order
        std::sort(hypergraph._incident_nets.begin() + hn.firstEntry(),
                  hypergraph._incident_nets.begin() + hn.firstInvalidEntry());
      });
      utils::Timer::instance().stop_timer("setup_incident_nets");
      utils::Timer::instance().stop_timer("setup_hypernodes");
    };

    tbb::parallel_invoke(assign_communities, setup_hyperedges, setup_hypernodes);
    utils::Timer::instance().stop_timer("contract_hypergraph");

    hypergraph._total_weight = _total_weight;   // didn't lose any vertices
    hypergraph._tmp_contraction_buffer = _tmp_contraction_buffer;
    _tmp_contraction_buffer = nullptr;
    return hypergraph;
  }


  // ! Copy static hypergraph in parallel
  StaticHypergraph StaticHypergraph::copy(parallel_tag_t) {
    StaticHypergraph hypergraph;

    hypergraph._num_hypernodes = _num_hypernodes;
    hypergraph._num_removed_hypernodes = _num_removed_hypernodes;
    hypergraph._num_hyperedges = _num_hyperedges;
    hypergraph._num_removed_hyperedges = _num_removed_hyperedges;
    hypergraph._max_edge_size = _max_edge_size;
    hypergraph._num_pins = _num_pins;
    hypergraph._total_degree = _total_degree;
    hypergraph._total_weight = _total_weight;

    tbb::parallel_invoke([&] {
      hypergraph._hypernodes.resize(_hypernodes.size());
      memcpy(hypergraph._hypernodes.data(), _hypernodes.data(),
             sizeof(Hypernode) * _hypernodes.size());
    }, [&] {
      hypergraph._incident_nets.resize(_incident_nets.size());
      memcpy(hypergraph._incident_nets.data(), _incident_nets.data(),
             sizeof(HyperedgeID) * _incident_nets.size());
    }, [&] {
      hypergraph._hyperedges.resize(_hyperedges.size());
      memcpy(hypergraph._hyperedges.data(), _hyperedges.data(),
             sizeof(Hyperedge) * _hyperedges.size());
    }, [&] {
      hypergraph._incidence_array.resize(_incidence_array.size());
      memcpy(hypergraph._incidence_array.data(), _incidence_array.data(),
             sizeof(HypernodeID) * _incidence_array.size());
    }, [&] {
      hypergraph._community_ids = _community_ids;
    });
    return hypergraph;
  }

  // ! Copy static hypergraph sequential
  StaticHypergraph StaticHypergraph::copy() {
    StaticHypergraph hypergraph;

    hypergraph._num_hypernodes = _num_hypernodes;
    hypergraph._num_removed_hypernodes = _num_removed_hypernodes;
    hypergraph._num_hyperedges = _num_hyperedges;
    hypergraph._num_removed_hyperedges = _num_removed_hyperedges;
    hypergraph._max_edge_size = _max_edge_size;
    hypergraph._num_pins = _num_pins;
    hypergraph._total_degree = _total_degree;
    hypergraph._total_weight = _total_weight;

    hypergraph._hypernodes.resize(_hypernodes.size());
    memcpy(hypergraph._hypernodes.data(), _hypernodes.data(),
           sizeof(Hypernode) * _hypernodes.size());
    hypergraph._incident_nets.resize(_incident_nets.size());
    memcpy(hypergraph._incident_nets.data(), _incident_nets.data(),
           sizeof(HyperedgeID) * _incident_nets.size());

    hypergraph._hyperedges.resize(_hyperedges.size());
    memcpy(hypergraph._hyperedges.data(), _hyperedges.data(),
           sizeof(Hyperedge) * _hyperedges.size());
    hypergraph._incidence_array.resize(_incidence_array.size());
    memcpy(hypergraph._incidence_array.data(), _incidence_array.data(),
           sizeof(HypernodeID) * _incidence_array.size());

    hypergraph._community_ids = _community_ids;

    return hypergraph;
  }




  void StaticHypergraph::memoryConsumption(utils::MemoryTreeNode* parent) const {
    ASSERT(parent);
    parent->addChild("Hypernodes", sizeof(Hypernode) * _hypernodes.size());
    parent->addChild("Incident Nets", sizeof(HyperedgeID) * _incident_nets.size());
    parent->addChild("Hyperedges", sizeof(Hyperedge) * _hyperedges.size());
    parent->addChild("Incidence Array", sizeof(HypernodeID) * _incidence_array.size());
    parent->addChild("Communities", sizeof(PartitionID) * _community_ids.capacity());
  }

  // ! Computes the total node weight of the hypergraph
  void StaticHypergraph::computeAndSetTotalNodeWeight(parallel_tag_t) {
    _total_weight = tbb::parallel_reduce(tbb::blocked_range<HypernodeID>(ID(0), _num_hypernodes), 0,
                                         [this](const tbb::blocked_range<HypernodeID>& range, HypernodeWeight init) {
                                           HypernodeWeight weight = init;
                                           for (HypernodeID hn = range.begin(); hn < range.end(); ++hn) {
                                             if (nodeIsEnabled(hn)) {
                                               weight += this->_hypernodes[hn].weight();
                                             }
                                           }
                                           return weight;
                                         }, std::plus<>());
  }

} // namespace
