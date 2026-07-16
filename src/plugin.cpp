#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
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
#include "sampler/profiler.h"
#include "spark_constants.h"

namespace {

using endstone::ColorFormat;

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

const std::string &tpsColor(float tps)
{
    if (tps >= 19.5f) {
        return ColorFormat::Green;
    }
    if (tps >= 18.0f) {
        return ColorFormat::Yellow;
    }
    if (tps >= 15.0f) {
        return ColorFormat::Gold;
    }
    return ColorFormat::Red;
}

const std::string &msptColor(float mspt)
{
    if (mspt <= 25.0f) {
        return ColorFormat::Green;
    }
    if (mspt <= 40.0f) {
        return ColorFormat::Yellow;
    }
    if (mspt <= 50.0f) {
        return ColorFormat::Gold;
    }
    return ColorFormat::Red;
}

#if !defined(_WIN32)
struct ProcInfo {
    bool ok = false;
    long rss_kb = 0;
    long threads = 0;
};

ProcInfo readProcStatus()
{
    ProcInfo info;
    std::ifstream f("/proc/self/status");
    if (!f) {
        return info;
    }
    std::string key;
    while (f >> key) {
        if (key == "VmRSS:") {
            f >> info.rss_kb;
            info.ok = true;
        }
        else if (key == "Threads:") {
            f >> info.threads;
        }
        std::getline(f, key);
    }
    return info;
}
#endif

}  // namespace

class SparkPlugin : public endstone::Plugin {
public:
    void onEnable() override
    {
        tick_task_ = getServer().getScheduler().runTaskTimer(
            *this, [this]() { onServerTick(); }, 0, 1);
        getLogger().info("endstone-spark v{} enabled. Run {}/spark{} to get started.", spark::kVersion,
                         ColorFormat::Gold, ColorFormat::Reset);
    }

