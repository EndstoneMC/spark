#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "selftest_allocation_internal.h"

namespace spark::selftest {

#if defined(_WIN32) || defined(__linux__)
namespace {

bool verifyAllocationLiveOnlyContentionRefusal()
{
    spark::AllocationSamplerConfig config;
    config.interval_bytes = 1;
    config.session_seed = spark::currentNativeThreadId();
    config.live_only = true;
    config.force_live_lock_contention_for_testing = true;

    spark::AllocationSampler sampler;
    std::string error;
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "allocation live contention: start failed: %s\n", error.c_str());
        return false;
    }

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(8);
    for (int thread = 0; thread < 8; ++thread) {
        workers.emplace_back([&start, thread]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 1000; ++i) {
                void *pointer = std::malloc(128U + static_cast<std::size_t>(thread) + static_cast<std::size_t>(i & 63));
                if (pointer == nullptr) {
                    continue;
                }
                void *replacement = std::realloc(pointer, 256U + static_cast<std::size_t>(i & 127));
                std::free(replacement != nullptr ? replacement : pointer);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto &worker : workers) {
        worker.join();
    }
    sampler.onTick(50.0);
    const bool stopped = sampler.stop(error);
    const bool refusal =
        !stopped && error == "allocation lifecycle tracking lost records; retained profile discarded" &&
        sampler.contentionDropped() != 0 && sampler.lifecycleDropped() != 0 && sampler.dataIncomplete();
    std::string shutdown_error;
    const bool shutdown = sampler.shutdown(shutdown_error);
    if (!refusal || !shutdown) {
        std::fprintf(stderr,
                     "allocation live contention: refusal failed "
                     "(stopped=%d contention=%llu lifecycle=%llu incomplete=%d error=%s shutdown=%s)\n",
                     static_cast<int>(stopped), static_cast<unsigned long long>(sampler.contentionDropped()),
                     static_cast<unsigned long long>(sampler.lifecycleDropped()),
                     static_cast<int>(sampler.dataIncomplete()), error.c_str(), shutdown_error.c_str());
        return false;
    }
    return true;
}

}  // namespace

