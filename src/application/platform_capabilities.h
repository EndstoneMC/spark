#ifndef SPARK_APPLICATION_PLATFORM_CAPABILITIES_H
#define SPARK_APPLICATION_PLATFORM_CAPABILITIES_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/metadata/platform_metadata.h"

namespace spark {

class PlayerPingProvider;

// Run a task on the server main thread (used for announcing export results
// after a background export completes).
class MainThreadDispatcher {
public:
    virtual ~MainThreadDispatcher() = default;
    virtual void runOnMainThread(std::function<void()> task) = 0;
};

struct WorldGaugeValues {
    int entities = 0;
    int tile_entities = 0;
    int chunks = 0;
    bool tile_entities_present = false;
};

// Gathers server and world metadata for profile export context and provides
// runtime server stats for health reports. The Endstone adapter implements
// this by querying the Endstone Server API.
class ProfileMetadataProvider {
public:
    virtual ~ProfileMetadataProvider() = default;
    virtual void gatherServerMetadata(ServerMetadata &metadata, std::int64_t now_ms) = 0;
    virtual void gatherWorldMetadata(WorldInfo &world, std::string_view minecraft_version) = 0;
    virtual std::vector<NativePluginSource> nativePluginSources() { return {}; }
    // Runtime queries used by /spark health (not export-specific).
    virtual std::int64_t serverUptimeSeconds() = 0;
    virtual std::int64_t playerCount() = 0;
    // Returns rolling world gauges. tile_entities_present stays false until a
    // complete low-frequency block-actor reconciliation has succeeded.
    virtual WorldGaugeValues worldGauges() { return {}; }
    // Returns the player ping provider, or nullptr if ping is not available.
    virtual PlayerPingProvider *playerPingProvider() = 0;
};

// Notifies the originating sender and logs results.
// Used by ProfilerService and TickMonitor to announce results back to the
// player who started the profiling/monitoring session.
class ResultNotifier {
public:
    virtual ~ResultNotifier() = default;
    virtual void notify(const std::string &sender_name, const std::string &text) = 0;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_PLATFORM_CAPABILITIES_H
