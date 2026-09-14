#include <zlib.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "../../net/native_exit_gate.h"
#include "../../net/websocket_lifecycle_test_support.h"
#include "../../net/withheld_http_server.h"
#include "application/platform_capabilities.h"
#include "application/profiler/profile_exporter.h"
#include "application/profiler/profiler_service.h"
#include "core/config/trusted_viewers.h"
#include "core/stats/statistics_service.h"
#include "net/gzip.h"
#ifndef _WIN32
#include <sys/syscall.h>
#endif

namespace spark {

extern thread_local std::function<void()> GzipStepForTesting;

struct ProfilerOpenTestAccess {
    static bool ownsSocket(const ProfilerOpenOrchestrator &open) { return static_cast<bool>(open.viewer_socket_); }
    static std::string upload(ProfilerOpenOrchestrator &open, const ExportContext &context,
                              const CancellationToken &cancellation)
    {
        return open.uploadSamplerData(context, cancellation);
    }
    static void setSocket(ProfilerOpenOrchestrator &open, std::shared_ptr<ViewerSocket> socket)
    {
        open.viewer_socket_ = std::move(socket);
        open.socket_state_ = ProfilerOpenOrchestrator::SocketState::Opening;
    }
    static ViewerUpdateWorker &worker(ProfilerOpenOrchestrator &open) { return *open.viewer_worker_; }
    static void setExecute(ProfilerOpenOrchestrator &open,
                           std::function<std::string(ViewerSocket &, const ViewerSocket::UploadCallback &)> fn)
    {
        open.viewer_open_fn_ = std::move(fn);
    }
};

struct ProfilerLifecycleTestAccess {
    static bool recoveryRetained(const Profiler &profiler)
    {
        return profiler.retain_recovery_journal_on_shutdown_.load(std::memory_order_acquire);
    }

    static void setRecoveryRemove(Profiler &profiler,
                                  std::function<void(const std::filesystem::path &, std::error_code &)> remover)
    {
        profiler.recovery_remove_function_ = std::move(remover);
    }
};

struct ProfileExporterTestAccess {
    using Upload = std::function<UploadResult(const std::string &, const std::string &, const std::string &,
                                              const std::string &, CancellationToken)>;
    using Save = std::function<ProfileFileResult(const std::filesystem::path &, std::string_view, std::int64_t)>;

    static void setUpload(ProfileExporter &exporter, Upload upload) { exporter.upload_function_ = std::move(upload); }
    static void setSave(ProfileExporter &exporter, Save save) { exporter.save_function_ = std::move(save); }
};

struct ProfilerServiceTestAccess {
    static void stopExportLoop(ProfilerService &service)
    {
        std::scoped_lock lock(service.export_mutex_);
        service.export_stop_requested_ = true;
    }
    static void setTimerHook(ProfilerService &service, std::function<void()> hook)
    {
        service.timeout_publication_hook_ = std::move(hook);
    }
    static bool armTimer(ProfilerService &service) { return service.armProfilerTimeout(1); }
    static bool reapTimer(ProfilerService &service)
    {
        return service.profiler_timeout_.reapUntil(std::chrono::steady_clock::now() + std::chrono::seconds(2));
    }
    static auto timerGeneration(const ProfilerService &service) { return service.timeout_generation_.load(); }
    static auto timerCompletion(const ProfilerService &service) { return service.timeout_completion_pending_.load(); }
    static void publishTimer(ProfilerService &service, std::uint64_t generation)
    {
        service.timeout_completion_pending_.store(generation);
    }
    static ProfilerOpenOrchestrator &viewer(ProfilerService &service) { return *service.viewer_open_; }
    using Export = std::function<ProfileExporter::Result(Profiler &, const ExportContext &, bool, CancellationToken)>;

    static Profiler &profiler(ProfilerService &service) { return service.profiler_; }

    static void setExport(ProfilerService &service, Export export_function)
    {
        service.export_function_ = std::move(export_function);
    }

    static void setPostPublicationHook(ProfilerService &service, std::function<void()> hook)
    {
        service.export_post_publication_hook_ = std::move(hook);
    }

    static void setPreparationHook(ProfilerService &service, std::function<void()> hook)
    {
        service.export_job_preparation_hook_ = std::move(hook);
    }

    static void setShutdownBudget(ProfilerService &service, std::chrono::milliseconds timeout)
    {
        service.export_shutdown_timeout_ = timeout;
    }

    static void finish(ProfilerService &service, bool save)
    {
        service.finishProfiler("Alice", true, "player-id", save, "lifecycle test");
    }

    static bool completionPending(const ProfilerService &service)
    {
        return service.export_completion_pending_.load(std::memory_order_acquire);
    }

    static bool workerExited(const ProfilerService &service)
    {
        std::scoped_lock lock(service.export_mutex_);
        return service.export_worker_exited_;
    }

    static bool workerJoinable(const ProfilerService &service)
    {
        std::scoped_lock lock(service.export_mutex_);
        return service.export_thread_.joinable();
    }

    static std::thread::id workerId(const ProfilerService &service)
    {
        std::scoped_lock lock(service.export_mutex_);
        return service.export_worker_id_for_testing_;
    }
};

}  // namespace spark

namespace {

using namespace std::chrono_literals;

std::uint64_t currentThreadId()
{
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
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
        std::this_thread::yield();
    }
    return predicate();
}

class TestDispatcher final : public spark::MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override
    {
        ++calls;
        task();
    }

    std::atomic<int> calls{0};
};

