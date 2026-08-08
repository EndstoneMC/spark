#include "platform/endstone/adapters.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/profiler/profiler.h"
#include "core/stats/ping_statistics.h"
#include "core/stats/system_stats.h"
#include "core/util/format.h"

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
            std::map<std::pair<int, int>, WorldRegion> regions;
            for (auto &[coordinate, chunk] : chunks) {
                auto region_coordinate = std::pair{floorDiv(coordinate.first, 32),
                                                   floorDiv(coordinate.second, 32)};
                WorldRegion &region = regions[region_coordinate];
                region.total_entities += chunk.total_entities;
                world.total_entities += chunk.total_entities;
                for (const auto &[type, count] : chunk.entity_counts) {
                    ctx.world.entity_counts[type] += count;
                }
                region.chunks.push_back(std::move(chunk));
            }
            for (auto &entry : regions) {
                world.regions.push_back(std::move(entry.second));
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

// --- EndstoneNotifier ---

void EndstoneNotifier::notify(const std::string &sender_name, const std::string &text)
{
    plugin_.getLogger().info("{}", text);
    auto player = server_.getPlayer(sender_name);
    if (player) {
        player->sendMessage("{}[spark] {}{}", ColorFormat::Gold, ColorFormat::Reset, text);
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

}  // namespace spark::endstone_adapter
