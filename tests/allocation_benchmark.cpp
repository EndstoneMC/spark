#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <latch>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#endif

#include "native/alloc/allocation_sampler.h"
#include "native/sampler/thread_info.h"

#ifdef __linux__
extern "C" void spark_benchmark_workload(std::size_t kind, std::size_t operations, std::uint64_t *requested_bytes);
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct ProcessCpuSnapshot {
    std::uint64_t elapsed_ns = 0;
    bool supported = false;
};

ProcessCpuSnapshot processCpuSnapshot() noexcept
{
#ifdef __linux__
    timespec time{};
    if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) != 0) {
        return {};
    }
    return {.elapsed_ns =
                static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(time.tv_nsec),
            .supported = true};
#elif defined(_WIN32)
    const std::clock_t time = std::clock();
    if (time == static_cast<std::clock_t>(-1)) {
        return {};
    }
    const double nanoseconds = static_cast<double>(time) * 1'000'000'000.0 / CLOCKS_PER_SEC;
    return {.elapsed_ns = static_cast<std::uint64_t>(nanoseconds), .supported = true};
#else
    return {};
#endif
}

void printBenchmarkMetadata(const spark::AllocationSampler &sampler)
{
#ifdef __linux__
    const char *ld_preload = std::getenv("LD_PRELOAD");
    const char *preload_status = ld_preload != nullptr && ld_preload[0] != '\0' ? "set" : "unset";
    std::fprintf(stderr, "spark_allocation_benchmark_metadata backend=\"%s\" ld_preload=%s path=omitted\n",
                 sampler.resolvedBackendName(), preload_status);
#elif defined(_WIN32)
    std::fprintf(stderr, "spark_allocation_benchmark_metadata backend=\"%s\" ld_preload=not-applicable\n",
                 sampler.resolvedBackendName());
#else
    std::fprintf(stderr, "spark_allocation_benchmark_metadata backend=\"%s\" ld_preload=not-applicable\n",
                 sampler.resolvedBackendName());
#endif
}

#ifdef _MSC_VER
#define SPARK_BENCHMARK_NOINLINE __declspec(noinline)
#else
#define SPARK_BENCHMARK_NOINLINE __attribute__((noinline))
#endif

SPARK_BENCHMARK_NOINLINE void *benchmarkMalloc(std::size_t bytes)
{
    return std::malloc(bytes);
}

SPARK_BENCHMARK_NOINLINE void *benchmarkCalloc(std::size_t bytes)
{
    return std::calloc(1, bytes);
}

SPARK_BENCHMARK_NOINLINE void *benchmarkRealloc(void *pointer, std::size_t bytes)
{
    return std::realloc(pointer, bytes);
}

SPARK_BENCHMARK_NOINLINE void benchmarkFree(void *pointer)
{
    std::free(pointer);
}

SPARK_BENCHMARK_NOINLINE void *benchmarkAlignedMalloc(std::size_t bytes)
{
#ifdef _WIN32
    return _aligned_malloc(bytes, 64);
#else
    const std::size_t aligned_bytes = (bytes + 63) & ~std::size_t{63};
    return std::aligned_alloc(64, aligned_bytes);
#endif
}

