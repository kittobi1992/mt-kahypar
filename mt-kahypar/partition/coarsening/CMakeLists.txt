set(CoarseningSources
        deterministic_multilevel_coarsener.cpp
        uncoarsening.cpp
        )

foreach(modtarget IN LISTS TARGETS_WANTING_ALL_SOURCES)
    target_sources(${modtarget} PRIVATE ${CoarseningSources})
endforeach()