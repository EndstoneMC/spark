#include <chrono>
#if !defined(ENDSTONE_SPARK_ENDSTONE_API_0_11)
#include <limits>
#endif
#include <string>

#include "platform/endstone/adapters.h"

namespace spark::endstone_adapter {

namespace {

#if !defined(ENDSTONE_SPARK_ENDSTONE_API_0_11)
constexpr bool KTileEntityScanSupported = true;
#else
constexpr bool KTileEntityScanSupported = false;
#endif

std::int64_t steadyNowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

WorldGaugeChunkKey chunkKey(const ::endstone::Chunk &chunk)
{
    return {.dimension = chunk.getDimension().getName(), .x = chunk.getX(), .z = chunk.getZ()};
}

#if !defined(ENDSTONE_SPARK_ENDSTONE_API_0_11)
int boundedSize(std::size_t size)
{
    const auto maximum = static_cast<std::size_t>(std::numeric_limits<int>::max());
    return static_cast<int>((std::min)(size, maximum));
}
#endif

}  // namespace

void EndstoneWorldGaugeProvider::init()
{
    if (initialized_) {
        return;
    }
    initialized_ = true;

    plugin_.registerEvent<::endstone::ActorSpawnEvent>(
        [this](::endstone::ActorSpawnEvent &event) { event_adapter_.actorSpawned(event.getActor().getId()); },
        ::endstone::EventPriority::Monitor, true);

    plugin_.registerEvent<::endstone::ActorRemoveEvent>(
        [this](::endstone::ActorRemoveEvent &event) { event_adapter_.actorRemoved(event.getActor().getId()); },
        ::endstone::EventPriority::Monitor);

    plugin_.registerEvent<::endstone::PlayerJoinEvent>(
        [this](::endstone::PlayerJoinEvent &event) { event_adapter_.playerSpawned(event.getPlayer().getId()); },
        ::endstone::EventPriority::Monitor);

    plugin_.registerEvent<::endstone::PlayerQuitEvent>(
        [this](::endstone::PlayerQuitEvent &event) { event_adapter_.playerRemoved(event.getPlayer().getId()); },
        ::endstone::EventPriority::Monitor);

    plugin_.registerEvent<::endstone::ChunkLoadEvent>(
        [this](::endstone::ChunkLoadEvent &event) { event_adapter_.chunkLoaded(chunkKey(event.getChunk())); },
        ::endstone::EventPriority::Monitor);

    plugin_.registerEvent<::endstone::ChunkUnloadEvent>(
        [this](::endstone::ChunkUnloadEvent &event) { event_adapter_.chunkUnloaded(chunkKey(event.getChunk())); },
        ::endstone::EventPriority::Monitor);

    reconcile(false);
    schedule_.start(steadyNowMs());
}

WorldGaugeValues EndstoneWorldGaugeProvider::worldGauges()
{
    const std::int64_t now = steadyNowMs();
    if (schedule_.tileReconcileDue(now, KTileEntityScanSupported)) {
        schedule_.runReconcile(steadyNowMs, [this] { return reconcile(true); }, true, steadyNowMs);
    }
    else if (schedule_.entityReconcileDue(now)) {
        schedule_.runReconcile(steadyNowMs, [this] { return reconcile(false); }, false, steadyNowMs);
    }

    const WorldGaugeCounts counts = event_adapter_.counts();
    return {.entities = counts.entities,
            .tile_entities = counts.tile_entities,
            .chunks = counts.chunks,
            .tile_entities_present = counts.tile_entities_present};
}

bool EndstoneWorldGaugeProvider::reconcile(bool include_tile_entities)
{
    WorldGaugeSnapshot snapshot;
#if !defined(ENDSTONE_SPARK_ENDSTONE_API_0_11)
    bool tile_scan_ok = include_tile_entities;
#else
    bool tile_scan_ok = false;
#endif
    ::endstone::Level *level = server_.getLevel();
    if (level == nullptr) {
        snapshot.available = false;
        event_adapter_.reconcile(snapshot);
        return false;
    }
    for (const auto &dimension : level->getDimensions()) {
        for (const auto &actor : dimension->getActors()) {
            snapshot.actor_ids.push_back(actor->getId());
        }
        for (const auto &chunk : dimension->getLoadedChunks()) {
            const WorldGaugeChunkKey key = chunkKey(*chunk);
            snapshot.chunks.push_back(key);
#if !defined(ENDSTONE_SPARK_ENDSTONE_API_0_11)
            if (!include_tile_entities) {
                continue;
            }
            try {
                snapshot.tile_entities.push_back({.chunk = key, .count = boundedSize(chunk->getBlockActors().size())});
            }
            catch (...) {
                tile_scan_ok = false;
            }
#else
            (void)include_tile_entities;
#endif
        }
    }
    for (const auto &player : server_.getOnlinePlayers()) {
        snapshot.player_ids.push_back(player->getId());
    }
    snapshot.tile_entities_complete = tile_scan_ok;
    event_adapter_.reconcile(snapshot);
    return snapshot.tile_entities_complete;
}

}  // namespace spark::endstone_adapter
