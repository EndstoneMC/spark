#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "application/health/health_command.h"
#include "application/platform_capabilities.h"
#include "application/profiler/profiler_service.h"
#include "core/config/trusted_viewers.h"
#include "core/profiler/profiler.h"
#include "selftest_internal.h"

namespace spark::selftest {

bool verifyAsyncNetworkCommands(std::uint64_t worker_tid)
{
    spark::StatisticsService statistics;
    spark::TrustedViewersState trusted_viewers(std::filesystem::temp_directory_path() / "spark-selftest-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata_provider;
    TestNotifier notifier;
    TestCommandSender sender;

    spark::ProfilerService service(statistics, {}, std::filesystem::temp_directory_path(), {}, {}, {}, false, 10,
                                   "by-pool", "default", trusted_viewers, dispatcher, metadata_provider, notifier);
    spark::ProfilerOptions options;
    options.interval_ms = 1;
    std::string error;
    if (!spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error)) {
        std::fprintf(stderr, "async viewer: profiler start failed: %s\n", error.c_str());
        return false;
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    spark::ProfilerServiceTestAccess::setViewerOpenFunction(
        service, [&mutex, &cv, &entered, &release](spark::ViewerSocket &, const spark::ViewerSocket::UploadCallback &) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            cv.notify_one();
            cv.wait(lock, [&release]() { return release; });
            return std::string();
        });
    service.cmdOpen(sender, spark::Arguments({"open"}, true));
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(2), [&entered]() { return entered; })) {
            std::fprintf(stderr, "async viewer: open worker did not start\n");
            return false;
        }
    }
    const bool viewer_metadata_off_thread = metadata_provider.usedOffThread();
    service.cmdCancel(sender);
    {
        std::scoped_lock lock(mutex);
        release = true;
    }
    cv.notify_one();
    service.shutdown();
    if (viewer_metadata_off_thread) {
        std::fprintf(stderr, "async viewer: platform metadata was captured off the owner thread\n");
        return false;
    }

    spark::HealthCommand health(statistics, metadata_provider, {}, {}, {}, trusted_viewers, dispatcher, notifier);
    entered = false;
    release = false;
    spark::HealthCommandTestAccess::setUploadFunction(
        health, [&mutex, &cv, &entered, &release](const std::string &, const std::string &, const std::string &,
                                                  const std::string &) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            cv.notify_one();
            cv.wait(lock, [&release]() { return release; });
            spark::UploadResult result;
            result.error = "controlled failure";
            return result;
        });
    health.cmdHealth(sender, spark::Arguments({"health", "--upload"}, true));
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(2), [&entered]() { return entered; })) {
            std::fprintf(stderr, "async health: upload worker did not start\n");
            return false;
        }
        release = true;
    }
    cv.notify_one();
    if (!waitForCondition([&health]() { return !spark::HealthCommandTestAccess::uploading(health); },
                          std::chrono::seconds(2))) {
        std::fprintf(stderr, "async health: controlled upload did not finish\n");
        return false;
    }
    if (metadata_provider.usedOffThread()) {
        std::fprintf(stderr, "async health: platform metadata was captured off the owner thread\n");
        return false;
    }
    health.shutdown();
    return true;
}

bool verifyBackgroundCommandValidation(std::uint64_t worker_tid)
{
    const auto profile_directory = std::filesystem::temp_directory_path() / "spark-background-state-selftest";
    std::filesystem::remove_all(profile_directory);
    spark::StatisticsService statistics;
    spark::TrustedViewersState trusted_viewers(std::filesystem::temp_directory_path() / "spark-selftest-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata_provider;
    TestNotifier notifier;
    spark::ProfilerService service(statistics, {}, profile_directory, {}, {}, {}, true, 10, "by-pool", "default",
                                   trusted_viewers, dispatcher, metadata_provider, notifier);
    service.setMainThreadId(worker_tid);
    service.startBackgroundProfiler();
    if (!service.running() || !service.isBackgroundRunning()) {
        std::fprintf(stderr, "background validation: background profiler did not start\n");
        return false;
    }

    TestCommandSender sender;
    service.cmdStart(sender, spark::Arguments({"start", "--interval", "invalid"}, true));
    if (sender.errors.empty() || !service.running() || !service.isBackgroundRunning()) {
        std::fprintf(stderr, "background validation: invalid foreground request stopped background profiling\n");
        return false;
    }

    service.cmdStart(sender, spark::Arguments({"start", "--interval", "1"}, true));
    if (!service.running() || service.isBackgroundRunning()) {
        std::fprintf(stderr, "background validation: valid foreground request did not replace background profiling\n");
        return false;
    }
    service.cmdCancel(sender);
    service.onTick(50.0);
    if (!service.running() || !service.isBackgroundRunning()) {
        std::fprintf(stderr,
                     "background validation: cancelled foreground profile did not restore background profiling\n");
        return false;
    }

    service.cmdStart(sender, spark::Arguments({"start", "--interval", "1", "--save-to-file"}, true));
    service.cmdStop(sender, spark::Arguments({"stop"}, true));
    if (!waitForCondition(
            [&service]() {
                service.onTick(0.0);
                return !service.exporting() && service.running() && service.isBackgroundRunning();
            },
            std::chrono::seconds(10))) {
        std::fprintf(stderr, "background validation: explicit stop did not restore background profiling\n");
        return false;
    }
    service.cmdCancel(sender);

    service.startBackgroundProfiler();
    service.cmdStart(sender, spark::Arguments({"start", "--interval", "1", "--timeout", "11", "--save-to-file"}, true));
    spark::ProfilerServiceTestAccess::expire(service);
    service.onTick(50.0);
    if (!waitForCondition(
            [&service]() {
                service.onTick(0.0);
                return !service.exporting();
            },
            std::chrono::seconds(10))) {
        std::fprintf(stderr, "background validation: timed profile did not finish exporting\n");
        return false;
    }
    service.onTick(50.0);
    if (!service.running() || !service.isBackgroundRunning()) {
        std::fprintf(stderr, "background validation: timed foreground profile did not restore background profiling\n");
        return false;
    }
    std::filesystem::remove_all(profile_directory);
    return true;
}

bool verifyRecoveryWriterLifetime(std::uint64_t worker_tid)
{
    const auto directory = std::filesystem::temp_directory_path() / "spark-recovery-lifetime-selftest";
    std::filesystem::remove_all(directory);
    for (int attempt = 0; attempt < 20; ++attempt) {
        spark::Profiler profiler;
        profiler.setRecoveryDirectory(directory);
        spark::ProfilerOptions options;
        options.interval_ms = 1;
        std::string error;
        if (!profiler.start(options, worker_tid, error)) {
            std::fprintf(stderr, "recovery lifetime: profiler start failed: %s\n", error.c_str());
            return false;
        }

        std::atomic<bool> running{true};
        std::thread watchdog([&profiler, &running]() {
            std::uint64_t sequence = 1;
            while (running.load(std::memory_order_acquire)) {
                profiler.journalStallBegin(sequence, sequence);
                profiler.journalStallEnd(sequence, sequence + 1);
                ++sequence;
            }
        });
        if (!profiler.cancel(error)) {
            running.store(false, std::memory_order_release);
            watchdog.join();
            std::fprintf(stderr, "recovery lifetime: profiler cancel failed: %s\n", error.c_str());
            return false;
        }
        running.store(false, std::memory_order_release);
        watchdog.join();
    }
    std::filesystem::remove_all(directory);
    return true;
}

}  // namespace spark::selftest
