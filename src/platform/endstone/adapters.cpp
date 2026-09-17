#include "platform/endstone/adapters.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if !defined(ENDSTONE_SPARK_ENDSTONE_API_0_11)
#include <array>
#include <charconv>
#include <variant>
#endif

#include "core/metadata/behavior_packs.h"
#if !defined(ENDSTONE_SPARK_ENDSTONE_API_0_11)
#include "core/metadata/gamerule_semantics.h"
#endif
#include "core/metadata/server_properties.h"
#include "core/stats/ping_statistics.h"
#include "core/stats/system_stats.h"
#include "core/util/format.h"
#include "core/util/monotonic_time.h"
#include "core/util/world_region.h"
#include "platform/endstone/native_plugin_attribution.h"

namespace spark::endstone_adapter {

using endstone::ColorFormat;

namespace {

std::string formatPlayerMessage(const std::string &message, const std::string &body_color = ColorFormat::Reset)
{
    std::string formatted;
    formatted.reserve(message.size() + 32);
    formatted += ColorFormat::DarkGray;
    formatted += '[';
    formatted += ColorFormat::Yellow;
    formatted += "\xE2\x9A\xA1";  // U+26A1 HIGH VOLTAGE SIGN (⚡)
    formatted += ColorFormat::DarkGray;
    formatted += "] ";
    formatted += body_color;
    formatted += message;
    return formatted;
}

}  // namespace

// --- EndstoneCommandSender ---

void EndstoneCommandSender::sendImpl(const std::string &message)
{
    if (isPlayer()) {
        sender_.sendMessage(formatPlayerMessage(message));
        return;
    }
    sender_.sendMessage(message);
}

void EndstoneCommandSender::errorImpl(const std::string &message)
{
    if (isPlayer()) {
        sender_.sendErrorMessage(formatPlayerMessage(message, ColorFormat::Red));
        return;
    }
    sender_.sendErrorMessage(message);
}

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

#if !defined(ENDSTONE_SPARK_ENDSTONE_API_0_11)
std::string formatGameRuleValue(const endstone::GameRuleValue &value)
{
    if (const auto *bool_value = std::get_if<bool>(&value)) {
        return *bool_value ? "true" : "false";
    }
    if (const auto *int_value = std::get_if<int>(&value)) {
        return std::to_string(*int_value);
    }
    const auto *float_value = std::get_if<float>(&value);
    if (float_value == nullptr) {
        return {};
    }

    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), *float_value, std::chars_format::general);
    if (result.ec != std::errc{}) {
        return {};
    }
    return {buffer, result.ptr};
}
#endif

#if !defined(ENDSTONE_SPARK_ENDSTONE_API_0_11)
constexpr std::array KBooleanGameRules{
    endstone::GameRule::CommandBlockOutput,
    endstone::GameRule::CommandBlocksEnabled,
    endstone::GameRule::DoDayLightCycle,
    endstone::GameRule::DoEntityDrops,
    endstone::GameRule::DoFireTick,
    endstone::GameRule::DoImmediateRespawn,
    endstone::GameRule::DoInsomnia,
    endstone::GameRule::DoLimitedCrafting,
    endstone::GameRule::DoMobLoot,
    endstone::GameRule::DoMobSpawning,
    endstone::GameRule::DoTileDrops,
    endstone::GameRule::DoWeatherCycle,
    endstone::GameRule::DrowningDamage,
    endstone::GameRule::FallDamage,
    endstone::GameRule::FireDamage,
    endstone::GameRule::FreezeDamage,
    endstone::GameRule::KeepInventory,
    endstone::GameRule::LocatorBar,
    endstone::GameRule::MobGriefing,
    endstone::GameRule::NaturalRegeneration,
    endstone::GameRule::ProjectilesCanBreakBlocks,
    endstone::GameRule::Pvp,
    endstone::GameRule::RecipesUnlock,
    endstone::GameRule::RespawnBlocksExplode,
    endstone::GameRule::SendCommandFeedback,
    endstone::GameRule::ShowBorderEffect,
    endstone::GameRule::ShowCoordinates,
    endstone::GameRule::ShowDaysPlayed,
    endstone::GameRule::ShowDeathMessages,
    endstone::GameRule::ShowRecipeMessages,
    endstone::GameRule::ShowTags,
    endstone::GameRule::TntExplodes,
    endstone::GameRule::TntExplosionDropDecay,
};

constexpr std::array KIntegerGameRules{
    endstone::GameRule::FunctionCommandLimit,      endstone::GameRule::MaxCommandChainLength,
    endstone::GameRule::PlayersSleepingPercentage, endstone::GameRule::PlayerWaypoints,
    endstone::GameRule::RandomTickSpeed,           endstone::GameRule::SpawnRadius,
};

template <typename T, std::size_t N>
void appendGameRules(WorldInfo &world, endstone::Level &level, const std::string &world_name,
                     std::string_view minecraft_version, const std::array<endstone::GameRuleId<T>, N> &rules)
{
    for (const auto id : rules) {
        const std::string rule_name(id.getKey());
        if (!shouldExportGameRule(rule_name, minecraft_version) || !level.hasGameRule(id)) {
            continue;
        }

        GameRuleInfo info;
        info.name = canonicalGameRuleName(rule_name);
        if (const auto default_value = resolveGameRuleDefault(rule_name, minecraft_version);
            default_value.has_value()) {
            info.default_value = normalizeGameRuleSemanticValue(rule_name, *default_value);
        }
        const std::string raw_value = formatGameRuleValue(endstone::GameRuleValue{level.getGameRule(id)});
        info.world_values[world_name] = normalizeGameRuleSemanticValue(rule_name, raw_value);
        world.game_rules.push_back(std::move(info));
    }
}
#endif

}  // namespace

