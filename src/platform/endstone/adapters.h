#ifndef SPARK_PLATFORM_ENDSTONE_ADAPTERS_H
#define SPARK_PLATFORM_ENDSTONE_ADAPTERS_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include <endstone/endstone.hpp>

#include "application/command/command_sender.h"
#include "application/platform_capabilities.h"

namespace spark::endstone_adapter {

// Adapts endstone::CommandSender to spark::CommandSender.
class EndstoneCommandSender : public CommandSender {
public:
    explicit EndstoneCommandSender(::endstone::CommandSender &sender) : sender_(sender) {}

    std::string getName() const override { return sender_.getName(); }
    bool isPlayer() const override { return sender_.asPlayer() != nullptr; }
    bool hasPermission(const std::string &name) const override { return sender_.hasPermission(name); }

private:
    void sendImpl(const std::string &message) override;
    void errorImpl(const std::string &message) override;

    ::endstone::CommandSender &sender_;
};

// Schedules tasks on the Endstone main thread.
class EndstoneDispatcher : public MainThreadDispatcher {
public:
    EndstoneDispatcher(::endstone::Plugin &plugin, ::endstone::Server &server) : plugin_(plugin), server_(server) {}

    void runOnMainThread(std::function<void()> task) override;

private:
    ::endstone::Plugin &plugin_;
    ::endstone::Server &server_;
};

// Polls player ping from the Endstone Server API.
class EndstonePlayerPingProvider : public PlayerPingProvider {
public:
    explicit EndstonePlayerPingProvider(::endstone::Server &server) : server_(server) {}

    std::map<std::string, int> poll() override;

private:
    ::endstone::Server &server_;
};

// Notifies a player by name and logs to the plugin logger.
class EndstoneNotifier : public ResultNotifier {
public:
    EndstoneNotifier(::endstone::Plugin &plugin, ::endstone::Server &server, bool disable_broadcast)
        : plugin_(plugin), server_(server), disable_broadcast_(disable_broadcast)
    {
    }

    void notify(const std::string &sender_name, const std::string &text) override;

private:
    ::endstone::Plugin &plugin_;
    ::endstone::Server &server_;
    bool disable_broadcast_;
};

// Maintains rolling entity and loaded-chunk counts via Endstone events
// with periodic full-scan reconciliation to correct drift.
class EndstoneWorldGaugeProvider {
public:
    EndstoneWorldGaugeProvider(::endstone::Plugin &plugin, ::endstone::Server &server)
        : plugin_(plugin), server_(server)
    {
    }

    void init();
    std::pair<int, int> worldGauges();

private:
    void reconcile();

    ::endstone::Plugin &plugin_;
    ::endstone::Server &server_;
    std::atomic<int> entity_count_{0};
    std::atomic<int> chunk_count_{0};
    std::int64_t last_reconcile_steady_ms_ = 0;
    bool initialized_ = false;
};

// Gathers server/world metadata from the Endstone API.
class EndstoneMetadataProvider : public ProfileMetadataProvider {
public:
    EndstoneMetadataProvider(::endstone::Plugin &plugin, ::endstone::Server &server, std::string bds_executable_sha256)
        : plugin_(plugin), server_(server), bds_executable_sha256_(std::move(bds_executable_sha256))
    {
    }

    void gatherServerMetadata(ExportContext &ctx, std::int64_t now_ms) override;
    void gatherWorldMetadata(ExportContext &ctx) override;
    std::vector<NativePluginSource> nativePluginSources() override;
    std::int64_t serverUptimeSeconds() override;
    std::int64_t playerCount() override;
    std::pair<int, int> worldGauges() override;
    PlayerPingProvider *playerPingProvider() override;

private:
    ::endstone::Plugin &plugin_;
    ::endstone::Server &server_;
    std::string bds_executable_sha256_;
    std::unique_ptr<EndstonePlayerPingProvider> ping_provider_;
    std::unique_ptr<EndstoneWorldGaugeProvider> world_gauges_;
};

}  // namespace spark::endstone_adapter

#endif  // SPARK_PLATFORM_ENDSTONE_ADAPTERS_H
