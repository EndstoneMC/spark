#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "application/health/health_command.h"
#include "application/platform_capabilities.h"
#include "application/profiler/profiler_service.h"
#include "core/profiler/profiler.h"
#include "core/ws/viewer_socket.h"

namespace spark::selftest {

struct ProtoField {
    int number = 0;
    int wire_type = 0;
    std::uint64_t varint = 0;
    double real = 0.0;
    std::string_view bytes;
};

bool readProtoVarint(std::string_view bytes, std::size_t &offset, std::uint64_t &value);
bool findProtoField(std::string_view bytes, int number, ProtoField &result, std::size_t occurrence = 0);

class TestDispatcher : public spark::MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override;
    void setReject(bool reject);

private:
    std::atomic<bool> reject_{false};
};

class TestMetadataProvider : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ServerMetadata &metadata, std::int64_t now_ms) override;
    void gatherWorldMetadata(spark::WorldInfo &world, std::string_view minecraft_version) override;
    std::int64_t serverUptimeSeconds() override;
    std::int64_t playerCount() override;
    spark::PlayerPingProvider *playerPingProvider() override;
    bool usedOffThread() const;

private:
    void checkThread();

    std::thread::id owner_thread_ = std::this_thread::get_id();
    std::atomic<bool> used_off_thread_{false};
};

class TestNotifier : public spark::ResultNotifier {
public:
    void notify(const std::string &sender_name, const std::string &text) override;
    bool contains(const std::string &text) const;

private:
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
};

class TestCommandSender : public spark::CommandSender {
public:
    [[nodiscard]] std::string getName() const override;
    [[nodiscard]] bool isPlayer() const override;
    std::vector<std::string> messages;
    std::vector<std::string> errors;

private:
    void sendImpl(const std::string &message) override;
    void errorImpl(const std::string &message) override;
};

void worker(std::atomic<std::uint64_t> &worker_tid, std::atomic<bool> &run);

template <typename Predicate>
bool waitForConditionUntil(Predicate pred, std::chrono::steady_clock::time_point deadline)
{
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

template <typename Predicate, typename Rep, typename Period>
bool waitForCondition(Predicate pred, std::chrono::duration<Rep, Period> timeout)
{
    return waitForConditionUntil(std::move(pred), std::chrono::steady_clock::now() + timeout);
}

void hotOuter();

bool verifySessionIsolation(std::uint64_t worker_tid);
bool verifyStopResponsiveness();
bool verifyTickFiltering(std::uint64_t worker_tid);
bool verifyThreadDiscovery();
bool verifyAllThreadSampling();
bool verifySelectedThreadSampling(std::uint64_t worker_tid);

bool verifyArgumentParsing();
bool verifyUploadFailure();
bool verifyTickMonitor();
bool verifyMultiThreadSerialization();
bool verifyHealthServerConfigurations();
bool verifyLiveProfilerWindowStatistics(std::uint64_t worker_tid);
bool verifyLiveExportStopCancel(std::uint64_t worker_tid);
bool verifyLiveExportTimeout(std::uint64_t worker_tid);
bool verifyViewerShutdownDuringLiveExport(std::uint64_t worker_tid);
bool verifyViewerDisconnectKeepsProfilerRunning(std::uint64_t worker_tid);
bool verifyAllocationViewerLifecycle(std::uint64_t worker_tid);
bool verifyWorkerExceptionBoundaries(std::uint64_t worker_tid);
bool verifyAsyncNetworkCommands(std::uint64_t worker_tid);
bool verifyBackgroundCommandValidation(std::uint64_t worker_tid);
bool verifyRecoveryWriterLifetime(std::uint64_t worker_tid);
bool verifySystemResourceStats();
bool verifyStatisticsService();
bool verifyWorldGaugeStatistics();
bool verifyWorldGaugeAbsentWhenNotRecorded();
bool verifyExecutableHash();

bool verifyCaptureLifecycle();
#ifdef __linux__
bool verifyActiveCaptureTeardown(std::uint64_t worker_tid);
bool verifyDelayedSignalLifecycle();
#endif
#ifdef _WIN32
bool verifyWindowsThreadActivityDetection();
#endif

bool verifyThreadSelectorSemantics();
bool verifyByteSampling();
#if defined(__linux__)
bool verifyAllocationThreadSelection();
bool verifyProcessWideAllocationSampling();
bool verifyAllocationContentionPolicy();
bool verifyAllocationResourcePressure();
bool verifyAllocationLifecycle();
#endif
#ifdef __linux__
bool verifyLinuxImportHooks();
#endif

}  // namespace spark::selftest