SPARK_BENCHMARK_NOINLINE void benchmarkAlignedFree(void *pointer)
{
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

#undef SPARK_BENCHMARK_NOINLINE

enum class MatrixWorkload {
    Tiny,
    Small,
    Medium,
    Mixed,
    Calloc,
    ReallocHeavy,
    Aligned,
};

struct WorkloadSpec {
    const char *name;
    MatrixWorkload workload;
};

constexpr WorkloadSpec KMatrixWorkloads[] = {
    {.name = "malloc_free_tiny", .workload = MatrixWorkload::Tiny},
    {.name = "malloc_free_small", .workload = MatrixWorkload::Small},
    {.name = "malloc_free_medium", .workload = MatrixWorkload::Medium},
    {.name = "malloc_free_mixed", .workload = MatrixWorkload::Mixed},
    {.name = "calloc_free", .workload = MatrixWorkload::Calloc},
    {.name = "realloc_heavy", .workload = MatrixWorkload::ReallocHeavy},
    {.name = "aligned_alloc_free", .workload = MatrixWorkload::Aligned},
};

struct WorkloadResult {
    std::uint64_t allocator_calls = 0;
    std::uint64_t successful_allocations = 0;
    std::uint64_t requested_bytes = 0;
    bool succeeded = true;
};

std::size_t allocationSize(MatrixWorkload workload, std::size_t iteration)
{
    switch (workload) {
    case MatrixWorkload::Tiny:
        return 16 + iteration % 49;
    case MatrixWorkload::Small:
    case MatrixWorkload::Calloc:
    case MatrixWorkload::ReallocHeavy:
    case MatrixWorkload::Aligned:
        return 64 + iteration % 193;
    case MatrixWorkload::Medium:
        return 256 + iteration % 769;
    case MatrixWorkload::Mixed:
        switch (iteration % 3) {
        case 0:
            return 16 + iteration % 49;
        case 1:
            return 64 + iteration % 193;
        default:
            return 256 + iteration % 769;
        }
    }
    return 0;
}

std::uint64_t expectedRequestedBytes(MatrixWorkload workload, std::size_t operations)
{
    std::uint64_t total = 0;
    if (workload == MatrixWorkload::ReallocHeavy) {
        total += allocationSize(MatrixWorkload::ReallocHeavy, 0);
        for (std::size_t i = 0; i < operations - 2; ++i) {
            total += allocationSize(MatrixWorkload::ReallocHeavy, i + 1);
        }
        return total;
    }
    for (std::size_t i = 0; i < operations / 2; ++i) {
        std::size_t bytes = allocationSize(workload, i);
        if (workload == MatrixWorkload::Aligned) {
            bytes = (bytes + 63) & ~std::size_t{63};
        }
        total += bytes;
    }
    return total;
}

void touchAllocation(void *pointer, std::size_t bytes, std::size_t iteration)
{
    auto *memory = static_cast<volatile unsigned char *>(pointer);
    memory[0] = static_cast<unsigned char>(iteration);
    if (bytes > 1) {
        memory[bytes - 1] = static_cast<unsigned char>(iteration >> 8);
    }
}

WorkloadResult runWorkload(MatrixWorkload workload, std::size_t operations)
{
    WorkloadResult result;
#ifdef __linux__
    const auto kind = static_cast<std::size_t>(workload);
    spark_benchmark_workload(kind, operations, &result.requested_bytes);
    result.allocator_calls = operations;
    result.successful_allocations = workload == MatrixWorkload::ReallocHeavy ? operations - 1 : operations / 2;
    result.succeeded = result.requested_bytes == expectedRequestedBytes(workload, operations);
    return result;
#else
    if (workload == MatrixWorkload::ReallocHeavy) {
        std::size_t bytes = allocationSize(MatrixWorkload::Small, 0);
        void *pointer = benchmarkMalloc(bytes);
        ++result.allocator_calls;
        if (pointer == nullptr) {
            result.succeeded = false;
            return result;
        }
        ++result.successful_allocations;
        result.requested_bytes += bytes;
        touchAllocation(pointer, bytes, 0);

        for (std::size_t i = 0; i < operations - 2; ++i) {
            bytes = allocationSize(MatrixWorkload::Small, i + 1);
            void *replacement = benchmarkRealloc(pointer, bytes);
            ++result.allocator_calls;
            if (replacement == nullptr) {
                result.succeeded = false;
                break;
            }
            pointer = replacement;
            ++result.successful_allocations;
            result.requested_bytes += bytes;
            touchAllocation(pointer, bytes, i + 1);
        }
        benchmarkFree(pointer);
        ++result.allocator_calls;
        return result;
    }

    for (std::size_t i = 0; i < operations / 2; ++i) {
        std::size_t bytes = allocationSize(workload, i);
        void *pointer = nullptr;
        if (workload == MatrixWorkload::Calloc) {
            pointer = benchmarkCalloc(bytes);
        }
        else if (workload == MatrixWorkload::Aligned) {
            bytes = (bytes + 63) & ~std::size_t{63};
            pointer = benchmarkAlignedMalloc(bytes);
        }
        else {
            pointer = benchmarkMalloc(bytes);
        }
        ++result.allocator_calls;
        if (pointer == nullptr) {
            result.succeeded = false;
            break;
        }
        ++result.successful_allocations;
        result.requested_bytes += bytes;
        touchAllocation(pointer, bytes, i);
        if (workload == MatrixWorkload::Aligned) {
            benchmarkAlignedFree(pointer);
        }
        else {
            benchmarkFree(pointer);
        }
        ++result.allocator_calls;
    }
    return result;
#endif
}

struct CounterSnapshot {
    std::uint64_t hook_calls = 0;
    std::uint64_t successful_allocations = 0;
    std::uint64_t observed_bytes = 0;
};

CounterSnapshot allocationCounters(const spark::AllocationSampler &sampler)
{
    return {.hook_calls = sampler.hookCalls(),
            .successful_allocations = sampler.successfulAllocationCalls(),
            .observed_bytes = sampler.observedBytes()};
}

double coverageRatio(std::uint64_t observed, std::uint64_t expected)
{
    if (expected == 0) {
        return observed == 0 ? 1.0 : 0.0;
    }
    return static_cast<double>(observed) / static_cast<double>(expected);
}

bool coverageWithinLimit(std::uint64_t observed, std::uint64_t expected, bool require_exact)
{
    if (observed > expected) {
        return false;
    }
    if (require_exact || expected == 0) {
        return observed == expected;
    }
    return observed * 100 >= expected * 95;
}

struct TrialResult {
    double elapsed_ns = 0;
    std::uint64_t cpu_elapsed_ns = 0;
    bool cpu_supported = false;
    WorkloadResult workload;
    CounterSnapshot counters;
};

void setCpuElapsed(TrialResult &result, const ProcessCpuSnapshot &before, const ProcessCpuSnapshot &after)
{
    result.cpu_supported = before.supported && after.supported && after.elapsed_ns > before.elapsed_ns;
    result.cpu_elapsed_ns = result.cpu_supported ? after.elapsed_ns - before.elapsed_ns : 0;
}

void addWorkloadResult(WorkloadResult &total, const WorkloadResult &worker)
{
    total.allocator_calls += worker.allocator_calls;
    total.successful_allocations += worker.successful_allocations;
    total.requested_bytes += worker.requested_bytes;
    total.succeeded = total.succeeded && worker.succeeded;
}

bool runMatrixTrial(spark::AllocationSampler &sampler, MatrixWorkload workload, std::size_t threads,
                    std::size_t operations, bool count_only, TrialResult &result)
{
    if (count_only) {
        spark::AllocationSamplerConfig config;
        config.session_seed = spark::currentNativeThreadId();
        config.count_only = true;
        std::string error;
        if (!sampler.start(config, error)) {
            std::fprintf(stderr, "persistent-matrix: count-only start failed: %s\n", error.c_str());
            return false;
        }
    }

    std::vector<WorkloadResult> worker_results(threads);
    const auto run_single_thread = [&] {
        const CounterSnapshot before = allocationCounters(sampler);
        const ProcessCpuSnapshot cpu_before = processCpuSnapshot();
        const auto start = Clock::now();
        worker_results[0] = runWorkload(workload, operations);
        const auto end = Clock::now();
        const ProcessCpuSnapshot cpu_after = processCpuSnapshot();
        const CounterSnapshot after = allocationCounters(sampler);
        result.elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();
        setCpuElapsed(result, cpu_before, cpu_after);
        result.counters = {.hook_calls = after.hook_calls - before.hook_calls,
                           .successful_allocations = after.successful_allocations - before.successful_allocations,
                           .observed_bytes = after.observed_bytes - before.observed_bytes};
    };

    if (threads == 1) {
        run_single_thread();
    }
    else {
        std::latch ready(static_cast<std::ptrdiff_t>(threads));
        std::latch begin(1);
        std::latch finished(static_cast<std::ptrdiff_t>(threads));
        std::latch finish_release(1);
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers.emplace_back([&, i] {
                static_cast<void>(runWorkload(workload, 2));
                ready.count_down();
                begin.wait();
                worker_results[i] = runWorkload(workload, operations);
                finished.count_down();
                finish_release.wait();
            });
        }
        ready.wait();
        const CounterSnapshot before = allocationCounters(sampler);
        const ProcessCpuSnapshot cpu_before = processCpuSnapshot();
        const auto start = Clock::now();
        begin.count_down();
        finished.wait();
        const auto end = Clock::now();
        const ProcessCpuSnapshot cpu_after = processCpuSnapshot();
        const CounterSnapshot after = allocationCounters(sampler);
        finish_release.count_down();
        for (std::thread &worker : workers) {
            worker.join();
        }
        result.elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();
        setCpuElapsed(result, cpu_before, cpu_after);
        result.counters = {.hook_calls = after.hook_calls - before.hook_calls,
                           .successful_allocations = after.successful_allocations - before.successful_allocations,
                           .observed_bytes = after.observed_bytes - before.observed_bytes};
    }

    for (const WorkloadResult &worker_result : worker_results) {
        addWorkloadResult(result.workload, worker_result);
    }
    if (count_only) {
        std::string error;
        if (!sampler.stop(error)) {
            std::fprintf(stderr, "persistent-matrix: count-only stop failed: %s\n", error.c_str());
            return false;
        }
    }

    if (!result.workload.succeeded) {
        std::fprintf(stderr, "persistent-matrix: allocation failed during workload\n");
        return false;
    }
#ifdef __linux__
    if (!result.cpu_supported) {
        std::fprintf(stderr, "persistent-matrix: CLOCK_PROCESS_CPUTIME_ID unavailable\n");
        return false;
    }
#endif
    if (count_only) {
        bool coverage_valid = result.counters.hook_calls >= result.workload.successful_allocations &&
                              result.counters.successful_allocations == result.workload.successful_allocations;
        bool observed_bytes_valid = result.counters.observed_bytes == result.workload.requested_bytes;
#ifdef __linux__
        const bool require_exact = threads == 1;
        coverage_valid =
            coverageWithinLimit(result.counters.hook_calls, result.workload.allocator_calls, require_exact) &&
            coverageWithinLimit(result.counters.successful_allocations, result.workload.successful_allocations,
                                require_exact) &&
            coverageWithinLimit(result.counters.observed_bytes, result.workload.requested_bytes, require_exact);
        observed_bytes_valid = true;
#elif defined(_WIN32)
        if (workload == MatrixWorkload::Aligned) {
            observed_bytes_valid = result.counters.observed_bytes >= result.workload.requested_bytes;
        }
#endif
        if (!coverage_valid || !observed_bytes_valid || sampler.sampleCount() != 0 || sampler.samplingPoints() != 0 ||
            sampler.droppedSamples() != 0) {
            std::fprintf(stderr,
                         "persistent-matrix: count-only coverage mismatch: hook_calls=%llu/%llu (%.4f) "
                         "allocations=%llu/%llu (%.4f) bytes=%llu/%llu (%.4f)\n",
                         static_cast<unsigned long long>(result.counters.hook_calls),
                         static_cast<unsigned long long>(result.workload.allocator_calls),
                         coverageRatio(result.counters.hook_calls, result.workload.allocator_calls),
                         static_cast<unsigned long long>(result.counters.successful_allocations),
                         static_cast<unsigned long long>(result.workload.successful_allocations),
                         coverageRatio(result.counters.successful_allocations, result.workload.successful_allocations),
                         static_cast<unsigned long long>(result.counters.observed_bytes),
                         static_cast<unsigned long long>(result.workload.requested_bytes),
                         coverageRatio(result.counters.observed_bytes, result.workload.requested_bytes));
            return false;
        }
    }
    return true;
}

