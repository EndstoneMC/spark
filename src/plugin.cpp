#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <endstone/endstone.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>

#include <sys/syscall.h>
#endif

#include "application/command/command_sender.h"
#include "application/spark_application.h"
#include "core/command/arguments.h"
#include "core/config/spark_config.h"
#include "core/config/trusted_viewers.h"
#include "core/stats/executable_hash.h"
#include "net/profile_file.h"
#include "platform/endstone/adapters.h"
#include "platform/endstone/papi_integration.h"
#include "spark_constants.h"

namespace {

std::uint64_t currentThreadId()
{
#ifdef _WIN32
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#endif
}
}  // namespace

// NOLINTBEGIN(bugprone-throwing-static-initialization,misc-use-anonymous-namespace,misc-use-internal-linkage)
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

        spark::SparkConfig config(getDataFolder() / "config.toml");
        if (!config.loadOrCreate()) {
            getLogger().warning("Failed to initialize spark config: {}", config.lastError());
        }

        spark::TrustedViewersState trusted_viewers(getDataFolder() / "trusted-viewers.json");
        trusted_viewers.load();

        notifier_ = std::make_unique<spark::endstone_adapter::EndstoneNotifier>(*this, getServer(),
                                                                                config.disable_response_broadcast);

        app_ = std::make_unique<spark::SparkApplication>(
            bds_executable_sha256_, spark::profileStorageDirectory(getDataFolder()), getDataFolder() / "activity.json",
            std::move(config), std::move(trusted_viewers), *dispatcher_, *metadata_provider_, *notifier_);

        app_->statistics().start();
        app_->statistics().recordPlayerCount(static_cast<std::int64_t>(getServer().getOnlinePlayers().size()));
        app_->enable();

        auto papi_api =
            getServer().getServiceManager().load<papi::PlaceholderAPI>(std::string(papi::PlaceholderAPI::ServiceName));
        const auto papi_result =
            papi_integration_.enable(*this, std::move(papi_api), app_->statistics(), spark::kVersion);
        if (papi_result == spark::endstone_adapter::PapiRegistrationResult::Registered) {
            getLogger().info("Registered the spark PlaceholderAPI expansion.");
        }
        else if (papi_result == spark::endstone_adapter::PapiRegistrationResult::Rejected) {
            getLogger().warning("PlaceholderAPI rejected the spark expansion; Spark will continue without it.");
        }

        tick_task_ = getServer().getScheduler().runTaskTimer(*this, [this]() { onServerTick(); }, 0, 1);
        getLogger().info("endstone-spark v{} enabled. Run {}/spark{} to get started.", spark::kVersion,
                         endstone::ColorFormat::Gold, endstone::ColorFormat::Reset);
    }

    void onDisable() override
    {
        papi_integration_.disable(*this);
        if (app_) {
            app_->shutdown();
        }
        getServer().getScheduler().cancelTasks(*this);
        tick_task_.reset();

        std::string shutdown_error;
        if (app_ && !app_->shutdownProfilerBackend(shutdown_error)) {
            std::fprintf(stderr, "[spark] profiler shutdown failed before plugin unload: %s\n", shutdown_error.c_str());
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
        app_->setMainThreadId(main_tid_.load());
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
    spark::endstone_adapter::PapiIntegration papi_integration_;
};

ENDSTONE_PLUGIN("spark", "0.5.3", SparkPlugin)
{
    description = "spark profiler for Endstone - find what's slowing your server down.";
    authors = {"ReallocAll <ReallocAll@outlook.com>"};
    prefix = "Spark";
    load = endstone::PluginLoadOrder::PostWorld;
    soft_depend = {"papi"};

    command("spark")
        .description("spark profiler")
        .usages(
            "/spark",
            "/spark (tps|cpu|ping|health|healthreport|ht|activity|activitylog|log|tickmonitor|tickmonitoring)<module: "
            "SparkStatusModule> [flags: message]",
            "/spark (profiler|sampler)<module: SparkProfilerModule> "
            "(start|stop|upload|info|cancel|open|trust-viewer)[action: SparkProfilerAction] [flags: message]")
        .permissions("endstone.command.spark");

    permission("endstone.command.spark")
        .description("Allows use of the spark profiler")
        .default_(endstone::PermissionDefault::Operator);

    permission("spark.profiler")
        .description("Allows use of /spark profiler")
        .default_(endstone::PermissionDefault::Operator);

    permission("spark.tps").description("Allows use of /spark tps").default_(endstone::PermissionDefault::Operator);

    permission("spark.ping").description("Allows use of /spark ping").default_(endstone::PermissionDefault::Operator);

    permission("spark.health")
        .description("Allows use of /spark health")
        .default_(endstone::PermissionDefault::Operator);

    permission("spark.activity")
        .description("Allows use of /spark activity")
        .default_(endstone::PermissionDefault::Operator);

    permission("spark.tickmonitor")
        .description("Allows use of /spark tickmonitor")
        .default_(endstone::PermissionDefault::Operator);
}
// NOLINTEND(bugprone-throwing-static-initialization,misc-use-anonymous-namespace,misc-use-internal-linkage)
