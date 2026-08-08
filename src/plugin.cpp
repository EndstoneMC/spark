#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <endstone/endstone.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "application/command/command_sender.h"
#include "application/spark_application.h"
#include "core/command/arguments.h"
#include "core/stats/executable_hash.h"
#include "net/profile_file.h"
#include "platform/endstone/adapters.h"
#include "spark_constants.h"

namespace {

std::uint64_t currentThreadId()
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#endif
}

}  // namespace

class SparkPlugin : public endstone::Plugin {
public:
    void onEnable() override
    {
        std::string hash_error;
        bds_executable_sha256_ = spark::currentExecutableSha256(hash_error);
        if (bds_executable_sha256_.empty()) {
            getLogger().warning("Unable to identify the BDS executable: {}", hash_error);
        }

        dispatcher_ = std::make_unique<spark::endstone_adapter::EndstoneDispatcher>(*this, getServer());
        metadata_provider_ = std::make_unique<spark::endstone_adapter::EndstoneMetadataProvider>(
            *this, getServer(), bds_executable_sha256_);
        notifier_ = std::make_unique<spark::endstone_adapter::EndstoneNotifier>(*this, getServer());

        app_ = std::make_unique<spark::SparkApplication>(
            bds_executable_sha256_,
            spark::profileStorageDirectory(getDataFolder()),
            getDataFolder() / "activity.json",
            *dispatcher_, *metadata_provider_, *notifier_);

        app_->statistics().start();
        app_->statistics().recordPlayerCount(
            static_cast<long>(getServer().getOnlinePlayers().size()));

        tick_task_ = getServer().getScheduler().runTaskTimer(
            *this, [this]() { onServerTick(); }, 0, 1);
        getLogger().info("endstone-spark v{} enabled. Run {}/spark{} to get started.", spark::kVersion,
                         endstone::ColorFormat::Gold, endstone::ColorFormat::Reset);
    }

    void onDisable() override
    {
        if (app_) {
            app_->shutdown();
        }
        getServer().getScheduler().cancelTasks(*this);
        tick_task_.reset();

        std::string shutdown_error;
        if (app_ && !app_->shutdownProfilerBackend(shutdown_error)) {
            std::fprintf(stderr, "[spark] profiler shutdown failed before plugin unload: %s\n",
                         shutdown_error.c_str());
            std::abort();
        }
    }

    bool onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                   const std::vector<std::string> &args) override
    {
        if (command.getName() != "spark") {
            return false;
        }
        if (main_tid_.load() == 0 && getServer().isPrimaryThread()) {
            main_tid_.store(currentThreadId());
        }

        std::vector<std::string> tokens;
        for (const auto &arg : args) {
            auto parsed = spark::Arguments::tokenize(arg);
            tokens.insert(tokens.end(), parsed.begin(), parsed.end());
        }

        spark::endstone_adapter::EndstoneCommandSender adapter(sender);
        app_->setMainThreadId(main_tid_.load());
        app_->dispatchCommand(adapter, tokens);
        return true;
    }

    void onServerTick()
    {
        if (main_tid_.load() == 0) {
            main_tid_.store(currentThreadId());
        }
        const double mspt = getServer().getCurrentMillisecondsPerTick();
        app_->onTick(mspt);
    }

private:
    std::string bds_executable_sha256_;
    std::atomic<std::uint64_t> main_tid_{0};
    std::shared_ptr<endstone::Task> tick_task_;

    std::unique_ptr<spark::endstone_adapter::EndstoneDispatcher> dispatcher_;
    std::unique_ptr<spark::endstone_adapter::EndstoneMetadataProvider> metadata_provider_;
    std::unique_ptr<spark::endstone_adapter::EndstoneNotifier> notifier_;
    std::unique_ptr<spark::SparkApplication> app_;
};

ENDSTONE_PLUGIN("spark", "0.4.1", SparkPlugin)
{
    description = "spark profiler for Endstone - find what's slowing your server down.";
    authors = {"ReallocAll <ReallocAll@outlook.com>"};
    prefix = "Spark";
    load = endstone::PluginLoadOrder::PostWorld;

    command("spark")
        .description("spark profiler")
        .usages("/spark", "/spark (tps|ping|health|activity|tickmonitor)<module: SparkStatusModule>",
                "/spark (profiler)<module: SparkProfilerModule> "
                "(start|stop|info|cancel)[action: SparkProfilerAction] [flags: message]")
        .permissions("endstone.command.spark");

    permission("endstone.command.spark")
        .description("Allows use of the spark profiler")
        .default_(endstone::PermissionDefault::Operator);
}
