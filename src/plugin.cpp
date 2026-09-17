#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>
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
#include "native/diagnostics/ci_diagnostics.h"
#include "native/python/python_profile_bridge.h"
#include "net/profile_file.h"
#include "platform/endstone/adapters.h"
#include "platform/endstone/papi_integration.h"
#include "platform/endstone/python_attribution.h"
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

bool isProfilerStart(const std::vector<std::string> &tokens)
{
    return tokens.size() >= 2 && (tokens[0] == "profiler" || tokens[0] == "sampler") && tokens[1] == "start";
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
        app_->setPythonStackProvider(&python_attribution_);
        spark::setGlobalPythonStackProvider(&python_attribution_);

        if (const char *mode = std::getenv("SPARK_PYTHON_ATTRIBUTION_MODE"); mode != nullptr) {
            const std::string value(mode);
            if (value == "off") {
                python_attribution_disabled_ = true;
                getLogger().info("Python attribution diagnostic mode: off");
            }
            else if (value == "shadow-only") {
                python_attribution_shadow_only_ = true;
                getLogger().info("Python attribution diagnostic mode: shadow-only");
            }
            else if (value != "auto" && !value.empty()) {
                getLogger().warning(
                    "Ignoring unknown SPARK_PYTHON_ATTRIBUTION_MODE '{}'; expected auto, off, or shadow-only.", value);
            }
        }

        // Start before app_->enable() so a configured background execution
        // profiler never has an initial native-only window. If no execution
        // profiler starts, syncPythonAttribution() immediately disables it.
        // Explicit benchmark modes are opt-in and never change the default lifecycle.
        if (!python_attribution_disabled_) {
            beginPythonSession();
        }

        app_->statistics().start();
        app_->statistics().recordPlayerCount(static_cast<std::int64_t>(getServer().getOnlinePlayers().size()));
        app_->enable();
        syncPythonAttribution();

#if !defined(ENDSTONE_SPARK_ENDSTONE_API_0_11)
        enableMetrics();
#endif

        auto papi_api =
            getServer().getServiceManager().load<papi::PlaceholderAPI>(std::string(papi::PlaceholderAPI::ServiceName));
        const auto papi_result = papi_integration_.enable(*this, std::move(papi_api), app_->statistics(), spark::kVersion);
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
#if !defined(ENDSTONE_SPARK_ENDSTONE_API_0_11)
        if (metrics_) {
            metrics_->shutdown();
            metrics_.reset();
        }
#endif
        papi_integration_.disable(*this);
        if (app_) {
            std::string application_shutdown_error;
            if (!app_->shutdown(application_shutdown_error)) {
                std::fprintf(stderr, "[spark] application shutdown failed before plugin unload: %s\n",
                             application_shutdown_error.c_str());
                std::abort();
            }
        }

        // app_->shutdown() has stopped sampling/export first. The PEP 669
        // callbacks can now be removed without racing a sampler snapshot.
        endPythonSession();

        getServer().getScheduler().cancelTasks(*this);
        tick_task_.reset();

        std::string shutdown_error;
        if (app_ && !app_->shutdownProfilerBackend(shutdown_error)) {
            std::fprintf(stderr, "[spark] profiler shutdown failed before plugin unload: %s\n", shutdown_error.c_str());
            std::abort();
        }
        spark::setGlobalPythonStackProvider(nullptr);
        app_.reset();
    }

    bool onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                   const std::vector<std::string> &args) override
    {
        spark::CiDiagnostics::Scope diagnostic_scope(
            spark::globalCiDiagnostics(), spark::CiDiagnosticContext::PluginCommand,
            spark::CiDiagnosticPhase::PluginCommandEnter, spark::CiDiagnosticPhase::PluginCommandExit,
            spark::CiDiagnosticPhase::PluginCommandExceptionalExit, spark::ciDiagnosticCurrentThreadId());
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

        // PEP 669 bootstrap happens before the native execution sampler starts.
        // Allocation profiles also pass here, but syncPythonAttribution() turns
        // monitoring back off immediately once the selected mode is known.
        if (isProfilerStart(tokens) && !python_attribution_disabled_) {
            beginPythonSession();
        }

        spark::endstone_adapter::EndstoneCommandSender adapter(sender);
        app_->setMainThreadId(main_tid_.load());
        app_->dispatchCommand(adapter, tokens);
        syncPythonAttribution();
        return true;
    }

    void onServerTick()
    {
        spark::CiDiagnostics::Scope diagnostic_scope(
            spark::globalCiDiagnostics(), spark::CiDiagnosticContext::PluginTick,
            spark::CiDiagnosticPhase::PluginTickEnter, spark::CiDiagnosticPhase::PluginTickExit,
            spark::CiDiagnosticPhase::PluginTickExceptionalExit, spark::ciDiagnosticCurrentThreadId());
        if (main_tid_.load() == 0) {
            main_tid_.store(currentThreadId());
        }
        app_->setMainThreadId(main_tid_.load());
        syncPythonAttribution();
        if (python_attribution_shadow_only_ && ++python_diagnostic_ticks_ % 200 == 0) {
            const auto state = python_attribution_.exportState();
            const auto &diag = state.diagnostics;
            const auto pushes = diag.py_start + diag.py_resume + diag.py_throw;
            const auto pops = diag.py_return + diag.py_yield + diag.py_unwind;
            getLogger().info("Python attribution benchmark: pushes={} pops={} PY_START={} PY_RESUME={} PY_THROW={} "
                             "PY_RETURN={} PY_YIELD={} PY_UNWIND={} threads={} max_depth={} overflows={} "
                             "snapshot_attempts={} snapshot_failures={} attributed_samples={} native_only_samples={} "
                             "cache_hits={} cache_misses={} code_objects={}",
                             pushes, pops, diag.py_start, diag.py_resume, diag.py_throw, diag.py_return, diag.py_yield,
                             diag.py_unwind, diag.registered_threads, diag.max_depth, diag.overflows,
                             diag.snapshot_attempts, diag.snapshot_failures, diag.attribution_samples,
                             diag.native_only_samples, diag.code_cache_hits, diag.code_cache_misses, diag.code_objects);
        }
        const double mspt = getServer().getCurrentMillisecondsPerTick();
        app_->onTick(mspt);
        // Timeouts, cancellation, export completion, and background restarts can
        // all change profiler state from onTick(). Keep monitoring through export
        // so final metadata is frozen before callbacks are removed.
        syncPythonAttribution();
    }