double callsPerSecond(std::uint64_t calls, double elapsed_ns)
{
    return static_cast<double>(calls) * 1'000'000'000.0 / elapsed_ns;
}

double bytesPerSecond(std::uint64_t bytes, double elapsed_ns)
{
    return static_cast<double>(bytes) * 1'000'000'000.0 / elapsed_ns;
}

bool hasCaptureLoss(const TrialResult &result)
{
    bool capture_loss = result.counters.successful_allocations < result.workload.successful_allocations ||
                        result.counters.observed_bytes < result.workload.requested_bytes;
#ifdef __linux__
    capture_loss = capture_loss || result.counters.hook_calls < result.workload.allocator_calls;
#endif
    return capture_loss;
}

void printMatrixPair(const WorkloadSpec &spec, std::size_t threads, std::size_t pair, const char *first_mode,
                     std::size_t operations, const TrialResult &off, const TrialResult &on)
{
    const double off_ns_per_call = off.elapsed_ns / static_cast<double>(off.workload.allocator_calls);
    const double on_ns_per_call = on.elapsed_ns / static_cast<double>(on.workload.allocator_calls);
    const double off_cpu_ns_per_call =
        off.cpu_supported ? static_cast<double>(off.cpu_elapsed_ns) / static_cast<double>(off.workload.allocator_calls)
                          : 0.0;
    const double on_cpu_ns_per_call =
        on.cpu_supported ? static_cast<double>(on.cpu_elapsed_ns) / static_cast<double>(on.workload.allocator_calls)
                         : 0.0;
    const double off_calls_per_second = callsPerSecond(off.workload.allocator_calls, off.elapsed_ns);
    const double on_calls_per_second = callsPerSecond(on.workload.allocator_calls, on.elapsed_ns);
    const double off_bytes_per_second = bytesPerSecond(off.workload.requested_bytes, off.elapsed_ns);
    const double on_bytes_per_second = bytesPerSecond(on.workload.requested_bytes, on.elapsed_ns);
    const double throughput_change = (on_calls_per_second / off_calls_per_second - 1.0) * 100.0;
    std::printf("trial,%s,%zu,%zu,%s,%zu,%llu,%llu,%.0f,%.2f,%llu,%.2f,%d,%.2f,%.2f,"
                "%.0f,%.2f,%llu,%.2f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,"
                "%llu,%llu,%.6f,%llu,%llu,%.6f,%llu,%llu,%.6f,%d\n",
                spec.name, threads, pair, first_mode, operations,
                static_cast<unsigned long long>(off.workload.allocator_calls),
                static_cast<unsigned long long>(off.workload.requested_bytes), off.elapsed_ns, off_ns_per_call,
                static_cast<unsigned long long>(off.cpu_elapsed_ns), off_cpu_ns_per_call, off.cpu_supported ? 1 : 0,
                off_calls_per_second, off_bytes_per_second, on.elapsed_ns, on_ns_per_call,
                static_cast<unsigned long long>(on.cpu_elapsed_ns), on_cpu_ns_per_call, on.cpu_supported ? 1 : 0,
                on_calls_per_second, on_bytes_per_second, on_ns_per_call - off_ns_per_call,
                on_cpu_ns_per_call - off_cpu_ns_per_call, throughput_change,
                static_cast<unsigned long long>(on.workload.allocator_calls),
                static_cast<unsigned long long>(on.counters.hook_calls),
                coverageRatio(on.counters.hook_calls, on.workload.allocator_calls),
                static_cast<unsigned long long>(on.workload.successful_allocations),
                static_cast<unsigned long long>(on.counters.successful_allocations),
                coverageRatio(on.counters.successful_allocations, on.workload.successful_allocations),
                static_cast<unsigned long long>(on.workload.requested_bytes),
                static_cast<unsigned long long>(on.counters.observed_bytes),
                coverageRatio(on.counters.observed_bytes, on.workload.requested_bytes), hasCaptureLoss(on) ? 1 : 0);
}

