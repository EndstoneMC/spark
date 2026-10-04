#include <atomic>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>

#include <sys/syscall.h>
#endif

#include "application/profiler/profiler_service.h"
#include "native/diagnostics/ci_diagnostics.h"

namespace spark {

struct ProfilerServiceTestAccess {
    static void announceResult(ProfilerService &service)
    {
        service.pending_outcome_ = ExportOutcome::Failed;
        service.pending_sender_ = "Alice";
        service.pending_result_ = "export failed";
        service.exporting_.store(true);
        service.announceResult();
    }

    static bool startProfiler(ProfilerService &service, const ProfilerOptions &options, std::uint64_t tid,
                              std::string &error)
    {
        return service.profiler_.start(options, tid, error);
    }

    static bool autoForeground(const ProfilerService &service)
    {
        return service.session_type_ == ProfilerService::SessionType::AutoForeground;
    }

    static bool manualForeground(const ProfilerService &service)
    {
        return service.session_type_ == ProfilerService::SessionType::ManualForeground;
    }

    static void ageAutoTrigger(ProfilerService &service) { service.auto_profiler_threshold_since_ms_ = 1; }

    static int activeInterval(const ProfilerService &service) { return service.profiler_.options().interval_ms; }
    static std::int64_t activeTimeout(const ProfilerService &service)
    {
        return service.profiler_.options().timeout_seconds;
    }
};

}  // namespace spark

namespace {

class Dispatcher final : public spark::MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override { task(); }
};

class Metadata final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ServerMetadata &, std::int64_t) override
    {
        if (throw_server) {
            throw std::runtime_error("server metadata failed");
        }
    }
    void gatherWorldMetadata(spark::WorldInfo &, std::string_view) override {}
    std::vector<spark::NativePluginSource> nativePluginSources() override
    {
        if (throw_native) {
            throw std::runtime_error("native metadata failed");
        }
        return {};
    }
    std::int64_t serverUptimeSeconds() override { return 0; }
    std::int64_t playerCount() override { return 0; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }

    bool throw_native = false;
    bool throw_server = false;
};

class ThrowingNotifier final : public spark::ResultNotifier {
public:
    void notify(const std::string &, const std::string &) override
    {
        ++calls;
        if (throwing) {
            throw std::runtime_error("notify failed");
        }
    }

    bool throwing = true;
    std::size_t calls = 0;
};

class Sender final : public spark::CommandSender {
public:
    [[nodiscard]] std::string getName() const override { return "Alice"; }
    [[nodiscard]] bool isPlayer() const override { return true; }
    std::vector<std::string> errors;

private:
    void sendImpl(const std::string &) override {}
    void errorImpl(const std::string &message) override { errors.push_back(message); }
};

std::uint64_t currentThreadId()
{
#ifdef _WIN32
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#elif defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#else
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

void test_throwing_notifier_does_not_strand_export()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-profiler-failure-viewers.json");
    spark::CiDiagnostics diagnostics;
    assert(diagnostics.openForTesting(true));
    spark::ProfilerService service(statistics, {}, {}, {}, {}, {}, false, 10, "by-pool", "default", trusted, dispatcher,
                                   metadata, notifier);

    spark::ProfilerServiceTestAccess::announceResult(service);
    assert(!service.exporting());
    assert(notifier.calls == 2);
    const auto notification = spark::readCiDiagnosticSnapshot(
        diagnostics.regionForTesting()->records[static_cast<std::size_t>(spark::CiDiagnosticContext::Notification)]);
    assert(notification.phase == spark::CiDiagnosticPhase::NotificationExceptionalExit);
}

void test_normal_notifier_publishes_normal_exit()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    notifier.throwing = false;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-profiler-failure-viewers.json");
    spark::CiDiagnostics diagnostics;
    assert(diagnostics.openForTesting(true));
    spark::ProfilerService service(statistics, {}, {}, {}, {}, {}, false, 10, "by-pool", "default", trusted, dispatcher,
                                   metadata, notifier);

    spark::ProfilerServiceTestAccess::announceResult(service);
    assert(!service.exporting());
    assert(notifier.calls == 2);
    const auto notification = spark::readCiDiagnosticSnapshot(
        diagnostics.regionForTesting()->records[static_cast<std::size_t>(spark::CiDiagnosticContext::Notification)]);
    assert(notification.phase == spark::CiDiagnosticPhase::NotificationExit);
}

void test_background_start_fails_closed_on_metadata_exception()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    metadata.throw_native = true;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-profiler-failure-viewers.json");
    spark::ProfilerService service(statistics, {}, {}, {}, {}, {}, true, 10, "by-pool", "default", trusted, dispatcher,
                                   metadata, notifier);
    service.setMainThreadId(currentThreadId());

    service.startBackgroundProfiler();
    assert(!service.running());
    assert(!service.isBackgroundRunning());
}

