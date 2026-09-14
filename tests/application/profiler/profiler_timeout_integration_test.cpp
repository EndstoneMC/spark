#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>

#include <sys/syscall.h>
#endif

#include "application/platform_capabilities.h"
#include "application/profiler/profiler_service.h"
#include "core/config/trusted_viewers.h"
#include "core/stats/statistics_service.h"

namespace spark {

struct ProfilerTestAccess {
    static bool samplerRunning(const Profiler &profiler) { return profiler.sampler_.running(); }
};

struct ProfilerServiceTestAccess {
    static bool start(ProfilerService &service, const ProfilerOptions &options, std::uint64_t main_tid,
                      std::string &error)
    {
        return service.profiler_.start(options, main_tid, error);
    }

    static bool armTimeout(ProfilerService &service, std::int64_t timeout_seconds)
    {
        return service.armProfilerTimeout(timeout_seconds);
    }

    static bool timeoutPending(const ProfilerService &service)
    {
        return service.timeout_completion_pending_.load(std::memory_order_acquire) != 0;
    }

    static void cancel(ProfilerService &service)
    {
        service.resetProfilerTimeout();
        service.profiler_.cancel();
    }

    static bool samplerRunning(const ProfilerService &service)
    {
        return ProfilerTestAccess::samplerRunning(service.profiler_);
    }

    static void diagnoseExport(const ProfilerService &service)
    {
        std::fprintf(stderr, "timeout export state: exporting=%d completion_pending=%d stopping=%d\n",
                     static_cast<int>(service.exporting_.load(std::memory_order_acquire)),
                     static_cast<int>(service.export_completion_pending_.load(std::memory_order_acquire)),
                     static_cast<int>(service.stopping_.load(std::memory_order_acquire)));
        bool job_pending = false;
        bool result_present = false;
        bool worker_exited = false;
        bool stop_requested = false;
        {
            const std::unique_lock lock(service.export_mutex_, std::try_to_lock);
            if (!lock.owns_lock()) {
                std::fprintf(stderr, "timeout export state: export_mutex=try-lock-unavailable\n");
                return;
            }
            job_pending = service.export_job_.has_value();
            result_present = service.export_result_.has_value();
            worker_exited = service.export_worker_exited_;
            stop_requested = service.export_stop_requested_;
        }
        std::fprintf(stderr,
                     "timeout export state: job_pending=%d result_present=%d worker_exited=%d stop_requested=%d\n",
                     static_cast<int>(job_pending), static_cast<int>(result_present), static_cast<int>(worker_exited),
                     static_cast<int>(stop_requested));
    }
};

}  // namespace spark

namespace {

using namespace std::chrono_literals;

class TestDispatcher final : public spark::MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override { task(); }
};

class TestMetadataProvider final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ServerMetadata & /*metadata*/, std::int64_t /*now_ms*/) override
    {
        server_metadata_calls_.fetch_add(1, std::memory_order_relaxed);
    }
    void gatherWorldMetadata(spark::WorldInfo & /*world*/, std::string_view /*minecraft_version*/) override {}
    std::int64_t serverUptimeSeconds() override { return 0; }
    std::int64_t playerCount() override { return 0; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }

    int serverMetadataCalls() const { return server_metadata_calls_.load(std::memory_order_relaxed); }

private:
    std::atomic<int> server_metadata_calls_{0};
};

class TestNotifier final : public spark::ResultNotifier {
public:
    void notify(const std::string & /*sender_name*/, const std::string &text) override
    {
        if (text == "Profiler stopped & saved locally!") {
            saved_success_.store(true, std::memory_order_relaxed);
        }
    }

