//
// Created by mlaupichler on 30.04.21.
//

#include "gmock/gmock.h"

#include <atomic>
#include "mt-kahypar/partition/factories.h"
#include <mt-kahypar/partition/refinement/i_refiner.h>

#include "mt-kahypar/definitions.h"
#include "mt-kahypar/datastructures/dynamic_hypergraph.h"
#include "mt-kahypar/datastructures/dynamic_hypergraph_factory.h"
#include "mt-kahypar/datastructures/partitioned_hypergraph.h"
#include "mt-kahypar/datastructures/asynch/array_lock_manager.h"
#include "mt-kahypar/datastructures/asynch/asynch_contraction_pool.h"
#include "mt-kahypar/partition/metrics.h"
#include "mt-kahypar/utils/randomize.h"

#include "smoke_test_common.h"

namespace mt_kahypar {
    namespace ds {

        DynamicHypergraph
        simulateAsyncNLevel(DynamicHypergraph &hypergraph, DynamicPartitionedHypergraph &partitioned_hypergraph,
                       const BatchVector &contraction_batches, const bool parallel) {

            auto timer_key = [&](const std::string &key) {
                if (parallel) {
                    return key + "_parallel";
                } else {
                    return key;
                }
            };

            parallel::scalable_vector <parallel::scalable_vector<ParallelHyperedge>> removed_hyperedges;
            for (size_t i = 0; i < contraction_batches.size(); ++i) {
                utils::Timer::instance().start_timer(timer_key("contractions"), "Contractions");
                const parallel::scalable_vector <Memento> &contractions = contraction_batches[i];
                if (parallel) {
                    tbb::parallel_for(0UL, contractions.size(), [&](const size_t j) {
                        const Memento &memento = contractions[j];
                        hypergraph.registerContraction(memento.u, memento.v);
                        hypergraph.contract(memento.v);
                    });
                } else {
                    for (size_t j = 0; j < contractions.size(); ++j) {
                        const Memento &memento = contractions[j];
                        hypergraph.registerContraction(memento.u, memento.v);
                        hypergraph.contract(memento.v);
                    }
                }
                utils::Timer::instance().stop_timer(timer_key("contractions"));

                utils::Timer::instance().start_timer(timer_key("remove_parallel_nets"), "Parallel Net Detection");
                removed_hyperedges.emplace_back(hypergraph.removeSinglePinAndParallelHyperedges());
                utils::Timer::instance().stop_timer(timer_key("remove_parallel_nets"));
            }

            utils::Timer::instance().start_timer(timer_key("copy_coarsest_hypergraph"), "Copy Coarsest Hypergraph");
            DynamicHypergraph coarsest_hypergraph;
            if (parallel) {
                coarsest_hypergraph = hypergraph.copy(TBBNumaArena::GLOBAL_TASK_GROUP);
            } else {
                coarsest_hypergraph = hypergraph.copy();
            }
            utils::Timer::instance().stop_timer(timer_key("copy_coarsest_hypergraph"));


            utils::Timer::instance().start_timer(timer_key("initial_partition"), "Initial Partition");

            {
                utils::Timer::instance().start_timer(timer_key("compactify_hypergraph"), "Compactify Hypergraph");
                auto res = DynamicHypergraphFactory::compactify(TBBNumaArena::GLOBAL_TASK_GROUP, hypergraph);
                DynamicHypergraph &compactified_hg = res.first;
                auto &hn_mapping = res.second;
                DynamicPartitionedHypergraph compactified_phg(
                        partitioned_hypergraph.k(), TBBNumaArena::GLOBAL_TASK_GROUP, compactified_hg);
                utils::Timer::instance().stop_timer(timer_key("compactify_hypergraph"));

                utils::Timer::instance().start_timer(timer_key("generate_random_partition"),
                                                     "Generate Random Partition");
                generateRandomPartition(compactified_phg);
                utils::Timer::instance().stop_timer(timer_key("generate_random_partition"));

                utils::Timer::instance().start_timer(timer_key("project_partition"), "Project Partition");
                partitioned_hypergraph.doParallelForAllNodes([&](const HypernodeID hn) {
                    partitioned_hypergraph.setOnlyNodePart(hn, compactified_phg.partID(hn_mapping[hn]));
                });
                utils::Timer::instance().stop_timer(timer_key("project_partition"));
            }

            utils::Timer::instance().start_timer(timer_key("initialize_partition"), "Initialize Partition");
            partitioned_hypergraph.initializePartition(TBBNumaArena::GLOBAL_TASK_GROUP);
            utils::Timer::instance().stop_timer(timer_key("initialize_partition"));

            utils::Timer::instance().start_timer(timer_key("initialize_gain_cache"),
                                                 "Initialize Initialize Gain Cache");
            partitioned_hypergraph.initializeGainCache();
            utils::Timer::instance().stop_timer(timer_key("initialize_gain_cache"));

            utils::Timer::instance().stop_timer(timer_key("initial_partition"));

            utils::Timer::instance().start_timer(timer_key("create_uncontraction_pools"), "Create Uncontraction Pools");
            auto versionedPools = hypergraph.createUncontractionGroupPoolsForVersions();
            utils::Timer::instance().stop_timer(timer_key("create_uncontraction_pools"));

            utils::Timer::instance().start_timer(timer_key("create_lock_manager"), "Create Lock Manager");
            auto lockManager = new ArrayLockManager<HypernodeID,ContractionGroupID>(hypergraph.initialNumNodes(),invalidGroupID);
            utils::Timer::instance().stop_timer(timer_key("create_lock_manager"));

            utils::Timer::instance().start_timer(timer_key("async_uncontractions"), "Asynchronous Uncontractions");
            while (!versionedPools.empty()) {
                TreeGroupPool *pool = versionedPools.back().get();

//                partitioned_hypergraph.uncontractUsingGroupPool(pool, lockManager, NOOP_LOCALIZED_REFINEMENT_FUNC);

                while (pool->hasActive()) {
                    auto groupID = pool->pickAnyActiveID();
                    auto group = pool->group(groupID);

                    // Attempt to acquire locks for representative and contracted nodes in the group. If any of the locks cannot be
                    // acquired, revert to previous state and attempt to pick an id again
                    auto range = IteratorRange(ds::GroupNodeIDIterator::getAtBegin(group), ds::GroupNodeIDIterator::getAtEnd(group));
                    bool acquired = lockManager->tryToAcquireMultipleLocks(range, groupID);
                    if (!acquired) {
                        pool->reactivate(groupID);
                        continue;
                    }
                    ASSERT(acquired);

                    partitioned_hypergraph.uncontract(group);

                    ASSERT(lockManager->isHeldBy(group.getRepresentative(),groupID) && "Representative of the group is not locked by the group id!");
                    ASSERT(std::all_of(ds::ContractionToNodeIDIteratorAdaptor(group.begin()),
                                       ds::ContractionToNodeIDIteratorAdaptor(group.end()),
                                       [&](const HypernodeID& hn) {return lockManager->isHeldBy(hn,groupID);})
                           && "Not all contracted nodes in the group are locked by the group id!");

                    // Extract refinement seeds and release locks for nodes that are not seeds
                    auto begin = ds::GroupNodeIDIterator::getAtBegin(group);
                    auto end = ds::GroupNodeIDIterator::getAtEnd(group);
                    DynamicPartitionedHypergraph::ReleaseFunction release_func = [&](const HypernodeID hn){
                        lockManager->strongReleaseLock(hn, groupID);
                    };
                    auto refinement_nodes = partitioned_hypergraph.extractBorderNodesAndReleaseOthers(begin,end, release_func);

                    // No refinement if refinement nodes are empty (all the locks are released already)
                    if (refinement_nodes.empty()) continue;

                    // No refinement

                    // Release locks of seed nodes
                    auto releaseRange = IteratorRange(refinement_nodes.begin(), refinement_nodes.end());
                    lockManager->strongReleaseMultipleLocks(releaseRange,groupID);

                    pool->activateSuccessors(groupID);
                }

                versionedPools.pop_back();

                if (!removed_hyperedges.empty()) {
                    utils::Timer::instance().start_timer(timer_key("restore_parallel_nets"), "Restore Parallel Nets");
                    partitioned_hypergraph.restoreSinglePinAndParallelNets(removed_hyperedges.back());
                    removed_hyperedges.pop_back();
                    utils::Timer::instance().stop_timer(timer_key("restore_parallel_nets"));
                }
            }
            utils::Timer::instance().stop_timer(timer_key("async_uncontractions"));

            return coarsest_hypergraph;
        }