bool runPersistentMatrix(std::size_t operations, std::size_t repeats)
{
    spark::AllocationSampler sampler;
    spark::AllocationSamplerConfig config;
    config.session_seed = spark::currentNativeThreadId();
    config.count_only = true;
    std::string error;
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "persistent-matrix: initial count-only start failed: %s\n", error.c_str());
        return false;
    }
    if (!sampler.stop(error)) {
        std::fprintf(stderr, "persistent-matrix: initial count-only stop failed: %s\n", error.c_str());
        return false;
    }
    printBenchmarkMetadata(sampler);

    std::printf("row,workload,threads,pair,first_mode,operations_per_thread,allocator_calls,requested_bytes,"
                "off_elapsed_ns,off_ns_per_call,off_cpu_elapsed_ns,off_cpu_ns_per_call,off_cpu_supported,"
                "off_calls_per_sec,off_requested_bytes_per_sec,on_elapsed_ns,on_ns_per_call,on_cpu_elapsed_ns,"
                "on_cpu_ns_per_call,on_cpu_supported,on_calls_per_sec,on_requested_bytes_per_sec,"
                "on_minus_off_ns_per_call,on_minus_off_cpu_ns_per_call,throughput_change_percent,"
                "on_expected_hook_calls,on_hook_calls,on_hook_call_coverage,on_expected_allocations,"
                "on_successful_allocations,on_allocation_coverage,on_expected_bytes,on_observed_bytes,"
                "on_byte_coverage,on_capture_loss\n");
    constexpr std::size_t k_thread_counts[] = {1, 4};
    for (const WorkloadSpec &spec : KMatrixWorkloads) {
        for (const std::size_t threads : k_thread_counts) {
            TrialResult warmup_off;
            TrialResult warmup_on;
            if (!runMatrixTrial(sampler, spec.workload, threads, operations, false, warmup_off) ||
                !runMatrixTrial(sampler, spec.workload, threads, operations, true, warmup_on)) {
                std::string ignored;
                sampler.shutdown(ignored);
                return false;
            }
            for (std::size_t pair = 1; pair <= repeats; ++pair) {
                TrialResult off;
                TrialResult on;
                const bool off_first = (pair % 2) == 1;
                if (off_first) {
                    if (!runMatrixTrial(sampler, spec.workload, threads, operations, false, off) ||
                        !runMatrixTrial(sampler, spec.workload, threads, operations, true, on)) {
                        std::string ignored;
                        sampler.shutdown(ignored);
                        return false;
                    }
                }
                else if (!runMatrixTrial(sampler, spec.workload, threads, operations, true, on) ||
                         !runMatrixTrial(sampler, spec.workload, threads, operations, false, off)) {
                    std::string ignored;
                    sampler.shutdown(ignored);
                    return false;
                }
                printMatrixPair(spec, threads, pair, off_first ? "off" : "on", operations, off, on);
            }
        }
    }
    if (!sampler.shutdown(error)) {
        std::fprintf(stderr, "persistent-matrix: shutdown failed: %s\n", error.c_str());
        return false;
    }
    return true;
}