namespace spark {

struct ProfilerTestAccess {
    static void expire(Profiler &profiler) { profiler.auto_end_time_ms_ = 1; }
    static void setLiveExportPausedHook(Profiler &profiler, std::function<void()> hook)
    {
        profiler.live_export_paused_hook_ = std::move(hook);
    }
    static void setStopRequestedHook(Profiler &profiler, std::function<void()> hook)
    {
        profiler.stop_requested_hook_ = std::move(hook);
    }
    static bool samplerRunning(const Profiler &profiler) { return profiler.sampler_.running(); }
    static bool allocationSamplerRunning(const Profiler &profiler) { return profiler.allocation_sampler_.running(); }
    static bool allocationHooksInstalled(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.hooksInstalled();
    }
    static std::uint64_t allocationLifecycleDropped(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.lifecycleDropped();
    }
    static std::uint64_t allocationContentionDropped(const Profiler &profiler)
    {
        return profiler.allocation_sampler_.contentionDropped();
    }
    static bool backendRunning(const Profiler &profiler)
    {
        return profiler.mode_ == ProfileMode::Allocation ? profiler.allocation_sampler_.running()
                                                         : profiler.sampler_.running();
    }
    static bool allocationSnapshot(Profiler &profiler, AllocationSnapshot &snapshot, std::string &error)
    {
        return profiler.allocation_sampler_.snapshot(snapshot, error);
    }
    static std::uint64_t samplerServiceStarts(const Profiler &profiler)
    {
        return profiler.sampler_.service_start_count_.load(std::memory_order_relaxed);
    }
    static bool stopRequested(const Profiler &profiler)
    {
        return profiler.sampling_stop_requested_.load(std::memory_order_acquire);
    }
};

struct ProfilerServiceTestAccess {
    static bool start(ProfilerService &service, const ProfilerOptions &options, std::uint64_t main_tid,
                      std::string &error)
    {
        return service.profiler_.start(options, main_tid, error);
    }

    static std::int64_t startTimeMs(const ProfilerService &service) { return service.profiler_.startTimeMs(); }

    static std::string buildLiveSamplerData(ProfilerService &service, std::int64_t now_ms)
    {
        return service.buildLiveSamplerData(service.captureLiveContext(now_ms));
    }

    static std::string liveExport(ProfilerService &service, const ExportContext &context)
    {
        return service.profiler_.liveExport(context);
    }

    static void cancel(ProfilerService &service) { service.profiler_.cancel(); }
    static void expire(ProfilerService &service) { ProfilerTestAccess::expire(service.profiler_); }
    static std::uint64_t sampleCount(const ProfilerService &service) { return service.profiler_.sampleCount(); }
    static void setViewerOpenFunction(
        ProfilerService &service,
        std::function<std::string(ViewerSocket &, const ViewerSocket::UploadCallback &)> open_function)
    {
        service.setViewerOpenFunctionForTesting(std::move(open_function));
    }
    static void setLiveExportPausedHook(ProfilerService &service, std::function<void()> hook)
    {
        ProfilerTestAccess::setLiveExportPausedHook(service.profiler_, std::move(hook));
    }
    static void setStopRequestedHook(ProfilerService &service, std::function<void()> hook)
    {
        ProfilerTestAccess::setStopRequestedHook(service.profiler_, std::move(hook));
    }
    static bool samplerRunning(const ProfilerService &service)
    {
        return ProfilerTestAccess::samplerRunning(service.profiler_);
    }
    static bool backendRunning(const ProfilerService &service)
    {
        return ProfilerTestAccess::backendRunning(service.profiler_);
    }
    static std::uint64_t samplerServiceStarts(const ProfilerService &service)
    {
        return ProfilerTestAccess::samplerServiceStarts(service.profiler_);
    }
    static bool stopRequested(const ProfilerService &service)
    {
        return ProfilerTestAccess::stopRequested(service.profiler_);
    }
    static bool viewerOpenPending(const ProfilerService &service) { return service.viewerOpenPending(); }
    static bool exportCompletionPending(const ProfilerService &service)
    {
        return service.export_completion_pending_.load();
    }
    static void setViewerSocket(ProfilerService &service, std::shared_ptr<ViewerSocket> socket)
    {
        service.setViewerSocketForTesting(std::move(socket));
    }
    static bool hasViewerSocket(const ProfilerService &service) { return service.hasViewerSocketForTesting(); }
    static std::shared_ptr<ViewerSocket> viewerSocket(const ProfilerService &service)
    {
        return service.viewerSocketForTesting();
    }
};

struct WebSocketClientTestAccess {
    static void setRunning(WebSocketClient &client, bool running) { client.running_.store(running); }
};

struct ViewerSocketTestAccess {
    static void markOpen(ViewerSocket &socket)
    {
        std::scoped_lock transport_lock(socket.transport_mutex_);
        socket.prepareOpen();
        socket.ws_ = std::make_unique<WebSocketClient>();
        WebSocketClientTestAccess::setRunning(*socket.ws_, true);
        socket.state_.store(ViewerSocket::ConnectionState::Open, std::memory_order_release);
    }

    static void terminate(ViewerSocket &socket, WebSocketClient::TerminationKind kind)
    {
        socket.onTransportClosed(socket.connection_generation_.load(std::memory_order_acquire), {.kind = kind});
    }
};

struct HealthCommandTestAccess {
    static void setUploadFunction(
        HealthCommand &health,
        std::function<UploadResult(const std::string &, const std::string &, const std::string &, const std::string &)>
            upload_function)
    {
        health.upload_fn_ = [upload_function = std::move(upload_function)](
                                const std::string &bytebin_url, const std::string &viewer_url,
                                const std::string &payload, const std::string &content_type, CancellationToken) {
            return upload_function(bytebin_url, viewer_url, payload, content_type);
        };
    }

    static bool uploading(const HealthCommand &health) { return health.uploading_.load(); }

    static HealthData capture(HealthCommand &health, const CommandSender &sender, std::int64_t now_ms)
    {
        return health.captureHealthData(sender, now_ms);
    }
};

class Sampler;

struct SamplerTestAccess {
    static void setSamplerThreadHook(Sampler &sampler, std::function<void()> hook);
    static bool workersJoinable(const Sampler &sampler);
    static bool verifyContinuousHistory();
};

}  // namespace spark
