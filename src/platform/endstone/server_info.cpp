#include "platform/endstone/server_info.h"

#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <endstone/endstone.hpp>

#include "sampler/profiler.h"
#include "stats/system_stats.h"

namespace spark::endstone_adapter {

namespace {

int floorDiv(int value, int divisor)
{
    int quotient = value / divisor;
    int remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

}  // namespace

void gatherServerInfo(ExportContext &ctx, ::endstone::Server &server,
                      const std::string &bds_executable_sha256,
                      std::int64_t now_ms)
{
    ctx.endstone_version = server.getVersion();
    ctx.minecraft_version = server.getMinecraftVersion();
    ctx.bds_executable_sha256 = bds_executable_sha256;
    ctx.player_count = static_cast<long>(server.getOnlinePlayers().size());
    ctx.online_mode = server.getOnlineMode() ? 2 : 1;
    {
        std::int64_t start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    server.getStartTime().time_since_epoch())
                                    .count();
        ctx.uptime_ms = now_ms - start_ms;
    }

    ctx.plugins.clear();
    for (endstone::Plugin *plugin : server.getPluginManager().getPlugins()) {
        const endstone::PluginDescription &desc = plugin->getDescription();
        std::string author;
        for (const std::string &a : desc.getAuthors()) {
            author += (author.empty() ? "" : ", ") + a;
        }
        ctx.plugins.push_back({desc.getName(), desc.getVersion(), author, desc.getDescription()});
    }
}

void gatherWorldInfo(ExportContext &ctx, ::endstone::Server &server)
{
    ctx.world = spark::WorldInfo{};
    if (endstone::Level *level = server.getLevel()) {
        for (endstone::Dimension *dimension : level->getDimensions()) {
            std::map<std::pair<int, int>, spark::WorldChunk> chunks;
            for (const auto &chunk : dimension->getLoadedChunks()) {
                if (chunk) {
                    int x = chunk->getX();
                    int z = chunk->getZ();
                    chunks.try_emplace({x, z}, spark::WorldChunk{x, z});
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

            spark::WorldEntry world;
            world.name = dimension->getName();
            std::map<std::pair<int, int>, spark::WorldRegion> regions;
            for (auto &[coordinate, chunk] : chunks) {
                auto region_coordinate = std::pair{floorDiv(coordinate.first, 32),
                                                   floorDiv(coordinate.second, 32)};
                spark::WorldRegion &region = regions[region_coordinate];
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

}  // namespace spark::endstone_adapter
