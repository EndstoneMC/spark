#include <cassert>
#include <cstdio>
#include <map>
#include <utility>

#include "core/util/world_region.h"

using namespace spark;  // NOLINT(google-build-using-namespace)

static WorldChunk makeChunk(int x, int z, int entities)  // NOLINT(misc-use-anonymous-namespace)
{
    WorldChunk c;
    c.x = x;
    c.z = z;
    c.total_entities = entities;
    return c;
}

static int countRegions(const std::vector<WorldRegion> &regions)  // NOLINT(misc-use-anonymous-namespace)
{
    return static_cast<int>(regions.size());
}

static int totalChunks(const std::vector<WorldRegion> &regions)  // NOLINT(misc-use-anonymous-namespace)
{
    int total = 0;
    for (const auto &r : regions) {
        total += static_cast<int>(r.chunks.size());
    }
    return total;
}

static int totalEntities(const std::vector<WorldRegion> &regions)  // NOLINT(misc-use-anonymous-namespace)
{
    int total = 0;
    for (const auto &r : regions) {
        total += r.total_entities;
    }
    return total;
}

int main()
{
    // Single chunk with entities
    {
        std::map<std::pair<int, int>, WorldChunk> chunks;
        chunks[{0, 0}] = makeChunk(0, 0, 5);
        auto regions = groupChunksIntoRegions(chunks);
        assert(countRegions(regions) == 1);
        assert(totalChunks(regions) == 1);
        assert(totalEntities(regions) == 5);
    }

    // Two adjacent chunks (4-neighbor)
    {
        std::map<std::pair<int, int>, WorldChunk> chunks;
        chunks[{0, 0}] = makeChunk(0, 0, 3);
        chunks[{1, 0}] = makeChunk(1, 0, 2);
        auto regions = groupChunksIntoRegions(chunks);
        assert(countRegions(regions) == 1);
        assert(totalChunks(regions) == 2);
        assert(totalEntities(regions) == 5);
    }

    // Two diagonally adjacent chunks (8-neighbor -> one region)
    {
        std::map<std::pair<int, int>, WorldChunk> chunks;
        chunks[{0, 0}] = makeChunk(0, 0, 1);
        chunks[{1, 1}] = makeChunk(1, 1, 1);
        auto regions = groupChunksIntoRegions(chunks);
        assert(countRegions(regions) == 1);
        assert(totalChunks(regions) == 2);
    }

    // Two disconnected chunks
    {
        std::map<std::pair<int, int>, WorldChunk> chunks;
        chunks[{0, 0}] = makeChunk(0, 0, 1);
        chunks[{5, 5}] = makeChunk(5, 5, 1);
        auto regions = groupChunksIntoRegions(chunks);
        assert(countRegions(regions) == 2);
        assert(totalChunks(regions) == 2);
    }

    // Negative coordinates
    {
        std::map<std::pair<int, int>, WorldChunk> chunks;
        chunks[{-1, -1}] = makeChunk(-1, -1, 2);
        chunks[{-2, -1}] = makeChunk(-2, -1, 3);
        auto regions = groupChunksIntoRegions(chunks);
        assert(countRegions(regions) == 1);
        assert(totalChunks(regions) == 2);
        assert(totalEntities(regions) == 5);
    }

    // Long horizontal strip
    {
        std::map<std::pair<int, int>, WorldChunk> chunks;
        for (int x = 0; x < 10; ++x) {
            chunks[{x, 0}] = makeChunk(x, 0, 1);
        }
        auto regions = groupChunksIntoRegions(chunks);
        assert(countRegions(regions) == 1);
        assert(totalChunks(regions) == 10);
    }

    // Multiple islands (two separate groups)
    {
        std::map<std::pair<int, int>, WorldChunk> chunks;
        chunks[{0, 0}] = makeChunk(0, 0, 1);
        chunks[{0, 1}] = makeChunk(0, 1, 1);
        chunks[{10, 10}] = makeChunk(10, 10, 1);
        chunks[{10, 11}] = makeChunk(10, 11, 1);
        auto regions = groupChunksIntoRegions(chunks);
        assert(countRegions(regions) == 2);
        assert(totalChunks(regions) == 4);
    }

    // Empty chunks excluded
    {
        std::map<std::pair<int, int>, WorldChunk> chunks;
        chunks[{0, 0}] = makeChunk(0, 0, 5);
        chunks[{1, 0}] = makeChunk(1, 0, 0);  // no entities
        chunks[{2, 0}] = makeChunk(2, 0, 3);
        auto regions = groupChunksIntoRegions(chunks);
        // (0,0) and (2,0) are not adjacent (they'd need (1,0) to connect,
        // but (1,0) has 0 entities and is excluded)
        assert(countRegions(regions) == 2);
        assert(totalChunks(regions) == 2);
        assert(totalEntities(regions) == 8);
    }

    // Empty input
    {
        std::map<std::pair<int, int>, WorldChunk> chunks;
        auto regions = groupChunksIntoRegions(chunks);
        assert(countRegions(regions) == 0);
    }

    // All chunks have 0 entities
    {
        std::map<std::pair<int, int>, WorldChunk> chunks;
        chunks[{0, 0}] = makeChunk(0, 0, 0);
        chunks[{1, 0}] = makeChunk(1, 0, 0);
        auto regions = groupChunksIntoRegions(chunks);
        assert(countRegions(regions) == 0);
    }

    // Large synthetic set: 50x50 grid with entities
    {
        std::map<std::pair<int, int>, WorldChunk> chunks;
        for (int x = 0; x < 50; ++x) {
            for (int z = 0; z < 50; ++z) {
                chunks[{x, z}] = makeChunk(x, z, 1);
            }
        }
        auto regions = groupChunksIntoRegions(chunks);
        assert(countRegions(regions) == 1);
        assert(totalChunks(regions) == 2500);
        assert(totalEntities(regions) == 2500);
    }

    // Large synthetic set: 4 separated islands
    {
        std::map<std::pair<int, int>, WorldChunk> chunks;
        for (int x = 0; x < 10; ++x) {
            for (int z = 0; z < 10; ++z) {
                chunks[{x, z}] = makeChunk(x, z, 1);
                chunks[{x + 100, z}] = makeChunk(x + 100, z, 1);
                chunks[{x, z + 100}] = makeChunk(x, z + 100, 1);
                chunks[{x + 100, z + 100}] = makeChunk(x + 100, z + 100, 1);
            }
        }
        auto regions = groupChunksIntoRegions(chunks);
        assert(countRegions(regions) == 4);
        assert(totalChunks(regions) == 400);
    }

    // Contiguous L-shape via diagonal
    {
        std::map<std::pair<int, int>, WorldChunk> chunks;
        chunks[{0, 0}] = makeChunk(0, 0, 1);
        chunks[{1, 1}] = makeChunk(1, 1, 1);
        chunks[{2, 1}] = makeChunk(2, 1, 1);
        auto regions = groupChunksIntoRegions(chunks);
        assert(countRegions(regions) == 1);
        assert(totalChunks(regions) == 3);
    }

    std::printf("All world region tests passed.\n");
    return 0;
}