bool verifyAllocationContentionPolicy()
{
    spark::AllocationSamplerConfig config;
    config.interval_bytes = 1;
    config.session_seed = spark::currentNativeThreadId();
    config.force_live_lock_contention_for_testing = true;

    spark::AllocationSampler sampler;
    std::string error;
    if (!sampler.start(config, error)) {
        std::fprintf(stderr, "allocation contention: start failed: %s\n", error.c_str());
        return false;
    }

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(8);
    for (int thread = 0; thread < 8; ++thread) {
        workers.emplace_back([&start, thread]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 1000; ++i) {
                void *pointer = std::malloc(128U + static_cast<std::size_t>(thread) + static_cast<std::size_t>(i & 63));
                if (pointer == nullptr) {
                    continue;
                }
                void *replacement = std::realloc(pointer, 256U + static_cast<std::size_t>(i & 127));
                std::free(replacement != nullptr ? replacement : pointer);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto &worker : workers) {
        worker.join();
    }
    sampler.onTick(50.0);
    if (!sampler.stop(error) || sampler.contentionDropped() == 0 || sampler.lifecycleDropped() == 0 ||
        !sampler.dataIncomplete()) {
        std::fprintf(stderr,
                     "allocation contention: bounded drop policy failed "
                     "(contention=%llu lifecycle=%llu incomplete=%d error=%s)\n",
                     static_cast<unsigned long long>(sampler.contentionDropped()),
                     static_cast<unsigned long long>(sampler.lifecycleDropped()),
                     static_cast<int>(sampler.dataIncomplete()), error.c_str());
        return false;
    }

    config.force_live_lock_contention_for_testing = false;
    if (!runAllocationSession(sampler, config, error) || !sampler.shutdown(error)) {
        return false;
    }
    return verifyAllocationLiveOnlyContentionRefusal();
}

bool verifyAllocationResourcePressure()
{
    spark::AllocationSamplerConfig config;
    config.interval_bytes = 1;
    config.session_seed = spark::currentNativeThreadId();
    config.hold_aggregator_until_event_drop_for_testing = true;

    std::string error;
    spark::AllocationSampler queue_sampler;
    if (!queue_sampler.start(config, error)) {
        std::fprintf(stderr, "allocation pressure: queue start failed: %s\n", error.c_str());
        return false;
    }
    constexpr std::uint64_t extra_allocation_events = 1024;
    for (std::uint64_t i = 0; i < spark::AllocationSampler::eventQueueCapacity() + extra_allocation_events; ++i) {
        void *pointer = std::malloc(64);
        if (pointer != nullptr) {
            static_cast<volatile unsigned char *>(pointer)[0] = static_cast<unsigned char>(i);
            std::free(pointer);
        }
    }
    if (!queue_sampler.stop(error) ||
        queue_sampler.eventQueueHighWaterMark() != spark::AllocationSampler::eventQueueCapacity() ||
        queue_sampler.droppedEvents() == 0 || !queue_sampler.dataIncomplete() || !queue_sampler.shutdown(error)) {
        std::fprintf(stderr,
                     "allocation pressure: queue did not saturate safely "
                     "(high-water=%llu capacity=%llu dropped=%llu error=%s)\n",
                     static_cast<unsigned long long>(queue_sampler.eventQueueHighWaterMark()),
                     static_cast<unsigned long long>(spark::AllocationSampler::eventQueueCapacity()),
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
    for (std::uint64_t i = 0; i < spark::AllocationSampler::tickEventCapacity() + extra_tick_events; ++i) {
        tick_sampler.onTick(2.0);
    }
    if (!tick_sampler.stop(error) || tick_sampler.droppedTickEvents() != extra_tick_events ||
        !tick_sampler.dataIncomplete() || !tick_sampler.shutdown(error)) {
        std::fprintf(stderr,
                     "allocation pressure: tick queue did not saturate at its declared "
                     "capacity (capacity=%llu dropped=%llu error=%s)\n",
                     static_cast<unsigned long long>(spark::AllocationSampler::tickEventCapacity()),
                     static_cast<unsigned long long>(tick_sampler.droppedTickEvents()), error.c_str());
        return false;
    }

    config.hold_aggregator_until_event_drop_for_testing = false;
    config.only_ticks_over_ms = 0;
    config.thread_state_limit_for_testing = 8;
    spark::AllocationSampler registry_sampler;
    if (!registry_sampler.start(config, error)) {
        std::fprintf(stderr, "allocation pressure: registry start failed: %s\n", error.c_str());
        return false;
    }
    std::atomic<int> registry_ready{0};
    std::atomic<bool> release_registry_threads{false};
    std::vector<std::thread> workers;
    workers.reserve(16);
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
                     static_cast<int>(registry_sampler.dataIncomplete()), error.c_str());
        return false;
    }

    config.live_only = true;
    config.thread_state_limit_for_testing = 0;
    spark::AllocationSampler live_sampler;
    std::vector<void *> retained;
    retained.reserve(static_cast<std::size_t>(spark::AllocationSampler::liveIndexCapacity() + 1024));
    if (!live_sampler.start(config, error)) {
        std::fprintf(stderr, "allocation pressure: live start failed: %s\n", error.c_str());
        return false;
    }
    for (std::uint64_t i = 0; i < spark::AllocationSampler::liveIndexCapacity() + 1024; ++i) {
        void *pointer = std::malloc(1);
        if (pointer != nullptr) {
            static_cast<volatile unsigned char *>(pointer)[0] = 1;
            retained.push_back(pointer);
        }
    }
    const bool stopped = live_sampler.stop(error);
    const bool bounded = !stopped &&
                         error == "allocation lifecycle tracking lost records; retained profile discarded" &&
                         live_sampler.lifecycleDropped() != 0 &&
                         live_sampler.peakLiveSamples() <= spark::AllocationSampler::liveIndexCapacity() &&
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
                     static_cast<int>(stopped), static_cast<unsigned long long>(live_sampler.peakLiveSamples()),
                     static_cast<unsigned long long>(spark::AllocationSampler::liveIndexCapacity()),
                     static_cast<unsigned long long>(live_sampler.lifecycleDropped()), error.c_str(),
                     shutdown_error.c_str());
        return false;
    }
    return true;
}
#endif

}  // namespace spark::selftest