class TestMetadata final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ExportContext &, std::int64_t) override {}
    void gatherWorldMetadata(spark::ExportContext &) override {}
    std::vector<spark::NativePluginSource> nativePluginSources() override
    {
        ++native_calls;
        if (fail_native) {
            throw std::runtime_error("metadata gate");
        }
        return {};
    }
    bool fail_native = false;
    int native_calls = 0;
    std::int64_t serverUptimeSeconds() override { return 0; }
    std::int64_t playerCount() override { return 0; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }
};

class TestNotifier final : public spark::ResultNotifier {
public:
    void notify(const std::string &, const std::string &text) override { messages.push_back(text); }

    std::vector<std::string> messages;
};

struct ServiceFixture {
    explicit ServiceFixture(const std::filesystem::path &root, bool background = false)
        : trusted(root / "trusted-viewers.json"),
          service(statistics, {}, root, {}, "https://viewer/", {}, background, 10, "by-pool", "default", trusted,
                  dispatcher, metadata, notifier)
    {
    }

    spark::StatisticsService statistics;
    TestMetadata metadata;
    TestDispatcher dispatcher;
    TestNotifier notifier;
    spark::TrustedViewersState trusted;
    spark::ProfilerService service;
};

struct BlockingExport {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    spark::CancellationToken cancellation;

    spark::ProfileExporter::Result operator()(spark::Profiler &, const spark::ExportContext &, bool,
                                              const spark::CancellationToken &token)
    {
        std::unique_lock lock(mutex);
        entered = true;
        cancellation = token;
        condition.notify_all();
        condition.wait(lock, [this] { return release; });
        spark::ProfileExporter::Result result;
        result.message = "blocked export released";
        return result;
    }

    bool waitEntered()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, 2s, [this] { return entered; });
    }

    void releaseExport()
    {
        std::scoped_lock lock(mutex);
        release = true;
        condition.notify_all();
    }
};

struct PostPublicationGate {
    std::mutex mutex;
    std::condition_variable condition;
    int calls = 0;
    bool release_first = false;
    bool first_done = false;

    void operator()()
    {
        std::unique_lock lock(mutex);
        ++calls;
        if (calls != 1) {
            return;
        }
        condition.notify_all();
        condition.wait(lock, [this] { return release_first; });
        first_done = true;
        condition.notify_all();
    }

    bool waitEntered()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, 2s, [this] { return calls != 0; });
    }

    void release()
    {
        std::scoped_lock lock(mutex);
        release_first = true;
        condition.notify_all();
    }

    bool waitDone()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, 2s, [this] { return first_done; });
    }
};

std::filesystem::path testRoot(std::string_view name)
{
    const auto root = std::filesystem::temp_directory_path() / ("spark_profiler_export_lifecycle_" + std::string(name));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    return root;
}

spark::ProfileExporter::Result deliveredResult(spark::ExportOutcome outcome, std::string message)
{
    spark::ProfileExporter::Result result;
    result.outcome = outcome;
    result.message = std::move(message);
    return result;
}

void testBlockedExportShutdownRetry()
{
    BlockingExport blocked;
    ServiceFixture fixture(testRoot("blocked"));
    spark::ProfilerServiceTestAccess::setShutdownBudget(fixture.service, 30ms);
    spark::ProfilerServiceTestAccess::setExport(fixture.service, std::ref(blocked));
    spark::ProfilerServiceTestAccess::finish(fixture.service, false);
    assert(blocked.waitEntered());

    std::string error;
    assert(!fixture.service.shutdown(error));
    assert(error.find("5000 milliseconds") != std::string::npos);
    assert(fixture.service.exporting());
    assert(spark::ProfilerServiceTestAccess::workerJoinable(fixture.service));
    assert(fixture.notifier.messages.empty());

    blocked.releaseExport();
    assert(waitFor([&] { return spark::ProfilerServiceTestAccess::workerExited(fixture.service); }, 2s));
    assert(fixture.service.shutdown(error));
    assert(!fixture.service.exporting());
    assert(!spark::ProfilerServiceTestAccess::workerJoinable(fixture.service));
    assert(fixture.notifier.messages.empty());
}

void testPublishedResultConsumedOnTickAndWorkerReused()
{
    PostPublicationGate publication_gate;
    std::atomic<int> export_calls{0};
    ServiceFixture fixture(testRoot("publish"));
    spark::ProfilerServiceTestAccess::setExport(
        fixture.service,
        [&export_calls](spark::Profiler &, const spark::ExportContext &, bool, const spark::CancellationToken &) {
            export_calls.fetch_add(1, std::memory_order_release);
            return deliveredResult(spark::ExportOutcome::Saved, "saved");
        });
    spark::ProfilerServiceTestAccess::setPostPublicationHook(fixture.service, std::ref(publication_gate));

    spark::ProfilerServiceTestAccess::finish(fixture.service, true);
    assert(publication_gate.waitEntered());
    assert(spark::ProfilerServiceTestAccess::completionPending(fixture.service));
    const std::thread::id worker_id = spark::ProfilerServiceTestAccess::workerId(fixture.service);

    fixture.service.onTick(0.0);
    assert(fixture.notifier.messages.size() == 2);
    assert(!fixture.service.exporting());
    assert(fixture.dispatcher.calls.load() == 0);
    fixture.service.onTick(0.0);
    assert(fixture.notifier.messages.size() == 2);

    publication_gate.release();
    assert(publication_gate.waitDone());
    assert(waitFor([&] { return export_calls.load(std::memory_order_acquire) == 1; }, 2s));

    spark::ProfilerServiceTestAccess::finish(fixture.service, true);
    assert(waitFor([&] { return export_calls.load(std::memory_order_acquire) == 2; }, 2s));
    assert(spark::ProfilerServiceTestAccess::workerId(fixture.service) == worker_id);
    assert(waitFor([&] { return spark::ProfilerServiceTestAccess::completionPending(fixture.service); }, 2s));
    fixture.service.onTick(0.0);
    assert(fixture.notifier.messages.size() == 4);

    std::string error;
    assert(fixture.service.shutdown(error));
}