void test_export_metadata_exception_restores_background()
{
    std::atomic<bool> run{true};
    std::atomic<std::uint64_t> worker_tid{0};
    std::thread worker([&] {
        worker_tid.store(currentThreadId(), std::memory_order_release);
        while (run.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    while (worker_tid.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    spark::StatisticsService statistics;
    Metadata metadata;
    metadata.throw_server = true;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-profiler-failure-viewers.json");
    spark::CiDiagnostics diagnostics;
    assert(diagnostics.openForTesting(true));
    spark::ProfilerService service(statistics, {}, {}, {}, {}, {}, true, 10, "by-pool", "default", trusted, dispatcher,
                                   metadata, notifier);
    service.setMainThreadId(worker_tid.load(std::memory_order_acquire));

    spark::ProfilerOptions options;
    options.interval_ms = 1;
    std::string error;
    assert(spark::ProfilerServiceTestAccess::startProfiler(service, options, worker_tid.load(std::memory_order_acquire),
                                                           error));
    Sender sender;
    service.cmdStop(sender, spark::Arguments({"stop"}, true));
    assert(!service.exporting());
    assert(service.running());
    assert(service.isBackgroundRunning());
    const auto notification = spark::readCiDiagnosticSnapshot(
        diagnostics.regionForTesting()->records[static_cast<std::size_t>(spark::CiDiagnosticContext::Notification)]);
    assert(notification.phase == spark::CiDiagnosticPhase::NotificationExceptionalExit);
    service.cmdCancel(sender);
    service.shutdown();

    run.store(false, std::memory_order_release);
    worker.join();
}

void test_foreground_cancel_restores_background_but_background_cancel_suppresses_it()
{
    std::atomic<bool> run{true};
    std::atomic<std::uint64_t> worker_tid{0};
    std::thread worker([&] {
        worker_tid.store(currentThreadId(), std::memory_order_release);
        while (run.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    while (worker_tid.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    notifier.throwing = false;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-profiler-resume-viewers.json");
    spark::ProfilerService service(statistics, {}, {}, {}, {}, {}, true, 10, "by-pool", "default", trusted, dispatcher,
                                   metadata, notifier);
    service.setMainThreadId(worker_tid.load(std::memory_order_acquire));
    service.startBackgroundProfiler();
    assert(service.running());
    assert(service.isBackgroundRunning());

    Sender sender;
    service.cmdStart(sender, spark::Arguments({"start"}, true));
    assert(service.running());
    assert(!service.isBackgroundRunning());
    service.cmdCancel(sender);
    assert(!service.running());

    service.onTick(1.0);
    assert(service.running());
    assert(service.isBackgroundRunning());

    service.cmdCancel(sender);
    assert(!service.running());
    service.onTick(1.0);
    assert(!service.running());

    service.shutdown();
    run.store(false, std::memory_order_release);
    worker.join();
}

void test_auto_profiler_replaces_and_restores_background()
{
    std::atomic<bool> run{true};
    std::atomic<std::uint64_t> worker_tid{0};
    std::thread worker([&] {
        worker_tid.store(currentThreadId(), std::memory_order_release);
        while (run.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    while (worker_tid.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    notifier.throwing = false;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-auto-profiler-viewers.json");
    spark::AutoProfilerConfig auto_config;
    auto_config.enabled = true;
    auto_config.mspt_threshold = 50.0;
    auto_config.trigger_duration_seconds = 5;
    auto_config.profile_duration_seconds = 9999;
    auto_config.interval_ms = 2;
    auto_config.cooldown_seconds = 30;

    spark::ProfilerService service(statistics, {}, {}, {}, {}, {}, true, 10, "by-pool", "default", trusted, dispatcher,
                                   metadata, notifier, auto_config);
    service.setMainThreadId(worker_tid.load(std::memory_order_acquire));
    service.startBackgroundProfiler();
    assert(service.running());
    assert(service.isBackgroundRunning());

    // A single pathological tick must not satisfy the sustained-duration gate.
    service.onTick(5000.0);
    assert(service.running());
    assert(service.isBackgroundRunning());

    spark::ProfilerServiceTestAccess::ageAutoTrigger(service);
    service.onTick(60.0);
    assert(service.running());
    assert(!service.isBackgroundRunning());
    assert(spark::ProfilerServiceTestAccess::autoForeground(service));
    assert(spark::ProfilerServiceTestAccess::activeInterval(service) == 2);
    assert(spark::ProfilerServiceTestAccess::activeTimeout(service) == 600);

    Sender sender;
    service.cmdCancel(sender);
    assert(!service.running());
    service.onTick(1.0);
    assert(service.running());
    assert(service.isBackgroundRunning());

    spark::ProfilerServiceTestAccess::ageAutoTrigger(service);
    service.onTick(60.0);
    assert(service.running());
    assert(service.isBackgroundRunning());

    service.cmdCancel(sender);
    service.shutdown();
    run.store(false, std::memory_order_release);
    worker.join();
}

void test_failed_auto_start_does_not_consume_cooldown()
{
    std::atomic<bool> run{true};
    std::atomic<std::uint64_t> worker_tid{0};
    std::thread worker([&] {
        worker_tid.store(currentThreadId(), std::memory_order_release);
        while (run.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    while (worker_tid.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    spark::StatisticsService statistics;
    Metadata metadata;
    metadata.throw_native = true;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    notifier.throwing = false;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-auto-retry-viewers.json");
    spark::AutoProfilerConfig auto_config;
    auto_config.enabled = true;
    auto_config.mspt_threshold = 50.0;
    auto_config.trigger_duration_seconds = 5;
    auto_config.profile_duration_seconds = 60;
    auto_config.interval_ms = 2;
    auto_config.cooldown_seconds = 300;

    spark::ProfilerService service(statistics, {}, {}, {}, {}, {}, false, 10, "by-pool", "default", trusted, dispatcher,
                                   metadata, notifier, auto_config);
    service.setMainThreadId(worker_tid.load(std::memory_order_acquire));

    spark::ProfilerServiceTestAccess::ageAutoTrigger(service);
    service.onTick(60.0);
    assert(!service.running());

    // A failed start never produced an automatic session, so it must not consume
    // the post-session cooldown. Once the transient failure clears, another
    // sustained threshold window may retry immediately.
    metadata.throw_native = false;
    spark::ProfilerServiceTestAccess::ageAutoTrigger(service);
    service.onTick(60.0);
    assert(service.running());
    assert(spark::ProfilerServiceTestAccess::autoForeground(service));

    Sender sender;
    service.cmdCancel(sender);
    service.shutdown();
    run.store(false, std::memory_order_release);
    worker.join();
}

void test_manual_profiler_preempts_auto_profiler()
{
    std::atomic<bool> run{true};
    std::atomic<std::uint64_t> worker_tid{0};
    std::thread worker([&] {
        worker_tid.store(currentThreadId(), std::memory_order_release);
        while (run.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });
    while (worker_tid.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    spark::StatisticsService statistics;
    Metadata metadata;
    Dispatcher dispatcher;
    ThrowingNotifier notifier;
    notifier.throwing = false;
    spark::TrustedViewersState trusted(std::filesystem::temp_directory_path() / "spark-auto-preempt-viewers.json");
    spark::AutoProfilerConfig auto_config;
    auto_config.enabled = true;
    auto_config.mspt_threshold = 50.0;
    auto_config.trigger_duration_seconds = 5;
    auto_config.profile_duration_seconds = 60;
    auto_config.interval_ms = 2;
    auto_config.cooldown_seconds = 30;

    spark::ProfilerService service(statistics, {}, {}, {}, {}, {}, true, 10, "by-pool", "default", trusted, dispatcher,
                                   metadata, notifier, auto_config);
    service.setMainThreadId(worker_tid.load(std::memory_order_acquire));
    service.startBackgroundProfiler();

    spark::ProfilerServiceTestAccess::ageAutoTrigger(service);
    service.onTick(60.0);
    assert(service.running());
    assert(spark::ProfilerServiceTestAccess::autoForeground(service));

    Sender sender;
    service.cmdStart(sender, spark::Arguments({"start", "--interval", "7"}, true));
    assert(sender.errors.empty());
    assert(service.running());
    assert(!service.isBackgroundRunning());
    assert(spark::ProfilerServiceTestAccess::manualForeground(service));
    assert(spark::ProfilerServiceTestAccess::activeInterval(service) == 7);

    service.cmdCancel(sender);
    assert(!service.running());
    service.onTick(1.0);
    assert(service.running());
    assert(service.isBackgroundRunning());

    // Preempting an automatic profile starts its cooldown, so high MSPT cannot
    // immediately replace the manually-restored background profile.
    spark::ProfilerServiceTestAccess::ageAutoTrigger(service);
    service.onTick(60.0);
    assert(service.running());
    assert(service.isBackgroundRunning());

    service.cmdCancel(sender);
    service.shutdown();
    run.store(false, std::memory_order_release);
    worker.join();
}

}  // namespace

int main()
{
    test_throwing_notifier_does_not_strand_export();
    test_normal_notifier_publishes_normal_exit();
    test_background_start_fails_closed_on_metadata_exception();
    test_export_metadata_exception_restores_background();
    test_foreground_cancel_restores_background_but_background_cancel_suppresses_it();
    test_auto_profiler_replaces_and_restores_background();
    test_failed_auto_start_does_not_consume_cooldown();
    test_manual_profiler_preempts_auto_profiler();
    return 0;
}
