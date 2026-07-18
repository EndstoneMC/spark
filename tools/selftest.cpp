// Offline end-to-end test for the sampler + spark serializer, with no BDS involved.
// Spawns a worker thread with a recognizable nested call pattern plus a sleeping
// phase, profiles it, and writes profile.pb (raw SamplerData) + profile.sparkprofile
// (gzipped, loadable in the spark viewer). Pass --upload to POST to bytebin.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "command/arguments.h"
#include "alloc/byte_sampler.h"
#include "alloc/allocation_sampler.h"
#include "net/bytebin.h"
#include "net/gzip.h"
#include "net/profile_file.h"
#include "sampler/capture.h"
#include "sampler/profiler.h"
#include "sampler/symbolicate.h"
#include "sampler/types.h"
#include "spark_constants.h"

namespace {

volatile double g_sink = 0.0;

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
    if (sampler.sampleCount() == 0 || sampler.sampleCount() != sampler.tree().sampleCount() ||
        sampler.modules().size() == 0 || sampler.numberOfTicks() != 50 || sampler.windowTicks().empty()) {
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
    std::this_thread::sleep_for(50ms);
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
    std::this_thread::sleep_for(50ms);
    sampler.onTick(50.0);
    sampler.stop();
    if (sampler.sampleCount() == 0 || sampler.sampleCount() != sampler.tree().sampleCount()) {
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
        if (spark::consumeSampledBytes(first, bytes, 64) !=
            spark::consumeSampledBytes(replay, bytes, 64)) {
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
            const std::uint64_t chunk =
                (std::min)(std::uint64_t{4096}, observed - consumed);
            points += spark::consumeSampledBytes(state, chunk, interval);
        }
        const double ratio = static_cast<double>(points) * static_cast<double>(interval) /
                             static_cast<double>(observed);
        if (ratio < 0.94 || ratio > 1.06 || state.bytes_until_sample == 0) {
            std::fprintf(stderr,
                         "byte sampling: interval=%llu produced implausible ratio %.6f\n",
                         static_cast<unsigned long long>(interval), ratio);
            return false;
        }
    }
    return true;
}

#if defined(_WIN32)
__declspec(noinline) bool exerciseNativeAllocations()
{
    for (std::size_t i = 0; i < 4096; ++i) {
        const std::size_t size = 512 + (i & 255);
        void *allocation = std::malloc(size);
        if (allocation == nullptr) {
            return false;
        }
        static_cast<volatile unsigned char *>(allocation)[0] =
            static_cast<unsigned char>(i);
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

    void *cross_thread = std::malloc(4096);
    if (cross_thread == nullptr) {
        return false;
    }
    std::thread releaser([cross_thread]() { std::free(cross_thread); });
    releaser.join();
    return true;
}

bool runAllocationSession(spark::AllocationSampler &sampler,
                          const spark::AllocationSamplerConfig &config,
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
    if (sampler.sampleCount() == 0 || sampler.observedBytes() == 0 ||
        sampler.freedSamples() == 0 || sampler.freedBytes() == 0 ||
        sampler.lifecycleDropped() != 0) {
        std::fprintf(stderr, "allocation lifecycle: session captured no allocations\n");
        return false;
    }
    return true;
}

bool verifyAllocationLifecycle()
{
    using namespace std::chrono_literals;

    spark::AllocationSamplerConfig config;
    config.interval_bytes = 256;
    config.target_tid = static_cast<std::uint64_t>(::GetCurrentThreadId());
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
    if (capabilities.size() != 19 || active_hooks < 7) {
        std::fprintf(stderr,
                     "allocation lifecycle: invalid hook capability report (%zu total, %zu active)\n",
                     capabilities.size(), active_hooks);
        return false;
    }

    config.fail_aggregator_for_testing = true;
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "allocation lifecycle: injected-failure start failed: %s\n",
                     error.c_str());
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
        std::fprintf(stderr,
                     "allocation lifecycle: aggregator failure was not surfaced safely: %s\n",
                     stop_error.c_str());
        return false;
    }

    config.fail_aggregator_for_testing = false;
    if (!runAllocationSession(sampler, config, error)) {
        std::fprintf(stderr, "allocation lifecycle: backend did not recover after failure\n");
        return false;
    }
    if (!sampler.shutdown(error) || sampler.hooksInstalled()) {
        std::fprintf(stderr, "allocation lifecycle: final hook cleanup failed: %s\n",
                     error.c_str());
        return false;
    }

    // A second instance in the same process models plugin reload: the old
    // active-instance pointer and trampolines must not obstruct new setup.
    spark::AllocationSampler reloaded;
    if (!runAllocationSession(reloaded, config, error) || !reloaded.shutdown(error) ||
        reloaded.hooksInstalled()) {
        std::fprintf(stderr, "allocation lifecycle: reload simulation failed: %s\n",
                     error.c_str());
        return false;
    }

