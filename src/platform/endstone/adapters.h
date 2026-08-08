#ifndef SPARK_PLATFORM_ENDSTONE_ADAPTERS_H
#define SPARK_PLATFORM_ENDSTONE_ADAPTERS_H

#include <cstdint>
#include <string>

#include <endstone/endstone.hpp>

#include "application/platform_capabilities.h"
#include "application/command/command_sender.h"

namespace spark::endstone_adapter {

// Adapts endstone::CommandSender to spark::CommandSender.
class EndstoneCommandSender : public CommandSender {
public:
    explicit EndstoneCommandSender(::endstone::CommandSender &sender) : sender_(sender) {}

    std::string getName() const override { return sender_.getName(); }
    bool isPlayer() const override { return sender_.asPlayer() != nullptr; }

private:
    void sendImpl(const std::string &message) override { sender_.sendMessage(message); }
    void errorImpl(const std::string &message) override { sender_.sendErrorMessage(message); }

    ::endstone::CommandSender &sender_;
};

// Schedules tasks on the Endstone main thread.
class EndstoneDispatcher : public MainThreadDispatcher {
public:
    EndstoneDispatcher(::endstone::Plugin &plugin, ::endstone::Server &server)
        : plugin_(plugin), server_(server) {}

    void runOnMainThread(std::function<void()> task) override;

private:
    ::endstone::Plugin &plugin_;
    ::endstone::Server &server_;
};

// Gathers server/world metadata from the Endstone API.
class EndstoneMetadataProvider : public ProfileMetadataProvider {
public:
    EndstoneMetadataProvider(::endstone::Plugin &plugin, ::endstone::Server &server,
                             std::string bds_executable_sha256)
        : plugin_(plugin), server_(server),
          bds_executable_sha256_(std::move(bds_executable_sha256)) {}

    void gatherServerMetadata(ExportContext &ctx, std::int64_t now_ms) override;
    void gatherWorldMetadata(ExportContext &ctx) override;
    std::int64_t serverUptimeSeconds() override;
    long playerCount() override;

private:
    ::endstone::Plugin &plugin_;
    ::endstone::Server &server_;
    std::string bds_executable_sha256_;
};

// Notifies a player by name and logs to the plugin logger.
class EndstoneNotifier : public ResultNotifier {
public:
    EndstoneNotifier(::endstone::Plugin &plugin, ::endstone::Server &server)
        : plugin_(plugin), server_(server) {}

    void notify(const std::string &sender_name, const std::string &text) override;

private:
    ::endstone::Plugin &plugin_;
    ::endstone::Server &server_;
};

}  // namespace spark::endstone_adapter

#endif  // SPARK_PLATFORM_ENDSTONE_ADAPTERS_H
