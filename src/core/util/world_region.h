#ifndef SPARK_CORE_UTIL_WORLD_REGION_H
#define SPARK_CORE_UTIL_WORLD_REGION_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <utility>
#include <vector>

#include "core/stats/system_stats.h"

namespace spark {

// Groups chunks into connected regions via 8-neighbor BFS, matching upstream spark.
std::vector<WorldRegion> groupChunksIntoRegions(const std::map<std::pair<int, int>, WorldChunk> &chunks);

}  // namespace spark

#endif  // SPARK_CORE_UTIL_WORLD_REGION_H