    bool savedSuccess() const { return saved_success_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> saved_success_{false};
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

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

void nativeWorker(std::atomic<bool> &run, std::atomic<std::uint64_t> &thread_id)
{
    thread_id.store(currentThreadId(), std::memory_order_release);
    while (run.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void verifyProfilerTimeoutLifecycle(std::uint64_t worker_tid, const std::filesystem::path &root)
{
    const auto lifecycle_started = std::chrono::steady_clock::now();
    spark::StatisticsService statistics;
    spark::TrustedViewersState trusted_viewers(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata_provider;
    TestNotifier notifier;
    spark::ProfilerService service(statistics, {}, root, {}, {}, {}, false, 10, "by-pool", "default", trusted_viewers,
                                   dispatcher, metadata_provider, notifier);
    spark::ProfilerOptions options;
    options.interval_ms = 1;
    options.save_to_file = true;
    std::string error;

    assert(spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error));
    assert(spark::ProfilerServiceTestAccess::armTimeout(service, 1));
    assert(waitFor(
        [&] {
            return spark::ProfilerServiceTestAccess::timeoutPending(service) &&
                   !spark::ProfilerServiceTestAccess::samplerRunning(service);
        },
        3s));
    assert(service.running());
    assert(!service.exporting());
    assert(metadata_provider.serverMetadataCalls() == 0);

    const auto export_tick_started = std::chrono::steady_clock::now();
    service.onTick(1.0);
    assert(metadata_provider.serverMetadataCalls() != 0);
    assert(!service.running());
    assert(service.exporting());
    const auto export_wait_started = std::chrono::steady_clock::now();
    const bool export_completed = waitFor(
        [&] {
            service.onTick(1.0);
            return !service.exporting();
        },
        6s);
    if (!export_completed) {
        const auto failed_at = std::chrono::steady_clock::now();
        const auto milliseconds = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        std::fprintf(stderr,
                     "timeout integration failure: phase=export-completion lifecycle_ms=%.3f "
                     "before_export_tick_ms=%.3f export_tick_ms=%.3f export_wait_ms=%.3f "
                     "metadata_calls=%d saved_success=%d\n",
                     milliseconds(failed_at - lifecycle_started), milliseconds(export_tick_started - lifecycle_started),
                     milliseconds(export_wait_started - export_tick_started),
                     milliseconds(failed_at - export_wait_started), metadata_provider.serverMetadataCalls(),
                     static_cast<int>(notifier.savedSuccess()));
        spark::ProfilerServiceTestAccess::diagnoseExport(service);
    }
    assert(export_completed);
    assert(notifier.savedSuccess());

    bool saved_profile_found = false;
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() == ".sparkprofile" && entry.is_regular_file() && entry.file_size() > 0) {
            saved_profile_found = true;
        }
    }
    assert(saved_profile_found);

    assert(spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error));
    assert(spark::ProfilerServiceTestAccess::armTimeout(service, 1));
    spark::ProfilerServiceTestAccess::cancel(service);
    std::this_thread::sleep_for(1200ms);
    assert(!service.running());
    assert(!spark::ProfilerServiceTestAccess::timeoutPending(service));

    assert(spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error));
    std::this_thread::sleep_for(1200ms);
    assert(service.running());
    assert(spark::ProfilerServiceTestAccess::samplerRunning(service));
    assert(!spark::ProfilerServiceTestAccess::timeoutPending(service));
    spark::ProfilerServiceTestAccess::cancel(service);
    service.shutdown();
}

}  // namespace

int main()
{
    const auto root = std::filesystem::temp_directory_path() / "spark-profiler-timeout-integration-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    std::atomic<bool> run{true};
    std::atomic<std::uint64_t> worker_tid{0};
    std::thread worker(nativeWorker, std::ref(run), std::ref(worker_tid));
    assert(waitFor([&] { return worker_tid.load(std::memory_order_acquire) != 0; }, 3s));

    verifyProfilerTimeoutLifecycle(worker_tid.load(std::memory_order_acquire), root);

    run.store(false, std::memory_order_release);
    worker.join();
    std::filesystem::remove_all(root, error);
    return 0;
}
