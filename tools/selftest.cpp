// Offline end-to-end test for the sampler + spark serializer, with no BDS involved.
// Spawns a worker thread with a recognizable nested call pattern plus a sleeping
// phase, profiles it, and writes profile.pb (raw SamplerData) + profile.sparkprofile
// (gzipped, loadable in the spark viewer). Pass --upload to POST to bytebin.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#include <sys/syscall.h>
#include <unistd.h>

#include "net/bytebin.h"
#include "net/gzip.h"
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
    g_worker_tid.store(static_cast<std::uint64_t>(::syscall(SYS_gettid)));
    while (g_run.load()) {
        hotOuter();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the "off-tick" sleep
    }
}

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
