#include <cassert>
#include <cstdint>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

#include "application/health/health_report.h"
#include "application/profiler/platform_metadata_capture.h"

namespace {

class Sender final : public spark::CommandSender {
public:
    [[nodiscard]] std::string getName() const override { return "Console"; }
    [[nodiscard]] bool isPlayer() const override { return false; }

    [[nodiscard]] bool contains(const std::string &text) const
    {
        return std::ranges::any_of(
            messages, [&text](const std::string &message) { return message.find(text) != std::string::npos; });
    }

    std::vector<std::string> messages;

private:
    void sendImpl(const std::string &message) override { messages.push_back(message); }
    void errorImpl(const std::string &message) override { messages.push_back(message); }
};

class Metadata final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ServerMetadata &, std::int64_t) override {}
    void gatherWorldMetadata(spark::WorldInfo &world, std::string_view) override
    {
        ++world_gather_calls;
        world.present = true;
        world.total_entities = 8;
        world.entity_counts["minecraft:zombie"] = 3;
    }
    std::int64_t serverUptimeSeconds() override { return 1; }
    std::int64_t playerCount() override { return 2; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }

    int world_gather_calls = 0;
};

class BridgeProvider final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ServerMetadata &metadata, std::int64_t) override
    {
        seen = metadata;
        if (throw_server) {
            throw std::runtime_error("server metadata failed");
        }
        metadata.endstone_version = "provider-endstone";
        metadata.minecraft_version = "provider-minecraft";
        metadata.bds_executable_sha256 = "provider-hash";
        metadata.player_count = 7;
        metadata.online_mode = 2;
        metadata.uptime_ms = 9000;
        metadata.plugins = {{.name = "provider-plugin"}};
        metadata.server_configurations = {{"provider", "{}"}};
    }
    void gatherWorldMetadata(spark::WorldInfo &, std::string_view) override {}
    std::int64_t serverUptimeSeconds() override { return 0; }
    std::int64_t playerCount() override { return 0; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }

    spark::ServerMetadata seen;
    bool throw_server = false;
};

spark::NetworkRateValues rate(double mean)
{
    spark::NetworkRateValues value;
    value.present = true;
    value.mean = mean;
    return value;
}

}  // namespace

int main()
{
    {
        BridgeProvider bridge;
        spark::ExportContext context;
        context.endstone_version = "seed-endstone";
        context.minecraft_version = "seed-minecraft";
        context.bds_executable_sha256 = "seed-hash";
        context.player_count = 3;
        context.online_mode = 1;
        context.uptime_ms = 4000;
        context.plugins = {{.name = "seed-plugin"}};
        context.server_configurations = {{"seed", "{}"}};
        context.comment = "untouched";
        spark::gatherPlatformServerMetadata(bridge, context, 1000);
        assert(bridge.seen.endstone_version == "seed-endstone");
        assert(bridge.seen.minecraft_version == "seed-minecraft");
        assert(bridge.seen.bds_executable_sha256 == "seed-hash");
        assert(bridge.seen.player_count == 3);
        assert(bridge.seen.online_mode == 1);
        assert(bridge.seen.uptime_ms == 4000);
        assert(bridge.seen.plugins.size() == 1 && bridge.seen.plugins.front().name == "seed-plugin");
        assert(bridge.seen.server_configurations.at("seed") == "{}");
        assert(context.endstone_version == "provider-endstone");
        assert(context.minecraft_version == "provider-minecraft");
        assert(context.bds_executable_sha256 == "provider-hash");
        assert(context.player_count == 7);
        assert(context.online_mode == 2);
        assert(context.uptime_ms == 9000);
        assert(context.plugins.size() == 1 && context.plugins.front().name == "provider-plugin");
        assert(context.server_configurations.at("provider") == "{}");
        assert(context.comment == "untouched");

        bridge.throw_server = true;
        bool threw = false;
        try {
            spark::gatherPlatformServerMetadata(bridge, context, 1000);
        }
        catch (const std::runtime_error &) {
            threw = true;
        }
        assert(threw);
    }

    spark::StatisticsService statistics;
    Metadata metadata;
    spark::NetworkInterfaceSnapshot active;
    active.rx_bytes_per_second = rate(2048.0);
    active.rx_packets_per_second = rate(12.0);
    active.tx_bytes_per_second = rate(0.0);
    active.tx_packets_per_second = rate(0.0);
    spark::NetworkInterfaceSnapshot idle;
    idle.rx_bytes_per_second = rate(0.0);
    idle.rx_packets_per_second = rate(0.0);
    idle.tx_bytes_per_second = rate(0.0);
    idle.tx_packets_per_second = rate(0.0);

    const std::map<std::string, spark::NetworkInterfaceSnapshot> snapshots{{"active", active}, {"idle", idle}};
    Sender summary;
    spark::showHealthReport(summary, statistics, metadata, snapshots, false, false);
    assert(summary.contains("2.00 KiB/s"));
    assert(summary.contains("12 pps"));
    assert(summary.contains("active RX"));
    assert(!summary.contains("active TX"));
    assert(!summary.contains("idle RX"));

    Sender detailed;
    spark::showHealthReport(detailed, statistics, metadata, snapshots, true, true);
    assert(detailed.contains("active TX"));
    assert(detailed.contains("idle RX"));
    assert(detailed.contains("idle TX"));

    const spark::HealthData data = spark::captureHealthData(statistics, metadata, "Console", false, 1000, {}, {});
    assert(metadata.world_gather_calls == 1);
    assert(data.world.present);
    assert(data.world.total_entities == 8);
    assert(data.world.entity_counts.at("minecraft:zombie") == 3);
    return 0;
}
