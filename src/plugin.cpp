#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <endstone/endstone.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "command/arguments.h"
#include "net/bytebin.h"
#include "net/gzip.h"
#include "net/profile_file.h"
#include "sampler/profiler.h"
#include "spark_constants.h"
#include "stats/executable_hash.h"
#include "stats/statistics_service.h"
#include "stats/tick_monitor.h"

namespace {

using endstone::ColorFormat;

enum class ExportOutcome {
    Failed,
    Uploaded,
    Saved,
};

std::uint64_t currentThreadId()
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#endif
}

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int floorDiv(int value, int divisor)
{
    int quotient = value / divisor;
    int remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

std::string formatDuration(std::int64_t seconds)
{
    if (seconds < 60) {
        return std::to_string(seconds) + "s";
    }
    long m = seconds / 60;
    long s = seconds % 60;
    if (m < 60) {
        return std::to_string(m) + "m " + std::to_string(s) + "s";
    }
    long h = m / 60;
    m %= 60;
    return std::to_string(h) + "h " + std::to_string(m) + "m";
}

std::string formatBytes(std::uint64_t bytes)
{
    constexpr const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < (sizeof(units) / sizeof(units[0]))) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.2f %s", value, units[unit]);
    return buffer;
}

template <typename Sender>
bool commandSenderIsPlayer(Sender &sender)
{
    if constexpr (requires(Sender &value) { value.asPlayer(); }) {
        return sender.asPlayer() != nullptr;
    }
    else {
        return dynamic_cast<endstone::Player *>(&sender) != nullptr;
    }
}

template <typename Value>
auto pointerFromApi(Value &&value)
{
    using Type = std::remove_reference_t<Value>;
    if constexpr (std::is_pointer_v<Type>) {
        return value;
    }
    else {
        return std::addressof(value);
    }
}

template <typename ActorType>
std::string actorTypeName(const ActorType &type)
{
    if constexpr (requires(const ActorType &value) { value.getId(); }) {
        return static_cast<std::string>(type.getId());
    }
    else {
        return type;
    }
}

template <typename Dimension>
std::string dimensionName(const Dimension &dimension)
{
    if constexpr (requires(const Dimension &value) { value.getName(); }) {
        return dimension.getName();
    }
    else {
        return static_cast<std::string>(dimension.getId());
    }
}

const std::string &tpsColor(double tps)
{
    if (tps >= 19.5) {
        return ColorFormat::Green;
    }
    if (tps >= 18.0) {
        return ColorFormat::Yellow;
    }
    if (tps >= 15.0) {
        return ColorFormat::Gold;
    }
    return ColorFormat::Red;
}

const std::string &msptColor(double mspt)
{
    if (mspt <= 25.0) {
        return ColorFormat::Green;
    }
    if (mspt <= 40.0) {
        return ColorFormat::Yellow;
    }
    if (mspt <= 50.0) {
        return ColorFormat::Gold;
    }
    return ColorFormat::Red;
}

const std::string &cpuColor(double usage)
{
    if (usage < 0.65) {
        return ColorFormat::Green;
    }
    if (usage < 0.85) {
        return ColorFormat::Yellow;
    }
    return ColorFormat::Red;
}

std::string formatNumber(double value, int precision)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), precision == 1 ? "%.1f" : "%.2f",
                  value);
    return buffer;
}

std::string formatTpsValue(const spark::RollingValue &value)
{
    if (!value.present) {
        return ColorFormat::Gray + std::string("n/a");
    }
    return tpsColor(value.value) + formatNumber(value.value, 1) +
           ColorFormat::Gray;
}

std::string formatCpuValue(const spark::RollingValue &value)
{
    if (!value.present) {
        return ColorFormat::Gray + std::string("n/a");
    }
    return cpuColor(value.value) + formatNumber(value.value * 100.0, 1) +
           "%" + ColorFormat::Gray;
}

std::string formatMsptValue(double value)
{
    return msptColor(value) + formatNumber(value, 2) + ColorFormat::Gray;
}

std::string formatMsptDistribution(const spark::DistributionValues &values)
{
    if (!values.present) {
        return ColorFormat::Gray + std::string("n/a");
    }
    return formatMsptValue(values.mean) + "/" + formatMsptValue(values.min) +
           "/" + formatMsptValue(values.median) + "/" +
           formatMsptValue(values.percentile95) + "/" +
           formatMsptValue(values.max);
}

}  // namespace

class SparkPlugin : public endstone::Plugin {
public:
    void onEnable() override
    {
        statistics_.start();
        statistics_.recordPlayerCount(
            static_cast<long>(getServer().getOnlinePlayers().size()));
        std::string hash_error;
        bds_executable_sha256_ = spark::currentExecutableSha256(hash_error);
        if (bds_executable_sha256_.empty()) {
            getLogger().warning("Unable to identify the BDS executable: {}", hash_error);
        }
        tick_task_ = getServer().getScheduler().runTaskTimer(
            *this, [this]() { onServerTick(); }, 0, 1);
        getLogger().info("endstone-spark v{} enabled. Run {}/spark{} to get started.", spark::kVersion,
                         ColorFormat::Gold, ColorFormat::Reset);
    }