    void onDisable() override
    {
        if (profiler_.running()) {
            profiler_.cancel();
        }
        if (export_thread_.joinable()) {
            export_thread_.join();
        }
        getServer().getScheduler().cancelTasks(*this);
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
        if (profiler_.running()) {
            profiler_.onTick(getServer().getCurrentMillisecondsPerTick());
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
        sender.sendMessage("{}/spark profiler start [flags] {}- start profiling the server thread", ColorFormat::Yellow,
                           ColorFormat::Gray);
        sender.sendMessage("{}/spark profiler stop {}- stop profiling and finalize the profile", ColorFormat::Yellow,
                           ColorFormat::Gray);
        sender.sendMessage("{}/spark profiler info {}- show status of the running profiler", ColorFormat::Yellow,
                           ColorFormat::Gray);
        sender.sendMessage("{}/spark profiler cancel {}- stop profiling without generating a profile", ColorFormat::Yellow,
                           ColorFormat::Gray);
        sender.sendMessage("{}/spark tps {}- ticks per second & tick duration", ColorFormat::Yellow,
                           ColorFormat::Gray);
        sender.sendMessage("{}/spark health {}- server health report", ColorFormat::Yellow, ColorFormat::Gray);
        sender.sendMessage("{}Flags: --interval <ms>, --timeout <seconds>, --only-ticks-over <ms>", ColorFormat::Gray);
        sender.sendMessage("{}       --save-to-file, --comment <text>, --include-sleeping", ColorFormat::Gray);
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
        if (args.boolFlag("alloc")) {
            sender.sendErrorMessage(
                "Allocation profiling isn't supported by endstone-spark (native execution profiler only).");
            return;
        }

        spark::ProfilerOptions options;
        auto interval = args.doubleFlag("interval");
        if (args.boolFlag("interval") && !interval) {
            sender.sendErrorMessage("The sampling interval must be a finite number.");
            return;
        }
        if (interval && *interval <= 0.0) {
            sender.sendErrorMessage("The sampling interval must be greater than 0ms.");
            return;
        }
        if (interval && *interval > spark::kMaxSamplingIntervalMs) {
            sender.sendErrorMessage("The sampling interval must not exceed {}ms.", spark::kMaxSamplingIntervalMs);
            return;
        }
        options.interval_ms = interval ? static_cast<int>(*interval + 0.5) : 4;
        if (options.interval_ms < 1) {
            options.interval_ms = 1;
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
        options.threads = args.stringFlag("thread");
        auto comments = args.stringFlag("comment");
        if (!comments.empty()) {
            options.comment = comments.front();
        }
        options.save_to_file = args.boolFlag("save-to-file");
        options.creator_name = sender.getName();
        options.creator_is_player = sender.asPlayer() != nullptr;

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

        sender.sendMessage("{}Profiler is now running!{} (async, {}ms interval)", ColorFormat::Gold,
                           ColorFormat::Gray, options.interval_ms);
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
        sender.sendMessage("{}Profiler is already running!", ColorFormat::Gold);
        std::int64_t ran = (nowMs() - profiler_.startTimeMs()) / 1000;
        sender.sendMessage("So far it has profiled for {} ({} samples).", formatDuration(ran),
                           profiler_.sampleCount());
        std::int64_t auto_end = profiler_.autoEndTimeMs();
        if (auto_end <= 0) {
            sender.sendMessage("To stop and finalize the profile, run: {}/spark profiler stop", ColorFormat::Gray);
        }
        else {
            sender.sendMessage("It finishes automatically in {}.", formatDuration((auto_end - nowMs()) / 1000));
        }
        sender.sendMessage("To cancel without generating a profile, run: {}/spark profiler cancel", ColorFormat::Gray);
    }

    void profilerCancel(endstone::CommandSender &sender)
    {
        if (!profiler_.running()) {
            sender.sendMessage("There isn't an active profiler running.");
            return;
        }
        profiler_.cancel();
        sender.sendMessage("{}Profiler has been cancelled.", ColorFormat::Gold);
    }

    // Fast on the main thread (join), then the slow export + network upload runs on a
    // background thread so the server tick never stalls. The background task and its
    // hop back to the main thread capture only `this` (params live in members) so the
    // std::function handed to Endstone stays in libc++'s ABI-stable small-buffer form,
    // which matters when the plugin and the runtime are built with different libc++.
    void finishProfiler(const std::string &sender_name, bool save, const std::string &comment)
    {
        pending_ctx_.endstone_version = getServer().getVersion();
        pending_ctx_.minecraft_version = getServer().getMinecraftVersion();
        pending_ctx_.comment = comment;
        pending_ctx_.tps = getServer().getAverageTicksPerSecond();
        pending_ctx_.mspt = getServer().getAverageMillisecondsPerTick();
        pending_ctx_.mspt_max = getServer().getCurrentMillisecondsPerTick();
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
        if (endstone::Level *level = getServer().getLevel()) {
            pending_ctx_.world.present = true;
            for (endstone::Dimension *dimension : level->getDimensions()) {
                std::vector<endstone::Actor *> actors = dimension->getActors();
                pending_ctx_.world.worlds.push_back({dimension->getName(), static_cast<int>(actors.size())});
                pending_ctx_.world.total_entities += static_cast<int>(actors.size());
                for (endstone::Actor *actor : actors) {
                    pending_ctx_.world.entity_counts[actor->getType()]++;
                }
            }
        }

        pending_save_ = save;
        pending_sender_ = sender_name;
        pending_folder_ = getDataFolder();

        profiler_.stopSampling();
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
        bool ok = false;
        std::string message;
        try {
            std::string body = profiler_.exportData(pending_ctx_);
            if (pending_save_) {
                std::error_code ec;
                std::filesystem::create_directories(pending_folder_, ec);
                std::filesystem::path path =
                    pending_folder_ / ("profile-" + std::to_string(nowMs()) + ".sparkprofile");
                std::ofstream out(path, std::ios::binary);
                out.write(body.data(), static_cast<std::streamsize>(body.size()));
                if (out) {
                    ok = true;
                    message = "Saved to " + path.string() + " — open it at " + spark::kViewerUrl;
                }
                else {
                    message = "Failed to write the profile to disk.";
                }
            }
            else {
                std::string gz = spark::gzipCompress(body);
                spark::UploadResult result =
                    spark::uploadToBytebin(gz, spark::kBytebinUrl, spark::kSamplerContentType,
                                                    std::string("endstone-spark/") + spark::kVersion);
                if (result.ok) {
                    ok = true;
                    message = std::string(spark::kViewerUrl) + result.key;
                }
                else {
                    message = "Upload failed (" + result.error + "). Retry with --save-to-file.";
                }
            }
        }
        catch (const std::exception &e) {
            message = std::string("Export failed: ") + e.what();
        }
        catch (...) {
            message = "Export failed with an unknown error.";
        }
        pending_ok_ = ok;
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
        const char *headline = !pending_ok_ ? "Profiler stopped."
                               : pending_save_ ? "Profiler stopped & saved!"
                                               : "Profiler stopped & upload complete!";
        announce(pending_sender_, headline);
        announce(pending_sender_, pending_result_);
        exporting_.store(false);
    }

    void announce(const std::string &sender_name, const std::string &text)
    {
        getLogger().info("{}", text);
        if (endstone::Player *player = getServer().getPlayer(sender_name)) {
            player->sendMessage("{}[spark] {}{}", ColorFormat::Gold, ColorFormat::Reset, text);
        }
    }

    void cmdTps(endstone::CommandSender &sender)
    {
        endstone::Server &s = getServer();
        float ctps = s.getCurrentTicksPerSecond();
        float atps = s.getAverageTicksPerSecond();
        float cmspt = s.getCurrentMillisecondsPerTick();
        float amspt = s.getAverageMillisecondsPerTick();
        sender.sendMessage("{}TPS {}(cur/avg){}: {}{:.1f}{} / {}{:.1f}", ColorFormat::Gold, ColorFormat::Gray,
                           ColorFormat::Reset, tpsColor(ctps), ctps, ColorFormat::Gray, tpsColor(atps), atps);
        sender.sendMessage("{}MSPT {}(cur/avg){}: {}{:.2f}ms{} / {}{:.2f}ms", ColorFormat::Gold, ColorFormat::Gray,
                           ColorFormat::Reset, msptColor(cmspt), cmspt, ColorFormat::Gray, msptColor(amspt), amspt);
    }

    void cmdHealth(endstone::CommandSender &sender)
    {
        cmdTps(sender);
        long uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() -
                                                                       getServer().getStartTime())
                          .count();
        sender.sendMessage("{}Uptime: {}{}", ColorFormat::Gold, ColorFormat::Gray, formatDuration(uptime));
        sender.sendMessage("{}Players online: {}{}", ColorFormat::Gold, ColorFormat::Gray,
                           getServer().getOnlinePlayers().size());
#if !defined(_WIN32)
        ProcInfo info = readProcStatus();
        if (info.ok) {
            sender.sendMessage("{}Memory (RSS): {}{} MB", ColorFormat::Gold, ColorFormat::Gray, info.rss_kb / 1024);
            sender.sendMessage("{}Threads: {}{}", ColorFormat::Gold, ColorFormat::Gray, info.threads);
        }
#endif
    }

    spark::Profiler profiler_;
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
    bool pending_ok_ = false;
};

ENDSTONE_PLUGIN("spark", "0.1.0", SparkPlugin)
{
    description = "spark profiler for Endstone — find what's slowing your server down.";
    authors = {"endstone-spark (profiler format & viewer by lucko/spark)"};
    prefix = "spark";
    load = endstone::PluginLoadOrder::PostWorld;

    command("spark")
        .description("spark profiler")
        .usages("/spark", "/spark (tps|health)<module: SparkStatusModule>",
                "/spark (profiler)<module: SparkProfilerModule> "
                "(start|stop|info|cancel)[action: SparkProfilerAction] [flags: message]")
        .permissions("endstone.command.spark");

    permission("endstone.command.spark")
        .description("Allows use of the spark profiler")
        .default_(endstone::PermissionDefault::Operator);
}
