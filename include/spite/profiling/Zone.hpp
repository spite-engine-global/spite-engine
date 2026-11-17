#pragma once

#include "Profiling.hpp"

#define SPITE_CONCAT_INNER(a, b) a##b
#define SPITE_CONCAT(a, b) SPITE_CONCAT_INNER(a, b)

#define SPITE_ZONE(name) spite::prof::ZoneScope SPITE_CONCAT(spiteZone_, __LINE__)(name, spite::prof::ZoneKind::Generic)

#define SPITE_FRAME() spite::prof::ZoneScope SPITE_CONCAT(spiteZone_, __LINE__)("Frame", spite::prof::ZoneKind::Frame)

#define SPITE_STAGE(name) spite::prof::ZoneScope SPITE_CONCAT(spiteZone_, __LINE__)(name, spite::prof::ZoneKind::Stage)

#define SPITE_SYSTEM(name)                                                                                             \
    spite::prof::ZoneScope SPITE_CONCAT(spiteZone_, __LINE__)(name, spite::prof::ZoneKind::System)

#define SPITE_ZONE_NAMED(name)                                                                                         \
    spite::prof::ZoneScope SPITE_CONCAT(spiteZone_, __LINE__)(name, spite::prof::ZoneKind::Generic)

#define SPITE_PARALLEL_CALL(var, name, itemCount) spite::prof::ParallelCallScope var(name, itemCount)

#define SPITE_WAIT(name) spite::prof::ZoneScope SPITE_CONCAT(spiteZone_, __LINE__)(name, spite::prof::ZoneKind::Wait)


// attributes allocations (allocsByZone) without a zone record: for hot per-frame sites
#define SPITE_ALLOCATION_ZONE(name)                                                                                    \
    const spite::prof::AllocationZoneScope SPITE_CONCAT(spiteAllocationZone_, __LINE__)(name)

// Per-frame sum.
#define SPITE_COUNTER(name, value) spite::prof::addCounter(name, value)
