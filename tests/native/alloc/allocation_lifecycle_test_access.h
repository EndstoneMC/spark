#ifndef ENDSTONE_SPARK_ALLOCATION_LIFECYCLE_TEST_ACCESS_H
#define ENDSTONE_SPARK_ALLOCATION_LIFECYCLE_TEST_ACCESS_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "native/alloc/allocation_sampler.h"

namespace spark::test {

struct TrackingGate {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> exited{false};
};

struct StartFailureGate {
    std::atomic<bool> before_thread_creation{false};
    std::atomic<bool> fail_now{false};
};

#if defined(__linux__)
struct LinuxAllocationTestControl {
    std::atomic<unsigned> start_failure{0};
    std::atomic<bool> key_delete_failure{false};
    bool (*before_admission)(void *) noexcept = nullptr;
    void *before_admission_context = nullptr;
    void (*aggregator_entry)() noexcept = nullptr;
    void (*before_event)() noexcept = nullptr;
    void (*before_final_record)() noexcept = nullptr;
    void (*snapshot_admitted)() noexcept = nullptr;
    void (*snapshot_before_aggregate)() noexcept = nullptr;
    void (*snapshot_before_restore)() noexcept = nullptr;
    std::atomic<bool> snapshot_restored{false};
    std::atomic<void (*)() noexcept> before_hook{nullptr};
    std::atomic<void (*)() noexcept> before_tls{nullptr};
    bool (*scan_module)(std::string_view) noexcept = nullptr;
};

struct HotShardLineForTesting {
    std::uint64_t hooks = 0;
    std::uint64_t successful = 0;
    std::uint64_t bytes = 0;
    std::uint64_t owner = 0;
};
#endif

struct AllocationLifecycleTestAccess {
    static bool holdTrackingCall(AllocationSampler &, TrackingGate &) noexcept;
    static void armThreadCreationFailure(AllocationSampler &, StartFailureGate &) noexcept;
    static void disarmThreadCreationFailure(AllocationSampler &) noexcept;
#if defined(__linux__)
    static bool configureLinux(AllocationSampler &, LinuxAllocationTestControl *) noexcept;
    static void requestLinuxRescan(AllocationSampler &) noexcept;
    static std::uint32_t linuxGroup(const AllocationSampler &) noexcept;
    static bool linuxKeyCreated(const AllocationSampler &) noexcept;
    static bool linuxRescanActive(const AllocationSampler &) noexcept;
    static std::uint64_t shardIndexForThreadPointer(std::uint64_t thread_pointer) noexcept;
    static std::size_t spillShardIndex() noexcept;
    static bool occupyShardOwnerForTesting(AllocationSampler &sampler, std::size_t index, std::uint64_t owner) noexcept;
    static HotShardLineForTesting hotShardLine(const AllocationSampler &sampler, std::size_t index) noexcept;
    static std::size_t hotShardCount() noexcept;
#endif
};

}  // namespace spark::test

#endif  // ENDSTONE_SPARK_ALLOCATION_LIFECYCLE_TEST_ACCESS_H