bool measurePristineWorkload(MatrixWorkload workload, std::size_t threads, std::size_t operations, TrialResult &result)
{
    std::vector<WorkloadResult> worker_results(threads);
    if (threads == 1) {
        const ProcessCpuSnapshot cpu_before = processCpuSnapshot();
        const auto start = Clock::now();
        worker_results[0] = runWorkload(workload, operations);
        const auto end = Clock::now();
        const ProcessCpuSnapshot cpu_after = processCpuSnapshot();
        result.elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();
        setCpuElapsed(result, cpu_before, cpu_after);
    }
    else {
        std::latch ready(static_cast<std::ptrdiff_t>(threads));
        std::latch begin(1);
        std::latch finished(static_cast<std::ptrdiff_t>(threads));
        std::latch finish_release(1);
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers.emplace_back([&, i] {
                static_cast<void>(runWorkload(workload, 2));
                ready.count_down();
                begin.wait();
                worker_results[i] = runWorkload(workload, operations);
                finished.count_down();
                finish_release.wait();
            });
        }
        ready.wait();
        const ProcessCpuSnapshot cpu_before = processCpuSnapshot();
        const auto start = Clock::now();
        begin.count_down();
        finished.wait();
        const auto end = Clock::now();
        const ProcessCpuSnapshot cpu_after = processCpuSnapshot();
        finish_release.count_down();
        for (std::thread &worker : workers) {
            worker.join();
        }
        result.elapsed_ns = std::chrono::duration<double, std::nano>(end - start).count();
        setCpuElapsed(result, cpu_before, cpu_after);
    }

    for (const WorkloadResult &worker_result : worker_results) {
        addWorkloadResult(result.workload, worker_result);
    }
    const std::uint64_t expected_calls = static_cast<std::uint64_t>(threads) * operations;
    const std::uint64_t expected_bytes = expectedRequestedBytes(workload, operations) * threads;
    if (!result.workload.succeeded || result.workload.allocator_calls != expected_calls ||
        result.workload.requested_bytes != expected_bytes) {
        std::fprintf(stderr, "pristine-off: workload mismatch: calls=%llu/%llu bytes=%llu/%llu\n",
                     static_cast<unsigned long long>(result.workload.allocator_calls),
                     static_cast<unsigned long long>(expected_calls),
                     static_cast<unsigned long long>(result.workload.requested_bytes),
                     static_cast<unsigned long long>(expected_bytes));
        return false;
    }
#ifdef __linux__
    if (!result.cpu_supported) {
        std::fprintf(stderr, "pristine-off: CLOCK_PROCESS_CPUTIME_ID unavailable\n");
        return false;
    }
#endif
    return true;
}

void printPristineTrial(const WorkloadSpec &spec, std::size_t threads, std::size_t trial, std::size_t operations,
                        const TrialResult &result)
{
    const double ns_per_call = result.elapsed_ns / static_cast<double>(result.workload.allocator_calls);
    const double cpu_ns_per_call = result.cpu_supported ? static_cast<double>(result.cpu_elapsed_ns) /
                                                              static_cast<double>(result.workload.allocator_calls)
                                                        : 0.0;
    std::printf("trial,pristine-off,%s,%zu,%zu,%zu,%llu,%llu,%.0f,%.2f,%llu,%.2f,%d,%.2f,%.2f\n", spec.name, threads,
                trial, operations, static_cast<unsigned long long>(result.workload.allocator_calls),
                static_cast<unsigned long long>(result.workload.requested_bytes), result.elapsed_ns, ns_per_call,
                static_cast<unsigned long long>(result.cpu_elapsed_ns), cpu_ns_per_call, result.cpu_supported ? 1 : 0,
                callsPerSecond(result.workload.allocator_calls, result.elapsed_ns),
                bytesPerSecond(result.workload.requested_bytes, result.elapsed_ns));
}