private:
    void beginPythonSession() noexcept
    {
        if (python_session_requested_) {
            return;
        }
        python_session_requested_ = true;
        std::string diagnostic;
        (void)python_attribution_.start(diagnostic);
        if (!diagnostic.empty() && diagnostic != last_python_diagnostic_) {
            getLogger().info("Python attribution: {}", diagnostic);
            last_python_diagnostic_ = diagnostic;
        }
    }

    void endPythonSession() noexcept
    {
        if (!python_session_requested_) {
            return;
        }
        python_attribution_.stop();
        python_session_requested_ = false;
    }

    void syncPythonAttribution() noexcept
    {
        if (!app_) {
            return;
        }
        if (python_attribution_disabled_) {
            endPythonSession();
            return;
        }
        if (python_attribution_shadow_only_) {
            beginPythonSession();
            return;
        }
        if (app_->executionProfilerRunning()) {
            beginPythonSession();
            return;
        }
        if (!app_->profilerExporting()) {
            endPythonSession();
        }
    }

#if !defined(ENDSTONE_SPARK_ENDSTONE_API_0_11)
    void enableMetrics() noexcept
    {
        std::unique_ptr<endstone::Metrics> metrics;
        try {
            metrics = std::make_unique<endstone::Metrics>(*this, 33350);
            metrics->addCustomChart(
                std::make_unique<endstone::SimplePie>("backend", [] { return std::string{"native"}; }));
            metrics_ = std::move(metrics);
        }
        catch (const std::exception &error) {
            if (metrics) {
                metrics->shutdown();
            }
            getLogger().warning("Unable to register bStats metrics: {}", error.what());
        }
        catch (...) {
            if (metrics) {
                metrics->shutdown();
            }
            getLogger().warning("Unable to register bStats metrics: unknown error");
        }
    }
#endif

    std::string bds_executable_sha256_;
    std::atomic<std::uint64_t> main_tid_{0};
    std::shared_ptr<endstone::Task> tick_task_;

    std::unique_ptr<spark::endstone_adapter::EndstoneDispatcher> dispatcher_;
    std::unique_ptr<spark::endstone_adapter::EndstoneMetadataProvider> metadata_provider_;
    std::unique_ptr<spark::endstone_adapter::EndstoneNotifier> notifier_;
    std::unique_ptr<spark::SparkApplication> app_;
    spark::endstone_adapter::PapiIntegration papi_integration_;
    spark::endstone_adapter::EndstonePythonAttribution python_attribution_;
    bool python_session_requested_ = false;
    bool python_attribution_disabled_ = false;
    bool python_attribution_shadow_only_ = false;
    std::uint64_t python_diagnostic_ticks_ = 0;
    std::string last_python_diagnostic_;
#if !defined(ENDSTONE_SPARK_ENDSTONE_API_0_11)
    std::unique_ptr<endstone::Metrics> metrics_;
#endif
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
            "(start|stop|upload|info|cancel|open|trust-viewer)[action: SparkProfilerAction] [flags: message]");

    permission("endstone.command.spark")
        .description("Allows use of the spark profiler")
        .default_(endstone::PermissionDefault::Operator);

    permission("spark").description("Allows use of spark commands").default_(endstone::PermissionDefault::Operator);

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