void EndstoneMetadataProvider::gatherServerMetadata(ServerMetadata &metadata, std::int64_t now_ms)
{
    metadata.endstone_version = server_.getVersion();
    metadata.minecraft_version = server_.getMinecraftVersion();
    metadata.bds_executable_sha256 = bds_executable_sha256_;
    metadata.player_count = static_cast<std::int64_t>(server_.getOnlinePlayers().size());
    metadata.online_mode = server_.getOnlineMode() ? 2 : 1;
    {
        std::int64_t start_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(server_.getStartTime().time_since_epoch()).count();
        metadata.uptime_ms = now_ms - start_ms;
    }

    metadata.plugins.clear();
    for (endstone::Plugin *plugin : server_.getPluginManager().getPlugins()) {
        const endstone::PluginDescription &desc = plugin->getDescription();
        std::string author;
        for (const std::string &a : desc.getAuthors()) {
            author += (author.empty() ? "" : ", ") + a;
        }
        metadata.plugins.push_back({.name = desc.getName(),
                                    .version = desc.getVersion(),
                                    .author = author,
                                    .description = desc.getDescription()});
    }

    // Strict allowlist parse; serialized as a JSON object string for server_configurations.
    auto properties = spark::parseServerProperties(std::filesystem::current_path() / "server.properties");
    if (!properties.empty()) {
        metadata.server_configurations.clear();
        metadata.server_configurations["server.properties"] = spark::serverPropertiesToJsonString(properties);
    }
}

std::vector<NativePluginSource> EndstoneMetadataProvider::nativePluginSources()
{
    const auto plugins = server_.getPluginManager().getPlugins();
    std::vector<NativePluginSource> sources;
    sources.reserve(plugins.size());
    for (endstone::Plugin *plugin : plugins) {
        const endstone::PluginDescription &desc = plugin->getDescription();
        const auto identity = identifyNativePluginModule(*plugin);
        if (identity.has_value()) {
            sources.push_back({.module_base = identity->module_base,
                               .module_path = identity->module_path,
                               .source_id = desc.getName()});
        }
    }
    return sources;
}

void EndstoneMetadataProvider::gatherWorldMetadata(WorldInfo &world, std::string_view minecraft_version)
{
    world = WorldInfo{};
    endstone::Level *level = server_.getLevel();
    if (level == nullptr) {
        return;
    }
    for (const auto &dimension : level->getDimensions()) {
        std::map<std::pair<int, int>, WorldChunk> chunks;
        for (const auto &chunk : dimension->getLoadedChunks()) {
            int x = chunk->getX();
            int z = chunk->getZ();
            chunks.try_emplace({x, z}, WorldChunk{.x = x, .z = z});
        }
        if (chunks.empty()) {
            continue;
        }

        for (const auto &actor : dimension->getActors()) {
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

        WorldEntry entry;
        entry.name = dimension->getName();
        auto regions = groupChunksIntoRegions(chunks);
        for (const auto &region : regions) {
            entry.total_entities += region.total_entities;
            for (const auto &chunk : region.chunks) {
                for (const auto &[type, count] : chunk.entity_counts) {
                    world.entity_counts[type] += count;
                }
            }
            entry.regions.push_back(region);
        }
        world.total_entities += entry.total_entities;
        world.worlds.push_back(std::move(entry));
    }

    const std::string world_name = level->getName();
#if !defined(ENDSTONE_SPARK_ENDSTONE_API_0_11)
    appendGameRules(world, *level, world_name, minecraft_version, KBooleanGameRules);
    appendGameRules(world, *level, world_name, minecraft_version, KIntegerGameRules);
#else
    (void)minecraft_version;
#endif

    world.data_packs = discoverActiveBehaviorPacks(std::filesystem::current_path(), world_name);
    world.present = !world.worlds.empty() || !world.game_rules.empty() || !world.data_packs.empty();
}

std::int64_t EndstoneMetadataProvider::serverUptimeSeconds()
{
    const auto start_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(server_.getStartTime().time_since_epoch()).count();
    return (monotonicUnixMillis() - start_ms) / 1000;
}

std::int64_t EndstoneMetadataProvider::playerCount()
{
    return static_cast<std::int64_t>(server_.getOnlinePlayers().size());
}

PlayerPingProvider *EndstoneMetadataProvider::playerPingProvider()
{
    if (!ping_provider_) {
        ping_provider_ = std::make_unique<EndstonePlayerPingProvider>(server_);
    }
    return ping_provider_.get();
}

WorldGaugeValues EndstoneMetadataProvider::worldGauges()
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
        auto *player = server_.getPlayer(sender_name);
        if (player) {
            player->sendMessage(formatPlayerMessage(text));
        }
    }
    else {
        for (const auto &player : server_.getOnlinePlayers()) {
            if (player->getName() == sender_name || player->hasPermission("endstone.command.spark")) {
                player->sendMessage(formatPlayerMessage(text));
            }
        }
    }
}

// --- EndstonePlayerPingProvider ---

std::map<std::string, int> EndstonePlayerPingProvider::poll()
{
    std::map<std::string, int> result;
    for (const auto &player : server_.getOnlinePlayers()) {
        // Offline/headless clients can be present before Endstone has safe device metadata.
        // Endstone's ping path may dereference that metadata, so only sample players with
        // an authenticated XUID. The cached XUID accessor is safe for partially-initialised
        // players and normal authenticated players keep the existing ping semantics.
        if (player->getXuid().empty()) {
            continue;
        }
        result.emplace(player->getName(), static_cast<int>(player->getPing().count()));
    }
    return result;
}

}  // namespace spark::endstone_adapter
