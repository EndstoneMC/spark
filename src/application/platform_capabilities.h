#ifndef SPARK_APPLICATION_PLATFORM_CAPABILITIES_H
#define SPARK_APPLICATION_PLATFORM_CAPABILITIES_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/profiler/profiler.h"
#include "core/stats/ping_statistics.h"

namespace spark {

// Run a task on the server main thread (used for announcing export results
// after a background export completes).
class MainThreadDispatcher {
public:
    virtual ~MainThreadDispatcher() = default;
    virtual void runOnMainThread(std::function<void()> task) = 0;
};

// Gathers server and world metadata for profile export context and provides
// runtime server stats for health reports. The Endstone adapter implements
// this by querying the Endstone Server API.
class ProfileMetadataProvider {
public:
    virtual ~ProfileMetadataProvider() = default;
    virtual void gatherServerMetadata(ExportContext &ctx, std::int64_t now_ms) = 0;
    virtual void gatherWorldMetadata(ExportContext &ctx) = 0;
    virtual std::vector<NativePluginSource> nativePluginSources() { return {}; }
    // Runtime queries used by /spark health (not export-specific).
    virtual std::int64_t serverUptimeSeconds() = 0;
    virtual std::int64_t playerCount() = 0;
    // Returns {entity_count, loaded_chunk_count} for rolling statistics.
    // Default returns {0, 0} when world gauges are not available.
    virtual std::pair<int, int> worldGauges() { return {0, 0}; }
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