    void onDisable() override
    {
        if (export_thread_.joinable()) {
            export_thread_.join();
        }
        // The export thread may have queued announceResult() just before it
        // exited. Remove every scheduler-owned callback before proving backend
        // quiescence so no task can re-enter old plugin code during/after reload.
        getServer().getScheduler().cancelTasks(*this);
        tick_task_.reset();

        std::string shutdown_error;
        if (!profiler_.shutdown(shutdown_error)) {
            std::fprintf(stderr, "[spark] profiler shutdown failed before plugin unload: %s\n",
                         shutdown_error.c_str());
            // Endstone cannot veto unload from onDisable(). Continuing would let
            // allocator entries or in-flight thunks reference an unloaded DLL.
            // Fail closed instead of pinning/leaking old plugin code across reload.
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
        std::string module = tokens.empty() ? std::string() : tokens[0];

        if (module.empty()) {
            sendHelp(sender);
        }
        else if (module == "tps") {
            cmdTps(sender);
        }
        else if (module == "health") {
            cmdHealth(sender);
        }
        else if (module == "tickmonitor") {
            std::vector<std::string> rest(tokens.begin() + 1, tokens.end());
            cmdTickMonitor(sender, spark::Arguments(rest));
        }
        else if (module == "profiler") {
            std::vector<std::string> rest(tokens.begin() + 1, tokens.end());
            cmdProfiler(sender, spark::Arguments(rest));
        }
        else {
            sendHelp(sender);
        }
        return true;
    }

private:
    void onServerTick()
    {
        if (main_tid_.load() == 0) {
            main_tid_.store(currentThreadId());
        }
        const bool profiler_running = profiler_.running();
        const bool tick_monitor_running = tick_monitor_.running();
        const double mspt = getServer().getCurrentMillisecondsPerTick();
        if (statistics_.onTick(mspt)) {
            statistics_.recordPlayerCount(
                static_cast<long>(getServer().getOnlinePlayers().size()));
        }
        if (tick_monitor_running) {
            processTickMonitor(mspt);
        }
        if (profiler_running) {
            std::string backend_error;
            const bool backend_failed = profiler_.backendFailure(backend_error);
            if (!backend_failed) {
                profiler_.onTick(mspt);
            }
            std::int64_t auto_end = profiler_.autoEndTimeMs();
            if (auto_end > 0 && nowMs() >= auto_end) {
                bool save = profiler_.options().save_to_file;
                finishProfiler(start_sender_name_, save, std::string());
            }
        }
    }

    void sendHelp(endstone::CommandSender &sender)
    {
        sender.sendMessage("{}endstone-spark {}v{}", ColorFormat::Gold, ColorFormat::Gray, spark::kVersion);
        sender.sendMessage("{}/spark profiler start [flags] {}- start an execution or allocation profile",
                           ColorFormat::Yellow, ColorFormat::Gray);
        sender.sendMessage("{}/spark profiler stop {}- stop profiling and finalize the profile", ColorFormat::Yellow,
                           ColorFormat::Gray);
        sender.sendMessage("{}/spark profiler info {}- show status of the running profiler", ColorFormat::Yellow,
                           ColorFormat::Gray);
        sender.sendMessage("{}/spark profiler cancel {}- stop profiling without generating a profile", ColorFormat::Yellow,
                           ColorFormat::Gray);
        sender.sendMessage(
            "{}/spark tps {}- rolling TPS, MSPT percentiles, and CPU usage",
            ColorFormat::Yellow, ColorFormat::Gray);
        sender.sendMessage(
            "{}/spark health {}- performance and host resource report",
            ColorFormat::Yellow, ColorFormat::Gray);
        sender.sendMessage("{}/spark tickmonitor {}- report unusually long ticks", ColorFormat::Yellow,
                           ColorFormat::Gray);
        sender.sendMessage("{}Modes: --alloc, --alloc-live-only", ColorFormat::Gray);
        sender.sendMessage("{}Thread selection: --thread <name|*>, --regex", ColorFormat::Gray);
        sender.sendMessage("{}Execution only: --include-sleeping", ColorFormat::Gray);
        sender.sendMessage(
            "{}Flags: --interval <ms|bytes>, --timeout <seconds>, --only-ticks-over <ms>",
            ColorFormat::Gray);
        sender.sendMessage("{}       --save-to-file (plugins/spark/profiles), --comment <text>",
                           ColorFormat::Gray);
    }

    void cmdProfiler(endstone::CommandSender &sender, const spark::Arguments &args)
    {
        const std::string &action = args.subCommand();
        if (action.empty() || action == "info") {
            profilerInfo(sender);
        }
        else if (action == "cancel") {
            profilerCancel(sender);
        }
        else if (action == "stop") {
            profilerStop(sender, args);
        }
        else if (action == "start") {
            profilerStart(sender, args);
        }
        else {
            profilerInfo(sender);
        }
    }

    void profilerStart(endstone::CommandSender &sender, const spark::Arguments &args)
    {
        if (profiler_.running()) {
            profilerInfo(sender);
            return;
        }
        if (exporting_.load()) {
            sender.sendMessage("The profiler has stopped; results are still being finalized.");
            return;
        }

        spark::ProfilerOptions options;
        options.alloc_live_only = args.boolFlag("alloc-live-only");
        options.alloc = args.boolFlag("alloc") || options.alloc_live_only;
#if !defined(_WIN32) && !defined(__linux__)
        if (options.alloc) {
            sender.sendErrorMessage(
                "The native allocation profiler is supported only on Windows x64 and Linux x86-64.");
            return;
        }
#endif
        options.threads = args.stringFlag("thread");
        options.regex = args.boolFlag("regex");
        if (args.boolFlag("thread") && options.threads.empty()) {
            sender.sendErrorMessage("--thread requires a thread name, pattern, or *.");
            return;
        }
        if (options.regex && options.threads.empty()) {
            sender.sendErrorMessage("--regex requires at least one --thread pattern.");
            return;
        }
        const auto all_selector = std::find(options.threads.begin(), options.threads.end(), "*");
        if (all_selector != options.threads.end() &&
            (options.regex || options.threads.size() != 1)) {
            sender.sendErrorMessage("--thread * cannot be combined with another --thread or --regex.");
            return;
        }

        auto interval = args.doubleFlag("interval");
        if (args.boolFlag("interval") && !interval) {
            sender.sendErrorMessage("The sampling interval must be a finite number.");
            return;
        }
        if (interval && *interval <= 0.0) {
            sender.sendErrorMessage("The sampling interval must be greater than zero.");
            return;
        }

        if (options.alloc) {
            if (interval && *interval > static_cast<double>(spark::kMaxAllocationIntervalBytes)) {
                sender.sendErrorMessage("The allocation interval must not exceed {} bytes.",
                                        spark::kMaxAllocationIntervalBytes);
                return;
            }
            options.allocation_interval_bytes = interval
                                                    ? static_cast<std::int32_t>(*interval + 0.5)
                                                    : spark::kDefaultAllocationIntervalBytes;
            if (options.allocation_interval_bytes < 1) {
                options.allocation_interval_bytes = 1;
            }
        }
        else {
            if (interval && *interval > spark::kMaxSamplingIntervalMs) {
                sender.sendErrorMessage("The sampling interval must not exceed {}ms.",
                                        spark::kMaxSamplingIntervalMs);
                return;
            }
            options.interval_ms = interval ? static_cast<int>(*interval + 0.5) : 4;
            if (options.interval_ms < 1) {
                options.interval_ms = 1;
            }
        }

        auto timeout_flag = args.intFlag("timeout");
        if (args.boolFlag("timeout") && !timeout_flag) {
            sender.sendErrorMessage("The timeout must be a whole number of seconds.");
            return;
        }
        long timeout = timeout_flag.value_or(-1);
        if (timeout_flag && timeout <= 10) {
            sender.sendErrorMessage("The timeout is too short for useful results — choose a value over 10 seconds.");
            return;
        }
        options.timeout_seconds = timeout;

        auto tick_threshold = args.intFlag("only-ticks-over");
        if (args.boolFlag("only-ticks-over") && !tick_threshold) {
            sender.sendErrorMessage("The tick threshold must be a whole number of milliseconds.");
            return;
        }
        if (tick_threshold && *tick_threshold <= 0) {
            sender.sendErrorMessage("The tick threshold must be greater than 0ms.");
            return;
        }
        options.only_ticks_over_ms = tick_threshold.value_or(-1);
        options.ignore_sleeping = !args.boolFlag("include-sleeping");
        auto comments = args.stringFlag("comment");
        if (!comments.empty()) {
            options.comment = comments.front();
        }
        options.save_to_file = args.boolFlag("save-to-file");
        options.creator_name = sender.getName();
        options.creator_is_player = commandSenderIsPlayer(sender);

        std::uint64_t tid = main_tid_.load();
        if (tid == 0) {
            sender.sendErrorMessage("The server thread hasn't been identified yet — try again in a moment.");
            return;
        }

        std::string error;
        if (!profiler_.start(options, tid, error)) {
            sender.sendErrorMessage("Couldn't start the profiler: {}", error);
            return;
        }
        start_sender_name_ = sender.getName();

        if (options.alloc) {
            if (options.alloc_live_only) {
                sender.sendMessage("{}Retained Allocation Profiler is now running!{} (async)",
                                   ColorFormat::Gold, ColorFormat::Gray);
            }
            else {
                sender.sendMessage("{}Allocation Profiler is now running!{} (async)",
                                   ColorFormat::Gold, ColorFormat::Gray);
            }
            if (options.threads.empty() ||
                (options.threads.size() == 1 && options.threads.front() == "*")) {
                sender.sendMessage(
                    "Sampling approximately every {} of native allocations across process threads.",
                    formatBytes(static_cast<std::uint64_t>(
                        options.allocation_interval_bytes)));
            }
            else {
                sender.sendMessage(
                    "Sampling approximately every {} of native allocations from matching threads.",
                    formatBytes(static_cast<std::uint64_t>(
                        options.allocation_interval_bytes)));
            }
            if (options.alloc_live_only) {
                sender.sendMessage("The result will contain only sampled allocations still live when profiling stops.");
            }
        }
        else {
            if (options.threads.empty()) {
                sender.sendMessage("{}Profiler is now running!{} (async, {}ms interval)", ColorFormat::Gold,
                                   ColorFormat::Gray, options.interval_ms);
            }
            else if (options.threads.size() == 1 && options.threads.front() == "*") {
                sender.sendMessage("{}Profiler is now running for all process threads!{} (async, {}ms interval)",
                                   ColorFormat::Gold, ColorFormat::Gray, options.interval_ms);
            }
            else {
                sender.sendMessage("{}Profiler is now running for selected process threads!{} (async, {}ms interval)",
                                   ColorFormat::Gold, ColorFormat::Gray, options.interval_ms);
            }
        }
        if (options.only_ticks_over_ms > 0) {
            sender.sendMessage("Only recording ticks longer than {}ms.", options.only_ticks_over_ms);
        }
        if (timeout <= 0) {
            sender.sendMessage("It runs in the background until stopped.");
            sender.sendMessage("To stop and finalize the profile, run: {}/spark profiler stop", ColorFormat::Gray);
        }
        else {
            if (timeout < 30) {
                sender.sendMessage("Tip: a timeout over 30s gives noticeably more accurate results.");
            }
            sender.sendMessage("Results will be returned automatically after {}.", formatDuration(timeout));
        }
    }

    void profilerStop(endstone::CommandSender &sender, const spark::Arguments &args)
    {
        if (!profiler_.running()) {
            sender.sendMessage(exporting_.load() ? "The profiler has stopped; results are still being finalized."
                                                 : "There isn't an active profiler running.");
            return;
        }
        std::string backend_error;
        if (profiler_.backendFailure(backend_error)) {
            std::string cleanup_error;
            if (!profiler_.cancel(cleanup_error)) {
                sender.sendMessage("{}Allocation profiler status: FAILED", ColorFormat::Red);
                sender.sendMessage("Unable to discard the failed session safely: {}", cleanup_error);
                return;
            }
            sender.sendMessage("{}Allocation profiler status: FAILED", ColorFormat::Red);
            sender.sendMessage("Incomplete profile data was discarded: {}", backend_error);
            sender.sendMessage("The allocation profiler backend is ready for a new session.");
            return;
        }
        bool save = profiler_.options().save_to_file || args.boolFlag("save-to-file");
        std::string comment;
        auto comments = args.stringFlag("comment");
        if (!comments.empty()) {
            comment = comments.front();
        }
        sender.sendMessage("{}Stopping the profiler and finalizing results, please wait...", ColorFormat::Gold);
        finishProfiler(sender.getName(), save, comment);
    }

    void profilerInfo(endstone::CommandSender &sender)
    {
        if (!profiler_.running()) {
            if (exporting_.load()) {
                sender.sendMessage("The profiler has stopped; results are still being finalized.");
                return;
            }
            sender.sendMessage("The profiler isn't running!");
            sender.sendMessage("To start a new one, run: {}/spark profiler start", ColorFormat::Gray);
            return;
        }
        const bool allocation = profiler_.mode() == spark::ProfileMode::Allocation;
        std::string backend_error;
        if (allocation && profiler_.backendFailure(backend_error)) {
            sender.sendMessage("{}Allocation Profiler status: FAILED", ColorFormat::Red);
            sender.sendMessage("Backend service failure: {}", backend_error);
            sendAllocationHookCoverage(sender);
            sender.sendMessage("The incomplete profile will not be exported.");
            sender.sendMessage("Run {}/spark profiler stop{} or {}/spark profiler cancel{} to discard it.",
                               ColorFormat::Gray, ColorFormat::Reset, ColorFormat::Gray,
                               ColorFormat::Reset);
            return;
        }
        if (allocation) {
            if (profiler_.options().alloc_live_only) {
                sender.sendMessage("{}Retained Allocation Profiler is already running!", ColorFormat::Gold);
            }
            else {
                sender.sendMessage("{}Allocation Profiler is already running!", ColorFormat::Gold);
            }
            sendAllocationHookCoverage(sender);
            const auto &threads = profiler_.options().threads;
            if (threads.empty() ||
                (threads.size() == 1 && threads.front() == "*")) {
                sender.sendMessage("Thread selection: all process threads.");
            }
            else {
                sender.sendMessage(
                    "Thread selection: {} {} selector{} (matched at aggregation).",
                    threads.size(), profiler_.options().regex ? "regex" : "exact-name",
                    threads.size() == 1 ? "" : "s");
            }
        }
        else {
            sender.sendMessage("{}Profiler is already running!", ColorFormat::Gold);
        }
        std::int64_t ran = (nowMs() - profiler_.startTimeMs()) / 1000;
        if (allocation) {
            if (profiler_.options().alloc_live_only) {
                sender.sendMessage("So far it has profiled for {} ({} tracked sampled allocations still live process-wide, {} estimated).",
                                   formatDuration(ran), profiler_.liveAllocationSamples(),
                                   formatBytes(profiler_.liveAllocationBytes()));
            }
            else {
                sender.sendMessage("So far it has profiled for {} ({} selected allocation samples, {} estimated; {} observed process-wide).",
                                   formatDuration(ran), profiler_.sampleCount(),
                                   formatBytes(profiler_.sampledAllocationBytes()),
                                   formatBytes(profiler_.observedAllocationBytes()));
            }
            sender.sendMessage("Process-wide tracked lifecycle: {} freed, {} still live ({}).",
                               profiler_.freedAllocationSamples(),
                               profiler_.liveAllocationSamples(),
                               formatBytes(profiler_.liveAllocationBytes()));
            if (profiler_.droppedSamples() != 0) {
                sender.sendMessage("Dropped allocation samples: {}", profiler_.droppedSamples());
            }
            if (profiler_.filteredAllocationSamples() != 0) {
                sender.sendMessage("Allocation samples excluded by thread selector: {}.",
                                   profiler_.filteredAllocationSamples());
            }
            if (profiler_.allocationThreadNameFailures() != 0) {
                sender.sendMessage(
                    "Allocation-origin thread names unavailable (failed closed for named selectors): {}.",
                    profiler_.allocationThreadNameFailures());
            }
        }
        else {
            sender.sendMessage("So far it has profiled for {} ({} samples).", formatDuration(ran),
                               profiler_.sampleCount());
        }
        std::int64_t auto_end = profiler_.autoEndTimeMs();
        if (auto_end <= 0) {
            sender.sendMessage("To stop and finalize the profile, run: {}/spark profiler stop", ColorFormat::Gray);
        }
        else {
            sender.sendMessage("It finishes automatically in {}.", formatDuration((auto_end - nowMs()) / 1000));
        }
        sender.sendMessage("To cancel without generating a profile, run: {}/spark profiler cancel", ColorFormat::Gray);
    }

    void sendAllocationHookCoverage(endstone::CommandSender &sender)
    {
        const auto &capabilities = profiler_.allocationHookCapabilities();
        std::size_t active = 0;
        std::size_t aliases = 0;
        std::string unavailable;
        for (const spark::AllocationHookCapability &capability : capabilities) {
            if (capability.status == spark::AllocationHookStatus::Active) {
                ++active;
            }
            else if (capability.status == spark::AllocationHookStatus::Alias) {
                ++aliases;
            }
            else {
                if (!unavailable.empty()) {
                    unavailable += ", ";
                }
                unavailable += capability.name;
                unavailable += '=';
                unavailable += spark::allocationHookStatusName(capability.status);
            }
        }
        sender.sendMessage("Native allocation hooks: {}/{} entry points covered ({} patched targets, {} aliases).",
                           active + aliases, capabilities.size(),
                           profiler_.allocationHookTargetCount(), aliases);
        if (!unavailable.empty()) {
            sender.sendMessage("Unavailable optional hooks: {}", unavailable);
        }
    }

    void profilerCancel(endstone::CommandSender &sender)
    {
        if (!profiler_.running()) {
            sender.sendMessage("There isn't an active profiler running.");
            return;
        }
        std::string backend_error;
        const bool failed = profiler_.backendFailure(backend_error);
        std::string error;
        if (!profiler_.cancel(error)) {
            sender.sendMessage("{}Unable to cancel the profiler safely: {}", ColorFormat::Red, error);
            return;
        }
        if (failed) {
            sender.sendMessage("{}Failed allocation profile data was discarded: {}", ColorFormat::Red,
                               backend_error);
            sender.sendMessage("The allocation profiler backend is ready for a new session.");
        }
        else {
            sender.sendMessage("{}Profiler has been cancelled.", ColorFormat::Gold);
        }
    }

    // Stop and join on the main thread; export and network upload run in the
    // background. That task and its main-thread hop capture only `this` so the
    // std::function handed to Endstone stays in libc++'s ABI-stable small-buffer form,
    // which matters when the plugin and the runtime are built with different libc++.
    void finishProfiler(const std::string &sender_name, bool save, const std::string &comment)
    {
        // Stop before gathering metadata so spark's own world/plugin snapshot
        // allocations do not pollute an allocation profile. Entry hooks remain
        // disabled pass-throughs between sessions; a backend service failure
        // blocks export of the partial data.
        std::string stop_error;
        if (!profiler_.stopSampling(stop_error)) {
            std::string backend_error;
            if (!profiler_.running() && profiler_.backendFailure(backend_error)) {
                announce(sender_name,
                         "Allocation profiler FAILED; incomplete profile data was discarded: " +
                             backend_error);
                announce(sender_name, "The allocation profiler backend is ready for a new session.");
            }
            else {
                announce(sender_name, "Profiler stop failed: " + stop_error);
            }
            return;
        }

        pending_ctx_.endstone_version = getServer().getVersion();
        pending_ctx_.minecraft_version = getServer().getMinecraftVersion();
        pending_ctx_.bds_executable_sha256 = bds_executable_sha256_;
        pending_ctx_.comment = comment;
        pending_ctx_.statistics = statistics_.snapshot();
        pending_ctx_.window_stats = statistics_.profileWindows(
            profiler_.startTimeMs(), profiler_.endTimeMs());
        pending_ctx_.system_stats = spark::gatherSystemStats(".");
        pending_ctx_.player_count = static_cast<long>(getServer().getOnlinePlayers().size());
        pending_ctx_.online_mode = getServer().getOnlineMode() ? 2 : 1;
        {
            std::int64_t start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        getServer().getStartTime().time_since_epoch())
                                        .count();
            pending_ctx_.uptime_ms = nowMs() - start_ms;
        }

        pending_ctx_.plugins.clear();
        for (endstone::Plugin *plugin : getServer().getPluginManager().getPlugins()) {
            const endstone::PluginDescription &desc = plugin->getDescription();
            std::string author;
            for (const std::string &a : desc.getAuthors()) {
                author += (author.empty() ? "" : ", ") + a;
            }
            pending_ctx_.plugins.push_back({desc.getName(), desc.getVersion(), author, desc.getDescription()});
        }

        pending_ctx_.world = spark::WorldInfo{};
        auto &&level_result = getServer().getLevel();
        if (endstone::Level *level = pointerFromApi(level_result)) {
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
                    it->second.entity_counts[actorTypeName(actor->getType())]++;
                }

                spark::WorldEntry world;
                world.name = dimensionName(*dimension);
                std::map<std::pair<int, int>, spark::WorldRegion> regions;
                for (auto &[coordinate, chunk] : chunks) {
                    auto region_coordinate = std::pair{floorDiv(coordinate.first, 32),
                                                       floorDiv(coordinate.second, 32)};
                    spark::WorldRegion &region = regions[region_coordinate];
                    region.total_entities += chunk.total_entities;
                    world.total_entities += chunk.total_entities;
                    for (const auto &[type, count] : chunk.entity_counts) {
                        pending_ctx_.world.entity_counts[type] += count;
                    }
                    region.chunks.push_back(std::move(chunk));
                }
                for (auto &entry : regions) {
                    world.regions.push_back(std::move(entry.second));
                }
                pending_ctx_.world.total_entities += world.total_entities;
                pending_ctx_.world.worlds.push_back(std::move(world));
            }
            pending_ctx_.world.present = !pending_ctx_.world.worlds.empty();
        }

        pending_save_ = save;
        pending_sender_ = sender_name;
        pending_folder_ = spark::profileStorageDirectory(getDataFolder());

        exporting_.store(true);
        // NOTE: Endstone's runTaskAsync has a use-after-free — scheduler.cpp submits
        // `[&task]{ task->run(); }`, capturing the loop variable by reference into a
        // queue it then erases. So we run the export on our own thread and hop back to
        // the main thread with a sync task (the safe path).
        if (export_thread_.joinable()) {
            export_thread_.join();
        }
        export_thread_ = std::thread([this]() {
            try {
                runExport();
            }
            catch (const std::exception &e) {
                std::fprintf(stderr, "[spark] export thread failed: %s\n", e.what());
                exporting_.store(false);
            }
            catch (...) {
                std::fprintf(stderr, "[spark] export thread failed with an unknown exception\n");
                exporting_.store(false);
            }
        });
    }

    // Background thread: no Endstone API except the scheduler hop-back at the end.
    void runExport()
    {
        ExportOutcome outcome = ExportOutcome::Failed;
        std::string message;
        try {
            std::string body = profiler_.exportData(pending_ctx_);
            std::string compressed = spark::gzipCompress(body);
            if (pending_save_) {
                spark::ProfileFileResult saved =
                    spark::saveProfileToDirectory(pending_folder_, compressed, nowMs());
                if (saved.ok) {
                    outcome = ExportOutcome::Saved;
                    message = "Saved to " + saved.path.string() + " — open it at " +
                              spark::kViewerUrl;
                }
                else {
                    message = "Failed to save the profile: " + saved.error;
                }
            }
            else {
                spark::UploadResult result =
                    spark::uploadToBytebin(compressed, spark::kBytebinUrl,
                                           spark::kSamplerContentType,
                                           std::string("endstone-spark/") + spark::kVersion);
                if (result.ok) {
                    outcome = ExportOutcome::Uploaded;
                    message = std::string(spark::kViewerUrl) + result.key;
                }
                else {
                    spark::ProfileFileResult saved =
                        spark::saveProfileToDirectory(pending_folder_, compressed, nowMs());
                    if (saved.ok) {
                        outcome = ExportOutcome::Saved;
                        message = "Upload failed (" + result.error +
                                  "), so the profile was saved to " + saved.path.string() +
                                  " — open it at " + spark::kViewerUrl;
                    }
                    else {
                        message = "Upload failed (" + result.error +
                                  ") and automatic local save failed (" + saved.error + ").";
                    }
                }
            }
        }
        catch (const std::exception &e) {
            message = std::string("Export failed: ") + e.what();
        }
        catch (...) {
            message = "Export failed with an unknown error.";
        }
        pending_outcome_ = outcome;
        pending_result_ = std::move(message);
        try {
            getServer().getScheduler().runTask(*this, [this]() { announceResult(); });
        }
        catch (...) {
            exporting_.store(false);
            throw;
        }
    }

    // Back on the main thread.
    void announceResult()
    {
        const char *headline = pending_outcome_ == ExportOutcome::Uploaded
                                   ? "Profiler stopped & upload complete!"
                               : pending_outcome_ == ExportOutcome::Saved
                                   ? "Profiler stopped & saved locally!"
                                   : "Profiler stopped.";
        announce(pending_sender_, headline);
        announce(pending_sender_, pending_result_);
        exporting_.store(false);
    }

    void announce(const std::string &sender_name, const std::string &text)
    {
        getLogger().info("{}", text);
        auto player = getServer().getPlayer(sender_name);
        if (player) {
            player->sendMessage("{}[spark] {}{}", ColorFormat::Gold, ColorFormat::Reset, text);
        }
    }

    void cmdTps(endstone::CommandSender &sender)
    {
        sendPerformanceReport(sender, statistics_.snapshot());
    }

    void sendPerformanceReport(endstone::CommandSender &sender,
                               const spark::StatisticsSnapshot &stats)
    {
        sender.sendMessage(
            "{}TPS {}(5s/10s/1m/5m/15m){}: {} / {} / {} / {} / {}",
            ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
            formatTpsValue(stats.tps.last_5s),
            formatTpsValue(stats.tps.last_10s),
            formatTpsValue(stats.tps.last_1m),
            formatTpsValue(stats.tps.last_5m),
            formatTpsValue(stats.tps.last_15m));
        sender.sendMessage(
            "{}MSPT 10s {}(mean/min/median/p95/max){}: {}",
            ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
            formatMsptDistribution(stats.mspt.last_10s));
        sender.sendMessage(
            "{}MSPT 1m  {}(mean/min/median/p95/max){}: {}",
            ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
            formatMsptDistribution(stats.mspt.last_1m));
        sender.sendMessage(
            "{}MSPT 5m  {}(mean/min/median/p95/max){}: {}",
            ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
            formatMsptDistribution(stats.mspt.last_5m));
        sender.sendMessage(
            "{}Process CPU {}(10s/1m/15m){}: {} / {} / {}",
            ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
            formatCpuValue(stats.cpu.process_last_10s),
            formatCpuValue(stats.cpu.process_last_1m),
            formatCpuValue(stats.cpu.process_last_15m));
        sender.sendMessage(
            "{}System CPU {}(10s/1m/15m){}: {} / {} / {}",
            ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
            formatCpuValue(stats.cpu.system_last_10s),
            formatCpuValue(stats.cpu.system_last_1m),
            formatCpuValue(stats.cpu.system_last_15m));

        const std::int64_t history_seconds =
            (stats.history_span_ms + 999) / 1000;
        if (stats.history_span_ms < spark::StatisticsService::kMaximumHistoryMs) {
            sender.sendMessage(
                "{}Statistics history: {}{} {}(longer windows currently use the available history)",
                ColorFormat::Gold, ColorFormat::Gray,
                formatDuration(history_seconds), ColorFormat::Gray);
        }
    }

    void cmdHealth(endstone::CommandSender &sender)
    {
        const spark::StatisticsSnapshot statistics = statistics_.snapshot();
        sendPerformanceReport(sender, statistics);

        const spark::ProcessStats process = spark::gatherProcessStats();
        const spark::SystemStats system = spark::gatherSystemStats(".");
        const std::int64_t uptime = std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::system_clock::now() -
                                        getServer().getStartTime())
                                        .count();
        sender.sendMessage("{}Uptime: {}{}", ColorFormat::Gold,
                           ColorFormat::Gray, formatDuration(uptime));
        sender.sendMessage("{}Players online: {}{}", ColorFormat::Gold,
                           ColorFormat::Gray,
                           getServer().getOnlinePlayers().size());

        if (process.rss_present && process.virtual_present) {
            sender.sendMessage(
                "{}Process memory {}(RSS/virtual){}: {} / {}",
                ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
                formatBytes(static_cast<std::uint64_t>(process.rss_bytes)),
                formatBytes(
                    static_cast<std::uint64_t>(process.virtual_bytes)));
        }
        else if (process.rss_present) {
            sender.sendMessage("{}Process RSS: {}{}", ColorFormat::Gold,
                               ColorFormat::Gray,
                               formatBytes(static_cast<std::uint64_t>(
                                   process.rss_bytes)));
        }
        else if (process.virtual_present) {
            sender.sendMessage("{}Process virtual memory: {}{}",
                               ColorFormat::Gold, ColorFormat::Gray,
                               formatBytes(static_cast<std::uint64_t>(
                                   process.virtual_bytes)));
        }
        if (process.threads_present) {
            sender.sendMessage("{}Process threads: {}{}", ColorFormat::Gold,
                               ColorFormat::Gray, process.threads);
        }
        if (system.memory_present) {
            sender.sendMessage("{}System memory {}(used/total){}: {} / {}",
                               ColorFormat::Gold, ColorFormat::Gray,
                               ColorFormat::Reset,
                               formatBytes(static_cast<std::uint64_t>(
                                   system.mem_used)),
                               formatBytes(static_cast<std::uint64_t>(
                                   system.mem_total)));
        }
        if (system.swap_present) {
            sender.sendMessage(
                "{}Swap/page file {}(used/total){}: {} / {}",
                ColorFormat::Gold, ColorFormat::Gray, ColorFormat::Reset,
                formatBytes(
                    static_cast<std::uint64_t>(system.swap_used)),
                formatBytes(
                    static_cast<std::uint64_t>(system.swap_total)));
        }
        if (system.disk_present) {
            sender.sendMessage("{}Disk {}(used/total){}: {} / {}",
                               ColorFormat::Gold, ColorFormat::Gray,
                               ColorFormat::Reset,
                               formatBytes(static_cast<std::uint64_t>(
                                   system.disk_used)),
                               formatBytes(static_cast<std::uint64_t>(
                                   system.disk_total)));
        }
        if (system.cpu_present) {
            sender.sendMessage("{}CPU: {}{} {}({} logical processors)",
                               ColorFormat::Gold, ColorFormat::Gray,
                               system.cpu_model.empty() ? "unknown model"
                                                        : system.cpu_model,
                               ColorFormat::Gray, system.cpu_threads);
        }
        if (system.os_present) {
            sender.sendMessage("{}OS: {}{} {} {}", ColorFormat::Gold,
                               ColorFormat::Gray, system.os_name,
                               system.os_version, system.os_arch);
        }
    }

    void cmdTickMonitor(endstone::CommandSender &sender, const spark::Arguments &args)
    {
        if (tick_monitor_.running()) {
            tick_monitor_.stop();
            sender.sendMessage("{}Tick monitor disabled.", ColorFormat::Gold);
            return;
        }

        const bool has_percentage = args.boolFlag("threshold");
        const bool has_duration = args.boolFlag("threshold-tick");
        if (has_percentage && has_duration) {
            sender.sendErrorMessage("Choose either --threshold or --threshold-tick, not both.");
            return;
        }

        spark::TickMonitorConfig config;
        if (has_percentage) {
            auto threshold = args.doubleFlag("threshold");
            if (!threshold || *threshold <= 0.0) {
                sender.sendErrorMessage("The percentage threshold must be a positive number.");
                return;
            }
            config.mode = spark::TickMonitorMode::Percentage;
            config.threshold = *threshold;
        }
        else if (has_duration) {
            auto threshold = args.doubleFlag("threshold-tick");
            if (!threshold || *threshold <= 0.0) {
                sender.sendErrorMessage("The tick duration threshold must be a positive number of milliseconds.");
                return;
            }
            config.mode = spark::TickMonitorMode::Duration;
            config.threshold = *threshold;
        }

        if (!tick_monitor_.start(config)) {
            sender.sendErrorMessage("Unable to start the tick monitor with the requested threshold.");
            return;
        }
        tick_monitor_sender_ = sender.getName();
        sender.sendMessage("{}Tick monitor started.{} Calculating the baseline over 120 ticks (about 6 seconds).",
                           ColorFormat::Gold, ColorFormat::Gray);
    }

    void processTickMonitor(double mspt)
    {
        spark::TickMonitorUpdate update = tick_monitor_.onTick(mspt);
        char message[256];
        if (update.setup_completed) {
            std::snprintf(message, sizeof(message),
                          "Tick monitor baseline ready: min %.2fms, average %.2fms, max %.2fms.",
                          update.setup_min_ms, update.baseline_ms, update.setup_max_ms);
            announce(tick_monitor_sender_, message);

            if (tick_monitor_.config().mode == spark::TickMonitorMode::Duration) {
                std::snprintf(message, sizeof(message), "Reporting ticks longer than %.2fms.",
                              tick_monitor_.config().threshold);
            }
            else {
                std::snprintf(message, sizeof(message),
                              "Reporting ticks more than %.2f%% above the baseline.",
                              tick_monitor_.config().threshold);
            }
            announce(tick_monitor_sender_, message);
        }
        if (update.report) {
            std::snprintf(message, sizeof(message),
                          "Tick #%llu lasted %.2fms (%.2f%% change from the %.2fms baseline).",
                          static_cast<unsigned long long>(update.tick), update.duration_ms,
                          update.percentage_change, update.baseline_ms);
            announce(tick_monitor_sender_, message);
        }
    }

    spark::Profiler profiler_;
    spark::StatisticsService statistics_;
    spark::TickMonitor tick_monitor_;
    std::string tick_monitor_sender_ = "CONSOLE";
    std::string bds_executable_sha256_;
    std::atomic<std::uint64_t> main_tid_{0};
    std::atomic<bool> exporting_{false};
    std::string start_sender_name_ = "CONSOLE";
    std::shared_ptr<endstone::Task> tick_task_;
    std::thread export_thread_;

    // Export params, set on the main thread before runExport() runs on export_thread_.
    spark::ExportContext pending_ctx_;
    std::filesystem::path pending_folder_;
    std::string pending_sender_ = "CONSOLE";
    std::string pending_result_;
    bool pending_save_ = false;
    ExportOutcome pending_outcome_ = ExportOutcome::Failed;
};

ENDSTONE_PLUGIN("spark", "0.4.1", SparkPlugin)
{
    description = "spark profiler for Endstone — find what's slowing your server down.";
    authors = {"ReallocAll <ReallocAll@outlook.com>"};
    prefix = "Spark";
    load = endstone::PluginLoadOrder::PostWorld;

    command("spark")
        .description("spark profiler")
        .usages("/spark", "/spark (tps|health|tickmonitor)<module: SparkStatusModule>",
                "/spark (profiler)<module: SparkProfilerModule> "
                "(start|stop|info|cancel)[action: SparkProfilerAction] [flags: message]")
        .permissions("endstone.command.spark");

    permission("endstone.command.spark")
        .description("Allows use of the spark profiler")
        .default_(endstone::PermissionDefault::Operator);
}