bool runPristineOffMatrix(std::size_t operations, std::size_t repeats)
{
    std::printf("row,mode,workload,threads,trial,operations_per_thread,allocator_calls,requested_bytes,elapsed_ns,"
                "ns_per_call,cpu_elapsed_ns,cpu_ns_per_call,cpu_supported,calls_per_sec,requested_bytes_per_sec\n");
    constexpr std::size_t k_thread_counts[] = {1, 4};
    for (const WorkloadSpec &spec : KMatrixWorkloads) {
        for (const std::size_t threads : k_thread_counts) {
            TrialResult warmup;
            if (!measurePristineWorkload(spec.workload, threads, operations, warmup)) {
                return false;
            }
            for (std::size_t trial = 1; trial <= repeats; ++trial) {
                TrialResult result;
                if (!measurePristineWorkload(spec.workload, threads, operations, result)) {
                    return false;
                }
                printPristineTrial(spec, threads, trial, operations, result);
            }
        }
    }
    return true;
}

bool parsePositive(std::string_view text, std::size_t minimum, std::size_t maximum, std::size_t &value)
{
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size() || parsed < minimum || parsed > maximum) {
        return false;
    }
    value = static_cast<std::size_t>(parsed);
    return true;
}

const WorkloadSpec *findPerfWorkload(std::string_view name)
{
    for (const WorkloadSpec &spec : KMatrixWorkloads) {
        const char *short_name = nullptr;
        switch (spec.workload) {
        case MatrixWorkload::Tiny:
            short_name = "tiny";
            break;
        case MatrixWorkload::Small:
            short_name = "small";
            break;
        case MatrixWorkload::Medium:
            short_name = "medium";
            break;
        case MatrixWorkload::Mixed:
            short_name = "mixed";
            break;
        case MatrixWorkload::Calloc:
            short_name = "calloc";
            break;
        case MatrixWorkload::ReallocHeavy:
            short_name = "realloc";
            break;
        case MatrixWorkload::Aligned:
            short_name = "aligned";
            break;
        }
        if (name == short_name) {
            return &spec;
        }
    }
    return nullptr;
}

void printPerfHeader()
{
    std::printf(
        "row,mode,workload,threads,operations_per_thread,allocator_calls,requested_bytes,elapsed_ns,ns_per_call,"
        "cpu_elapsed_ns,cpu_ns_per_call,cpu_supported,calls_per_sec,requested_bytes_per_sec,"
        "expected_hook_calls,observed_hook_calls,hook_call_coverage,expected_allocations,observed_allocations,"
        "allocation_coverage,expected_bytes,observed_bytes,byte_coverage,capture_loss\n");
    std::fflush(stdout);
}

void printPerfTrial(const char *mode, const WorkloadSpec &spec, std::size_t threads, std::size_t operations,
                    const TrialResult &result, bool capture_supported)
{
    const double ns_per_call = result.elapsed_ns / static_cast<double>(result.workload.allocator_calls);
    const double cpu_ns_per_call = result.cpu_supported ? static_cast<double>(result.cpu_elapsed_ns) /
                                                              static_cast<double>(result.workload.allocator_calls)
                                                        : 0.0;
    if (!capture_supported) {
        std::printf("trial,%s,%s,%zu,%zu,%llu,%llu,%.0f,%.2f,%llu,%.2f,%d,%.2f,%.2f,"
                    "NA,NA,NA,NA,NA,NA,NA,NA,NA,not-applicable\n",
                    mode, spec.name, threads, operations,
                    static_cast<unsigned long long>(result.workload.allocator_calls),
                    static_cast<unsigned long long>(result.workload.requested_bytes), result.elapsed_ns, ns_per_call,
                    static_cast<unsigned long long>(result.cpu_elapsed_ns), cpu_ns_per_call,
                    result.cpu_supported ? 1 : 0, callsPerSecond(result.workload.allocator_calls, result.elapsed_ns),
                    bytesPerSecond(result.workload.requested_bytes, result.elapsed_ns));
        return;
    }

    std::printf("trial,%s,%s,%zu,%zu,%llu,%llu,%.0f,%.2f,%llu,%.2f,%d,%.2f,%.2f,%llu,%llu,%.6f,%llu,%llu,"
                "%.6f,%llu,%llu,%.6f,%d\n",
                mode, spec.name, threads, operations, static_cast<unsigned long long>(result.workload.allocator_calls),
                static_cast<unsigned long long>(result.workload.requested_bytes), result.elapsed_ns, ns_per_call,
                static_cast<unsigned long long>(result.cpu_elapsed_ns), cpu_ns_per_call, result.cpu_supported ? 1 : 0,
                callsPerSecond(result.workload.allocator_calls, result.elapsed_ns),
                bytesPerSecond(result.workload.requested_bytes, result.elapsed_ns),
                static_cast<unsigned long long>(result.workload.allocator_calls),
                static_cast<unsigned long long>(result.counters.hook_calls),
                coverageRatio(result.counters.hook_calls, result.workload.allocator_calls),
                static_cast<unsigned long long>(result.workload.successful_allocations),
                static_cast<unsigned long long>(result.counters.successful_allocations),
                coverageRatio(result.counters.successful_allocations, result.workload.successful_allocations),
                static_cast<unsigned long long>(result.workload.requested_bytes),
                static_cast<unsigned long long>(result.counters.observed_bytes),
                coverageRatio(result.counters.observed_bytes, result.workload.requested_bytes),
                hasCaptureLoss(result) ? 1 : 0);
}

