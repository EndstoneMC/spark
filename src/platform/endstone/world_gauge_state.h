#ifndef SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_STATE_H
#define SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_STATE_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spark::endstone_adapter {

struct WorldGaugeChunkKey {
    std::string dimension;
    int x = 0;
    int z = 0;

    auto operator<=>(const WorldGaugeChunkKey &) const = default;
};

struct WorldGaugeTileEntityCount {
    WorldGaugeChunkKey chunk;
    int count = 0;
};

struct WorldGaugeSnapshot {
    std::vector<std::int64_t> actor_ids;
    std::vector<std::int64_t> player_ids;
    std::vector<WorldGaugeChunkKey> chunks;
    std::vector<WorldGaugeTileEntityCount> tile_entities;
    bool tile_entities_complete = false;
    bool available = true;
};

struct WorldGaugeCounts {
    int players = 0;
    int entities = 0;
    int tile_entities = 0;
    int chunks = 0;
    bool tile_entities_present = false;

    bool operator==(const WorldGaugeCounts &) const = default;
};

class EndstoneWorldGaugeState {
public:
    void actorSpawned(std::int64_t actor_id)
    {
        std::scoped_lock lock(mutex_);
        actor_ids_.insert(actor_id);
    }

    void actorRemoved(std::int64_t actor_id)
    {
        std::scoped_lock lock(mutex_);
        actor_ids_.erase(actor_id);
    }

    void playerSpawned(std::int64_t player_id)
    {
        std::scoped_lock lock(mutex_);
        player_ids_.insert(player_id);
    }

    void playerRemoved(std::int64_t player_id)
    {
        std::scoped_lock lock(mutex_);
        player_ids_.erase(player_id);
        actor_ids_.erase(player_id);
    }

    void chunkLoaded(WorldGaugeChunkKey key)
    {
        std::scoped_lock lock(mutex_);
        const bool inserted = chunks_.insert(key).second;
        tile_entities_by_chunk_.try_emplace(std::move(key), 0);
        if (inserted) {
            tile_entities_complete_ = false;
        }
    }

    void chunkUnloaded(const WorldGaugeChunkKey &key)
    {
        std::scoped_lock lock(mutex_);
        chunks_.erase(key);
        tile_entities_by_chunk_.erase(key);
    }

    void reconcile(const WorldGaugeSnapshot &snapshot)
    {
        if (!snapshot.available) {
            return;
        }

        std::set<std::int64_t> actors(snapshot.actor_ids.begin(), snapshot.actor_ids.end());
        std::set<std::int64_t> players(snapshot.player_ids.begin(), snapshot.player_ids.end());
        std::set<WorldGaugeChunkKey> chunks(snapshot.chunks.begin(), snapshot.chunks.end());

        std::scoped_lock lock(mutex_);
        actor_ids_ = std::move(actors);
        player_ids_ = std::move(players);

        if (!snapshot.tile_entities_complete && tile_entities_complete_) {
            for (const WorldGaugeChunkKey &chunk : chunks) {
                if (!chunks_.contains(chunk)) {
                    tile_entities_complete_ = false;
                    break;
                }
            }
        }
        chunks_ = std::move(chunks);

        if (snapshot.tile_entities_complete) {
            tile_entities_by_chunk_.clear();
            for (const WorldGaugeTileEntityCount &entry : snapshot.tile_entities) {
                if (chunks_.contains(entry.chunk)) {
                    tile_entities_by_chunk_[entry.chunk] = (std::max)(0, entry.count);
                }
            }
            for (const WorldGaugeChunkKey &chunk : chunks_) {
                tile_entities_by_chunk_.try_emplace(chunk, 0);
            }
            tile_entities_complete_ = true;
        }

        for (auto it = tile_entities_by_chunk_.begin(); it != tile_entities_by_chunk_.end();) {
            if (!chunks_.contains(it->first)) {
                it = tile_entities_by_chunk_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    [[nodiscard]] WorldGaugeCounts counts() const
    {
        std::scoped_lock lock(mutex_);
        std::size_t entity_count = actor_ids_.size();
        for (const std::int64_t player_id : player_ids_) {
            if (!actor_ids_.contains(player_id)) {
                ++entity_count;
            }
        }
        std::int64_t tile_entity_count = 0;
        for (const auto &entry : tile_entities_by_chunk_) {
            tile_entity_count += entry.second;
        }
        return {.players = static_cast<int>(player_ids_.size()),
                .entities = static_cast<int>(entity_count),
                .tile_entities = static_cast<int>(
                    (std::min)(tile_entity_count, static_cast<std::int64_t>(std::numeric_limits<int>::max()))),
                .chunks = static_cast<int>(chunks_.size()),
                .tile_entities_present = tile_entities_complete_};
    }

private:
    mutable std::mutex mutex_;
    std::set<std::int64_t> actor_ids_;
    std::set<std::int64_t> player_ids_;
    std::set<WorldGaugeChunkKey> chunks_;
    std::map<WorldGaugeChunkKey, int> tile_entities_by_chunk_;
    bool tile_entities_complete_ = false;
};

}  // namespace spark::endstone_adapter

#endif  // SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_STATE_H