void testPreparationThrowPublishesTerminalFailure()
{
    ServiceFixture fixture(testRoot("throw"));
    spark::ProfilerServiceTestAccess::setPreparationHook(fixture.service, [] { throw std::runtime_error("prepare"); });
    spark::ProfilerServiceTestAccess::finish(fixture.service, false);
    assert(waitFor([&] { return spark::ProfilerServiceTestAccess::completionPending(fixture.service); }, 2s));
    fixture.service.onTick(0.0);
    assert(!fixture.service.exporting());
    assert(waitFor([&] { return spark::ProfilerServiceTestAccess::workerExited(fixture.service); }, 2s));
    assert(spark::ProfilerServiceTestAccess::workerJoinable(fixture.service));
    assert(fixture.notifier.messages.size() == 2);

    spark::ProfilerServiceTestAccess::setPreparationHook(fixture.service, {});
    spark::ProfilerServiceTestAccess::finish(fixture.service, false);
    assert(!spark::ProfilerServiceTestAccess::completionPending(fixture.service));
    assert(!fixture.service.exporting());
    assert(spark::ProfilerServiceTestAccess::workerExited(fixture.service));
    assert(spark::ProfilerServiceTestAccess::workerJoinable(fixture.service));
    assert(fixture.notifier.messages.size() == 3);
    assert(fixture.notifier.messages.back().find("worker has exited") != std::string::npos);

    std::string error;
    assert(fixture.service.shutdown(error));
}