        TEST(AAsyncNlevel, SimulatesContractionsAndAsynchPoolUncontractions) {
            const HypernodeID num_hypernodes = 10000;
            const HypernodeID num_hyperedges = 10000;
            const HypernodeID max_edge_size = 30;
            const HypernodeID num_contractions = 9950;
            const bool show_timings = false;
            const bool debug = true;

            if (debug) LOG << "Generate Random Hypergraph";
            DynamicHypergraph original_hypergraph = generateRandomHypergraph(num_hypernodes, num_hyperedges,
                                                                             max_edge_size);
            DynamicHypergraph sequential_hg = original_hypergraph.copy(TBBNumaArena::GLOBAL_TASK_GROUP);
            DynamicPartitionedHypergraph sequential_phg(4, TBBNumaArena::GLOBAL_TASK_GROUP, sequential_hg);
            DynamicHypergraph parallel_hg = original_hypergraph.copy(TBBNumaArena::GLOBAL_TASK_GROUP);
            DynamicPartitionedHypergraph parallel_phg(4, TBBNumaArena::GLOBAL_TASK_GROUP, parallel_hg);

            if (debug) LOG << "Determine random contractions";
            BatchVector contractions = generateRandomContractions(num_hypernodes, num_contractions);

            utils::Timer::instance().clear();

            if (debug) LOG << "Simulate async n-Level sequentially";
            utils::Timer::instance().start_timer("sequential_n_level", "Sequential n-Level");
            DynamicHypergraph coarsest_sequential_hg = simulateAsyncNLevel(sequential_hg, sequential_phg, contractions,
                                                                      false);
            utils::Timer::instance().stop_timer("sequential_n_level");

//            if (debug) LOG << "Simulate n-Level in parallel";
//            utils::Timer::instance().start_timer("parallel_n_level", "Parallel n-Level");
//            DynamicHypergraph coarsest_parallel_hg = simulateNLevel(parallel_hg, parallel_phg, contractions, true);
//            utils::Timer::instance().stop_timer("parallel_n_level");

            if (debug) LOG << "Verify equality of hypergraphs";
//            verifyEqualityOfHypergraphs(coarsest_sequential_hg, coarsest_parallel_hg);
            verifyEqualityOfHypergraphs(original_hypergraph, sequential_hg);
//            verifyEqualityOfHypergraphs(original_hypergraph, parallel_hg);

            if (debug) LOG << "Verify gain cache of hypergraphs";
            verifyGainCache(sequential_phg);
//            verifyGainCache(parallel_phg);

            if (debug) LOG << "Verify number of incident cut hyperedges";
            verifyNumIncidentCutHyperedges(sequential_phg);
//            verifyNumIncidentCutHyperedges(parallel_phg);

            if (show_timings) {
                LOG << utils::Timer::instance(true);
            }
        }

    } // namespace ds
} // namespace mt_kahypar