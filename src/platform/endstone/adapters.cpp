#include "platform/endstone/adapters.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/profiler/profiler.h"
#include "core/stats/ping_statistics.h"
#include "core/stats/system_stats.h"
#include "core/util/format.h"
#include "core/util/world_region.h"
#include "core/metadata/server_properties.h"

namespace spark::endstone_adapter {

using endstone::ColorFormat;

// --- EndstoneDispatcher ---

void EndstoneDispatcher::runOnMainThread(std::function<void()> task)
{
    server_.getScheduler().runTask(plugin_, std::move(task));
}

// --- EndstoneMetadataProvider ---

namespace {

int floorDiv(int value, int divisor)
{
    int quotient = value / divisor;
    int remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

}  // namespace

void EndstoneMetadataProvider::gatherServerMetadata(ExportContext &ctx,
                                                     std::int64_t now_ms)
{
    ctx.endstone_version = server_.getVersion();
    ctx.minecraft_version = server_.getMinecraftVersion();
    ctx.bds_executable_sha256 = bds_executable_sha256_;
    ctx.player_count = static_cast<long>(server_.getOnlinePlayers().size());
    ctx.online_mode = server_.getOnlineMode() ? 2 : 1;
    {
        std::int64_t start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    server_.getStartTime().time_since_epoch())
                                    .count();
        ctx.uptime_ms = now_ms - start_ms;
    }

    ctx.plugins.clear();
    for (endstone::Plugin *plugin : server_.getPluginManager().getPlugins()) {
        const endstone::PluginDescription &desc = plugin->getDescription();
        std::string author;
        for (const std::string &a : desc.getAuthors()) {
            author += (author.empty() ? "" : ", ") + a;
        }
        ctx.plugins.push_back({desc.getName(), desc.getVersion(), author, desc.getDescription()});
    }

    // Parse server.properties with a strict allowlist for performance
    // diagnostics. Sensitive fields are never included.
    ctx.server_configurations =
        spark::parseServerProperties(std::filesystem::current_path() / "server.properties");
}

void EndstoneMetadataProvider::gatherWorldMetadata(ExportContext &ctx)
{
    ctx.world = WorldInfo{};
    if (endstone::Level *level = server_.getLevel()) {
        for (endstone::Dimension *dimension : level->getDimensions()) {
            std::map<std::pair<int, int>, WorldChunk> chunks;
            for (const auto &chunk : dimension->getLoadedChunks()) {
                if (chunk) {
                    int x = chunk->getX();
                    int z = chunk->getZ();
                    chunks.try_emplace({x, z}, WorldChunk{x, z});
                }
            }
            if (chunks.empty()) {
                continue;
            }

            for (endstone::Actor *actor : dimension->getActors()) {
                if (!actor) {
                    continue;
                }
                endstone::Location location = actor->getLocation();
                int chunk_x = floorDiv(location.getBlockX(), 16);
                int chunk_z = floorDiv(location.getBlockZ(), 16);
                auto it = chunks.find({chunk_x, chunk_z});
                if (it == chunks.end()) {
                    continue;
                }
                it->second.total_entities++;
                it->second.entity_counts[actor->getType()]++;
            }

            WorldEntry world;
            world.name = dimension->getName();
            auto regions = groupChunksIntoRegions(chunks);
            for (const auto &region : regions) {
                world.total_entities += region.total_entities;
                for (const auto &chunk : region.chunks) {
                    for (const auto &[type, count] : chunk.entity_counts) {
                        ctx.world.entity_counts[type] += count;
                    }
                }
                world.regions.push_back(std::move(region));
            }
            ctx.world.total_entities += world.total_entities;
            ctx.world.worlds.push_back(std::move(world));
        }
        ctx.world.present = !ctx.world.worlds.empty();
    }
}

std::int64_t EndstoneMetadataProvider::serverUptimeSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now() - server_.getStartTime())
        .count();
}

long EndstoneMetadataProvider::playerCount()
{
    return static_cast<long>(server_.getOnlinePlayers().size());
}