bool runPersistentPerf(const char *mode, const WorkloadSpec &spec, std::size_t threads, std::size_t operations)
{
    printPerfHeader();
    TrialResult result;
    if (std::string_view(mode) == "on") {
        spark::AllocationSampler sampler;
        if (!runMatrixTrial(sampler, spec.workload, threads, operations, true, result)) {
            std::string ignored;
            sampler.shutdown(ignored);
            return false;
        }
        printBenchmarkMetadata(sampler);
        std::string error;
        if (!sampler.shutdown(error)) {
            std::fprintf(stderr, "persistent-perf: shutdown failed: %s\n", error.c_str());
            return false;
        }
        printPerfTrial("on", spec, threads, operations, result, true);
        return true;
    }

    if (!measurePristineWorkload(spec.workload, threads, operations, result)) {
        return false;
    }
    printPerfTrial("pristine", spec, threads, operations, result, false);
    return true;
}

void allocationWork(std::size_t operations)
{
    for (std::size_t i = 0; i < operations; ++i) {
        void *pointer = std::malloc(64 + (i & 63));
        if (pointer != nullptr) {
            static_cast<volatile unsigned char *>(pointer)[0] = static_cast<unsigned char>(i);
            std::free(pointer);
        }
    }
}

double measure(std::size_t threads, std::size_t operations_per_thread)
{
    const auto start = Clock::now();
    if (threads == 1) {
        allocationWork(operations_per_thread);
    }
    else {
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers.emplace_back(allocationWork, operations_per_thread);
        }
        for (std::thread &worker : workers) {
            worker.join();
        }
    }
    const auto elapsed = Clock::now() - start;
    return std::chrono::duration<double, std::nano>(elapsed).count();
}

double runTrials(std::size_t threads, std::size_t operations_per_thread)
{
    std::vector<double> trials;
    trials.reserve(5);
    for (int trial = 0; trial < 5; ++trial) {
        trials.push_back(measure(threads, operations_per_thread));
    }
    std::ranges::sort(trials);
    return trials[trials.size() / 2];
}

void printResult(const char *name, std::size_t threads, std::int32_t interval, bool live_only, bool count_only,
                 std::size_t operations_per_thread, double elapsed_ns, std::uint64_t samples, std::uint64_t dropped,
                 std::uint64_t observed_bytes)
{
    const auto operations = static_cast<double>(threads * operations_per_thread);
    std::printf("%s,%zu,%d,%d,%d,%zu,%.0f,%.2f,%llu,%llu,%llu\n", name, threads, interval, live_only ? 1 : 0,
                count_only ? 1 : 0, threads * operations_per_thread, elapsed_ns, elapsed_ns / operations,
                static_cast<unsigned long long>(samples), static_cast<unsigned long long>(dropped),
                static_cast<unsigned long long>(observed_bytes));
}

bool runProfiledCase(spark::AllocationSampler &sampler, const char *name, std::size_t threads, std::int32_t interval,
                     bool live_only, bool count_only, std::size_t operations_per_thread, bool saturated)
{
    spark::AllocationSamplerConfig config;
    config.interval_bytes = interval;
    config.session_seed = spark::currentNativeThreadId();
    config.live_only = live_only;
    config.count_only = count_only;
#ifdef SPARK_ALLOCATION_BENCHMARK_CURRENT
    config.aggregator_delay_ms_for_testing = saturated ? 1000 : 0;
#else
    (void)saturated;
#endif

    std::string error;
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "%s: start failed: %s\n", name, error.c_str());
        return false;
    }
    const double elapsed = runTrials(threads, operations_per_thread);
    if (!sampler.stop(error)) {
        std::fprintf(stderr, "%s: stop failed: %s\n", name, error.c_str());
        return false;
    }
#ifdef SPARK_ALLOCATION_BENCHMARK_CURRENT
    const std::uint64_t dropped = sampler.droppedEvents();
#else
    const std::uint64_t dropped = sampler.droppedSamples();
#endif
    printResult(name, threads, interval, live_only, count_only, operations_per_thread, elapsed, sampler.sampleCount(),
                dropped, sampler.observedBytes());
    return true;
}

