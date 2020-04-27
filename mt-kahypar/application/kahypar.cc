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

#include <iostream>

#include "mt-kahypar/application/command_line_options.h"
#include "mt-kahypar/definitions.h"
#include "mt-kahypar/mt_kahypar.h"
#include "mt-kahypar/partition/registries/register_memory_pool.h"
#include "mt-kahypar/io/hypergraph_io.h"
#include "mt-kahypar/io/sql_plottools_serializer.h"
#include "mt-kahypar/partition/context.h"
#include "mt-kahypar/partition/partitioner.h"

#include "mt-kahypar/utils/profiler.h"
#include "mt-kahypar/utils/randomize.h"
#include "mt-kahypar/utils/timer.h"

int main(int argc, char* argv[]) {
  mt_kahypar::Context context;
  mt_kahypar::processCommandLineInput(context, argc, argv);
  mt_kahypar::io::printBanner(context);

  mt_kahypar::utils::Randomize::instance().setSeed(context.partition.seed);
  if ( context.shared_memory.use_localized_random_shuffle ) {
    mt_kahypar::utils::Randomize::instance().enableLocalizedParallelShuffle(
      context.shared_memory.shuffle_block_size);
  }

  size_t num_available_cpus = mt_kahypar::HardwareTopology::instance().num_cpus();
  if ( num_available_cpus < context.shared_memory.num_threads ) {
    WARNING("There are currently only" << num_available_cpus << "cpus available."
      << "Setting number of threads from" << context.shared_memory.num_threads
      << "to" << num_available_cpus);
    context.shared_memory.num_threads = num_available_cpus;
  }

  // Initialize TBB task arenas on numa nodes
  mt_kahypar::TBBNumaArena::instance(context.shared_memory.num_threads);

  #if !KAHYPAR_ENABLE_NUMA_AWARE_PARTITIONING
  // In case, we have compiled KaHyPar in non NUMA-aware mode we set the membind policy
  // to interleaved allocations in order to distribute allocations evenly across
  // NUMA nodes
  hwloc_cpuset_t cpuset = mt_kahypar::TBBNumaArena::instance().used_cpuset();
  mt_kahypar::parallel::HardwareTopology<>::instance().activate_interleaved_membind_policy(cpuset);
  hwloc_bitmap_free(cpuset);
  #endif

  // Read Hypergraph
  mt_kahypar::Hypergraph hypergraph = mt_kahypar::io::readHypergraphFile<
    mt_kahypar::Hypergraph, mt_kahypar::HypergraphFactory>(
      context.partition.graph_filename, mt_kahypar::TBBNumaArena::GLOBAL_TASK_GROUP);

  // Initialize Memory Pool
  mt_kahypar::register_memory_pool(hypergraph, context);

  if ( context.partition.enable_profiler ) {
    mt_kahypar::utils::Profiler::instance(context.partition.snapshot_interval).start();
  }

  // Partition Hypergraph
  mt_kahypar::HighResClockTimepoint start = std::chrono::high_resolution_clock::now();
  mt_kahypar::PartitionedHypergraph<> partitioned_hypergraph =
    mt_kahypar::partition::Partitioner(context).partition(hypergraph);
  mt_kahypar::HighResClockTimepoint end = std::chrono::high_resolution_clock::now();

  if ( context.partition.enable_profiler ) {
    mt_kahypar::utils::Profiler::instance().stop();
  }

  // Print Stats
  std::chrono::duration<double> elapsed_seconds(end - start);
  mt_kahypar::io::printPartitioningResults(
    partitioned_hypergraph, context, elapsed_seconds);

  if ( context.partition.enable_profiler ) {
    std::cout << mt_kahypar::utils::Profiler::instance() << std::endl;
  }

  if ( context.partition.sp_process_output ) {
    std::cout << mt_kahypar::io::serializer::serialize(
      partitioned_hypergraph, context, elapsed_seconds) << std::endl;
  }

  if (context.partition.write_partition_file) {
    mt_kahypar::io::writePartitionFile(
      partitioned_hypergraph, context.partition.graph_partition_filename);
  }

  mt_kahypar::parallel::MemoryPool::instance().free_memory_chunks();
  mt_kahypar::TBBNumaArena::instance().terminate();
  return 0;
}