PlayerPingProvider *EndstoneMetadataProvider::playerPingProvider()
{
    if (!ping_provider_) {
        ping_provider_ = std::make_unique<EndstonePlayerPingProvider>(server_);
    }
    return ping_provider_.get();
}

std::pair<int, int> EndstoneMetadataProvider::worldGauges()
{
    if (!world_gauges_) {
        world_gauges_ = std::make_unique<EndstoneWorldGaugeProvider>(plugin_, server_);
        world_gauges_->init();
    }
    return world_gauges_->worldGauges();
}

// --- EndstoneNotifier ---

void EndstoneNotifier::notify(const std::string &sender_name, const std::string &text)
{
    plugin_.getLogger().info("{}", text);
    if (disable_broadcast_) {
        auto player = server_.getPlayer(sender_name);
        if (player) {
            player->sendMessage("{}[spark] {}{}", ColorFormat::Gold, ColorFormat::Reset, text);
        }
    } else {
        for (auto *player : server_.getOnlinePlayers()) {
            if (player && player->hasPermission("endstone.command.spark")) {
                player->sendMessage("{}[spark] {}{}", ColorFormat::Gold, ColorFormat::Reset, text);
            }
        }
    }
}

// --- EndstonePlayerPingProvider ---

std::map<std::string, int> EndstonePlayerPingProvider::poll()
{
    std::map<std::string, int> result;
    for (const auto &player : server_.getOnlinePlayers()) {
        if (player) {
            result.emplace(player->getName(),
                           static_cast<int>(player->getPing().count()));
        }
    }
    return result;
}

// --- EndstoneWorldGaugeProvider ---

namespace {

constexpr std::int64_t kReconcileIntervalMs = 30000;

std::int64_t steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void EndstoneWorldGaugeProvider::init()
{
    if (initialized_) {
        return;
    }
    initialized_ = true;

    plugin_.registerEvent<::endstone::ActorSpawnEvent>(
        [this](::endstone::ActorSpawnEvent &event) {
            if (event.getActor().asPlayer() == nullptr) {
                entity_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }, ::endstone::EventPriority::Monitor);

    plugin_.registerEvent<::endstone::ActorRemoveEvent>(
        [this](::endstone::ActorRemoveEvent &event) {
            if (event.getActor().asPlayer() == nullptr) {
                entity_count_.fetch_sub(1, std::memory_order_relaxed);
            }
        }, ::endstone::EventPriority::Monitor);

    plugin_.registerEvent<::endstone::ChunkLoadEvent>(
        [this](::endstone::ChunkLoadEvent &) {
            chunk_count_.fetch_add(1, std::memory_order_relaxed);
        }, ::endstone::EventPriority::Monitor);

    plugin_.registerEvent<::endstone::ChunkUnloadEvent>(
        [this](::endstone::ChunkUnloadEvent &) {
            chunk_count_.fetch_sub(1, std::memory_order_relaxed);
        }, ::endstone::EventPriority::Monitor);

    reconcile();
}

std::pair<int, int> EndstoneWorldGaugeProvider::worldGauges()
{
    std::int64_t now = steadyNowMs();
    if (now - last_reconcile_steady_ms_ >= kReconcileIntervalMs) {
        reconcile();
    }
    return {entity_count_.load(std::memory_order_relaxed),
            chunk_count_.load(std::memory_order_relaxed)};
}

void EndstoneWorldGaugeProvider::reconcile()
{
    last_reconcile_steady_ms_ = steadyNowMs();

    int entities = 0;
    int chunks = 0;
    if (::endstone::Level *level = server_.getLevel()) {
        for (::endstone::Dimension *dimension : level->getDimensions()) {
            for (::endstone::Actor *actor : dimension->getActors()) {
                if (actor && actor->asPlayer() == nullptr) {
                    ++entities;
                }
            }
            chunks += static_cast<int>(dimension->getLoadedChunks().size());
        }
    }
    entity_count_.store(entities, std::memory_order_relaxed);
    chunk_count_.store(chunks, std::memory_order_relaxed);
}

}  // namespace spark::endstone_adapter