    spark::Profiler failed_profiler;
    spark::ProfilerOptions options;
    options.alloc = true;
    options.allocation_interval_bytes = 256;
    options.fail_allocation_aggregator_for_testing = true;
    if (!failed_profiler.start(options, config.target_tid, error)) {
        std::fprintf(stderr, "profiler failure state: injected start failed: %s\n",
                     error.c_str());
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
        std::fprintf(stderr, "profiler failure state: failed session did not cancel cleanly: %s\n",
                     error.c_str());
        return false;
    }
    options.fail_allocation_aggregator_for_testing = false;
    if (!failed_profiler.start(options, config.target_tid, error) ||
        !exerciseNativeAllocations() || !failed_profiler.stopSampling(error)) {
        std::fprintf(stderr, "profiler failure state: healthy restart failed: %s\n",
                     error.c_str());
        return false;
    }
    spark::ExportContext allocation_context;
    const std::string allocation_profile = failed_profiler.exportData(allocation_context);
    if (allocation_profile.find("Allocation hook capabilities") == std::string::npos ||
        allocation_profile.find("Allocation hook targets installed") == std::string::npos ||
        !failed_profiler.shutdown(error)) {
        std::fprintf(stderr, "allocation capability metadata: export validation failed: %s\n",
                     error.c_str());
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
    if (!profiler.start(options, static_cast<std::uint64_t>(::GetCurrentThreadId()), error)) {
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
    std::free(released);
    profiler.onTick(50.0);
    if (!profiler.stopSampling(error)) {
        std::fprintf(stderr, "retained allocation: stop failed: %s\n", error.c_str());
        std::free(retained);
        return false;
    }

    spark::ExportContext context;
    const std::string profile = profiler.exportData(context);
    const bool valid = profiler.sampleCount() != 0 &&
                       profiler.sampledAllocationBytes() >= 8192 &&
                       profiler.freedAllocationSamples() != 0 &&
                       profile.find("Allocation live-only") != std::string::npos &&
                       profile.find("Allocation retained maximum age ms") != std::string::npos;
    std::free(retained);
    if (!profiler.shutdown(error) || !valid) {
        std::fprintf(stderr,
                     "retained allocation: profile validation failed: %s "
                     "(samples=%llu bytes=%llu freed=%llu live-meta=%d age-meta=%d)\n",
                     error.c_str(),
                     static_cast<unsigned long long>(profiler.sampleCount()),
                     static_cast<unsigned long long>(profiler.sampledAllocationBytes()),
                     static_cast<unsigned long long>(profiler.freedAllocationSamples()),
                     profile.find("Allocation live-only") != std::string::npos,
                     profile.find("Allocation retained maximum age ms") != std::string::npos);
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

    if (!verifyArgumentParsing() || !verifyUploadFailure() || !verifyCaptureLifecycle() ||
        !verifyByteSampling() ||
        !verifyStopResponsiveness() ||
        !verifySessionIsolation(g_worker_tid.load()) || !verifyTickFiltering(g_worker_tid.load())
#if defined(_WIN32)
        || !verifyAllocationLifecycle() || !verifyRetainedAllocationProfile()
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
    std::string bytes = profiler.stop(ctx);

    g_run.store(false);
    w.join();

    std::ofstream("profile.pb", std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    std::string gz = spark::gzipCompress(bytes);
    std::ofstream("profile.sparkprofile", std::ios::binary).write(gz.data(), static_cast<std::streamsize>(gz.size()));

    spark::ProfileFileResult saved = spark::saveProfileToDirectory(".", gz, 42);
    if (!saved.ok) {
        std::fprintf(stderr, "profile file: atomic save failed: %s\n", saved.error.c_str());
        return 1;
    }
    std::ifstream saved_stream(saved.path, std::ios::binary);
    std::string round_trip((std::istreambuf_iterator<char>(saved_stream)),
                           std::istreambuf_iterator<char>());
    saved_stream.close();
    std::error_code cleanup_error;
    std::filesystem::remove(saved.path, cleanup_error);
    if (round_trip != gz || cleanup_error) {
        std::fprintf(stderr, "profile file: saved gzip payload did not round-trip cleanly\n");
        return 1;
    }

    std::printf("samples=%llu proto=%zuB gzip=%zuB\n", static_cast<unsigned long long>(profiler.sampleCount()),
                bytes.size(), gz.size());
    std::printf("wrote profile.pb, profile.sparkprofile\n");

    if (upload) {
        auto result = spark::uploadToBytebin(gz, spark::kBytebinUrl,
                                                      spark::kSamplerContentType,
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