int runDefaultBenchmark()
{
    constexpr std::size_t k_operations = 200000;
    constexpr std::size_t k_pressure_operations = 16384;

    std::printf("case,threads,interval,live_only,count_only,operations_per_trial,median_ns,"
                "ns_per_op,samples_all_trials,dropped_all_trials,observed_bytes\n");
    printResult("unprofiled", 1, 0, false, false, k_operations, runTrials(1, k_operations), 0, 0, 0);
    printResult("unprofiled", 4, 0, false, false, k_operations, runTrials(4, k_operations), 0, 0, 0);

    spark::AllocationSampler sampler;
    if (!runProfiledCase(sampler, "count-only", 1, spark::kDefaultAllocationIntervalBytes, false, true, k_operations,
                         false) ||
        !runProfiledCase(sampler, "count-only", 4, spark::kDefaultAllocationIntervalBytes, false, true, k_operations,
                         false)) {
        std::string ignored;
        sampler.shutdown(ignored);
        return 1;
    }
    printResult("disabled-hooks", 1, 0, false, false, k_operations, runTrials(1, k_operations), 0, 0, 0);
    printResult("disabled-hooks", 4, 0, false, false, k_operations, runTrials(4, k_operations), 0, 0, 0);

    if (!runProfiledCase(sampler, "normal-default", 1, spark::kDefaultAllocationIntervalBytes, false, false,
                         k_operations, false) ||
        !runProfiledCase(sampler, "normal-default", 4, spark::kDefaultAllocationIntervalBytes, false, false,
                         k_operations, false) ||
        !runProfiledCase(sampler, "normal-4k", 1, 4096, false, false, k_operations, false) ||
        !runProfiledCase(sampler, "normal-4k", 4, 4096, false, false, k_operations, false) ||
        !runProfiledCase(sampler, "live-4k", 1, 4096, true, false, k_operations, false) ||
        !runProfiledCase(sampler, "saturated", 4, 1, false, false, k_pressure_operations, true)) {
        std::string ignored;
        sampler.shutdown(ignored);
        return 1;
    }

    std::string error;
    if (!sampler.shutdown(error)) {
        std::fprintf(stderr, "shutdown failed: %s\n", error.c_str());
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 1) {
        return runDefaultBenchmark();
    }

    const std::string_view mode(argv[1]);
    const bool persistent = mode == "--persistent-matrix";
    const bool pristine = mode == "--persistent-pristine-off";
    const bool perf = mode == "--persistent-perf";
    if (!persistent && !pristine && !perf) {
        std::fprintf(stderr, "usage: spark_allocation_benchmark [--persistent-matrix|--persistent-pristine-off "
                             "[--operations=N] [--repeats=N]] | --persistent-perf --mode=on|pristine "
                             "--workload=tiny|small|medium|mixed|calloc|realloc|aligned --threads=1|4 "
                             "--operations=N\n");
        return 2;
    }
    if (perf) {
        std::string_view perf_mode;
        std::string_view workload_name;
        std::size_t threads = 0;
        std::size_t operations = 0;
        bool have_mode = false;
        bool have_workload = false;
        bool have_threads = false;
        bool have_operations = false;
        for (int i = 2; i < argc; ++i) {
            const std::string_view argument(argv[i]);
            if (argument.starts_with("--mode=")) {
                perf_mode = argument.substr(7);
                have_mode = true;
            }
            else if (argument.starts_with("--workload=")) {
                workload_name = argument.substr(11);
                have_workload = true;
            }
            else if (argument.starts_with("--threads=")) {
                have_threads = parsePositive(argument.substr(10), 1, 4, threads) && (threads == 1 || threads == 4);
                if (!have_threads) {
                    std::fprintf(stderr, "persistent-perf: --threads must be 1 or 4\n");
                    return 2;
                }
            }
            else if (argument.starts_with("--operations=")) {
                have_operations = parsePositive(argument.substr(13), 2, 100000000, operations) && operations % 2 == 0;
                if (!have_operations) {
                    std::fprintf(stderr,
                                 "persistent-perf: --operations must be an even number between 2 and 100000000\n");
                    return 2;
                }
            }
            else {
                std::fprintf(stderr, "persistent-perf: unknown option: %s\n", argv[i]);
                return 2;
            }
        }
        if (!have_mode || !have_workload || !have_threads || !have_operations ||
            (perf_mode != "on" && perf_mode != "pristine")) {
            std::fprintf(stderr, "persistent-perf: mode, workload, threads, and operations are required\n");
            return 2;
        }
        const WorkloadSpec *spec = findPerfWorkload(workload_name);
        if (spec == nullptr) {
            std::fprintf(stderr, "persistent-perf: unknown workload: %.*s\n", static_cast<int>(workload_name.size()),
                         workload_name.data());
            return 2;
        }
        return runPersistentPerf(perf_mode == "on" ? "on" : "pristine", *spec, threads, operations) ? 0 : 1;
    }

    std::size_t operations = 20000;
    std::size_t repeats = 15;
    for (int i = 2; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument.starts_with("--operations=")) {
            if (!parsePositive(argument.substr(13), 2, 100000000, operations) || operations % 2 != 0) {
                std::fprintf(stderr,
                             "persistent-matrix: --operations must be an even number between 2 and 100000000\n");
                return 2;
            }
        }
        else if (argument.starts_with("--repeats=")) {
            if (!parsePositive(argument.substr(10), 15, 100000, repeats)) {
                std::fprintf(stderr, "persistent-matrix: --repeats must be at least 15\n");
                return 2;
            }
        }
        else {
            std::fprintf(stderr, "persistent-matrix: unknown option: %s\n", argv[i]);
            return 2;
        }
    }
    const bool succeeded =
        persistent ? runPersistentMatrix(operations, repeats) : runPristineOffMatrix(operations, repeats);
    return succeeded ? 0 : 1;
}
