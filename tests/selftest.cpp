// Offline integration tests for sampling, allocation hooks, and spark serialization;
// no BDS is involved. The default mode writes profile.pb and profile.sparkprofile.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#include <sys/syscall.h>
#endif

#include "application/health/health_command.h"
#include "application/platform_capabilities.h"
#include "application/profiler/profiler_service.h"
#include "core/command/arguments.h"
#include "core/config/trusted_viewers.h"
#include "native/alloc/allocation_sampler.h"
#include "native/alloc/allocation_thread_filter.h"
#include "native/alloc/byte_sampler.h"
#if defined(__linux__)
#include "native/alloc/elf_import_hooks.h"
#endif
#include "core/profiler/profiler.h"
#include "core/stats/executable_hash.h"
#include "core/stats/statistics_service.h"
#include "core/stats/tick_monitor.h"
#include "native/sampler/capture.h"
#include "native/sampler/thread_info.h"
#include "native/sampler/types.h"
#include "native/symbol/symbolicate.h"
#include "net/bytebin.h"
#include "net/gzip.h"
#include "net/profile_file.h"
#include "proto/sampler_data.h"
#include "spark_constants.h"

namespace spark {

struct ProfilerTestAccess {
    static void expire(Profiler &profiler) { profiler.auto_end_time_ms_ = 1; }
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

    static void cancel(ProfilerService &service) { service.profiler_.cancel(); }
    static void expire(ProfilerService &service) { ProfilerTestAccess::expire(service.profiler_); }
    static std::uint64_t sampleCount(const ProfilerService &service) { return service.profiler_.sampleCount(); }
    static void setViewerOpenFunction(
        ProfilerService &service,
        std::function<std::string(ViewerSocket &, const ViewerSocket::UploadCallback &)> open_function)
    {
        service.viewer_open_fn_ = std::move(open_function);
    }
};

struct HealthCommandTestAccess {
    static void setUploadFunction(
        HealthCommand &health,
        std::function<UploadResult(const std::string &, const std::string &, const std::string &, const std::string &)>
            upload_function)
    {
        health.upload_fn_ = std::move(upload_function);
    }

    static bool uploading(const HealthCommand &health) { return health.uploading_.load(); }
};

struct SamplerTestAccess {
    static bool verifyContinuousHistory()
    {
        Sampler continuous;
        continuous.config_.continuous = true;
        Sample sample;
        sample.thread_id = 1;
        sample.thread_name = "Server thread";
        sample.weight = 1;
        sample.frames.push_back({0, 1, 1});
        for (std::int32_t window = 0; window <= 7200; ++window) {
            sample.window = window;
            continuous.acceptSample(sample);
            continuous.window_ticks_[window] = WindowTickStats{1, 1.0, 1.0};
            continuous.maybePruneTickHistory(window);
        }
        for (std::uint64_t tick = 0; tick < 10000; ++tick) {
            continuous.recordTickDecision(tick, true);
        }
        const auto &root = continuous.tree_.root();
        const auto thread = continuous.thread_trees_.find(1);
        if (root.times.size() != 3601 || root.times.begin()->first != 3600 || root.times.rbegin()->first != 7200 ||
            continuous.window_ticks_.size() != 3601 || continuous.sampleCount() != 3601 ||
            thread == continuous.thread_trees_.end() || thread->second.tree.root().times.size() != 3601 ||
            continuous.tick_decisions_.size() > Sampler::kTickDecisionCapacity) {
            return false;
        }

        Sampler foreground;
        foreground.config_.continuous = false;
        for (std::int32_t window = 0; window <= 7200; ++window) {
            sample.window = window;
            foreground.acceptSample(sample);
        }
        return foreground.tree_.root().times.size() == 7201 && foreground.sampleCount() == 7201;
    }
};

}  // namespace spark

namespace {

volatile double g_sink = 0.0;

struct ProtoField {
    int number = 0;
    int wire_type = 0;
    std::uint64_t varint = 0;
    double real = 0.0;
    std::string_view bytes;
};

bool readProtoVarint(std::string_view bytes, std::size_t &offset, std::uint64_t &value)
{
    value = 0;
    for (int shift = 0; shift < 64 && offset < bytes.size(); shift += 7) {
        const unsigned char byte = static_cast<unsigned char>(bytes[offset++]);
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

bool nextProtoField(std::string_view bytes, std::size_t &offset, ProtoField &field)
{
    std::uint64_t tag = 0;
    if (!readProtoVarint(bytes, offset, tag) || tag == 0) {
        return false;
    }
    field = ProtoField{};
    field.number = static_cast<int>(tag >> 3);
    field.wire_type = static_cast<int>(tag & 7);
    if (field.wire_type == 0) {
        return readProtoVarint(bytes, offset, field.varint);
    }
    if (field.wire_type == 1) {
        if (offset + sizeof(std::uint64_t) > bytes.size()) {
            return false;
        }
        std::uint64_t bits = 0;
        std::memcpy(&bits, bytes.data() + offset, sizeof(bits));
        std::memcpy(&field.real, &bits, sizeof(bits));
        offset += sizeof(bits);
        return true;
    }
    if (field.wire_type == 2) {
        std::uint64_t size = 0;
        if (!readProtoVarint(bytes, offset, size) || size > bytes.size() - offset) {
            return false;
        }
        field.bytes = bytes.substr(offset, static_cast<std::size_t>(size));
        offset += static_cast<std::size_t>(size);
        return true;
    }
    if (field.wire_type == 5) {
        if (offset + sizeof(std::uint32_t) > bytes.size()) {
            return false;
        }
        offset += sizeof(std::uint32_t);
        return true;
    }
    return false;
}

bool findProtoField(std::string_view bytes, int number, ProtoField &result, std::size_t occurrence = 0)
{
    std::size_t offset = 0;
    std::size_t matched = 0;
    while (offset < bytes.size()) {
        ProtoField field;
        if (!nextProtoField(bytes, offset, field)) {
            return false;
        }
        if (field.number == number && matched++ == occurrence) {
            result = field;
            return true;
        }
    }
    return false;
}

bool nearlyEqual(double actual, double expected)
{
    return std::abs(actual - expected) < 0.000001;
}

bool findProtoPath(std::string_view bytes, std::initializer_list<int> path, ProtoField &result)
{
    std::size_t index = 0;
    for (int number : path) {
        if (!findProtoField(bytes, number, result)) {
            return false;
        }
        if (++index < path.size()) {
            if (result.wire_type != 2) {
                return false;
            }
            bytes = result.bytes;
        }
    }
    return true;
}

bool protoRealEquals(std::string_view bytes, std::initializer_list<int> path, double expected)
{
    ProtoField field;
    return findProtoPath(bytes, path, field) && field.wire_type == 1 && nearlyEqual(field.real, expected);
}

bool protoVarintEquals(std::string_view bytes, std::initializer_list<int> path, std::uint64_t expected)
{
    ProtoField field;
    return findProtoPath(bytes, path, field) && field.wire_type == 0 && field.varint == expected;
}

class TestDispatcher : public spark::MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override { task(); }
};

class TestMetadataProvider : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ExportContext &, std::int64_t) override { checkThread(); }
    void gatherWorldMetadata(spark::ExportContext &) override { checkThread(); }
    std::int64_t serverUptimeSeconds() override { return 0; }
    long playerCount() override { return 0; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }

    bool usedOffThread() const { return used_off_thread_.load(); }

private:
    void checkThread()
    {
        if (std::this_thread::get_id() != owner_thread_) {
            used_off_thread_.store(true);
        }
    }

    std::thread::id owner_thread_ = std::this_thread::get_id();
    std::atomic<bool> used_off_thread_{false};
};

class TestNotifier : public spark::ResultNotifier {
public:
    void notify(const std::string &, const std::string &) override {}
};

class TestCommandSender : public spark::CommandSender {
public:
    std::string getName() const override { return "Console"; }
    bool isPlayer() const override { return false; }
    std::vector<std::string> messages;
    std::vector<std::string> errors;

private:
    void sendImpl(const std::string &message) override { messages.push_back(message); }
    void errorImpl(const std::string &message) override { errors.push_back(message); }
};

#if defined(_WIN32)
void __cdecl ignoreInvalidParameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, std::uintptr_t) {}
#endif

double hotInner(int n)
{
    double s = 0.0;
    for (int i = 0; i < n * 1000; ++i) {
        s += std::sin(i * 0.5) * std::cos(i * 0.25);
    }
    return s;
}

void hotMiddle(int rounds)
{
    for (int i = 0; i < rounds; ++i) {
        g_sink += hotInner(40);
    }
}

void hotOuter()
{
    hotMiddle(20);
}

std::atomic<std::uint64_t> g_worker_tid{0};
std::atomic<bool> g_run{true};

// Poll a condition with a bounded deadline; capture timing is non-deterministic.
template <typename Predicate, typename Rep, typename Period>
bool waitForCondition(Predicate pred, std::chrono::duration<Rep, Period> timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

void worker()
{
#if defined(_WIN32)
    g_worker_tid.store(static_cast<std::uint64_t>(GetCurrentThreadId()));
#else
    g_worker_tid.store(static_cast<std::uint64_t>(::syscall(SYS_gettid)));
#endif
    while (g_run.load()) {
        hotOuter();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the "off-tick" sleep
    }
}

bool verifySessionIsolation(std::uint64_t worker_tid)
{
    using namespace std::chrono_literals;

    spark::SamplerConfig config;
    config.interval_us = 1000;
    config.ignore_sleeping = false;

    spark::Sampler sampler;
    sampler.setTarget(worker_tid);
    if (!sampler.start(config)) {
        std::fprintf(stderr, "session isolation: sampler start failed\n");
        return false;
    }
    // Wait for at least one sample before the observation loop; first capture is non-deterministic.
    waitForCondition([&] { return sampler.sampleCount() > 0; }, 2s);
    std::uint64_t observed_samples = 0;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(1ms);
        sampler.onTick(50.0);
        std::uint64_t current_samples = sampler.sampleCount();
        if (current_samples < observed_samples) {
            std::fprintf(stderr, "session isolation: live sample count moved backwards\n");
            sampler.stop();
            return false;
        }
        observed_samples = current_samples;
    }
    sampler.stop();
    if (sampler.sampleCount() == 0 || sampler.tree().sampleCount() == 0 || sampler.modules().size() == 0 ||
        sampler.numberOfTicks() != 50 || sampler.windowTicks().empty()) {
        std::fprintf(stderr, "session isolation: first sampler session did not collect expected state\n");
        return false;
    }

    sampler.setTarget(0);
    if (!sampler.start(config)) {
        std::fprintf(stderr, "session isolation: sampler restart failed\n");
        return false;
    }
    sampler.stop();
    if (sampler.sampleCount() != 0 || sampler.modules().size() != 0 || sampler.numberOfTicks() != 0 ||
        !sampler.windowTicks().empty()) {
        std::fprintf(stderr, "session isolation: stop/restart retained sampler state\n");
        return false;
    }

    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.interval_ms = 1;
    options.ignore_sleeping = false;
    std::string error;
    if (!profiler.start(options, worker_tid, error)) {
        std::fprintf(stderr, "session isolation: profiler start failed: %s\n", error.c_str());
        return false;
    }
    std::this_thread::sleep_for(50ms);
    profiler.cancel();
    if (profiler.sampleCount() == 0) {
        std::fprintf(stderr, "session isolation: cancelled session did not collect a sample\n");
        return false;
    }

    if (!profiler.start(options, 0, error)) {
        std::fprintf(stderr, "session isolation: profiler restart failed: %s\n", error.c_str());
        return false;
    }
    spark::ExportContext context;
    profiler.stop(context);
    if (profiler.sampleCount() != 0) {
        std::fprintf(stderr, "session isolation: cancel/restart retained samples\n");
        return false;
    }

    return true;
}

bool verifyCaptureLifecycle()
{
    for (int i = 0; i < 3; ++i) {
        if (!spark::Capture::arm()) {
            std::fprintf(stderr, "capture lifecycle: arm failed on iteration %d\n", i + 1);
            return false;
        }
        spark::Capture::disarm();
    }
    return true;
}

