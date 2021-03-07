/*******************************************************************************
 * This file is part of MT-KaHyPar.
 *
 * Copyright (C) 2021 Noah Wahl <noah.ares@yahoo.de>
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

#include "mt-kahypar/partition/refinement/fm/local_search_scheduler.h"

namespace mt_kahypar {
  template<typename FMStrategy>
  void LocalSearchScheduler<FMStrategy>::performLocalSearches(
    PartitionedHypergraph& phg, size_t numSeeds, size_t numSearches) {
      search_data = vec<SearchData<FMStrategy>>(numSearches, {context, numNodes, sharedData});
      initSearches(phg, numSeeds);
  }

  template<typename FMStrategy>
  void LocalSearchScheduler<FMStrategy>::initSearches(PartitionedHypergraph& phg, size_t numSeeds) {
    auto task = [&](const size_t task_id) {
      auto& fm = ets_fm.local();
      fm.setup(phg, task_id, numSeeds, search_data[task_id]);
    };
  }
}

// instantiate templates
#include "mt-kahypar/partition/refinement/fm/strategies/gain_cache_strategy.h"
#include "mt-kahypar/partition/refinement/fm/strategies/gain_delta_strategy.h"
#include "mt-kahypar/partition/refinement/fm/strategies/recompute_gain_strategy.h"
#include <mt-kahypar/partition/refinement/fm/strategies/gain_cache_on_demand_strategy.h>

namespace mt_kahypar {
  template class LocalSearchScheduler<GainCacheStrategy>;
  template class LocalSearchScheduler<GainDeltaStrategy>;
  template class LocalSearchScheduler<RecomputeGainStrategy>;
  template class LocalSearchScheduler<GainCacheOnDemandStrategy>;
}