void testShutdownConsumesResultSilentlyAndRetainsJournal()
{
    const auto root = testRoot("silent-shutdown");
    ServiceFixture fixture(root);
    const auto journal = root / "journal";
    const auto sentinel = journal / "sentinel";
    std::filesystem::create_directories(journal);
    {
        std::ofstream output(sentinel, std::ios::binary);
        output << "recovery-sentinel";
    }
    assert(std::filesystem::exists(sentinel));
    fixture.service.setRecoveryDirectory(journal);
    spark::ProfilerServiceTestAccess::setExport(
        fixture.service, [](spark::Profiler &, const spark::ExportContext &, bool, const spark::CancellationToken &) {
            return deliveredResult(spark::ExportOutcome::Saved, "saved");
        });
    spark::ProfilerServiceTestAccess::finish(fixture.service, true);
    assert(waitFor([&] { return spark::ProfilerServiceTestAccess::completionPending(fixture.service); }, 2s));

    std::string error;
    assert(fixture.service.shutdown(error));
    assert(fixture.service.shutdownBackend(error));
    assert(fixture.notifier.messages.empty());
    assert(spark::ProfilerLifecycleTestAccess::recoveryRetained(
        spark::ProfilerServiceTestAccess::profiler(fixture.service)));
    assert(std::filesystem::exists(sentinel));
    std::ifstream input(sentinel, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    assert(contents == "recovery-sentinel");
    fixture.service.onTick(0.0);
    assert(fixture.notifier.messages.empty());
}

void testExporterFallbackCancellationStatus()
{
    const auto root = testRoot("exporter");
    std::atomic<int> uploads{0};
    std::atomic<int> saves{0};

    spark::ProfileExporter exporter(root, "https://bytebin/", "https://viewer/");
    spark::Profiler profiler;
    spark::ProfileExporterTestAccess::setUpload(exporter, [&uploads](const std::string &, const std::string &,
                                                                     const std::string &, const std::string &,
                                                                     const spark::CancellationToken &) {
        ++uploads;
        return spark::UploadResult{.ok = false, .key = {}, .error = "offline"};
    });
    spark::ProfileExporterTestAccess::setSave(
        exporter, [&saves](const std::filesystem::path &path, std::string_view, std::int64_t) {
            ++saves;
            return spark::ProfileFileResult{.ok = true, .path = path / "fallback.sparkprofile", .error = {}};
        });
    auto result = exporter.exportProfile(profiler, {}, false);
    assert(result.outcome == spark::ExportOutcome::Saved);
    assert(uploads.load() == 1);
    assert(saves.load() == 1);
    assert(!result.retain_recovery_journal);

    spark::CancellationSource during_upload;
    uploads.store(0);
    saves.store(0);
    spark::ProfileExporterTestAccess::setUpload(
        exporter, [&during_upload, &uploads](const std::string &, const std::string &, const std::string &,
                                             const std::string &, const spark::CancellationToken &) {
            ++uploads;
            during_upload.requestStop();
            return spark::UploadResult{.ok = false, .key = {}, .error = "offline"};
        });
    result = exporter.exportProfile(profiler, {}, false, during_upload.token());
    assert(result.outcome == spark::ExportOutcome::Failed);
    assert(result.retain_recovery_journal);
    assert(uploads.load() == 1);
    assert(saves.load() == 0);

    spark::CancellationSource during_fallback;
    uploads.store(0);
    saves.store(0);
    spark::ProfileExporterTestAccess::setUpload(exporter, [&uploads](const std::string &, const std::string &,
                                                                     const std::string &, const std::string &,
                                                                     const spark::CancellationToken &) {
        ++uploads;
        return spark::UploadResult{.ok = false, .key = {}, .error = "offline"};
    });
    spark::ProfileExporterTestAccess::setSave(
        exporter, [&during_fallback, &saves](const std::filesystem::path &path, std::string_view, std::int64_t) {
            ++saves;
            during_fallback.requestStop();
            return spark::ProfileFileResult{.ok = true, .path = path / "fallback.sparkprofile", .error = {}};
        });
    result = exporter.exportProfile(profiler, {}, false, during_fallback.token());
    assert(result.outcome == spark::ExportOutcome::Saved);
    assert(result.retain_recovery_journal);
    assert(uploads.load() == 1);
    assert(saves.load() == 1);
}

void testCancellationBeforeOwnedExportLeavesContext()
{
    spark::ProfileExporter exporter(testRoot("owned-cancel"), "https://bytebin/", "https://viewer/");
    spark::Profiler profiler;
    spark::ExportContext context;
    context.endstone_version = "endstone-test";
    context.comment = "cancelled export";
    context.metrics.tps.push_back({.timestamp_ms = 1, .value = 20.0});
    context.plugins.push_back({.name = "Plugin", .version = "1", .author = "Author", .description = "Description"});
    context.server_configurations.emplace("difficulty", "normal");
    const auto expected_metrics = context.metrics.tps.size();
    const auto expected_plugins = context.plugins.size();

    spark::CancellationSource cancellation;
    cancellation.requestStop();
    const auto result = exporter.exportProfile(profiler, std::move(context), false, cancellation.token());
    assert(result.outcome == spark::ExportOutcome::Failed);
    assert(result.retain_recovery_journal);
    assert(context.endstone_version == "endstone-test");
    assert(context.comment == "cancelled export");
    assert(context.metrics.tps.size() == expected_metrics);
    assert(context.plugins.size() == expected_plugins);
    assert(context.server_configurations.at("difficulty") == "normal");
}

void testSuccessfulExportReportsCleanupWarning()
{
    const auto root = testRoot("cleanup-warning");
    std::filesystem::create_directories(root / "journal");
    ServiceFixture fixture(root);
    fixture.service.setRecoveryDirectory(root / "journal");
    spark::ProfilerServiceTestAccess::setExport(
        fixture.service, [](spark::Profiler &, const spark::ExportContext &, bool, const spark::CancellationToken &) {
            return deliveredResult(spark::ExportOutcome::Saved, "saved");
        });
    spark::ProfilerLifecycleTestAccess::setRecoveryRemove(spark::ProfilerServiceTestAccess::profiler(fixture.service),
                                                          [](const std::filesystem::path &, std::error_code &error) {
                                                              error =
                                                                  std::make_error_code(std::errc::permission_denied);
                                                          });

    spark::ProfilerServiceTestAccess::finish(fixture.service, true);
    assert(waitFor([&] { return spark::ProfilerServiceTestAccess::completionPending(fixture.service); }, 2s));
    fixture.service.onTick(0.0);
    assert(fixture.notifier.messages.size() == 2);
    assert(fixture.notifier.messages.back().find("Recovery journal retained") != std::string::npos);
    assert(spark::ProfilerLifecycleTestAccess::recoveryRetained(
        spark::ProfilerServiceTestAccess::profiler(fixture.service)));

    std::string error;
    assert(fixture.service.shutdown(error));
}

void testGzipCancellationAndRoundTrip()
{
    for (const auto size : {0, 1, 16384, 65537}) {
        std::string input(size, 'x');
        for (int i = 0; i < size; ++i) {
            input[i] = static_cast<char>((i * 17 + i / 257) % 256);
        }
        const std::string compressed = spark::gzipCompress(input);
        z_stream stream{};
        assert(inflateInit2(&stream, 31) == Z_OK);
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
        stream.avail_in = static_cast<uInt>(compressed.size());
        std::string output(size + 1, '\0');
        stream.next_out = reinterpret_cast<Bytef *>(output.data());
        stream.avail_out = static_cast<uInt>(output.size());
        assert(inflate(&stream, Z_FINISH) == Z_STREAM_END);
        output.resize(stream.total_out);
        assert(inflateEnd(&stream) == Z_OK);
        assert(output == input);
    }

    spark::CancellationSource cancellation;
    int steps = 0;
    spark::GzipStepForTesting = [&] {
        ++steps;
        cancellation.requestStop();
    };
    bool cancelled = false;
    try {
        spark::gzipCompress(std::string(65537, 'x'), cancellation.token());
    }
    catch (const std::runtime_error &error) {
        cancelled = std::string(error.what()) == "gzip: cancelled";
    }
    spark::GzipStepForTesting = {};
    assert(cancelled && steps == 1);

    spark::Profiler profiler;
    spark::ProfileExporter exporter(testRoot("gzip-cancel"), "http://127.0.0.1:1", "https://viewer/");
    int uploads = 0;
    int saves = 0;
    spark::ProfileExporterTestAccess::setUpload(
        exporter, [&](const auto &, const auto &, const auto &, const auto &, const auto &) {
            ++uploads;
            return spark::UploadResult{};
        });
    spark::ProfileExporterTestAccess::setSave(exporter, [&](const auto &, auto, auto) {
        ++saves;
        return spark::ProfileFileResult{};
    });
    cancellation.reset();
    spark::GzipStepForTesting = [&] {
        cancellation.requestStop();
    };
    const auto result = exporter.exportProfile(profiler, {}, false, cancellation.token());
    spark::GzipStepForTesting = {};
    assert(result.outcome == spark::ExportOutcome::Failed);
    assert(result.retain_recovery_journal);
    assert(uploads == 0 && saves == 0);
}

void testActualFinalUploadCancellation()
{
    spark::test::WithheldHttpServer server;
    spark::Profiler profiler;
    spark::ProfileExporter exporter(testRoot("http-cancel"), server.url(), "https://viewer/");
    spark::CancellationSource cancellation;
    spark::ProfileExporter::Result result;
    std::atomic<bool> done{false};
    std::atomic<int> saves{0};
    spark::ProfileExporterTestAccess::setSave(exporter, [&](const auto &, auto, auto) {
        ++saves;
        return spark::ProfileFileResult{};
    });
    std::thread worker([&] {
        result = exporter.exportProfile(profiler, {}, false, cancellation.token());
        done.store(true);
    });
    assert(server.waitReceived());
    cancellation.requestStop();
    assert(waitFor([&] { return done.load(); }, 3s));
    worker.join();
    assert(result.outcome == spark::ExportOutcome::Failed);
    assert(result.retain_recovery_journal);
    assert(saves.load() == 0);
}

void testWorkerTokenReachesRealOpen()
{
    ServiceFixture fixture(testRoot("open-token"));
    auto &profiler = spark::ProfilerServiceTestAccess::profiler(fixture.service);
    std::string error;
    assert(profiler.start({}, currentThreadId(), error));
    auto &open = spark::ProfilerServiceTestAccess::viewer(fixture.service);
    auto socket = std::make_shared<spark::ViewerSocket>(spark::ViewerSocket::Config{}, spark::Crypto::KeyPair{});
    std::atomic<bool> entered{false};
    std::atomic<bool> cancelled{false};
    spark::ViewerSocketTestAccess::onTransportCreated(*socket, [&](spark::WebSocketClient &transport) {
        spark::WebSocketClientTestAccess::setCreateChannel(transport, [&](const spark::CancellationToken &token) {
            entered.store(true);
            cancelled.store(token.waitForStop(3s));
            return std::string();
        });
    });
    spark::ProfilerOpenTestAccess::setSocket(open, socket);
    auto &worker = spark::ProfilerOpenTestAccess::worker(open);
    assert(worker.start());
    assert(worker.enqueueOpen({}, socket, "Alice"));
    assert(waitFor([&] { return entered.load(); }, 2s));
    open.close();
    assert(open.retireUntil(std::chrono::steady_clock::now() + 2s));
    assert(cancelled.load());
    assert(!spark::ViewerSocketTestAccess::hasTransport(*socket));
    assert(fixture.service.shutdown(error));
}

void testActualLiveUploadCancellation()
{
    spark::test::WithheldHttpServer server;
    spark::Profiler profiler;
    std::string error;
    assert(profiler.start({}, currentThreadId(), error));
    spark::StatisticsService statistics;
    spark::TrustedViewersState trusted(testRoot("live-http") / "trusted.json");
    TestDispatcher dispatcher;
    TestMetadata metadata;
    TestNotifier notifier;
    spark::ProfilerOpenOrchestrator open(profiler, statistics, {}, server.url(), "https://viewer/", {}, trusted,
                                         dispatcher, metadata, notifier);
    auto socket = std::make_shared<spark::ViewerSocket>(spark::ViewerSocket::Config{}, spark::Crypto::KeyPair{});
    spark::ViewerSocketTestAccess::beginOpen(*socket);
    assert(spark::ViewerSocketTestAccess::markOpen(*socket));
    spark::ProfilerOpenTestAccess::setSocket(open, socket);
    auto &worker = spark::ProfilerOpenTestAccess::worker(open);
    assert(worker.start());
    assert(worker.enqueueSampler({}, socket, worker.generation()));
    assert(server.waitReceived());
    open.close();
    assert(open.retireUntil(std::chrono::steady_clock::now() + 3s));
    assert(!spark::ViewerSocketTestAccess::hasTransport(*socket));
    assert(profiler.cancel(error));
    open.shutdown();
}

void testSharedShutdownDeadline()
{
    ServiceFixture fixture(testRoot("shared-deadline"));
    auto &profiler = spark::ProfilerServiceTestAccess::profiler(fixture.service);
    std::string error;
    assert(profiler.start({}, currentThreadId(), error));
    auto &open = spark::ProfilerServiceTestAccess::viewer(fixture.service);
    auto socket = std::make_shared<spark::ViewerSocket>(spark::ViewerSocket::Config{}, spark::Crypto::KeyPair{});
    std::mutex mutex;
    std::condition_variable cv;
    bool release = false;
    std::atomic<bool> entered{false};
    spark::CancellationToken viewer_cancellation;
    spark::ViewerSocketTestAccess::onTransportCreated(*socket, [&](spark::WebSocketClient &transport) {
        spark::WebSocketClientTestAccess::setCreateChannel(transport, [&](const spark::CancellationToken &token) {
            std::unique_lock lock(mutex);
            viewer_cancellation = token;
            entered.store(true);
            cv.wait(lock, [&] { return release; });
            return std::string();
        });
    });
    spark::ProfilerOpenTestAccess::setSocket(open, socket);
    auto &worker = spark::ProfilerOpenTestAccess::worker(open);
    assert(worker.start());
    assert(worker.enqueueOpen({}, socket, "Alice"));
    assert(waitFor([&] { return entered.load(); }, 2s));
    BlockingExport blocked;
    std::atomic<bool> timer_entered{false};
    spark::ProfilerServiceTestAccess::setTimerHook(fixture.service, [&] {
        std::unique_lock lock(mutex);
        timer_entered.store(true);
        cv.wait(lock, [&] { return release; });
    });
    assert(spark::ProfilerServiceTestAccess::armTimer(fixture.service));
    assert(waitFor([&] { return timer_entered.load(); }, 2s));
    spark::ProfilerServiceTestAccess::setExport(fixture.service, std::ref(blocked));
    spark::ProfilerServiceTestAccess::finish(fixture.service, false);
    assert(blocked.waitEntered());
    spark::ProfilerServiceTestAccess::setShutdownBudget(fixture.service, 100ms);
    const auto begin = std::chrono::steady_clock::now();
    std::thread shutdown([&] { assert(!fixture.service.shutdown(error)); });
    assert(waitFor([&] { return blocked.cancellation.stopRequested() && viewer_cancellation.stopRequested(); }, 50ms));
    shutdown.join();
    assert(std::chrono::steady_clock::now() - begin < 200ms);
    assert(spark::ViewerSocketTestAccess::hasTransport(*socket));
    assert(error.find("timer") != std::string::npos);
    spark::ProfilerServiceTestAccess::setShutdownBudget(fixture.service, 0ms);
    assert(!fixture.service.shutdown(error));
    {
        std::scoped_lock lock(mutex);
        release = true;
    }
    cv.notify_all();
    blocked.releaseExport();
    spark::ProfilerServiceTestAccess::setShutdownBudget(fixture.service, 2s);
    assert(fixture.service.shutdown(error));
    assert(!spark::ViewerSocketTestAccess::hasTransport(*socket));
}

class LifecycleSender final : public spark::CommandSender {
public:
    [[nodiscard]] std::string getName() const override { return "Lifecycle"; }
    [[nodiscard]] bool isPlayer() const override { return false; }
    std::vector<std::string> errors;

private:
    void sendImpl(const std::string &) override {}
    void errorImpl(const std::string &message) override { errors.push_back(message); }
};

void testStaleTimerPublicationCannotStopReplacement()
{
    ServiceFixture fixture(testRoot("stale-timer"));
    fixture.service.setMainThreadId(currentThreadId());
    LifecycleSender sender;
    fixture.service.cmdStart(sender, spark::Arguments({"start"}, true));
    assert(fixture.service.running());
    std::mutex mutex;
    std::condition_variable cv;
    bool release = false;
    std::atomic<bool> entered{false};
    spark::ProfilerServiceTestAccess::setTimerHook(fixture.service, [&] {
        std::unique_lock lock(mutex);
        entered.store(true);
        cv.wait(lock, [&] { return release; });
    });
    assert(spark::ProfilerServiceTestAccess::armTimer(fixture.service));
    const auto old_generation = spark::ProfilerServiceTestAccess::timerGeneration(fixture.service);
    assert(waitFor([&] { return entered.load(); }, 2s));
    fixture.service.cmdCancel(sender);
    fixture.service.cmdStart(sender, spark::Arguments({"start"}, true));
    assert(!fixture.service.running());
    assert(sender.errors.back() == "previous profiler timer still stopping; retry");
    {
        std::scoped_lock lock(mutex);
        release = true;
    }
    cv.notify_all();
    assert(spark::ProfilerServiceTestAccess::reapTimer(fixture.service));
    assert(spark::ProfilerServiceTestAccess::timerCompletion(fixture.service) == old_generation);
    fixture.service.onTick(0.0);
    assert(!fixture.service.exporting());
    fixture.service.cmdStart(sender, spark::Arguments({"start"}, true));
    assert(fixture.service.running());
    spark::ProfilerServiceTestAccess::publishTimer(fixture.service, old_generation);
    fixture.service.onTick(0.0);
    assert(fixture.service.running());
    assert(!fixture.service.exporting());
    fixture.service.cmdCancel(sender);
    std::string error;
    assert(fixture.service.shutdown(error));
    fixture.service.cmdStart(sender, spark::Arguments({"start"}, true));
    assert(!fixture.service.running());
    assert(!spark::ProfilerServiceTestAccess::armTimer(fixture.service));
}

void testBackgroundViewerReplacement(bool pending, bool fail_metadata, bool fail_start, bool hold_retirement)
{
    ServiceFixture fixture(testRoot("background-replacement"), true);
    auto &open = spark::ProfilerServiceTestAccess::viewer(fixture.service);
    fixture.service.setMainThreadId(currentThreadId());
    fixture.service.startBackgroundProfiler();
    assert(fixture.service.isBackgroundRunning());
    LifecycleSender sender;
    spark::ProfilerOpenTestAccess::setExecute(open, [](spark::ViewerSocket &socket, const auto &) {
        spark::ViewerSocketTestAccess::beginOpen(socket);
        assert(spark::ViewerSocketTestAccess::markOpen(socket));
        return std::string("https://old-viewer/");
    });
    fixture.service.cmdOpen(sender, spark::Arguments({"open"}, true));
    auto &worker = spark::ProfilerOpenTestAccess::worker(open);
    assert(waitFor([&] { return worker.available(); }, 2s));
    assert(worker.openPending());
    if (!pending) {
        open.onTick("Lifecycle");
        assert(open.viewerSocket());
    }
    const auto old_socket = open.viewerSocket();
    if (!pending) {
        const auto calls = fixture.metadata.native_calls;
        fixture.service.cmdStart(sender, spark::Arguments({"start", "--regex"}, true));
        assert(fixture.service.isBackgroundRunning());
        assert(open.viewerSocket() == old_socket);
        assert(fixture.metadata.native_calls == calls);
        fixture.service.setMainThreadId(0);
        fixture.service.cmdStart(sender, spark::Arguments({"start"}, true));
        assert(open.viewerSocket() == old_socket);
        assert(fixture.service.isBackgroundRunning());
        fixture.service.setMainThreadId(currentThreadId());
    }
    fixture.metadata.fail_native = fail_metadata;
    std::atomic<bool> release_gate{false};
    std::atomic<bool> gate_entered{false};
    std::thread holder;
    if (hold_retirement) {
        assert(old_socket);
        holder = std::thread([&] {
            auto lock = spark::ViewerSocketTestAccess::lockOpen(*old_socket);
            gate_entered.store(true);
            while (!release_gate.load()) {
                std::this_thread::yield();
            }
        });
        assert(waitFor([&] { return gate_entered.load(); }, 2s));
    }
    const auto metadata_calls = fixture.metadata.native_calls;
    fixture.service.cmdStart(sender, fail_start ? spark::Arguments({"start", "--thread", "[", "--regex"}, true)
                                                : spark::Arguments({"start"}, true));
    if (hold_retirement) {
        assert(!fixture.service.running());
        assert(sender.errors.back() == "previous live viewer still closing; retry");
        assert(fixture.metadata.native_calls == metadata_calls);
        release_gate.store(true);
        holder.join();
        fixture.service.cmdStart(sender, spark::Arguments({"start"}, true));
    }
    assert(!open.viewerSocket());
    assert(!worker.openPending());
    if (fail_metadata || fail_start) {
        assert(!fixture.service.running());
        assert(!sender.errors.empty());
        fixture.metadata.fail_native = false;
        fixture.service.cmdStart(sender, spark::Arguments({"start"}, true));
    }
    assert(fixture.service.running());
    assert(!fixture.service.isBackgroundRunning());
    open.onTick("Lifecycle");
    assert(!open.viewerSocket());
    fixture.service.cmdOpen(sender, spark::Arguments({"open"}, true));
    assert(waitFor([&] { return worker.available(); }, 2s));
    open.onTick("Lifecycle");
    assert(open.viewerSocket());
    assert(open.viewerSocket() != old_socket);
    fixture.service.cmdCancel(sender);
    std::string error;
    assert(fixture.service.shutdown(error));
}

void testFailedBackgroundCancelPreservesViewer()
{
    const auto root = testRoot("background-cancel-failure");
    ServiceFixture fixture(root, true);
    fixture.service.setRecoveryDirectory(root / "journal");
    fixture.service.setMainThreadId(currentThreadId());
    fixture.service.startBackgroundProfiler();
    assert(fixture.service.isBackgroundRunning());
    auto &open = spark::ProfilerServiceTestAccess::viewer(fixture.service);
    auto socket = std::make_shared<spark::ViewerSocket>(spark::ViewerSocket::Config{}, spark::Crypto::KeyPair{});
    spark::ViewerSocketTestAccess::beginOpen(*socket);
    assert(spark::ViewerSocketTestAccess::markOpen(*socket));
    spark::ProfilerOpenTestAccess::setSocket(open, socket);
    const auto metadata_calls = fixture.metadata.native_calls;
    auto &profiler = spark::ProfilerServiceTestAccess::profiler(fixture.service);
    spark::ProfilerLifecycleTestAccess::setRecoveryRemove(profiler, [](const auto &, std::error_code &error) {
        error = std::make_error_code(std::errc::permission_denied);
    });
    LifecycleSender sender;
    fixture.service.cmdStart(sender, spark::Arguments({"start"}, true));
    assert(!sender.errors.empty());
    assert(sender.errors.back().find("Couldn't stop the background profiler safely") != std::string::npos);
    assert(socket->isOpen());
    assert(fixture.metadata.native_calls == metadata_calls);
    spark::ProfilerLifecycleTestAccess::setRecoveryRemove(profiler, {});
    assert(open.retireUntil(std::chrono::steady_clock::now() + 2s));
    std::string error;
    assert(fixture.service.shutdown(error));
}

void testActiveExportRejectsReplacement()
{
    ServiceFixture fixture(testRoot("active-export-start"), true);
    fixture.service.setMainThreadId(currentThreadId());
    fixture.service.startBackgroundProfiler();
    BlockingExport blocked;
    spark::ProfilerServiceTestAccess::setExport(fixture.service, std::ref(blocked));
    spark::ProfilerServiceTestAccess::finish(fixture.service, false);
    assert(blocked.waitEntered());
    const auto calls = fixture.metadata.native_calls;
    LifecycleSender sender;
    fixture.service.cmdStart(sender, spark::Arguments({"start"}, true));
    assert(!fixture.service.running());
    assert(fixture.service.exporting());
    assert(fixture.metadata.native_calls == calls);
    blocked.releaseExport();
    std::string error;
    assert(fixture.service.shutdown(error));
}

void testExportNativeExitIsRequired()
{
    spark::test::NativeExitGate gate;
    ServiceFixture fixture(testRoot("native-exit"));
    spark::ProfilerServiceTestAccess::setShutdownBudget(fixture.service, 20ms);
    spark::ProfilerServiceTestAccess::setExport(fixture.service, [&](auto &, const auto &, bool, const auto &) {
        spark::test::holdNativeThreadExit(gate);
        spark::ProfilerServiceTestAccess::stopExportLoop(fixture.service);
        return spark::ProfileExporter::Result{};
    });
    spark::ProfilerServiceTestAccess::finish(fixture.service, false);
    gate.waitEntered();
    assert(spark::ProfilerServiceTestAccess::workerExited(fixture.service));
    std::string error;
    const auto begin = std::chrono::steady_clock::now();
    assert(!fixture.service.shutdown(error));
    assert(std::chrono::steady_clock::now() - begin < 250ms);
    assert(spark::ProfilerServiceTestAccess::workerJoinable(fixture.service));
    spark::ProfilerServiceTestAccess::finish(fixture.service, false);
    assert(spark::ProfilerServiceTestAccess::workerJoinable(fixture.service));
    gate.unblock();
    spark::ProfilerServiceTestAccess::setShutdownBudget(fixture.service, 2s);
    assert(fixture.service.shutdown(error));
}

void testGenerationCancellationDrainsSignedCloseWithZeroBudget()
{
    ServiceFixture fixture(testRoot("generation-zero-close"));
    auto &profiler = spark::ProfilerServiceTestAccess::profiler(fixture.service);
    std::string error;
    assert(profiler.start({}, currentThreadId(), error));
    auto &open = spark::ProfilerServiceTestAccess::viewer(fixture.service);
    const auto key = spark::Crypto::generateKeyPair();
    const auto expected = spark::encodeServerClose(key.private_key_pkcs8);
    auto socket = std::make_shared<spark::ViewerSocket>(spark::ViewerSocket::Config{}, key);
    spark::test::NativeExitGate handshake;
    spark::WebSocketClient *transport = nullptr;
    std::vector<std::string> sent;
    spark::ViewerSocketTestAccess::onTransportCreated(*socket, [&](spark::WebSocketClient &client) {
        transport = &client;
        assert(spark::WebSocketClientTestAccess::localCloseMessage(client) == expected);
        spark::WebSocketClientTestAccess::prepareIo(
            client, [&] { handshake.block(); },
            [&](std::string_view bytes) {
                sent.emplace_back(bytes);
                return std::pair{CURLE_OK, bytes.size()};
            });
    });
    spark::ProfilerOpenTestAccess::setSocket(open, socket);
    auto &worker = spark::ProfilerOpenTestAccess::worker(open);
    assert(worker.start());
    const auto generation = worker.enqueueOpen({}, socket, "Lifecycle");
    assert(generation);
    handshake.waitEntered();
    assert(sent.empty());
    auto send_lock = spark::WebSocketClientTestAccess::lockSend(*transport);
    open.close();
    assert(!worker.current(*generation));
    open.onTick("Lifecycle");
    assert(spark::ProfilerOpenTestAccess::ownsSocket(open));
    send_lock.unlock();
    handshake.unblock();
    assert(waitFor(
        [&] {
            open.onTick("Lifecycle");
            return !spark::ProfilerOpenTestAccess::ownsSocket(open);
        },
        3s));
    assert(sent == std::vector<std::string>{expected});
    assert(!spark::ViewerSocketTestAccess::hasTransport(*socket));
    open.onTick("Lifecycle");
    assert(sent.size() == 1);
    assert(fixture.service.shutdown(error));
}

[[noreturn]] void failClosedTerminate() noexcept
{
    std::_Exit(91);
}

int runDestructorFailureChild()
{
    std::set_terminate(failClosedTerminate);
    BlockingExport blocked;
    ServiceFixture fixture(testRoot("destructor"));
    spark::ProfilerServiceTestAccess::setShutdownBudget(fixture.service, 20ms);
    spark::ProfilerServiceTestAccess::setExport(fixture.service, std::ref(blocked));
    spark::ProfilerServiceTestAccess::finish(fixture.service, false);
    assert(blocked.waitEntered());
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--destructor-fail-closed") {
        return runDestructorFailureChild();
    }
    testBlockedExportShutdownRetry();
    testPublishedResultConsumedOnTickAndWorkerReused();
    testPreparationThrowPublishesTerminalFailure();
    testShutdownConsumesResultSilentlyAndRetainsJournal();
    testExporterFallbackCancellationStatus();
    testCancellationBeforeOwnedExportLeavesContext();
    testSuccessfulExportReportsCleanupWarning();
    testGzipCancellationAndRoundTrip();
    testActualFinalUploadCancellation();
    testWorkerTokenReachesRealOpen();
    testActualLiveUploadCancellation();
    testSharedShutdownDeadline();
    testStaleTimerPublicationCannotStopReplacement();
    testBackgroundViewerReplacement(true, false, false, false);
    testBackgroundViewerReplacement(false, false, false, false);
    testBackgroundViewerReplacement(false, true, false, false);
    testBackgroundViewerReplacement(false, false, true, false);
    testBackgroundViewerReplacement(false, false, false, true);
    testFailedBackgroundCancelPreservesViewer();
    testActiveExportRejectsReplacement();
    testExportNativeExitIsRequired();
    testGenerationCancellationDrainsSignedCloseWithZeroBudget();
    return 0;
}