#if defined(_WIN32)
bool verifyWindowsThreadActivityDetection()
{
    using namespace std::chrono_literals;

    std::atomic<bool> run{true};
    std::atomic<std::uint64_t> active_tid{0};
    std::atomic<std::uint64_t> sleeping_tid{0};
    std::atomic<std::uint64_t> work{0};
    HANDLE release_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (release_event == nullptr) {
        std::fprintf(stderr, "Windows thread activity: event creation failed\n");
        return false;
    }

    std::thread active([&] {
        active_tid.store(static_cast<std::uint64_t>(::GetCurrentThreadId()));
        while (run.load(std::memory_order_relaxed)) {
            work.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread sleeping([&] {
        sleeping_tid.store(static_cast<std::uint64_t>(::GetCurrentThreadId()));
        ::WaitForSingleObject(release_event, INFINITE);
    });
    while (active_tid.load() == 0 || sleeping_tid.load() == 0) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(20ms);

    auto finish = [&] {
        spark::Capture::disarm();
        run.store(false, std::memory_order_relaxed);
        ::SetEvent(release_event);
        active.join();
        sleeping.join();
        ::CloseHandle(release_event);
    };

    if (!spark::Capture::arm()) {
        std::fprintf(stderr, "Windows thread activity: capture arm failed\n");
        finish();
        return false;
    }
    const bool active_baseline = spark::Capture::isThreadRunning(active_tid.load());
    const bool sleeping_baseline = spark::Capture::isThreadRunning(sleeping_tid.load());
    std::this_thread::sleep_for(40ms);
    const bool active_running = spark::Capture::isThreadRunning(active_tid.load());
    const bool sleeping_running = spark::Capture::isThreadRunning(sleeping_tid.load());
    spark::Capture::disarm();

    if (!spark::Capture::arm()) {
        std::fprintf(stderr, "Windows thread activity: capture re-arm failed\n");
        finish();
        return false;
    }
    const bool restarted_baseline = spark::Capture::isThreadRunning(active_tid.load());
    finish();

    if (active_baseline || sleeping_baseline || !active_running || sleeping_running || restarted_baseline) {
        std::fprintf(stderr, "Windows thread activity: cycle-time classification failed\n");
        return false;
    }
    return true;
}
#endif

bool verifyStopResponsiveness()
{
    using namespace std::chrono_literals;

    spark::SamplerConfig config;
    config.interval_us = 5'000'000;
    spark::Sampler sampler;
    sampler.setTarget(0);
    if (!sampler.start(config)) {
        std::fprintf(stderr, "stop responsiveness: sampler start failed\n");
        return false;
    }
    if (sampler.start(config)) {
        std::fprintf(stderr, "stop responsiveness: running sampler started twice\n");
        sampler.stop();
        return false;
    }
    std::this_thread::sleep_for(10ms);
    auto before = std::chrono::steady_clock::now();
    sampler.stop();
    auto elapsed = std::chrono::steady_clock::now() - before;
    if (elapsed >= 500ms) {
        std::fprintf(stderr, "stop responsiveness: stop took too long\n");
        return false;
    }

    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.interval_ms = spark::kMaxSamplingIntervalMs + 1;
    std::string error;
    if (profiler.start(options, 0, error)) {
        std::fprintf(stderr, "stop responsiveness: excessive interval was accepted\n");
        profiler.cancel();
        return false;
    }

    if constexpr (sizeof(long) > sizeof(std::int32_t)) {
        options.interval_ms = 4;
        options.timeout_seconds = (std::numeric_limits<long>::max)();
        if (profiler.start(options, 0, error)) {
            std::fprintf(stderr, "stop responsiveness: overflowing timeout was accepted\n");
            profiler.cancel();
            return false;
        }
    }
    options.interval_ms = 1;
    options.timeout_seconds = -1;
    if (!profiler.start(options, 0, error)) {
        std::fprintf(stderr, "stop responsiveness: profiler did not recover after failed start\n");
        return false;
    }
    profiler.cancel();
    return true;
}

bool verifyArgumentParsing()
{
    auto integer = [](const std::string &text) {
        spark::Arguments args({"start", "--value", text});
        return args.intFlag("value");
    };
    auto floating = [](const std::string &text) {
        spark::Arguments args({"start", "--value", text});
        return args.doubleFlag("value");
    };

    if (integer("100") != 100 || integer("-1") != -1 || integer("abc") || integer("100abc") ||
        integer("999999999999999999999999999999999999")) {
        std::fprintf(stderr, "argument parsing: integer validation failed\n");
        return false;
    }
    if (floating("1.25") != 1.25 || floating("-1.25") != -1.25 || floating("abc") || floating("100abc") ||
        floating("1e9999") || floating("NaN") || floating("inf")) {
        std::fprintf(stderr, "argument parsing: floating-point validation failed\n");
        return false;
    }

    spark::Arguments missing({"start", "--value"});
    if (!missing.boolFlag("value") || missing.intFlag("value") || missing.doubleFlag("value")) {
        std::fprintf(stderr, "argument parsing: missing value validation failed\n");
        return false;
    }
    const std::vector<std::string> quoted =
        spark::Arguments::tokenize(R"(start --thread "Server thread" --thread '^Worker \d+$' --regex)");
    spark::Arguments selected(quoted);
    const std::vector<std::string> threads = selected.stringFlag("thread");
    if (threads.size() != 2 || threads[0] != "Server thread" || threads[1] != R"(^Worker \d+$)" ||
        !selected.boolFlag("regex")) {
        std::fprintf(stderr, "argument parsing: quoted thread selector validation failed\n");
        return false;
    }
    return true;
}

bool setCurrentThreadName(const char *name)
{
#if defined(_WIN32)
    int length = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (length <= 1) {
        return false;
    }
    std::vector<wchar_t> wide(static_cast<std::size_t>(length));
    if (::MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), length) == 0) {
        return false;
    }
    return SUCCEEDED(::SetThreadDescription(::GetCurrentThread(), wide.data()));
#elif defined(__linux__)
    return ::pthread_setname_np(::pthread_self(), name) == 0;
#else
    (void)name;
    return false;
#endif
}

bool verifyThreadSelectorSemantics()
{
    std::string error;
    spark::ThreadSelector selector;
    if (!selector.configure(false, false, {"alpha", "BETA"}, error) || !selector.matches("ALPHA") ||
        !selector.matches("beta") || selector.matches("alphabet")) {
        std::fprintf(stderr, "thread selector: exact-name semantics failed\n");
        return false;
    }
    if (!selector.configure(false, true, {R"(worker-\d+)", "server"}, error) || !selector.matches("WORKER-42") ||
        !selector.matches("Server") || selector.matches("worker-42-extra")) {
        std::fprintf(stderr, "thread selector: regex/full-match semantics failed\n");
        return false;
    }
    if (selector.configure(false, true, {"["}, error) || error.find("invalid thread name regex") == std::string::npos) {
        std::fprintf(stderr, "thread selector: invalid regex was accepted\n");
        return false;
    }
    if (!selector.configure(true, false, {}, error) || !selector.matches("anything")) {
        std::fprintf(stderr, "thread selector: all-thread semantics failed\n");
        return false;
    }

    spark::AllocationThreadFilter identities(256, 16);
    if (!identities.configure(false, false, {"spark-id-a"}, error)) {
        return false;
    }
    const std::uint64_t tid = spark::currentNativeThreadId();
    if (!setCurrentThreadName("spark-id-a")) {
        std::fprintf(stderr, "thread selector: could not name current thread\n");
        return false;
    }
    const spark::AllocationThreadSelection first = identities.resolve(1, tid);
    if (!setCurrentThreadName("spark-id-b")) {
        return false;
    }
    const spark::AllocationThreadSelection second = identities.resolve(2, tid);
    const spark::AllocationThreadSelection replay = identities.resolve(1, tid);
    if (!first.selected || second.selected || !replay.selected || replay.display_name != first.display_name) {
        std::fprintf(stderr, "thread selector: session identity/TID reuse isolation failed\n");
        return false;
    }

    spark::AllocationThreadFilter unavailable(256, 16);
    if (!unavailable.configure(false, false, {"Thread 18446744073709551615"}, error)) {
        return false;
    }
    const auto missing = unavailable.resolve(1, (std::numeric_limits<std::uint64_t>::max)());
    if (missing.selected || missing.name_available || unavailable.nameFailures() != 1) {
        std::fprintf(stderr, "thread selector: unavailable names did not fail closed\n");
        return false;
    }
    spark::AllocationThreadFilter bounded(256, 1);
    if (!bounded.configure(false, false, {"spark-id-b"}, error) || !bounded.resolve(1, tid).selected ||
        bounded.resolve(2, tid).selected || bounded.cacheDrops() != 1) {
        std::fprintf(stderr, "thread selector: identity cache did not fail bounded\n");
        return false;
    }
    return true;
}

bool verifyUploadFailure()
{
    using namespace std::chrono_literals;

    auto before = std::chrono::steady_clock::now();
    spark::UploadResult result =
        spark::uploadToBytebin("test", "http://127.0.0.1:1", "application/octet-stream", "spark-selftest");
    auto elapsed = std::chrono::steady_clock::now() - before;
    if (result.ok || result.error.empty() || elapsed >= 5s) {
        std::fprintf(stderr, "upload failure: invalid target was not rejected promptly\n");
        return false;
    }
    return true;
}

bool verifyTickFiltering(std::uint64_t worker_tid)
{
    using namespace std::chrono_literals;

    spark::SamplerConfig config;
    config.interval_us = 1000;
    config.ignore_sleeping = false;
    config.only_ticks_over_ms = 10;

    spark::Sampler sampler;
    sampler.setTarget(worker_tid);
    if (!sampler.start(config)) {
        std::fprintf(stderr, "tick filtering: fast session start failed\n");
        return false;
    }
    // Wait for the sampler to complete at least one capture iteration before
    // emitting a tick event, so that buffered samples exist to filter.
    {
        const auto seq0 = sampler.samplerHeartbeat().sequence.load();
        waitForCondition([&] { return sampler.samplerHeartbeat().sequence.load() > seq0; }, 2s);
    }
    sampler.onTick(1.0);
    sampler.stop();
    if (sampler.sampleCount() != 0) {
        std::fprintf(stderr, "tick filtering: fast tick samples were retained\n");
        return false;
    }

    if (!sampler.start(config)) {
        std::fprintf(stderr, "tick filtering: slow session start failed\n");
        return false;
    }
    {
        const auto seq0 = sampler.samplerHeartbeat().sequence.load();
        waitForCondition([&] { return sampler.samplerHeartbeat().sequence.load() > seq0; }, 2s);
    }
    sampler.onTick(50.0);
    sampler.stop();
    if (sampler.sampleCount() == 0 || sampler.tree().sampleCount() == 0) {
        std::fprintf(stderr, "tick filtering: slow tick samples were not retained\n");
        return false;
    }
    return true;
}

bool verifyByteSampling()
{
    constexpr std::uint64_t seed = 0x7f4a7c159e3779b9ULL;
    spark::ByteSamplingState first;
    spark::ByteSamplingState replay;

    spark::resetByteSamplingState(first, 1, seed, 1);
    if (spark::consumeSampledBytes(first, 100000, 1) != 100000) {
        std::fprintf(stderr, "byte sampling: interval=1 was not exact\n");
        return false;
    }

    spark::resetByteSamplingState(first, 1, seed, 64);
    first.bytes_until_sample = 7;
    constexpr std::uint64_t large_request = 1'000'000'000'033ULL;
    constexpr std::uint64_t expected_points = 1 + (large_request - 7) / 64;
    if (spark::consumeSampledBytes(first, large_request, 64) != expected_points ||
        first.bytes_until_sample != 64 - ((large_request - 7) % 64)) {
        std::fprintf(stderr, "byte sampling: large allocation crossing count was incorrect\n");
        return false;
    }

    spark::resetByteSamplingState(first, 2, seed, 64);
    spark::resetByteSamplingState(replay, 2, seed, 64);
    for (int i = 0; i < 1000; ++i) {
        const std::uint64_t bytes = static_cast<std::uint64_t>((i * 7919) % 4096 + 1);
        if (spark::consumeSampledBytes(first, bytes, 64) != spark::consumeSampledBytes(replay, bytes, 64)) {
            std::fprintf(stderr, "byte sampling: identical session seed did not replay\n");
            return false;
        }
    }

    for (const std::uint64_t interval : {4ULL, 64ULL, 1024ULL}) {
        spark::ByteSamplingState state;
        spark::resetByteSamplingState(state, interval, seed ^ interval, interval);
        constexpr std::uint64_t observed = 4'000'000;
        std::uint64_t points = 0;
        for (std::uint64_t consumed = 0; consumed < observed; consumed += 4096) {
            const std::uint64_t chunk = (std::min)(std::uint64_t{4096}, observed - consumed);
            points += spark::consumeSampledBytes(state, chunk, interval);
        }
        const double ratio =
            static_cast<double>(points) * static_cast<double>(interval) / static_cast<double>(observed);
        if (ratio < 0.94 || ratio > 1.06 || state.bytes_until_sample == 0) {
            std::fprintf(stderr, "byte sampling: interval=%llu produced implausible ratio %.6f\n",
                         static_cast<unsigned long long>(interval), ratio);
            return false;
        }
    }
    return true;
}

#if defined(_WIN32) || defined(__linux__)
#if defined(_WIN32)
#define SPARK_NOINLINE __declspec(noinline)
#else
#define SPARK_NOINLINE __attribute__((noinline))
#endif
SPARK_NOINLINE bool exerciseNativeAllocations()
{
    for (std::size_t i = 0; i < 4096; ++i) {
        const std::size_t size = 512 + (i & 255);
        void *allocation = std::malloc(size);
        if (allocation == nullptr) {
            return false;
        }
        static_cast<volatile unsigned char *>(allocation)[0] = static_cast<unsigned char>(i);
        std::free(allocation);
    }
    void *resized = std::malloc(1024);
    if (resized == nullptr) {
        return false;
    }
    void *replacement = std::realloc(resized, 4096);
    if (replacement == nullptr) {
        std::free(resized);
        return false;
    }
    std::free(replacement);

#if defined(_WIN32)
    void *recalloced = _recalloc(nullptr, 32, 32);
    if (recalloced == nullptr) {
        std::fprintf(stderr, "native allocations: _recalloc failed\n");
        return false;
    }
    void *recalloced_replacement = _recalloc(recalloced, 64, 32);
    if (recalloced_replacement == nullptr) {
        std::fprintf(stderr, "native allocations: resized _recalloc failed\n");
        std::free(recalloced);
        return false;
    }
    std::free(recalloced_replacement);

    void *aligned = _aligned_malloc(1024, 64);
    if (aligned == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_malloc failed\n");
        return false;
    }
    void *aligned_replacement = _aligned_realloc(aligned, 4096, 64);
    if (aligned_replacement == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_realloc failed\n");
        _aligned_free(aligned);
        return false;
    }
    void *aligned_recalloced = _aligned_recalloc(aligned_replacement, 128, 64, 64);
    if (aligned_recalloced == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_recalloc failed\n");
        _aligned_free(aligned_replacement);
        return false;
    }
    _aligned_free(aligned_recalloced);

    void *offset = _aligned_offset_malloc(1024, 64, 16);
    if (offset == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_offset_malloc failed\n");
        return false;
    }
    void *offset_replacement = _aligned_offset_realloc(offset, 4096, 64, 16);
    if (offset_replacement == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_offset_realloc failed\n");
        _aligned_free(offset);
        return false;
    }
    void *offset_recalloced = _aligned_offset_recalloc(offset_replacement, 128, 64, 64, 16);
    if (offset_recalloced == nullptr) {
        std::fprintf(stderr, "native allocations: _aligned_offset_recalloc failed\n");
        _aligned_free(offset_replacement);
        return false;
    }
    _aligned_free(offset_recalloced);
#elif defined(__linux__)
    void *array = ::reallocarray(nullptr, 32, 32);
    if (array == nullptr) {
        return false;
    }
    void *array_replacement = ::reallocarray(array, 64, 32);
    if (array_replacement == nullptr) {
        std::free(array);
        return false;
    }
    std::free(array_replacement);
#endif

    void *cross_thread = std::malloc(4096);
    if (cross_thread == nullptr) {
        return false;
    }
    std::thread releaser([cross_thread]() { std::free(cross_thread); });
    releaser.join();
    return true;
}

bool verifyTickMonitor()
{
    spark::TickMonitor monitor;
    spark::TickMonitorConfig config;
    config.setup_ticks = 3;
    config.threshold = 100.0;
    if (!monitor.start(config)) {
        std::fprintf(stderr, "tick monitor: valid percentage configuration was rejected\n");
        return false;
    }

    monitor.onTick(10.0);
    monitor.onTick(20.0);
    spark::TickMonitorUpdate setup = monitor.onTick(30.0);
    if (!setup.setup_completed || setup.report || setup.tick != 3 || setup.baseline_ms != 20.0 ||
        setup.setup_min_ms != 10.0 || setup.setup_max_ms != 30.0) {
        std::fprintf(stderr, "tick monitor: baseline calculation failed\n");
        return false;
    }
    if (monitor.onTick(40.0).report) {
        std::fprintf(stderr, "tick monitor: percentage threshold boundary was included\n");
        return false;
    }
    spark::TickMonitorUpdate spike = monitor.onTick(50.0);
    if (!spike.report || spike.tick != 5 || spike.percentage_change != 150.0) {
        std::fprintf(stderr, "tick monitor: percentage spike was not reported\n");
        return false;
    }

    config.mode = spark::TickMonitorMode::Duration;
    config.threshold = 25.0;
    config.setup_ticks = 1;
    if (!monitor.start(config) || !monitor.onTick(10.0).setup_completed || monitor.onTick(25.0).report ||
        !monitor.onTick(25.01).report) {
        std::fprintf(stderr, "tick monitor: duration threshold failed\n");
        return false;
    }
    monitor.stop();
    if (monitor.running() || monitor.onTick(100.0).report) {
        std::fprintf(stderr, "tick monitor: stop did not reset running state\n");
        return false;
    }

    config.threshold = 0.0;
    if (monitor.start(config)) {
        std::fprintf(stderr, "tick monitor: invalid configuration was accepted\n");
        return false;
    }
    return true;
}

bool verifyThreadDiscovery()
{
    const std::uint64_t current = spark::currentNativeThreadId();
    std::vector<spark::ThreadInfo> threads = spark::enumerateProcessThreads();
    if (current == 0 || threads.empty()) {
        std::fprintf(stderr, "thread discovery: current process threads were not enumerated\n");
        return false;
    }

    bool found_current = false;
    std::uint64_t previous = 0;
    for (const spark::ThreadInfo &thread : threads) {
        if (thread.id == 0 || thread.name.empty() || (previous != 0 && thread.id <= previous)) {
            std::fprintf(stderr, "thread discovery: invalid or unordered thread entry\n");
            return false;
        }
        found_current = found_current || thread.id == current;
        previous = thread.id;
    }
    if (!found_current) {
        std::fprintf(stderr, "thread discovery: current thread is missing\n");
        return false;
    }
    return true;
}

bool verifyMultiThreadSerialization()
{
    spark::ModuleTable modules;
    spark::ModuleId module = modules.intern("selftest-module");
    spark::FrameKey first{module, 0x10, 0x10};
    spark::FrameKey second{module, 0x20, 0x20};

    spark::CallTree first_tree;
    first_tree.log({first}, 0);
    spark::CallTree second_tree;
    second_tree.log({second}, 1);

    spark::ProfileMetadata metadata;
    metadata.interval = 1000;
    metadata.regex_threads = true;
    metadata.thread_patterns = {"worker-.*"};
    std::unordered_map<spark::FrameKey, spark::ResolvedFrame, spark::FrameKeyHash> resolved;
    resolved[first] = {"selftest", "firstFrame"};
    resolved[second] = {"selftest", "secondFrame"};
    std::vector<spark::ThreadTreeView> threads{{"worker-one", &first_tree}, {"worker-two", &second_tree}};
    std::string profile = spark::buildSamplerData(metadata, threads, resolved);
    if (profile.find("worker-one") == std::string::npos || profile.find("worker-two") == std::string::npos ||
        profile.find("worker-.*") == std::string::npos || profile.find("firstFrame") == std::string::npos ||
        profile.find("secondFrame") == std::string::npos || spark::collectFrameKeys(threads).size() != 2) {
        std::fprintf(stderr, "multi-thread serialization: thread trees were not preserved\n");
        return false;
    }
    return true;
}

bool verifyStatisticsSerialization()
{
    spark::ModuleTable modules;
    const spark::ModuleId module = modules.intern("statistics-module");
    const spark::FrameKey frame{module, 0x10, 0x10};
    spark::CallTree tree;
    tree.log({frame}, 0);

    spark::ProfileMetadata metadata;
    metadata.start_time_ms = 1'000;
    metadata.end_time_ms = 1'800;
    metadata.platform_stats.present = true;
    metadata.system_stats.present = true;
    metadata.system_stats.cpu_threads = 8;

    metadata.statistics.tps.last_1m = {true, 19.0, 60'000, 1140};
    metadata.statistics.tps.last_5m = {true, 18.0, 300'000, 5400};
    metadata.statistics.tps.last_15m = {true, 17.0, 900'000, 15300};
    metadata.statistics.mspt.last_1m = {true, 10.0, 1.0, 9.0, 20.0, 30.0, 60'000, 1140};
    metadata.statistics.mspt.last_5m = {true, 11.0, 2.0, 10.0, 22.0, 35.0, 300'000, 5400};
    metadata.statistics.cpu.process_last_1m = {true, 0.25, 60'000, 60};
    metadata.statistics.cpu.process_last_15m = {true, 0.20, 900'000, 900};
    metadata.statistics.cpu.system_last_1m = {true, 0.50, 60'000, 60};
    metadata.statistics.cpu.system_last_15m = {true, 0.40, 900'000, 900};

    spark::WindowStats window;
    window.ticks_present = true;
    window.ticks = 17;
    window.cpu_process_present = true;
    window.cpu_process = 0.25;
    window.cpu_system_present = true;
    window.cpu_system = 0.50;
    window.tps_present = true;
    window.tps = 17.0;
    window.mspt_present = true;
    window.mspt_median = 9.0;
    window.mspt_max = 30.0;
    window.players_present = true;
    window.players = 4;
    window.start_time_ms = 1'000;
    window.end_time_ms = 1'800;
    window.duration_ms = 800;
    metadata.window_stats[0] = window;

    std::unordered_map<spark::FrameKey, spark::ResolvedFrame, spark::FrameKeyHash> resolved;
    resolved[frame] = {"statistics", "sample"};
    const std::string profile = spark::buildSamplerData(metadata, tree, resolved);

    if (!protoRealEquals(profile, {1, 8, 4, 1}, 19.0) || !protoRealEquals(profile, {1, 8, 4, 2}, 18.0) ||
        !protoRealEquals(profile, {1, 8, 4, 3}, 17.0) || !protoRealEquals(profile, {1, 8, 5, 1, 1}, 10.0) ||
        !protoRealEquals(profile, {1, 8, 5, 1, 3}, 1.0) || !protoRealEquals(profile, {1, 8, 5, 1, 4}, 9.0) ||
        !protoRealEquals(profile, {1, 8, 5, 1, 5}, 20.0) || !protoRealEquals(profile, {1, 9, 1, 2, 1}, 0.25) ||
        !protoRealEquals(profile, {1, 9, 1, 2, 2}, 0.20)) {
        std::fprintf(stderr, "statistics serialization: rolling metadata did not "
                             "round-trip through the current protocol\n");
        return false;
    }

    ProtoField statistics;
    ProtoField omitted;
    if (!protoVarintEquals(profile, {7, 1}, 0) || !protoVarintEquals(profile, {7, 2, 1}, 17) ||
        !protoRealEquals(profile, {7, 2, 4}, 17.0) || !protoRealEquals(profile, {7, 2, 5}, 9.0) ||
        !protoRealEquals(profile, {7, 2, 6}, 30.0) || !protoVarintEquals(profile, {7, 2, 7}, 4) ||
        !protoVarintEquals(profile, {7, 2, 11}, 1'000) || !protoVarintEquals(profile, {7, 2, 12}, 1'800) ||
        !protoVarintEquals(profile, {7, 2, 13}, 800) || !findProtoPath(profile, {7, 2}, statistics) ||
        findProtoField(statistics.bytes, 8, omitted) || findProtoField(statistics.bytes, 10, omitted)) {
        std::fprintf(stderr, "statistics serialization: per-second fields or omitted "
                             "gauges were incorrect\n");
        return false;
    }

    ProtoField platform;
    ProtoField system;
    if (!findProtoPath(profile, {1, 8}, platform) || findProtoField(platform.bytes, 1, omitted) ||
        !findProtoPath(profile, {1, 9}, system) || findProtoField(system.bytes, 2, omitted) ||
        findProtoField(system.bytes, 4, omitted) || findProtoField(system.bytes, 5, omitted)) {
        std::fprintf(stderr, "statistics serialization: unavailable resource fields "
                             "were serialized as real observations\n");
        return false;
    }
    return true;
}

bool verifyLiveProfilerWindowStatistics(std::uint64_t worker_tid)
{
    spark::StatisticsService statistics;
    spark::TrustedViewersState trusted_viewers(std::filesystem::temp_directory_path() / "spark-selftest-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata_provider;
    TestNotifier notifier;
    spark::ProfilerService service(statistics, {}, std::filesystem::temp_directory_path(), {}, {}, {}, false, 10,
                                   "by-pool", "default", trusted_viewers, dispatcher, metadata_provider, notifier);

    spark::ProfilerOptions options;
    options.interval_ms = 1;
    options.ignore_sleeping = false;
    std::string error;
    if (!spark::ProfilerServiceTestAccess::start(service, options, worker_tid, error)) {
        std::fprintf(stderr, "live profiler windows: profiler start failed: %s\n", error.c_str());
        return false;
    }

    const std::int64_t profile_start = spark::ProfilerServiceTestAccess::startTimeMs(service);
    spark::CpuSnapshot cpu;
    cpu.valid = true;
    cpu.process_ticks_per_second = 100.0;
    cpu.cpu_threads = 2;
    statistics.startAt(0, profile_start, cpu);
    statistics.recordTickAt(5.0, 100);
    statistics.recordTickAt(7.0, 600);
    cpu.process_ticks += 20;
    cpu.system_busy += 20;
    cpu.system_total += 100;
    cpu.wall_ms = 1'000;
    statistics.recordCpuSnapshot(cpu);
    statistics.recordTickAt(6.0, 1'100);
    statistics.recordTickAt(8.0, 1'600);
    cpu.process_ticks += 40;
    cpu.system_busy += 40;
    cpu.system_total += 100;
    cpu.wall_ms = 2'000;
    statistics.recordCpuSnapshot(cpu);

    std::string live_data;
    for (int update = 0; update < 3; ++update) {
        live_data = spark::ProfilerServiceTestAccess::buildLiveSamplerData(service, profile_start + 2'000 + update);
        if (live_data.empty()) {
            std::fprintf(stderr, "live profiler windows: repeated live export failed\n");
            spark::ProfilerServiceTestAccess::cancel(service);
            return false;
        }
    }
    const std::uint64_t samples_after_exports = spark::ProfilerServiceTestAccess::sampleCount(service);
    if (!waitForCondition(
            [&service, samples_after_exports]() {
                return spark::ProfilerServiceTestAccess::sampleCount(service) > samples_after_exports;
            },
            std::chrono::seconds(2))) {
        std::fprintf(stderr, "live profiler windows: sampler did not resume after repeated exports\n");
        spark::ProfilerServiceTestAccess::cancel(service);
        return false;
    }
    spark::ProfilerServiceTestAccess::cancel(service);

    ProtoField time_windows;
    if (!findProtoField(live_data, 6, time_windows) || time_windows.wire_type != 2) {
        std::fprintf(stderr, "live profiler windows: time_windows was absent\n");
        return false;
    }
    std::size_t windows_offset = 0;
    std::vector<std::uint64_t> windows;
    std::uint64_t window = 0;
    while (windows_offset < time_windows.bytes.size() && readProtoVarint(time_windows.bytes, windows_offset, window)) {
        windows.push_back(window);
    }

    std::vector<std::uint64_t> statistic_windows;
    bool has_graph_fields = false;
    for (std::size_t occurrence = 0;; ++occurrence) {
        ProtoField entry;
        if (!findProtoField(live_data, 7, entry, occurrence)) {
            break;
        }
        ProtoField key;
        ProtoField value;
        if (entry.wire_type != 2 || !findProtoField(entry.bytes, 1, key) || !findProtoField(entry.bytes, 2, value) ||
            value.wire_type != 2) {
            std::fprintf(stderr, "live profiler windows: malformed time_window_statistics entry\n");
            return false;
        }
        statistic_windows.push_back(key.varint);
        ProtoField tps;
        ProtoField mspt;
        ProtoField cpu_process;
        ProtoField cpu_system;
        has_graph_fields = has_graph_fields ||
                           (findProtoField(value.bytes, 4, tps) && findProtoField(value.bytes, 5, mspt) &&
                            findProtoField(value.bytes, 2, cpu_process) && findProtoField(value.bytes, 3, cpu_system));
    }

    if (windows.size() < 2 || statistic_windows != windows || !has_graph_fields) {
        std::fprintf(stderr,
                     "live profiler windows: expected matching drawable windows "
                     "(windows=%zu statistics=%zu graph-fields=%d)\n",
                     windows.size(), statistic_windows.size(), has_graph_fields);
        return false;
    }
    return true;
}

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
    service.cmdOpen(sender);
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
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_one();
    service.shutdown();
    if (viewer_metadata_off_thread) {
        std::fprintf(stderr, "async viewer: platform metadata was captured off the owner thread\n");
        return false;
    }

    spark::HealthCommand health(statistics, metadata_provider, {}, {}, dispatcher, notifier);
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
    health.cmdHealth(sender, spark::Arguments({"health", "--upload"}));
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
    service.cmdStart(sender, spark::Arguments({"start", "--interval", "invalid"}));
    if (sender.errors.empty() || !service.running() || !service.isBackgroundRunning()) {
        std::fprintf(stderr, "background validation: invalid foreground request stopped background profiling\n");
        return false;
    }

    service.cmdStart(sender, spark::Arguments({"start", "--interval", "1"}));
    if (!service.running() || service.isBackgroundRunning()) {
        std::fprintf(stderr, "background validation: valid foreground request did not replace background profiling\n");
        return false;
    }
    service.cmdCancel(sender);
    service.onTick(50.0);
    if (service.running()) {
        std::fprintf(stderr, "background validation: cancelled foreground profile restarted background profiling\n");
        return false;
    }

    service.startBackgroundProfiler();
    service.cmdStart(sender, spark::Arguments({"start", "--interval", "1", "--save-to-file"}));
    service.cmdStop(sender, spark::Arguments({"stop"}));
    if (!waitForCondition([&service]() { return !service.exporting(); }, std::chrono::seconds(10)) ||
        !service.running() || !service.isBackgroundRunning()) {
        std::fprintf(stderr, "background validation: explicit stop did not restore background profiling\n");
        return false;
    }
    service.cmdCancel(sender);

    service.startBackgroundProfiler();
    service.cmdStart(sender, spark::Arguments({"start", "--interval", "1", "--timeout", "11", "--save-to-file"}));
    spark::ProfilerServiceTestAccess::expire(service);
    service.onTick(50.0);
    if (!waitForCondition([&service]() { return !service.exporting(); }, std::chrono::seconds(10))) {
        std::fprintf(stderr, "background validation: timed profile did not finish exporting\n");
        return false;
    }
    service.onTick(50.0);
    if (service.running()) {
        std::fprintf(stderr, "background validation: timed foreground profile restarted background profiling\n");
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

bool verifySystemResourceStats()
{
    const spark::ProcessStats process = spark::gatherProcessStats();
    if (!process.rss_present || process.rss_bytes <= 0 || !process.virtual_present ||
        process.virtual_bytes < process.rss_bytes || !process.threads_present || process.threads < 1) {
        std::fprintf(stderr,
                     "system resources: process RSS/virtual memory/thread query failed "
                     "(rss=%lld virtual=%lld threads=%d)\n",
                     static_cast<long long>(process.rss_bytes), static_cast<long long>(process.virtual_bytes),
                     process.threads);
        return false;
    }

    const spark::SystemStats system = spark::gatherSystemStats(".");
    if (!system.present || !system.cpu_present || system.cpu_threads < 1 || !system.memory_present ||
        system.mem_total <= 0 || system.mem_used < 0 || system.mem_used > system.mem_total || !system.swap_present ||
        system.swap_total < 0 || system.swap_used < 0 || system.swap_used > system.swap_total || !system.disk_present ||
        system.disk_total <= 0 || system.disk_used < 0 || system.disk_used > system.disk_total || !system.os_present ||
        system.os_name.empty() || system.os_arch.empty()) {
        std::fprintf(stderr, "system resources: host availability/value validation "
                             "failed\n");
        return false;
    }
    return true;
}

bool verifyAllThreadSampling()
{
    using namespace std::chrono_literals;

    std::atomic<bool> keep_workers_running{true};
    std::atomic<std::uint64_t> first_progress{0};
    std::atomic<std::uint64_t> second_progress{0};
    auto busy_worker = [&](std::atomic<std::uint64_t> &progress) {
        while (keep_workers_running.load(std::memory_order_relaxed)) {
            progress.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread first_worker(busy_worker, std::ref(first_progress));
    std::thread second_worker(busy_worker, std::ref(second_progress));
    while (first_progress.load(std::memory_order_relaxed) == 0 ||
           second_progress.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }
    auto stop_workers = [&] {
        keep_workers_running.store(false, std::memory_order_relaxed);
        first_worker.join();
        second_worker.join();
    };

    spark::SamplerConfig config;
    config.interval_us = 2000;
    config.ignore_sleeping = false;
    config.all_threads = true;

    spark::Sampler sampler;
    if (!sampler.start(config)) {
        std::fprintf(stderr, "all-thread sampling: sampler start failed\n");
        stop_workers();
        return false;
    }
    // Hosted Windows runners can spend most of a short observation interval
    // inside one expensive StackWalk64 attempt.  Poll until at least two
    // thread trees are captured, with a generous deadline for slow hosts.
    waitForCondition([&] { return sampler.threadTrees().size() >= 2 && sampler.sampleCount() > 0; }, 10s);
    sampler.stop();
    stop_workers();

    if (sampler.threadTrees().size() < 2 || sampler.sampleCount() == 0) {
        std::fprintf(stderr, "all-thread sampling: fewer than two process threads were captured\n");
        return false;
    }
    if (sampler.sampleCount() > 750) {
        std::fprintf(stderr, "all-thread sampling: stack-walk interval budget was exceeded\n");
        return false;
    }
    std::uint64_t thread_weight_sum = 0;
    for (const auto &[id, thread] : sampler.threadTrees()) {
        const std::uint64_t weight_us = thread.tree.sampleCount();
        if (id == 0 || thread.thread_name.empty() || thread.tree.empty()) {
            std::fprintf(stderr, "all-thread sampling: invalid per-thread call tree\n");
            return false;
        }
        thread_weight_sum += weight_us;
    }
    // A bounded round-robin sweep can capture a thread only once on a slow host.
    // Preserve the invariant that every accepted weight reaches both tree views
    // without requiring a minimum number of scheduling turns within 200ms.
    if (sampler.tree().sampleCount() != thread_weight_sum) {
        std::fprintf(stderr, "all-thread sampling: combined tree lost elapsed-time weight\n");
        return false;
    }
    return true;
}

bool verifyStatisticsService()
{
    auto close = [](double actual, double expected) {
        return std::abs(actual - expected) < 0.000001;
    };
    auto initialCpu = [] {
        spark::CpuSnapshot snapshot;
        snapshot.valid = true;
        snapshot.process_ticks_per_second = 100.0;
        snapshot.cpu_threads = 2;
        snapshot.wall_ms = 0;
        return snapshot;
    };

    auto tps_service = std::make_unique<spark::StatisticsService>();
    tps_service->startAt(0, 1'000'000, initialCpu());
    for (int second = 0; second < 900; ++second) {
        const int rate = second < 600 ? 20 : second < 840 ? 18 : second < 890 ? 15 : second < 895 ? 10 : 5;
        for (int tick = 1; tick <= rate; ++tick) {
            const std::int64_t timestamp =
                static_cast<std::int64_t>(second) * 1000 + static_cast<std::int64_t>(tick) * 1000 / rate;
            tps_service->recordTickAt(2.0, timestamp);
        }
    }
    const spark::StatisticsSnapshot tps = tps_service->snapshotAt(900'000);
    if (!close(tps.tps.last_5s.value, 5.0) || !close(tps.tps.last_10s.value, 7.5) ||
        !close(tps.tps.last_1m.value, 13.75) || !close(tps.tps.last_5m.value, 17.15) ||
        !close(tps.tps.last_15m.value, 19.05) || tps.tps.last_5s.samples != 25 ||
        tps.history_span_ms != spark::StatisticsService::kMaximumHistoryMs) {
        std::fprintf(stderr, "statistics service: TPS windows were not independently "
                             "time-weighted\n");
        return false;
    }

    auto mspt_service = std::make_unique<spark::StatisticsService>();
    mspt_service->startAt(0, 2'000'000, initialCpu());
    for (int tick = 1; tick <= 1000; ++tick) {
        mspt_service->recordTickAt(static_cast<double>((tick - 1) % 100 + 1), static_cast<std::int64_t>(tick) * 10);
    }
    const spark::StatisticsSnapshot mspt = mspt_service->snapshotAt(10'000);
    const spark::DistributionValues &distribution = mspt.mspt.last_10s;
    if (!distribution.present || distribution.samples != 1000 || !close(distribution.mean, 50.5) ||
        !close(distribution.min, 1.0) || !close(distribution.median, 50.5) || !close(distribution.percentile95, 95.0) ||
        !close(distribution.max, 100.0) || mspt.mspt.last_1m.span_ms != 10'000) {
        std::fprintf(stderr, "statistics service: MSPT distribution or partial-window "
                             "span was incorrect\n");
        return false;
    }

    auto cpu_service = std::make_unique<spark::StatisticsService>();
    spark::CpuSnapshot cpu = initialCpu();
    cpu_service->startAt(0, 3'000'000, cpu);
    for (int second = 1; second <= 900; ++second) {
        const unsigned long long process_delta = second <= 840 ? 20 : (second <= 890 ? 40 : 80);
        const unsigned long long busy_delta = second <= 840 ? 20 : (second <= 890 ? 40 : 80);
        cpu.process_ticks += process_delta;
        cpu.system_busy += busy_delta;
        cpu.system_total += 100;
        cpu.wall_ms = static_cast<std::int64_t>(second) * 1000;
        cpu_service->recordCpuSnapshot(cpu);
    }
    const spark::StatisticsSnapshot cpu_stats = cpu_service->snapshotAt(900'000);
    if (!close(cpu_stats.cpu.process_last_10s.value, 0.4) || !close(cpu_stats.cpu.process_last_1m.value, 14.0 / 60.0) ||
        !close(cpu_stats.cpu.process_last_15m.value, 98.0 / 900.0) ||
        !close(cpu_stats.cpu.system_last_10s.value, 0.8) || !close(cpu_stats.cpu.system_last_1m.value, 28.0 / 60.0) ||
        !close(cpu_stats.cpu.system_last_15m.value, 196.0 / 900.0)) {
        std::fprintf(stderr, "statistics service: CPU windows were not independently "
                             "time-weighted\n");
        return false;
    }

    auto window_service = std::make_unique<spark::StatisticsService>();
    spark::CpuSnapshot window_cpu = initialCpu();
    window_service->startAt(0, 4'000'000, window_cpu);
    window_service->recordPlayerCountAt(2, 0);
    window_service->recordTickAt(1.0, 100);
    window_service->recordTickAt(9.0, 600);
    window_cpu.process_ticks += 20;
    window_cpu.system_busy += 20;
    window_cpu.system_total += 100;
    window_cpu.wall_ms = 1'000;
    window_service->recordCpuSnapshot(window_cpu);
    window_service->recordPlayerCountAt(3, 1'000);
    window_service->recordTickAt(2.0, 1'100);
    window_service->recordTickAt(8.0, 1'600);
    window_cpu.process_ticks += 40;
    window_cpu.system_busy += 40;
    window_cpu.system_total += 100;
    window_cpu.wall_ms = 2'000;
    window_service->recordCpuSnapshot(window_cpu);
    const auto windows = window_service->profileWindows(4'000'000, 4'002'000);
    auto first_window = windows.find(0);
    auto second_window = windows.find(1);
    if (windows.size() != 2 || first_window == windows.end() || second_window == windows.end() ||
        first_window->second.ticks != 2 || !close(first_window->second.tps, 2.0) ||
        !close(first_window->second.mspt_median, 5.0) || !close(first_window->second.mspt_max, 9.0) ||
        !close(first_window->second.cpu_process, 0.1) || !close(first_window->second.cpu_system, 0.2) ||
        first_window->second.players != 3 || first_window->second.start_time_ms != 4'000'000 ||
        first_window->second.end_time_ms != 4'001'000 || first_window->second.duration_ms != 1'000 ||
        second_window->second.players != 3 || second_window->second.entities_present ||
        second_window->second.chunks_present) {
        std::fprintf(stderr,
                     "statistics service: per-second profile windows were "
                     "incorrect (count=%zu first ticks=%d tps=%.3f "
                     "median=%.3f max=%.3f process=%.3f system=%.3f "
                     "players=%d start=%lld end=%lld duration=%d; "
                     "second players=%d)\n",
                     windows.size(), first_window == windows.end() ? -1 : first_window->second.ticks,
                     first_window == windows.end() ? -1.0 : first_window->second.tps,
                     first_window == windows.end() ? -1.0 : first_window->second.mspt_median,
                     first_window == windows.end() ? -1.0 : first_window->second.mspt_max,
                     first_window == windows.end() ? -1.0 : first_window->second.cpu_process,
                     first_window == windows.end() ? -1.0 : first_window->second.cpu_system,
                     first_window == windows.end() ? -1 : first_window->second.players,
                     static_cast<long long>(first_window == windows.end() ? -1 : first_window->second.start_time_ms),
                     static_cast<long long>(first_window == windows.end() ? -1 : first_window->second.end_time_ms),
                     first_window == windows.end() ? -1 : first_window->second.duration_ms,
                     second_window == windows.end() ? -1 : second_window->second.players);
        return false;
    }
    return true;
}

bool verifyWorldGaugeStatistics()
{
    spark::CpuSnapshot cpu;
    cpu.valid = true;
    cpu.process_ticks_per_second = 100.0;
    cpu.cpu_threads = 2;
    cpu.wall_ms = 0;

    auto svc = std::make_unique<spark::StatisticsService>();
    svc->startAt(0, 5'000'000, cpu);
    svc->recordPlayerCountAt(1, 0);
    svc->recordWorldGaugesAt(10, 20, 0);
    svc->recordTickAt(5.0, 100);
    svc->recordTickAt(5.0, 600);
    cpu.process_ticks += 20;
    cpu.system_busy += 20;
    cpu.system_total += 100;
    cpu.wall_ms = 1'000;
    svc->recordCpuSnapshot(cpu);
    svc->recordPlayerCountAt(2, 1'000);
    svc->recordWorldGaugesAt(15, 25, 1'000);
    svc->recordTickAt(5.0, 1'100);
    svc->recordTickAt(5.0, 1'600);
    cpu.process_ticks += 40;
    cpu.system_busy += 40;
    cpu.system_total += 100;
    cpu.wall_ms = 2'000;
    svc->recordCpuSnapshot(cpu);
    svc->recordPlayerCountAt(3, 2'000);
    svc->recordWorldGaugesAt(20, 30, 2'000);

    const auto windows = svc->profileWindows(5'000'000, 5'002'000);
    auto first = windows.find(0);
    auto second = windows.find(1);
    // The gauge loop picks the last sample within each window's end time,
    // matching the existing players behavior.
    if (windows.size() != 2 || first == windows.end() || second == windows.end() || !first->second.entities_present ||
        first->second.entities != 15 || !first->second.chunks_present || first->second.chunks != 25 ||
        !second->second.entities_present || second->second.entities != 20 || !second->second.chunks_present ||
        second->second.chunks != 30) {
        std::fprintf(stderr,
                     "world gauge statistics: entities/chunks not correct "
                     "(first ents=%d chunks=%d; second ents=%d chunks=%d)\n",
                     first == windows.end() ? -1 : first->second.entities,
                     first == windows.end() ? -1 : first->second.chunks,
                     second == windows.end() ? -1 : second->second.entities,
                     second == windows.end() ? -1 : second->second.chunks);
        return false;
    }
    return true;
}

bool verifyWorldGaugeAbsentWhenNotRecorded()
{
    spark::CpuSnapshot cpu;
    cpu.valid = true;
    cpu.process_ticks_per_second = 100.0;
    cpu.cpu_threads = 2;
    cpu.wall_ms = 0;

    auto svc = std::make_unique<spark::StatisticsService>();
    svc->startAt(0, 6'000'000, cpu);
    svc->recordPlayerCountAt(1, 0);
    svc->recordTickAt(5.0, 100);
    svc->recordTickAt(5.0, 600);
    cpu.process_ticks += 20;
    cpu.system_busy += 20;
    cpu.system_total += 100;
    cpu.wall_ms = 1'000;
    svc->recordCpuSnapshot(cpu);

    const auto windows = svc->profileWindows(6'000'000, 6'001'000);
    auto first = windows.find(0);
    if (windows.size() != 1 || first == windows.end() || first->second.entities_present ||
        first->second.chunks_present) {
        std::fprintf(stderr,
                     "world gauge absent: entities_present=%d chunks_present=%d "
                     "(expected both false)\n",
                     first == windows.end() ? -1 : first->second.entities_present,
                     first == windows.end() ? -1 : first->second.chunks_present);
        return false;
    }
    return true;
}

std::string escapeRegex(const std::string &text)
{
    std::string escaped;
    for (char ch : text) {
        if (std::string_view(R"(\.^$|()[]{}*+?)").find(ch) != std::string_view::npos) {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

bool verifySelectedThreadSampling(std::uint64_t worker_tid)
{
    using namespace std::chrono_literals;

    const std::vector<spark::ThreadInfo> discovered = spark::enumerateProcessThreads();
    auto worker = std::find_if(discovered.begin(), discovered.end(),
                               [worker_tid](const spark::ThreadInfo &thread) { return thread.id == worker_tid; });
    if (worker == discovered.end()) {
        std::fprintf(stderr, "selected-thread sampling: worker thread was not discovered\n");
        return false;
    }

    spark::Sampler sampler;
    spark::SamplerConfig invalid;
    invalid.regex_threads = true;
    invalid.thread_patterns = {"["};
    if (sampler.start(invalid) || sampler.lastError().find("invalid thread name regex") == std::string::npos) {
        std::fprintf(stderr, "selected-thread sampling: invalid regex did not fail cleanly\n");
        sampler.stop();
        return false;
    }

    spark::SamplerConfig exact;
    exact.interval_us = 2000;
    exact.ignore_sleeping = false;
    exact.thread_patterns = {worker->name};
    std::transform(exact.thread_patterns.front().begin(), exact.thread_patterns.front().end(),
                   exact.thread_patterns.front().begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (!sampler.start(exact)) {
        std::fprintf(stderr, "selected-thread sampling: exact-name start failed: %s\n", sampler.lastError().c_str());
        return false;
    }
    // The sampler thread needs time to start, enumerate process threads, and
    // complete at least one stack-walk capture.  Poll for a sample instead of
    // relying on a fixed sleep that may expire before the first capture.
    waitForCondition([&] { return sampler.sampleCount() > 0; }, 2s);
    sampler.stop();
    if (sampler.threadTrees().empty()) {
        std::fprintf(stderr, "selected-thread sampling: exact-name selector captured no threads\n");
        return false;
    }
    for (const auto &[id, thread] : sampler.threadTrees()) {
        if (thread.thread_name.rfind(worker->name + " (#", 0) != 0) {
            std::fprintf(stderr, "selected-thread sampling: exact-name selector captured an unexpected thread\n");
            return false;
        }
    }

    spark::SamplerConfig regex = exact;
    regex.regex_threads = true;
    regex.thread_patterns = {escapeRegex(worker->name)};
    if (!sampler.start(regex)) {
        std::fprintf(stderr, "selected-thread sampling: regex start failed: %s\n", sampler.lastError().c_str());
        return false;
    }
    waitForCondition([&] { return sampler.sampleCount() > 0; }, 2s);
    sampler.stop();
    if (sampler.threadTrees().empty()) {
        std::fprintf(stderr, "selected-thread sampling: regex selector captured no threads\n");
        return false;
    }
    return true;
}

bool verifyExecutableHash()
{
    if (spark::sha256Hex("") != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ||
        spark::sha256Hex("abc") != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        std::fprintf(stderr, "executable hash: SHA-256 vector mismatch\n");
        return false;
    }

    std::string error;
    const std::string first = spark::currentExecutableSha256(error);
    const std::string second = spark::currentExecutableSha256(error);
    if (first.size() != 64 || first != second || !std::all_of(first.begin(), first.end(), [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        })) {
        std::fprintf(stderr, "executable hash: current executable hashing failed: %s\n", error.c_str());
        return false;
    }
    return true;
}

void allocationBurst(int count = 96)
{
    for (int i = 0; i < count; ++i) {
        void *pointer = std::malloc(static_cast<std::size_t>(512 + i));
        if (pointer != nullptr) {
            static_cast<volatile unsigned char *>(pointer)[0] = static_cast<unsigned char>(i);
            std::free(pointer);
        }
    }
}

bool allocationTreesHaveOnly(const spark::AllocationSampler &sampler,
                             const std::vector<std::string_view> &expected_names)
{
    std::vector<bool> found(expected_names.size(), false);
    for (const auto &[id, thread] : sampler.threadTrees()) {
        bool allowed = false;
        for (std::size_t i = 0; i < expected_names.size(); ++i) {
            if (thread.thread_name.rfind(std::string(expected_names[i]) + " (#", 0) == 0) {
                found[i] = true;
                allowed = true;
                break;
            }
        }
        if (!allowed || id == 0 || thread.tree.empty()) {
            return false;
        }
    }
    return std::all_of(found.begin(), found.end(), [](bool value) { return value; });
}

bool runNamedAllocationWorkers(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
                               const std::vector<const char *> &names, std::string &error)
{
    using namespace std::chrono_literals;
    if (!sampler.start(config, error)) {
        return false;
    }
    std::atomic<int> ready{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> workers;
    workers.reserve(names.size());
    for (const char *name : names) {
        workers.emplace_back([&, name] {
            if (!setCurrentThreadName(name)) {
                ready.fetch_add(1000, std::memory_order_release);
                return;
            }
            allocationBurst();
            ready.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < static_cast<int>(names.size())) {
        std::this_thread::yield();
    }
    const bool named = ready.load(std::memory_order_acquire) < 1000;
    std::this_thread::sleep_for(30ms);
    release.store(true, std::memory_order_release);
    for (std::thread &worker : workers) {
        worker.join();
    }
    sampler.onTick(50.0);
    const bool stopped = sampler.stop(error);
    return named && stopped;
}

bool verifyAllocationThreadSelection()
{
    std::string error;
    spark::AllocationSampler sampler;

    spark::AllocationSamplerConfig exact;
    exact.interval_bytes = 1;
    exact.session_seed = spark::currentNativeThreadId();
    exact.all_threads = false;
    exact.thread_patterns = {"SPARK-ALLOC-A", "spark-alloc-b"};
    if (!runNamedAllocationWorkers(sampler, exact, {"spark-alloc-a", "spark-alloc-b", "spark-alloc-x"}, error) ||
        sampler.sampleCount() == 0 || !allocationTreesHaveOnly(sampler, {"spark-alloc-a", "spark-alloc-b"}) ||
        sampler.filteredSamples() == 0) {
        std::fprintf(stderr,
                     "allocation thread selection: exact/multiple selection failed "
                     "(samples=%llu filtered=%llu error=%s)\n",
                     static_cast<unsigned long long>(sampler.sampleCount()),
                     static_cast<unsigned long long>(sampler.filteredSamples()), error.c_str());
        return false;
    }

    spark::AllocationSamplerConfig regex = exact;
    regex.regex_threads = true;
    regex.thread_patterns = {R"(spark-dyn-\d+)"};
    if (!runNamedAllocationWorkers(sampler, regex, {"spark-dyn-42", "spark-other"}, error) ||
        !allocationTreesHaveOnly(sampler, {"spark-dyn-42"})) {
        std::fprintf(stderr, "allocation thread selection: regex/dynamic selection failed: %s\n", error.c_str());
        return false;
    }

    spark::AllocationSamplerConfig no_match = exact;
    no_match.thread_patterns = {"spark-never"};
    if (!runNamedAllocationWorkers(sampler, no_match, {"spark-no-match"}, error) || sampler.sampleCount() != 0 ||
        !sampler.threadTrees().empty() || sampler.filteredSamples() == 0) {
        std::fprintf(stderr,
                     "allocation thread selection: no-match profile was not empty "
                     "(samples=%llu roots=%zu filtered=%llu error=%s)\n",
                     static_cast<unsigned long long>(sampler.sampleCount()), sampler.threadTrees().size(),
                     static_cast<unsigned long long>(sampler.filteredSamples()), error.c_str());
        return false;
    }

    spark::AllocationSamplerConfig invalid = exact;
    invalid.regex_threads = true;
    invalid.thread_patterns = {"["};
    if (sampler.start(invalid, error) || error.find("invalid thread name regex") == std::string::npos) {
        std::fprintf(stderr, "allocation thread selection: invalid regex was accepted\n");
        sampler.stop(error);
        return false;
    }

    spark::AllocationSamplerConfig retained = exact;
    retained.live_only = true;
    retained.thread_patterns = {"spark-live-src"};
    if (!sampler.start(retained, error)) {
        std::fprintf(stderr, "allocation thread selection: live-only start failed: %s\n", error.c_str());
        return false;
    }
    std::atomic<void *> retained_pointer{nullptr};
    std::atomic<void *> released_pointer{nullptr};
    std::thread allocator([&] {
        setCurrentThreadName("spark-live-src");
        retained_pointer.store(std::malloc(8192), std::memory_order_release);
        released_pointer.store(std::malloc(4096), std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    });
    allocator.join();
    std::thread releaser([&] {
        setCurrentThreadName("spark-live-free");
        void *pointer = released_pointer.load(std::memory_order_acquire);
        void *replacement = std::realloc(pointer, 16384);
        std::free(replacement != nullptr ? replacement : pointer);
    });
    releaser.join();
    std::atomic<void *> unselected_retained{nullptr};
    std::thread unselected([&] {
        setCurrentThreadName("spark-live-no");
        unselected_retained.store(std::malloc(2048), std::memory_order_release);
    });
    unselected.join();
    sampler.onTick(50.0);
    const bool live_stopped = sampler.stop(error);
    const bool live_valid = live_stopped && sampler.sampleCount() != 0 && sampler.freedSamples() != 0 &&
                            allocationTreesHaveOnly(sampler, {"spark-live-src"});
    std::free(retained_pointer.load(std::memory_order_acquire));
    std::free(unselected_retained.load(std::memory_order_acquire));
    if (!live_valid) {
        std::fprintf(stderr,
                     "allocation thread selection: live-only/cross-thread lifecycle failed "
                     "(samples=%llu freed=%llu roots=%zu error=%s)\n",
                     static_cast<unsigned long long>(sampler.sampleCount()),
                     static_cast<unsigned long long>(sampler.freedSamples()), sampler.threadTrees().size(),
                     error.c_str());
        return false;
    }
    if (!sampler.shutdown(error)) {
        return false;
    }

    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.alloc = true;
    options.allocation_interval_bytes = 1;
    options.threads = {"spark-prof-a", "spark-prof-b"};
    if (!profiler.start(options, spark::currentNativeThreadId(), error)) {
        std::fprintf(stderr, "allocation thread selection: ProfilerOptions selector was rejected: %s\n", error.c_str());
        return false;
    }
    std::thread profiler_worker([] {
        setCurrentThreadName("spark-prof-b");
        allocationBurst();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    });
    profiler_worker.join();
    profiler.onTick(50.0);
    if (!profiler.stopSampling(error) || profiler.sampleCount() == 0) {
        std::fprintf(stderr,
                     "allocation thread selection: profiler integration failed "
                     "(samples=%llu error=%s)\n",
                     static_cast<unsigned long long>(profiler.sampleCount()), error.c_str());
        return false;
    }

    options.threads = {"*"};
    if (!profiler.start(options, spark::currentNativeThreadId(), error)) {
        std::fprintf(stderr, "allocation thread selection: * selector start failed: %s\n", error.c_str());
        return false;
    }
    allocationBurst();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    profiler.onTick(50.0);
    if (!profiler.stopSampling(error) || profiler.sampleCount() == 0) {
        std::fprintf(stderr, "allocation thread selection: * selector captured no samples\n");
        return false;
    }

    options.threads = {"*", "spark-prof-b"};
    if (profiler.start(options, spark::currentNativeThreadId(), error) ||
        error.find("--thread * cannot be combined") == std::string::npos) {
        std::fprintf(stderr, "allocation thread selection: ambiguous * selector was accepted\n");
        profiler.cancel();
        return false;
    }
    options.threads.clear();
    options.regex = true;
    if (profiler.start(options, spark::currentNativeThreadId(), error) ||
        error.find("--regex requires") == std::string::npos) {
        std::fprintf(stderr, "allocation thread selection: patternless regex was accepted\n");
        profiler.cancel();
        return false;
    }
    return profiler.shutdown(error);
}

bool runAllocationSession(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
                          std::string &error)
{
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "allocation lifecycle: start failed: %s\n", error.c_str());
        return false;
    }
    if (!exerciseNativeAllocations()) {
        std::fprintf(stderr, "allocation lifecycle: test allocation failed\n");
        return false;
    }
    sampler.onTick(50.0);
    if (!sampler.stop(error)) {
        std::fprintf(stderr, "allocation lifecycle: stop failed: %s\n", error.c_str());
        return false;
    }
    if (sampler.sampleCount() == 0 || sampler.observedBytes() == 0 || sampler.freedSamples() == 0 ||
        sampler.freedBytes() == 0 || sampler.lifecycleDropped() != 0) {
        std::fprintf(stderr,
                     "allocation lifecycle: invalid counters "
                     "(samples=%llu observed=%llu freed=%llu freed-bytes=%llu "
                     "lifecycle-dropped=%llu)\n",
                     static_cast<unsigned long long>(sampler.sampleCount()),
                     static_cast<unsigned long long>(sampler.observedBytes()),
                     static_cast<unsigned long long>(sampler.freedSamples()),
                     static_cast<unsigned long long>(sampler.freedBytes()),
                     static_cast<unsigned long long>(sampler.lifecycleDropped()));
        return false;
    }
    return true;
}

bool verifyProcessWideAllocationSampling()
{
    using namespace std::chrono_literals;

    spark::AllocationSamplerConfig config;
    config.interval_bytes = 1;
    config.session_seed = spark::currentNativeThreadId();

    spark::AllocationSampler sampler;
    std::string error;
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "process-wide allocation: start failed: %s\n", error.c_str());
        return false;
    }

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    auto allocate_on_worker = [&]() {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 64; ++i) {
            void *pointer = std::malloc(static_cast<std::size_t>(512 + i));
            if (pointer != nullptr) {
                static_cast<volatile unsigned char *>(pointer)[0] = static_cast<unsigned char>(i);
                std::free(pointer);
            }
        }
    };
    std::thread first(allocate_on_worker);
    std::thread second(allocate_on_worker);
    while (ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    first.join();
    second.join();

    std::atomic<void *> handoff{nullptr};
    std::thread allocator([&]() { handoff.store(std::malloc(4096), std::memory_order_release); });
    allocator.join();
    void *original = handoff.load(std::memory_order_acquire);
    if (original == nullptr) {
        std::fprintf(stderr, "process-wide allocation: handoff malloc failed\n");
        sampler.stop(error);
        return false;
    }
    std::thread resizer([&]() {
        void *replacement = std::realloc(original, 1024 * 1024);
        handoff.store(replacement != nullptr ? replacement : original, std::memory_order_release);
    });
    resizer.join();
    std::thread releaser([&]() { std::free(handoff.load(std::memory_order_acquire)); });
    releaser.join();

    void *failed = std::malloc(1024);
    if (failed == nullptr) {
        std::fprintf(stderr, "process-wide allocation: failure probe malloc failed\n");
        sampler.stop(error);
        return false;
    }
    const std::size_t impossible = (std::numeric_limits<std::size_t>::max)();
    volatile std::size_t impossible_runtime = impossible;
    void *failure = std::realloc(failed, impossible);
    if (failure != nullptr) {
        std::free(failure);
    }
    else {
        std::free(failed);
    }
#if defined(_WIN32)
    _invalid_parameter_handler previous_handler = _set_thread_local_invalid_parameter_handler(ignoreInvalidParameter);
#endif
    void *calloc_overflow = std::calloc(impossible_runtime, 2);
    if (calloc_overflow != nullptr) {
        std::fprintf(stderr, "process-wide allocation: calloc overflow succeeded (%p)\n", calloc_overflow);
        std::free(calloc_overflow);
#if defined(_WIN32)
        _set_thread_local_invalid_parameter_handler(previous_handler);
#endif
        sampler.stop(error);
        return false;
    }
#if defined(_WIN32)
    void *recalloc_overflow = _recalloc(nullptr, impossible_runtime, 2);
    _set_thread_local_invalid_parameter_handler(previous_handler);
#else
    void *recalloc_overflow = ::reallocarray(nullptr, impossible_runtime, 2);
#endif
    if (recalloc_overflow != nullptr) {
        std::fprintf(stderr, "process-wide allocation: recalloc overflow succeeded\n");
        std::free(recalloc_overflow);
        sampler.stop(error);
        return false;
    }
    void *from_null = std::realloc(nullptr, 2048);
    std::free(from_null);
    void *to_zero = std::malloc(2048);
    if (to_zero != nullptr) {
        void *zero_result = std::realloc(to_zero, 0);
        std::free(zero_result);
    }

    for (int i = 0; i < 300; ++i) {
        std::thread short_lived([]() {
            void *pointer = std::malloc(128);
            if (pointer != nullptr) {
                static_cast<volatile unsigned char *>(pointer)[0] = 1;
                std::free(pointer);
            }
        });
        short_lived.join();
    }

    sampler.onTick(50.0);
    if (!sampler.stop(error)) {
        std::fprintf(stderr, "process-wide allocation: stop failed: %s\n", error.c_str());
        return false;
    }

    const auto &trees = sampler.threadTrees();
    const auto overflow = trees.find(0);
    if (sampler.sampleCount() == 0 || sampler.samplingPoints() == 0 || sampler.enqueuedSamples() == 0 ||
        sampler.eventQueueHighWaterMark() == 0 || sampler.freedSamples() == 0 ||
        trees.size() != sampler.threadRootCapacity() || sampler.overflowThreadCount() < 40 ||
        sampler.overflowThreadCount() > 512 || overflow == trees.end() ||
        overflow->second.thread_name != "<other threads>" || overflow->second.tree.empty()) {
        std::fprintf(stderr,
                     "process-wide allocation: invalid coverage "
                     "(samples=%llu points=%llu enqueued=%llu high-water=%llu freed=%llu "
                     "threads=%zu overflow=%llu)\n",
                     static_cast<unsigned long long>(sampler.sampleCount()),
                     static_cast<unsigned long long>(sampler.samplingPoints()),
                     static_cast<unsigned long long>(sampler.enqueuedSamples()),
                     static_cast<unsigned long long>(sampler.eventQueueHighWaterMark()),
                     static_cast<unsigned long long>(sampler.freedSamples()), trees.size(),
                     static_cast<unsigned long long>(sampler.overflowThreadCount()));
        return false;
    }
    for (const auto &[id, thread] : trees) {
        if (thread.thread_name.empty() || thread.tree.empty()) {
            std::fprintf(stderr, "process-wide allocation: empty thread root %llu\n",
                         static_cast<unsigned long long>(id));
            return false;
        }
    }
    std::vector<spark::ThreadTreeView> views;
    for (const auto &[id, thread] : trees) {
        views.push_back({thread.thread_name, &thread.tree});
    }
    std::unordered_map<spark::FrameKey, spark::ResolvedFrame, spark::FrameKeyHash> resolved;
    for (const spark::FrameKey &frame : spark::collectFrameKeys(views)) {
        resolved.emplace(frame, spark::ResolvedFrame{"selftest", "allocation"});
    }
    spark::ProfileMetadata metadata;
    metadata.mode = spark::ProfileMode::Allocation;
    metadata.all_threads = true;
    const std::string profile = spark::buildSamplerData(metadata, views, resolved);
    if (profile.find("<other threads>") == std::string::npos || profile.find("session #") == std::string::npos) {
        std::fprintf(stderr, "process-wide allocation: thread roots were not serialized\n");
        return false;
    }
    return sampler.shutdown(error);
}

bool verifyAllocationResourcePressure()
{
    spark::AllocationSamplerConfig config;
    config.interval_bytes = 1;
    config.session_seed = spark::currentNativeThreadId();
    config.aggregator_delay_ms_for_testing = 1000;

    std::string error;
    spark::AllocationSampler queue_sampler;
    if (!queue_sampler.start(config, error)) {
        std::fprintf(stderr, "allocation pressure: queue start failed: %s\n", error.c_str());
        return false;
    }
    std::vector<std::thread> workers;
    workers.reserve(8);
    for (int thread = 0; thread < 8; ++thread) {
        workers.emplace_back([]() {
            for (int i = 0; i < 4096; ++i) {
                void *pointer = std::malloc(64);
                if (pointer != nullptr) {
                    static_cast<volatile unsigned char *>(pointer)[0] = static_cast<unsigned char>(i);
                    std::free(pointer);
                }
            }
        });
    }
    for (std::thread &worker_thread : workers) {
        worker_thread.join();
    }
    if (!queue_sampler.stop(error) || queue_sampler.eventQueueHighWaterMark() != queue_sampler.eventQueueCapacity() ||
        queue_sampler.droppedEvents() == 0 || !queue_sampler.dataIncomplete() || !queue_sampler.shutdown(error)) {
        std::fprintf(stderr,
                     "allocation pressure: queue did not saturate safely "
                     "(high-water=%llu capacity=%llu dropped=%llu error=%s)\n",
                     static_cast<unsigned long long>(queue_sampler.eventQueueHighWaterMark()),
                     static_cast<unsigned long long>(queue_sampler.eventQueueCapacity()),
                     static_cast<unsigned long long>(queue_sampler.droppedEvents()), error.c_str());
        return false;
    }

    config.only_ticks_over_ms = 1;
    spark::AllocationSampler tick_sampler;
    if (!tick_sampler.start(config, error)) {
        std::fprintf(stderr, "allocation pressure: tick queue start failed: %s\n", error.c_str());
        return false;
    }
    constexpr std::uint64_t extra_tick_events = 1024;
    for (std::uint64_t i = 0; i < tick_sampler.tickEventCapacity() + extra_tick_events; ++i) {
        tick_sampler.onTick(2.0);
    }
    if (!tick_sampler.stop(error) || tick_sampler.droppedTickEvents() != extra_tick_events ||
        !tick_sampler.dataIncomplete() || !tick_sampler.shutdown(error)) {
        std::fprintf(stderr,
                     "allocation pressure: tick queue did not saturate at its declared "
                     "capacity (capacity=%llu dropped=%llu error=%s)\n",
                     static_cast<unsigned long long>(tick_sampler.tickEventCapacity()),
                     static_cast<unsigned long long>(tick_sampler.droppedTickEvents()), error.c_str());
        return false;
    }

    config.aggregator_delay_ms_for_testing = 0;
    config.only_ticks_over_ms = 0;
    config.thread_state_limit_for_testing = 8;
    spark::AllocationSampler registry_sampler;
    if (!registry_sampler.start(config, error)) {
        std::fprintf(stderr, "allocation pressure: registry start failed: %s\n", error.c_str());
        return false;
    }
    std::atomic<int> registry_ready{0};
    std::atomic<bool> release_registry_threads{false};
    workers.clear();
    for (int thread = 0; thread < 16; ++thread) {
        workers.emplace_back([&]() {
            void *pointer = std::malloc(128);
            if (pointer != nullptr) {
                static_cast<volatile unsigned char *>(pointer)[0] = 1;
            }
            registry_ready.fetch_add(1, std::memory_order_release);
            while (!release_registry_threads.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            std::free(pointer);
        });
    }
    while (registry_ready.load(std::memory_order_acquire) != 16) {
        std::this_thread::yield();
    }
    release_registry_threads.store(true, std::memory_order_release);
    for (std::thread &worker_thread : workers) {
        worker_thread.join();
    }
    if (!registry_sampler.stop(error) || registry_sampler.threadStateDrops() == 0 ||
        !registry_sampler.dataIncomplete() || !registry_sampler.shutdown(error)) {
        std::fprintf(stderr,
                     "allocation pressure: thread registry did not fail bounded "
                     "(state-drops=%llu incomplete=%d error=%s)\n",
                     static_cast<unsigned long long>(registry_sampler.threadStateDrops()),
                     registry_sampler.dataIncomplete(), error.c_str());
        return false;
    }

    config.live_only = true;
    config.thread_state_limit_for_testing = 0;
    spark::AllocationSampler live_sampler;
    std::vector<void *> retained;
    retained.reserve(static_cast<std::size_t>(live_sampler.liveIndexCapacity() + 1024));
    if (!live_sampler.start(config, error)) {
        std::fprintf(stderr, "allocation pressure: live start failed: %s\n", error.c_str());
        return false;
    }
    for (std::uint64_t i = 0; i < live_sampler.liveIndexCapacity() + 1024; ++i) {
        void *pointer = std::malloc(1);
        if (pointer != nullptr) {
            static_cast<volatile unsigned char *>(pointer)[0] = 1;
            retained.push_back(pointer);
        }
    }
    const bool stopped = live_sampler.stop(error);
    const bool bounded = !stopped && live_sampler.lifecycleDropped() != 0 &&
                         live_sampler.peakLiveSamples() <= live_sampler.liveIndexCapacity() &&
                         live_sampler.dataIncomplete();
    for (void *pointer : retained) {
        std::free(pointer);
    }
    std::string shutdown_error;
    const bool shutdown = live_sampler.shutdown(shutdown_error);
    if (!bounded || !shutdown) {
        std::fprintf(stderr,
                     "allocation pressure: live index did not fail closed "
                     "(stopped=%d peak=%llu capacity=%llu lifecycle-dropped=%llu "
                     "error=%s shutdown=%s)\n",
                     stopped, static_cast<unsigned long long>(live_sampler.peakLiveSamples()),
                     static_cast<unsigned long long>(live_sampler.liveIndexCapacity()),
                     static_cast<unsigned long long>(live_sampler.lifecycleDropped()), error.c_str(),
                     shutdown_error.c_str());
        return false;
    }
    return true;
}

bool verifyAllocationLifecycle()
{
    using namespace std::chrono_literals;

    spark::AllocationSamplerConfig config;
    config.interval_bytes = 256;
    const std::uint64_t server_tid = spark::currentNativeThreadId();
    config.session_seed = server_tid;
    std::string error;

    spark::AllocationSampler sampler;
    if (!runAllocationSession(sampler, config, error) || !sampler.hooksInstalled() ||
        !runAllocationSession(sampler, config, error) || !sampler.hooksInstalled()) {
        return false;
    }
    const auto &capabilities = sampler.hookCapabilities();
    std::size_t active_hooks = 0;
    for (const spark::AllocationHookCapability &capability : capabilities) {
        active_hooks += capability.status == spark::AllocationHookStatus::Active ? 1 : 0;
    }
#if defined(_WIN32)
    constexpr std::size_t expected_capabilities = 19;
#else
    constexpr std::size_t expected_capabilities = 7;
#endif
    if (capabilities.size() != expected_capabilities || active_hooks < 3) {
        std::fprintf(stderr, "allocation lifecycle: invalid hook capability report (%zu total, %zu active)\n",
                     capabilities.size(), active_hooks);
        return false;
    }

    config.fail_aggregator_for_testing = true;
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "allocation lifecycle: injected-failure start failed: %s\n", error.c_str());
        return false;
    }
    bool failed = false;
    for (int i = 0; i < 1000; ++i) {
        if (sampler.failure(error)) {
            failed = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    std::string stop_error;
    const bool stopped_cleanly = sampler.stop(stop_error);
    if (!failed || stopped_cleanly || sampler.running() ||
        stop_error.find("injected allocation aggregator failure") == std::string::npos) {
        std::fprintf(stderr, "allocation lifecycle: aggregator failure was not surfaced safely: %s\n",
                     stop_error.c_str());
        return false;
    }

    config.fail_aggregator_for_testing = false;
    if (!runAllocationSession(sampler, config, error)) {
        std::fprintf(stderr, "allocation lifecycle: backend did not recover after failure\n");
        return false;
    }

    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "allocation lifecycle: concurrent-stop start failed: %s\n", error.c_str());
        return false;
    }
    std::atomic<bool> allocate{true};
    std::vector<std::thread> concurrent_workers;
    for (int i = 0; i < 4; ++i) {
        concurrent_workers.emplace_back([&allocate]() {
            while (allocate.load(std::memory_order_relaxed)) {
                void *pointer = std::malloc(256);
                if (pointer != nullptr) {
                    static_cast<volatile unsigned char *>(pointer)[0] = 1;
                    std::free(pointer);
                }
            }
        });
    }
    std::this_thread::sleep_for(20ms);
    const bool concurrent_stop = sampler.stop(error);
    allocate.store(false, std::memory_order_relaxed);
    for (std::thread &worker_thread : concurrent_workers) {
        worker_thread.join();
    }
    if (!concurrent_stop) {
        std::fprintf(stderr, "allocation lifecycle: concurrent stop failed: %s\n", error.c_str());
        return false;
    }

#if defined(_WIN32)
    HMODULE fixture = ::LoadLibraryA(SPARK_WINDOWS_ALLOCATION_FIXTURE_PATH);
    using FixtureRun = void (*)(volatile LONG *);
    auto fixture_run = fixture == nullptr
                         ? nullptr
                         : reinterpret_cast<FixtureRun>(::GetProcAddress(fixture, "sparkAllocationFixtureRun"));
    volatile LONG fixture_running = 1;
    std::thread fixture_worker;
    if (fixture_run != nullptr) {
        fixture_worker = std::thread(fixture_run, &fixture_running);
        std::this_thread::sleep_for(20ms);
    }
    const bool shutdown = fixture_run != nullptr && sampler.shutdown(error);
    ::InterlockedExchange(&fixture_running, 0);
    if (fixture_worker.joinable()) {
        fixture_worker.join();
    }
    if (fixture != nullptr) {
        ::FreeLibrary(fixture);
    }
    if (!shutdown || sampler.hooksInstalled()) {
        std::fprintf(stderr, "allocation lifecycle: concurrent hook shutdown failed: %s\n", error.c_str());
        return false;
    }
#else
    if (!sampler.shutdown(error) || sampler.hooksInstalled()) {
        std::fprintf(stderr, "allocation lifecycle: final hook cleanup failed: %s\n", error.c_str());
        return false;
    }
#endif

    // A second instance in the same process models plugin reload: the old
    // active-instance pointer and trampolines must not obstruct new setup.
    spark::AllocationSampler reloaded;
    if (!runAllocationSession(reloaded, config, error) || !reloaded.shutdown(error) || reloaded.hooksInstalled()) {
        std::fprintf(stderr, "allocation lifecycle: reload simulation failed: %s\n", error.c_str());
        return false;
    }

    spark::Profiler failed_profiler;
    spark::ProfilerOptions options;
    options.alloc = true;
    options.allocation_interval_bytes = 256;
    options.fail_allocation_aggregator_for_testing = true;
    if (!failed_profiler.start(options, server_tid, error)) {
        std::fprintf(stderr, "profiler failure state: injected start failed: %s\n", error.c_str());
        return false;
    }
    bool profiler_failed = false;
    for (int i = 0; i < 1000; ++i) {
        if (failed_profiler.backendFailure(error)) {
            profiler_failed = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    if (!profiler_failed || !failed_profiler.cancel(error) || failed_profiler.running()) {
        std::fprintf(stderr, "profiler failure state: failed session did not cancel cleanly: %s\n", error.c_str());
        return false;
    }
    options.fail_allocation_aggregator_for_testing = false;
    if (!failed_profiler.start(options, server_tid, error) || !exerciseNativeAllocations() ||
        !failed_profiler.stopSampling(error)) {
        std::fprintf(stderr, "profiler failure state: healthy restart failed: %s\n", error.c_str());
        return false;
    }
    spark::ExportContext allocation_context;
    const std::string allocation_profile = failed_profiler.exportData(allocation_context);
    if (allocation_profile.find("Allocation hook capabilities") == std::string::npos ||
        allocation_profile.find("Allocation hook targets installed") == std::string::npos ||
        allocation_profile.find("Allocation thread filter stage") == std::string::npos ||
        !failed_profiler.shutdown(error)) {
        std::fprintf(stderr, "allocation capability metadata: export validation failed: %s\n", error.c_str());
        return false;
    }
    return true;
}

bool verifyRetainedAllocationProfile()
{
    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.alloc = true;
    options.alloc_live_only = true;
    options.allocation_interval_bytes = 1;
    std::string error;
    const std::uint64_t server_tid = spark::currentNativeThreadId();
    if (!profiler.start(options, server_tid, error)) {
        std::fprintf(stderr, "retained allocation: start failed: %s\n", error.c_str());
        return false;
    }

    void *retained = std::malloc(8192);
    void *released = std::malloc(4096);
    if (retained == nullptr || released == nullptr) {
        std::free(retained);
        std::free(released);
        return false;
    }
    static_cast<volatile unsigned char *>(retained)[0] = 1;
    static_cast<volatile unsigned char *>(released)[0] = 2;
    void *resized = std::realloc(retained, 16384);
    if (resized != nullptr) {
        retained = resized;
    }
    void *failed_resize = std::realloc(retained, (std::numeric_limits<std::size_t>::max)());
    if (failed_resize != nullptr) {
        std::free(failed_resize);
        std::free(released);
        return false;
    }
    static_cast<volatile unsigned char *>(retained)[0] = 3;
    std::free(released);
    profiler.onTick(50.0);
    if (!profiler.stopSampling(error)) {
        std::fprintf(stderr, "retained allocation: stop failed: %s\n", error.c_str());
        std::free(retained);
        return false;
    }

    spark::ExportContext context;
    const std::string profile = profiler.exportData(context);
    const bool valid = profiler.sampleCount() != 0 && profiler.sampledAllocationBytes() >= 8192 &&
                       profiler.freedAllocationSamples() != 0 &&
                       profile.find("Allocation live-only") != std::string::npos &&
                       profile.find("Allocation retained maximum age ms") != std::string::npos;
    std::free(retained);
    if (!profiler.shutdown(error) || !valid) {
        std::fprintf(stderr,
                     "retained allocation: profile validation failed: %s "
                     "(samples=%llu bytes=%llu freed=%llu live-meta=%d age-meta=%d)\n",
                     error.c_str(), static_cast<unsigned long long>(profiler.sampleCount()),
                     static_cast<unsigned long long>(profiler.sampledAllocationBytes()),
                     static_cast<unsigned long long>(profiler.freedAllocationSamples()),
                     profile.find("Allocation live-only") != std::string::npos,
                     profile.find("Allocation retained maximum age ms") != std::string::npos);
        return false;
    }
    return true;
}
#endif

#if defined(__linux__)
pid_t linuxHookProbe() noexcept
{
    return static_cast<pid_t>(-12345);
}

pid_t (*volatile linux_getpid_call)() = &::getpid;

bool verifyLinuxImportHooks()
{
    const pid_t expected = ::getpid();
    spark::ElfImportHooks hooks;
    const spark::ElfImportHookSpec spec{"getpid", reinterpret_cast<void *>(&linuxHookProbe), true};
    std::string error;
    if (!hooks.prepare(std::span<const spark::ElfImportHookSpec>(&spec, 1), error) || hooks.targetCount() == 0 ||
        !hooks.install(error)) {
        std::fprintf(stderr, "linux import hooks: setup failed: %s\n", error.c_str());
        return false;
    }
    if (linux_getpid_call() != static_cast<pid_t>(-12345)) {
        std::fprintf(stderr, "linux import hooks: replacement was not observed\n");
        return false;
    }

    void *fixture = ::dlopen(SPARK_ELF_HOOK_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    auto fixture_getpid =
        fixture == nullptr ? nullptr : reinterpret_cast<pid_t (*)()>(::dlsym(fixture, "sparkElfHookFixtureGetpid"));
    if (fixture_getpid == nullptr || fixture_getpid() != expected || !hooks.rescan(error) ||
        fixture_getpid() != static_cast<pid_t>(-12345) || hooks.hookedModuleCount() < 2) {
        std::fprintf(stderr, "linux import hooks: loaded-module rescan failed: %s\n", error.c_str());
        if (fixture != nullptr) {
            ::dlclose(fixture);
        }
        return false;
    }
    ::dlclose(fixture);

    if (!hooks.uninstall(error) || linux_getpid_call() != expected) {
        std::fprintf(stderr, "linux import hooks: restoration failed: %s\n", error.c_str());
        return false;
    }
    return true;
}
#endif

}  // namespace

int main(int argc, char **argv)
{
    using namespace std::chrono_literals;

    // Diagnostic: resolve a spread of addresses in a given binary to reproduce
    // symbolication crashes (e.g. the stripped bedrock_server) offline.
    if (argc > 1 && std::string(argv[1]) == "--probe") {
        std::string path = argc > 2 ? argv[2] : "";
        spark::ModuleTable modules;
        spark::ModuleId mid = modules.intern(path);
        std::vector<spark::FrameKey> keys;
        for (std::uint64_t rva = 0x100000; rva < 0x8000000; rva += 0x20000) {
            spark::FrameKey k;
            k.module = mid;
            k.rva = rva;
            keys.push_back(k);
        }
        std::fprintf(stderr, "probe: resolving %zu frames from %s\n", keys.size(), path.c_str());
        auto resolved = spark::resolveFrames(modules, keys);
        std::size_t named = 0;
        for (auto &[k, v] : resolved) {
            if (v.method_name.rfind("0x", 0) != 0) {
                ++named;
            }
        }
        std::fprintf(stderr, "probe: resolved=%zu named=%zu (no crash)\n", resolved.size(), named);
        return 0;
    }

    if (argc > 1 && std::string(argv[1]) == "--allocation-only") {
#if defined(_WIN32) || defined(__linux__)
        if (!verifyAllocationLifecycle()) {
            std::fprintf(stderr, "allocation-only: lifecycle test failed\n");
            return 1;
        }
        if (!verifyRetainedAllocationProfile()) {
            std::fprintf(stderr, "allocation-only: retained profile test failed\n");
            return 1;
        }
        if (!verifyAllocationThreadSelection()) {
            std::fprintf(stderr, "allocation-only: thread selection test failed\n");
            return 1;
        }
        if (!verifyProcessWideAllocationSampling()) {
            std::fprintf(stderr, "allocation-only: process-wide test failed\n");
            return 1;
        }
        if (!verifyAllocationResourcePressure()) {
            std::fprintf(stderr, "allocation-only: resource pressure test failed\n");
            return 1;
        }
        return 0;
#else
        return 0;
#endif
    }

    if (argc > 1 && std::string(argv[1]) == "--statistics-only") {
        return verifyStatisticsService() && verifySystemResourceStats() && verifyWorldGaugeStatistics() &&
                       verifyWorldGaugeAbsentWhenNotRecorded()
                 ? 0
                 : 1;
    }

    int seconds = 4;
    bool upload = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--upload") {
            upload = true;
        }
        else if (a.rfind("--seconds=", 0) == 0) {
            seconds = std::atoi(a.c_str() + 10);
        }
    }

    std::thread w(worker);
    while (g_worker_tid.load() == 0) {
        std::this_thread::sleep_for(1ms);
    }

    if (!verifyArgumentParsing() || !spark::SamplerTestAccess::verifyContinuousHistory() ||
        !verifyThreadSelectorSemantics() || !verifyTickMonitor() || !verifyStatisticsService() ||
        !verifySystemResourceStats() || !verifyWorldGaugeStatistics() || !verifyWorldGaugeAbsentWhenNotRecorded() ||
        !verifyThreadDiscovery() || !verifyMultiThreadSerialization() || !verifyStatisticsSerialization() ||
        !verifyLiveProfilerWindowStatistics(g_worker_tid.load()) || !verifyAsyncNetworkCommands(g_worker_tid.load()) ||
        !verifyBackgroundCommandValidation(g_worker_tid.load()) || !verifyRecoveryWriterLifetime(g_worker_tid.load()) ||
        !verifyUploadFailure() || !verifyCaptureLifecycle() ||
#if defined(_WIN32)
        !verifyWindowsThreadActivityDetection() ||
#endif
        !verifyAllThreadSampling() || !verifySelectedThreadSampling(g_worker_tid.load()) || !verifyExecutableHash() ||
        !verifyByteSampling() || !verifyStopResponsiveness() || !verifySessionIsolation(g_worker_tid.load()) ||
        !verifyTickFiltering(g_worker_tid.load())
#if defined(_WIN32)
        || !verifyAllocationLifecycle() || !verifyRetainedAllocationProfile() || !verifyAllocationThreadSelection() ||
        !verifyProcessWideAllocationSampling() || !verifyAllocationResourcePressure()
#elif defined(__linux__)
        || !verifyLinuxImportHooks() || !verifyAllocationLifecycle() || !verifyRetainedAllocationProfile() ||
        !verifyAllocationThreadSelection() || !verifyProcessWideAllocationSampling() ||
        !verifyAllocationResourcePressure()
#endif
    ) {
        g_run.store(false);
        w.join();
        return 1;
    }

    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.interval_ms = 4;
    options.ignore_sleeping = true;

    std::string error;
    if (!profiler.start(options, g_worker_tid.load(), error)) {
        std::fprintf(stderr, "profiler start failed: %s\n", error.c_str());
        g_run.store(false);
        w.join();
        return 1;
    }

    // Drive ~20 "ticks" per second so windows/bucketing exercise like a real server.
    for (int i = 0; i < seconds * 20; ++i) {
        std::this_thread::sleep_for(50ms);
        profiler.onTick(30.0);
    }

    spark::ExportContext ctx;
    ctx.endstone_version = "0.11.5";
    ctx.minecraft_version = "1.26.33";
    std::string executable_hash_error;
    ctx.bds_executable_sha256 = spark::currentExecutableSha256(executable_hash_error);
    std::string bytes = profiler.stop(ctx);

    if (bytes.find("BDS executable SHA-256") == std::string::npos ||
        bytes.find(ctx.bds_executable_sha256) == std::string::npos) {
        std::fprintf(stderr, "executable hash: profile metadata is missing\n");
        g_run.store(false);
        w.join();
        return 1;
    }

    g_run.store(false);
    w.join();

    std::ofstream("profile.pb", std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    std::string gz = spark::gzipCompress(bytes);
    std::ofstream("profile.sparkprofile", std::ios::binary).write(gz.data(), static_cast<std::streamsize>(gz.size()));

    const std::filesystem::path profile_root =
        std::filesystem::temp_directory_path() /
        ("spark-profile-selftest-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path profile_directory = spark::profileStorageDirectory(profile_root);
    spark::ProfileFileResult saved = spark::saveProfileToDirectory(profile_directory, gz, 42);
    if (!saved.ok) {
        std::fprintf(stderr, "profile file: atomic save failed: %s\n", saved.error.c_str());
        return 1;
    }
    if (saved.path.parent_path() != profile_directory) {
        std::fprintf(stderr, "profile file: local profile used the wrong directory\n");
        std::error_code cleanup_error;
        std::filesystem::remove_all(profile_root, cleanup_error);
        return 1;
    }
    std::ifstream saved_stream(saved.path, std::ios::binary);
    std::string round_trip((std::istreambuf_iterator<char>(saved_stream)), std::istreambuf_iterator<char>());
    saved_stream.close();
    std::error_code cleanup_error;
    std::filesystem::remove_all(profile_root, cleanup_error);
    if (round_trip != gz || cleanup_error) {
        std::fprintf(stderr, "profile file: saved gzip payload did not round-trip cleanly\n");
        return 1;
    }

    std::printf("samples=%llu proto=%zuB gzip=%zuB\n", static_cast<unsigned long long>(profiler.sampleCount()),
                bytes.size(), gz.size());
    std::printf("wrote profile.pb, profile.sparkprofile\n");

    if (upload) {
        auto result = spark::uploadToBytebin(gz, spark::kBytebinUrl, spark::kSamplerContentType,
                                             std::string("endstone-spark/") + spark::kVersion);
        if (result.ok) {
            std::printf("%s%s\n", spark::kViewerUrl, result.key.c_str());
        }
        else {
            std::printf("upload failed: %s\n", result.error.c_str());
        }
    }
    return 0;
}
