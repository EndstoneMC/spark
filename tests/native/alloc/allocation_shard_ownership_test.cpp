#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <string>
#include <vector>

#include <sys/mman.h>

#include "native/alloc/allocation_lifecycle_test_access.h"
#include "native/alloc/allocation_sampler.h"
#include "native/sampler/thread_info.h"

namespace {

// Oracle for candidate 3b. Distinct live thread pointers can hash to one claim index; this exercises
// occupied lines and spill contention while recording identities and claim-index collisions.
// Every allocation touches its memory: an untouched malloc/free pair is eliminated by the
// optimiser and would be counted as zero calls.

constexpr std::size_t KBytes = 512;
constexpr std::size_t KOps = 2000;
constexpr unsigned KThreads = 8;
constexpr std::uint64_t KDiagnosticForeignBase = 0x6c6c000000000001ULL;
constexpr std::uint64_t KFakeIdentityLowMask = (std::uint64_t{1} << 39) - 1;
constexpr std::uint64_t KFakeIdentitySaltLimit = std::uint64_t{1} << 29;
constexpr std::uint64_t KFakeIdentityInverseMultiplier = 0xf1de83e19937733dULL;
constexpr std::uint64_t KFakeIdentityPrefix =
    (KDiagnosticForeignBase ^ (KDiagnosticForeignBase >> 17)) & ~KFakeIdentityLowMask;

struct SeedEntry {
    std::size_t index = 0;
    std::uint64_t owner = 0;
};

struct SeedPlan {
    spark::AllocationSampler *sampler = nullptr;
    std::vector<SeedEntry> entries;
    std::vector<std::uint64_t> live_identities;
    bool prevalidated = false;
    bool exercise_rejections = false;
    bool force_failure = false;
    bool require_restart_precondition = false;
    bool restart_precondition = false;
    bool callback_called = false;
    bool reset_verified = false;
    bool invalid_rejections_verified = false;
    bool plan_verified = false;
    bool seeds_verified = false;
    bool duplicate_rejection_verified = false;
    bool callback_succeeded = false;
    std::size_t lines_scanned = 0;
    std::size_t seeds_claimed = 0;
    std::uint64_t callback_thread_pointer = 0;
};

std::atomic<std::uint64_t> ReaderThreadPointer{0};

std::uint64_t thisThreadPointer();

bool beforeAdmission(void *raw) noexcept
{
    auto *plan = static_cast<SeedPlan *>(raw);
    if (plan == nullptr || plan->sampler == nullptr || plan->callback_called) {
        return false;
    }
    plan->callback_called = true;
    plan->callback_thread_pointer = thisThreadPointer();

    const std::size_t count = spark::test::AllocationLifecycleTestAccess::hotShardCount();
    const std::size_t spill = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    bool reset = count != 0 && spill < count;
    for (std::size_t index = 0; index < count; ++index) {
        const auto line = spark::test::AllocationLifecycleTestAccess::hotShardLine(*plan->sampler, index);
        plan->lines_scanned += 1;
        reset = reset && line.owner == 0 && line.hooks == 0 && line.successful == 0 && line.bytes == 0;
    }
    plan->reset_verified = reset && plan->lines_scanned == count;
    if (!plan->reset_verified || (plan->require_restart_precondition && !plan->restart_precondition) ||
        !plan->prevalidated || plan->entries.size() > spill || plan->callback_thread_pointer == 0) {
        return false;
    }

    if (plan->exercise_rejections) {
        const std::uint64_t token = KDiagnosticForeignBase;
        const bool zero_rejected =
            !spark::test::AllocationLifecycleTestAccess::occupyShardOwnerForTesting(*plan->sampler, 0, 0);
        const bool spill_rejected =
            !spark::test::AllocationLifecycleTestAccess::occupyShardOwnerForTesting(*plan->sampler, spill, token);
        const bool range_rejected = !spark::test::AllocationLifecycleTestAccess::occupyShardOwnerForTesting(
            *plan->sampler, count + 4096, token);
        plan->invalid_rejections_verified = zero_rejected && spill_rejected && range_rejected;
        if (!plan->invalid_rejections_verified || plan->entries.empty()) {
            return false;
        }
    }

    for (const SeedEntry &entry : plan->entries) {
        if (entry.index >= spill || entry.owner == 0 || entry.owner == plan->callback_thread_pointer ||
            spark::test::AllocationLifecycleTestAccess::shardIndexForThreadPointer(entry.owner) != entry.index) {
            return false;
        }
        for (std::uint64_t identity : plan->live_identities) {
            if (identity == 0 || identity == entry.owner) {
                return false;
            }
        }
    }
    for (std::size_t index = 0; index < plan->entries.size(); ++index) {
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (plan->entries[index].index == plan->entries[prior].index ||
                plan->entries[index].owner == plan->entries[prior].owner) {
                return false;
            }
        }
    }
    plan->plan_verified = true;

    for (std::size_t entry_index = 0; entry_index < plan->entries.size(); ++entry_index) {
        const SeedEntry &entry = plan->entries[entry_index];
        if (!spark::test::AllocationLifecycleTestAccess::occupyShardOwnerForTesting(*plan->sampler, entry.index,
                                                                                    entry.owner)) {
            return false;
        }
        const auto line = spark::test::AllocationLifecycleTestAccess::hotShardLine(*plan->sampler, entry.index);
        if (line.owner != entry.owner || line.hooks != 0 || line.successful != 0 || line.bytes != 0) {
            return false;
        }
        plan->seeds_claimed += 1;
        if (plan->exercise_rejections && entry_index == 0) {
            plan->duplicate_rejection_verified =
                !spark::test::AllocationLifecycleTestAccess::occupyShardOwnerForTesting(*plan->sampler, entry.index,
                                                                                        entry.owner);
            if (!plan->duplicate_rejection_verified) {
                return false;
            }
        }
    }
    plan->seeds_verified = plan->seeds_claimed == plan->entries.size();
    if (!plan->seeds_verified || plan->force_failure) {
        return false;
    }
    plan->callback_succeeded = true;
    return true;
}

std::uint64_t thisThreadPointer()
{
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(__builtin_thread_pointer()));
}

bool checkedFakeIdentityExclusionBound(std::size_t live_count, std::size_t previous_owner_count, std::uint64_t &bound)
{
    if (live_count > std::numeric_limits<std::size_t>::max() - previous_owner_count) {
        return false;
    }
    const std::size_t total = live_count + previous_owner_count;
    if (static_cast<std::uint64_t>(total) >= KFakeIdentitySaltLimit) {
        return false;
    }
    bound = static_cast<std::uint64_t>(total);
    return true;
}

bool constructFakeIdentityForIndex(std::size_t index, std::uint64_t salt, std::uint64_t &identity)
{
    const std::size_t spill = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    if (index >= spill || salt >= KFakeIdentitySaltLimit) {
        return false;
    }

    const std::uint64_t a = salt >> 19;
    const std::uint64_t b = salt & ((std::uint64_t{1} << 19) - 1);
    const std::uint64_t product_low39 = (a << 29) | (b << 10) | (static_cast<std::uint64_t>(index) ^ a);
    const std::uint64_t mixed =
        KFakeIdentityPrefix | ((product_low39 * KFakeIdentityInverseMultiplier) & KFakeIdentityLowMask);
    identity = mixed ^ (mixed >> 17) ^ (mixed >> 34) ^ (mixed >> 51);
    return identity != 0;
}

bool findFakeIdentityForIndex(std::size_t index, const std::vector<std::uint64_t> &live_identities,
                              const std::vector<std::uint64_t> &previous_owners, std::uint64_t &identity)
{
    const std::size_t spill = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    std::uint64_t exclusion_bound = 0;
    if (index >= spill ||
        !checkedFakeIdentityExclusionBound(live_identities.size(), previous_owners.size(), exclusion_bound)) {
        return false;
    }

    for (std::uint64_t salt = 0; salt <= exclusion_bound; ++salt) {
        std::uint64_t candidate = 0;
        if (!constructFakeIdentityForIndex(index, salt, candidate) ||
            spark::test::AllocationLifecycleTestAccess::shardIndexForThreadPointer(candidate) != index) {
            return false;
        }
        if (std::ranges::find(live_identities, candidate) == live_identities.end() &&
            std::ranges::find(previous_owners, candidate) == previous_owners.end()) {
            identity = candidate;
            return true;
        }
    }
    return false;
}

int checkFakeIdentityConstruction()
{
    const std::size_t spill = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    constexpr std::size_t edge_salt_count = 6;
    constexpr std::uint64_t edge_salts[edge_salt_count] = {
        0, 1, 31, (std::uint64_t{1} << 19) - 1, std::uint64_t{1} << 19, KFakeIdentitySaltLimit - 1};
    std::vector<std::uint64_t> primary_tokens;
    primary_tokens.reserve(spill);
    for (std::size_t index = 0; index < spill; ++index) {
        std::uint64_t primary = 0;
        if (!constructFakeIdentityForIndex(index, 0, primary) ||
            spark::test::AllocationLifecycleTestAccess::shardIndexForThreadPointer(primary) != index) {
            std::fprintf(stderr, "fake identity inverse failed for index %zu at salt zero\n", index);
            return 1;
        }
        primary_tokens.push_back(primary);
        std::uint64_t edge_tokens[edge_salt_count]{};
        for (std::size_t salt_index = 0; salt_index < edge_salt_count; ++salt_index) {
            const std::uint64_t salt = edge_salts[salt_index];
            std::uint64_t token = 0;
            if (!constructFakeIdentityForIndex(index, salt, token) ||
                spark::test::AllocationLifecycleTestAccess::shardIndexForThreadPointer(token) != index) {
                std::fprintf(stderr, "fake identity inverse failed for index %zu at salt %llu\n", index,
                             static_cast<unsigned long long>(salt));
                return 1;
            }
            const std::uint64_t mixed = token ^ (token >> 17);
            if ((mixed & ~KFakeIdentityLowMask) != KFakeIdentityPrefix) {
                std::fprintf(stderr, "fake identity inverse changed the fixed prefix at index %zu\n", index);
                return 1;
            }
            for (std::size_t prior = 0; prior < salt_index; ++prior) {
                if (edge_tokens[prior] == token) {
                    std::fprintf(stderr, "fake identity inverse reused a salt token at index %zu\n", index);
                    return 1;
                }
            }
            edge_tokens[salt_index] = token;
        }
    }
    std::ranges::sort(primary_tokens);
    if (std::ranges::adjacent_find(primary_tokens) != primary_tokens.end()) {
        std::fprintf(stderr, "fake identity inverse produced duplicate primary tokens\n");
        return 1;
    }

    std::uint64_t invalid_token = 0;
    std::uint64_t selected_token = 0;
    if (constructFakeIdentityForIndex(spill, 0, invalid_token) ||
        constructFakeIdentityForIndex(spill + 1, 0, invalid_token) ||
        constructFakeIdentityForIndex(0, KFakeIdentitySaltLimit, invalid_token) ||
        findFakeIdentityForIndex(spill, {}, {}, invalid_token)) {
        std::fprintf(stderr, "fake identity inverse accepted an invalid index or salt\n");
        return 1;
    }

    std::uint64_t bound = 0;
    const auto salt_limit = static_cast<std::size_t>(KFakeIdentitySaltLimit);
    if (!checkedFakeIdentityExclusionBound(salt_limit - 1, 0, bound) || bound != salt_limit - 1 ||
        checkedFakeIdentityExclusionBound(salt_limit, 0, bound) ||
        checkedFakeIdentityExclusionBound(std::numeric_limits<std::size_t>::max(), 1, bound)) {
        std::fprintf(stderr, "fake identity inverse exclusion bound validation failed\n");
        return 1;
    }

    constexpr std::size_t exclusion_index = 448;
    std::uint64_t salt_zero = 0;
    std::uint64_t salt_one = 0;
    std::uint64_t salt_two = 0;
    if (!constructFakeIdentityForIndex(exclusion_index, 0, salt_zero) ||
        !constructFakeIdentityForIndex(exclusion_index, 1, salt_one) ||
        !constructFakeIdentityForIndex(exclusion_index, 2, salt_two) ||
        !findFakeIdentityForIndex(exclusion_index, {salt_zero}, {salt_one}, selected_token) ||
        selected_token != salt_two || selected_token == salt_zero || selected_token == salt_one) {
        std::fprintf(stderr, "fake identity inverse did not skip known-live and previous-owner tokens\n");
        return 1;
    }

    std::fprintf(stderr, "fake identity inverse: %zu unique primary tokens and bounded exclusion checks passed\n",
                 primary_tokens.size());
    return 0;
}

void resetSeedPlan(SeedPlan &plan, spark::AllocationSampler &sampler, const std::vector<std::uint64_t> &live_identities)
{
    plan.sampler = &sampler;
    plan.entries.clear();
    plan.live_identities = live_identities;
    std::ranges::sort(plan.live_identities);
    const auto unique_live_identities = std::ranges::unique(plan.live_identities);
    plan.live_identities.erase(unique_live_identities.begin(), unique_live_identities.end());
    plan.prevalidated = true;
    plan.exercise_rejections = false;
    plan.force_failure = false;
    plan.require_restart_precondition = false;
    plan.restart_precondition = false;
    plan.callback_called = false;
    plan.reset_verified = false;
    plan.invalid_rejections_verified = false;
    plan.plan_verified = false;
    plan.seeds_verified = false;
    plan.duplicate_rejection_verified = false;
    plan.callback_succeeded = false;
    plan.lines_scanned = 0;
    plan.seeds_claimed = 0;
    plan.callback_thread_pointer = 0;
}

bool addSeedEntries(SeedPlan &plan, const std::vector<std::size_t> &indices)
{
    const std::size_t spill = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    std::vector<std::size_t> unique_indices = indices;
    std::ranges::sort(unique_indices);
    const auto unique_index_range = std::ranges::unique(unique_indices);
    unique_indices.erase(unique_index_range.begin(), unique_index_range.end());
    if (unique_indices.size() > spill) {
        return false;
    }

    std::vector<std::uint64_t> owners;
    owners.reserve(unique_indices.size());
    for (std::size_t index : unique_indices) {
        std::uint64_t owner = 0;
        if (index >= spill || !findFakeIdentityForIndex(index, plan.live_identities, owners, owner) || owner == 0) {
            return false;
        }
        owners.push_back(owner);
        plan.entries.push_back({index, owner});
    }
    plan.prevalidated = true;
    return true;
}

bool armSeedPlan(spark::AllocationSampler &sampler, spark::test::LinuxAllocationTestControl &control,
                 SeedPlan &plan) noexcept
{
    plan.sampler = &sampler;
    control.before_admission = beforeAdmission;
    control.before_admission_context = &plan;
    return spark::test::AllocationLifecycleTestAccess::configureLinux(sampler, &control);
}

bool startSeededSampler(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
                        spark::test::LinuxAllocationTestControl &control, SeedPlan &plan, std::string &error)
{
    if (!armSeedPlan(sampler, control, plan)) {
        error = "could not configure the Linux pre-admission seed plan";
        return false;
    }
    return sampler.start(config, error);
}

bool callbackEvidenceValid(const SeedPlan &plan)
{
    return plan.callback_called && plan.reset_verified && plan.plan_verified && plan.seeds_verified &&
           plan.callback_succeeded &&
           plan.lines_scanned == spark::test::AllocationLifecycleTestAccess::hotShardCount() &&
           plan.seeds_claimed == plan.entries.size() &&
           (!plan.exercise_rejections || (plan.invalid_rejections_verified && plan.duplicate_rejection_verified));
}

bool stopSessionSafely(spark::AllocationSampler &sampler, std::string &error, const char *label)
{
    const bool stopped = sampler.stop(error);
    if (!stopped) {
        std::fprintf(stderr, "%s: sampler stop failed: %s\n", label, error.c_str());
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (sampler.backendCleanupPending()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "%s: backend cleanup remained pending; preserving seed context by failing closed\n",
                         label);
            std::fflush(nullptr);
            std::_Exit(2);
        }
        sched_yield();
    }
    return stopped;
}

bool shutdownSamplerSafely(spark::AllocationSampler &sampler, std::string &error, const char *label)
{
    const bool shutdown = sampler.shutdown(error);
    if (!shutdown) {
        std::fprintf(stderr, "%s: sampler shutdown failed: %s\n", label, error.c_str());
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (sampler.backendCleanupPending()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "%s: backend shutdown remained pending; preserving seed context by failing closed\n",
                         label);
            std::fflush(nullptr);
            std::_Exit(2);
        }
        sched_yield();
    }
    return shutdown;
}

struct Counters {
    std::uint64_t hooks = 0;
    std::uint64_t successful = 0;
    std::uint64_t bytes = 0;
};

Counters read(spark::AllocationSampler &sampler)
{
    return {.hooks = sampler.hookCalls(),
            .successful = sampler.successfulAllocationCalls(),
            .bytes = sampler.observedBytes()};
}

std::size_t countOccupiedOwnerLines(spark::AllocationSampler &sampler)
{
    const std::size_t spill = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    std::size_t occupied = 0;
    for (std::size_t index = 0; index < spill; ++index) {
        occupied += spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, index).owner != 0 ? 1U : 0U;
    }
    return occupied;
}

Counters operator-(const Counters &a, const Counters &b)
{
    return {.hooks = a.hooks - b.hooks, .successful = a.successful - b.successful, .bytes = a.bytes - b.bytes};
}

void *allocateTouched()
{
    void *p = std::malloc(KBytes);
    if (p != nullptr) {
        static_cast<volatile unsigned char *>(p)[0] = 0x5a;
    }
    return p;
}

void pairOnce()
{
    std::free(allocateTouched());
}

void runMeasuredPairs(std::size_t ops, std::uint64_t &successful, std::uint64_t &bytes)
{
    for (std::size_t index = 0; index < ops; ++index) {
        void *allocation = allocateTouched();
        if (allocation != nullptr) {
            ++successful;
            bytes += KBytes;
            std::free(allocation);
        }
    }
}

struct Gate {
    std::atomic<unsigned> arrived{0};
    std::atomic<bool> go{false};
    std::atomic<unsigned> warmup_done{0};
    std::atomic<bool> work_go{false};
    std::atomic<bool> cancel{false};
    std::atomic<unsigned> done{0};
};

struct ChurnIdentityEvidence {
    std::uint64_t thread_pointer = 0;
    std::size_t claim_index = 0;
    std::uint64_t observed_owner = 0;
};

struct WorkerArg {
    Gate *gate = nullptr;
    std::size_t ops = 0;
    ChurnIdentityEvidence *identity = nullptr;
    std::uint64_t warmup_successful = 0;
    std::uint64_t warmup_bytes = 0;
    std::uint64_t successful = 0;
    std::uint64_t bytes = 0;
};

void *worker(void *raw)
{
    auto &arg = *static_cast<WorkerArg *>(raw);
    const std::uint64_t tp = thisThreadPointer();
    const std::size_t index = spark::test::AllocationLifecycleTestAccess::shardIndexForThreadPointer(tp);
    if (arg.identity != nullptr) {
        arg.identity->thread_pointer = tp;
        arg.identity->claim_index = index;
    }

    arg.gate->arrived.fetch_add(1, std::memory_order_release);
    while (!arg.gate->go.load(std::memory_order_acquire)) {
        sched_yield();
    }
    if (arg.gate->cancel.load(std::memory_order_acquire)) {
        arg.gate->done.fetch_add(1, std::memory_order_release);
        return nullptr;
    }

    for (std::size_t i = 0; i < 32; ++i) {
        void *allocation = allocateTouched();
        if (allocation != nullptr) {
            ++arg.warmup_successful;
            arg.warmup_bytes += KBytes;
            std::free(allocation);
        }
    }

    arg.gate->warmup_done.fetch_add(1, std::memory_order_release);
    while (!arg.gate->work_go.load(std::memory_order_acquire)) {
        sched_yield();
    }
    if (arg.gate->cancel.load(std::memory_order_acquire)) {
        arg.gate->done.fetch_add(1, std::memory_order_release);
        return nullptr;
    }

    for (std::size_t i = 0; i < arg.ops; ++i) {
        void *p = allocateTouched();
        if (p != nullptr) {
            ++arg.successful;
            arg.bytes += KBytes;
            std::free(p);
        }
    }

    arg.gate->done.fetch_add(1, std::memory_order_release);
    return nullptr;
}

void waitFor(const std::atomic<unsigned> &counter, unsigned target)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (counter.load(std::memory_order_acquire) < target) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "timed out waiting for %u workers (observed %u)\n", target,
                         counter.load(std::memory_order_acquire));
            std::fflush(nullptr);
            std::_Exit(2);
        }
        sched_yield();
    }
}

std::atomic<bool> StopReader{false};
std::atomic<bool> ReaderGo{false};
std::atomic<bool> ReaderStarted{false};
std::atomic<bool> ReaderExited{false};
std::atomic<bool> SawDecrease{false};
std::atomic<std::uint64_t> ReaderIterations{0};
std::vector<std::uint64_t> LastHooks;
std::vector<std::uint64_t> LastSuccessful;
std::vector<std::uint64_t> LastBytes;

// The reader owns these baselines; exactly one reader runs at a time and it is joined before the next arm.
void resetReaderBaselines(spark::AllocationSampler &sampler)
{
    const std::size_t count = spark::test::AllocationLifecycleTestAccess::hotShardCount();
    LastHooks.assign(count, 0);
    LastSuccessful.assign(count, 0);
    LastBytes.assign(count, 0);
    for (std::size_t index = 0; index < count; ++index) {
        const auto line = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, index);
        LastHooks[index] = line.hooks;
        LastSuccessful[index] = line.successful;
        LastBytes[index] = line.bytes;
    }
    SawDecrease.store(false, std::memory_order_release);
    ReaderIterations.store(0, std::memory_order_release);
}

void prepareReader(spark::AllocationSampler &sampler)
{
    resetReaderBaselines(sampler);
    StopReader.store(false, std::memory_order_release);
    ReaderGo.store(false, std::memory_order_release);
    ReaderStarted.store(false, std::memory_order_release);
    ReaderExited.store(false, std::memory_order_release);
    ReaderThreadPointer.store(0, std::memory_order_release);
}

void *reader(void *raw)
{
    auto *sampler = static_cast<spark::AllocationSampler *>(raw);
    ReaderThreadPointer.store(thisThreadPointer(), std::memory_order_release);
    ReaderStarted.store(true, std::memory_order_release);
    while (!ReaderGo.load(std::memory_order_acquire) && !StopReader.load(std::memory_order_acquire)) {
        sched_yield();
    }
    while (!StopReader.load(std::memory_order_acquire)) {
        for (std::size_t index = 0; index < LastHooks.size(); ++index) {
            // Per-line monotonicity; owner is excluded because resetSession may change it between sessions.
            const auto line = spark::test::AllocationLifecycleTestAccess::hotShardLine(*sampler, index);
            if (line.hooks < LastHooks[index] || line.successful < LastSuccessful[index] ||
                line.bytes < LastBytes[index]) {
                SawDecrease.store(true, std::memory_order_release);
            }
            LastHooks[index] = std::max(line.hooks, LastHooks[index]);
            LastSuccessful[index] = std::max(line.successful, LastSuccessful[index]);
            LastBytes[index] = std::max(line.bytes, LastBytes[index]);
        }
        ReaderIterations.fetch_add(1, std::memory_order_release);
        sched_yield();
    }
    ReaderExited.store(true, std::memory_order_release);
    return nullptr;
}

template <typename Value>
bool waitAtLeast(const std::atomic<Value> &counter, Value target, const char *label)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (counter.load(std::memory_order_acquire) < target) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "%s: timed out waiting for %llu (observed %llu)\n", label,
                         static_cast<unsigned long long>(target),
                         static_cast<unsigned long long>(counter.load(std::memory_order_acquire)));
            return false;
        }
        sched_yield();
    }
    return true;
}

[[noreturn]] void abortTimedOutWorkers(const char *label)
{
    std::fprintf(stderr, "%s: workers did not quiesce before the bounded deadline\n", label);
    std::fflush(nullptr);
    std::_Exit(2);
}

void checkedJoin(pthread_t thread, const char *label)
{
    timespec deadline{};
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        std::fprintf(stderr, "%s: could not read clock for bounded pthread join\n", label);
        std::fflush(nullptr);
        std::_Exit(2);
    }
    deadline.tv_sec += 10;

    int result = 0;
    do {
        result = pthread_timedjoin_np(thread, nullptr, &deadline);
    } while (result == EINTR);
    if (result != 0) {
        std::fprintf(stderr, "%s: pthread_timedjoin_np failed: %d\n", label, result);
        std::fflush(nullptr);
        std::_Exit(2);
    }
}

void checkedJoinAll(const std::vector<pthread_t> &threads, const char *label)
{
    for (pthread_t thread : threads) {
        checkedJoin(thread, label);
    }
}

int calibrate(spark::AllocationSampler &sampler, Counters &per_pair)
{
    std::uint64_t warmup_successful = 0;
    std::uint64_t warmup_bytes = 0;
    runMeasuredPairs(64, warmup_successful, warmup_bytes);
    const Counters before = read(sampler);
    constexpr std::size_t k_probe = 50;
    std::uint64_t actual_successful = 0;
    std::uint64_t actual_bytes = 0;
    runMeasuredPairs(k_probe, actual_successful, actual_bytes);
    const Counters delta = read(sampler) - before;
    if (warmup_successful != 64 || warmup_bytes != 64 * KBytes || actual_successful != k_probe ||
        actual_bytes != k_probe * KBytes || delta.successful != k_probe || delta.bytes != k_probe * KBytes ||
        delta.hooks < delta.successful) {
        std::fprintf(stderr, "calibration failed: hooks %llu successful %llu bytes %llu\n",
                     static_cast<unsigned long long>(delta.hooks), static_cast<unsigned long long>(delta.successful),
                     static_cast<unsigned long long>(delta.bytes));
        return 1;
    }
    per_pair = {.hooks = delta.hooks / k_probe, .successful = 1, .bytes = KBytes};
    std::fprintf(stderr, "calibration: hooks per pair ~%llu, successful 1, bytes %zu\n",
                 static_cast<unsigned long long>(per_pair.hooks), KBytes);
    return 0;
}

void cancelAndJoinWorkers(Gate &gate, std::vector<pthread_t> &threads, const char *label);

int runBatch(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
             spark::test::LinuxAllocationTestControl &control, SeedPlan &plan, bool force_spill, const char *label)
{
    Gate gate;
    std::vector<ChurnIdentityEvidence> identities(KThreads);
    std::vector<WorkerArg> args;
    std::vector<pthread_t> threads;
    args.reserve(KThreads);
    threads.reserve(KThreads);
    for (unsigned index = 0; index < KThreads; ++index) {
        args.push_back({&gate, KOps, &identities[index]});
    }
    for (unsigned index = 0; index < KThreads; ++index) {
        pthread_t thread{};
        if (pthread_create(&thread, nullptr, worker, &args[index]) != 0) {
            std::fprintf(stderr, "%s: pthread_create failed\n", label);
            cancelAndJoinWorkers(gate, threads, label);
            return 1;
        }
        threads.push_back(thread);
    }
    if (!waitAtLeast(gate.arrived, KThreads, label)) {
        cancelAndJoinWorkers(gate, threads, label);
        return 1;
    }

    prepareReader(sampler);
    pthread_t reader_thread{};
    if (pthread_create(&reader_thread, nullptr, reader, &sampler) != 0) {
        std::fprintf(stderr, "%s: monotonic reader creation failed\n", label);
        cancelAndJoinWorkers(gate, threads, label);
        return 1;
    }
    if (!waitAtLeast(ReaderStarted, true, label)) {
        StopReader.store(true, std::memory_order_release);
        ReaderGo.store(true, std::memory_order_release);
        cancelAndJoinWorkers(gate, threads, label);
        if (!waitAtLeast(ReaderExited, true, label)) {
            abortTimedOutWorkers(label);
        }
        checkedJoin(reader_thread, label);
        return 1;
    }

    bool started_here = false;
    std::string stop_error;
    auto finish = [&](int result) {
        if (started_here && !stopSessionSafely(sampler, stop_error, label)) {
            result = 1;
        }
        return result;
    };
    if (force_spill) {
        std::vector<std::uint64_t> live;
        std::vector<std::size_t> claim_indices;
        live.reserve(KThreads + 2);
        claim_indices.reserve(KThreads);
        live.push_back(thisThreadPointer());
        for (const auto &identity : identities) {
            if (identity.thread_pointer == 0 ||
                identity.claim_index >= spark::test::AllocationLifecycleTestAccess::spillShardIndex()) {
                std::fprintf(stderr, "%s: invalid parked worker identity\n", label);
                StopReader.store(true, std::memory_order_release);
                ReaderGo.store(true, std::memory_order_release);
                cancelAndJoinWorkers(gate, threads, label);
                if (!waitAtLeast(ReaderExited, true, label)) {
                    abortTimedOutWorkers(label);
                }
                checkedJoin(reader_thread, label);
                return 1;
            }
            live.push_back(identity.thread_pointer);
            claim_indices.push_back(identity.claim_index);
        }
        live.push_back(ReaderThreadPointer.load(std::memory_order_acquire));
        resetSeedPlan(plan, sampler, live);
        if (!addSeedEntries(plan, claim_indices) || !startSeededSampler(sampler, config, control, plan, stop_error)) {
            std::fprintf(stderr, "%s: seeded sampler start failed: %s\n", label, stop_error.c_str());
            StopReader.store(true, std::memory_order_release);
            ReaderGo.store(true, std::memory_order_release);
            cancelAndJoinWorkers(gate, threads, label);
            if (!waitAtLeast(ReaderExited, true, label)) {
                abortTimedOutWorkers(label);
            }
            checkedJoin(reader_thread, label);
            if (sampler.backendCleanupPending()) {
                stopSessionSafely(sampler, stop_error, label);
            }
            return 1;
        }
        started_here = true;
        if (!callbackEvidenceValid(plan)) {
            std::fprintf(stderr, "%s: pre-admission seed evidence is incomplete\n", label);
            StopReader.store(true, std::memory_order_release);
            ReaderGo.store(true, std::memory_order_release);
            cancelAndJoinWorkers(gate, threads, label);
            if (!waitAtLeast(ReaderExited, true, label)) {
                abortTimedOutWorkers(label);
            }
            checkedJoin(reader_thread, label);
            return finish(1);
        }
    }

    gate.go.store(true, std::memory_order_release);
    if (!waitAtLeast(gate.warmup_done, KThreads, label)) {
        StopReader.store(true, std::memory_order_release);
        abortTimedOutWorkers(label);
    }
    resetReaderBaselines(sampler);
    const Counters before = read(sampler);
    ReaderGo.store(true, std::memory_order_release);
    gate.work_go.store(true, std::memory_order_release);

    if (!waitAtLeast(gate.done, KThreads, label)) {
        StopReader.store(true, std::memory_order_release);
        abortTimedOutWorkers(label);
    }
    StopReader.store(true, std::memory_order_release);
    if (!waitAtLeast(ReaderExited, true, label)) {
        abortTimedOutWorkers(label);
    }
    checkedJoin(reader_thread, label);
    checkedJoinAll(threads, label);
    if (ReaderIterations.load(std::memory_order_acquire) == 0 || SawDecrease.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "%s: monotonic reader did not complete or saw a decrease\n", label);
        return finish(1);
    }

    const Counters got = read(sampler) - before;
    std::uint64_t actual_successful = 0;
    std::uint64_t actual_bytes = 0;
    std::uint64_t setup_successful = 0;
    std::uint64_t setup_bytes = 0;
    for (const WorkerArg &arg : args) {
        actual_successful += arg.successful;
        actual_bytes += arg.bytes;
        setup_successful += arg.warmup_successful;
        setup_bytes += arg.warmup_bytes;
    }
    const std::uint64_t expected_successful = static_cast<std::uint64_t>(KOps) * KThreads;
    if (setup_successful != 32 * KThreads || setup_bytes != 32 * KThreads * KBytes ||
        actual_successful != expected_successful || actual_bytes != expected_successful * KBytes) {
        std::fprintf(stderr, "%s: workload allocations incomplete: successful %llu/%llu bytes %llu/%llu\n", label,
                     static_cast<unsigned long long>(actual_successful),
                     static_cast<unsigned long long>(expected_successful),
                     static_cast<unsigned long long>(actual_bytes),
                     static_cast<unsigned long long>(expected_successful) * KBytes);
        return finish(1);
    }
    const double coverage = static_cast<double>(got.successful) / static_cast<double>(actual_successful);
    std::fprintf(stderr,
                 "%s: expected=%llu actual_successful=%llu actual_bytes=%llu observed_successful=%llu "
                 "observed_bytes=%llu coverage=%.6f reader_iterations=%llu\n",
                 label, static_cast<unsigned long long>(expected_successful),
                 static_cast<unsigned long long>(actual_successful), static_cast<unsigned long long>(actual_bytes),
                 static_cast<unsigned long long>(got.successful), static_cast<unsigned long long>(got.bytes), coverage,
                 static_cast<unsigned long long>(ReaderIterations.load(std::memory_order_acquire)));
    const bool valid = got.successful <= expected_successful && got.hooks >= got.successful &&
                       got.bytes >= got.successful * KBytes && got.bytes <= actual_bytes && coverage >= 0.90;
    if (!valid) {
        std::fprintf(stderr, "%s: observed counters failed workload sanity checks\n", label);
    }
    return finish(valid ? 0 : 1);
}

int runMonotoneWhileSpilling(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
                             spark::test::LinuxAllocationTestControl &control, SeedPlan &plan)
{
    return runBatch(sampler, config, control, plan, true, "forced-spill monotonic reader");
}

int checkChurn(spark::AllocationSampler &sampler)
{
    constexpr unsigned k_rounds = 96;
    constexpr std::size_t k_per_round = 40;
    std::vector<ChurnIdentityEvidence> identities(k_rounds);
    const Counters before = read(sampler);
    const std::size_t spill = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    const auto spill_before = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, spill);
    const std::size_t occupied_owner_lines_before = countOccupiedOwnerLines(sampler);
    std::uint64_t actual_successful = 0;
    std::uint64_t actual_bytes = 0;
    for (unsigned round = 0; round < k_rounds; ++round) {
        Gate gate;
        WorkerArg arg{.gate = &gate, .ops = k_per_round, .identity = &identities[round]};
        pthread_t t{};
        if (pthread_create(&t, nullptr, worker, &arg) != 0) {
            std::fprintf(stderr, "churn create failed\n");
            return 1;
        }
        if (!waitAtLeast(gate.arrived, 1U, "churn worker arrival")) {
            gate.cancel.store(true, std::memory_order_release);
            gate.go.store(true, std::memory_order_release);
            gate.work_go.store(true, std::memory_order_release);
            if (!waitAtLeast(gate.done, 1U, "churn worker exit")) {
                abortTimedOutWorkers("churn worker");
            }
            checkedJoin(t, "churn worker arrival failure");
            return 1;
        }
        gate.go.store(true, std::memory_order_release);
        if (!waitAtLeast(gate.warmup_done, 1U, "churn worker warmup")) {
            gate.cancel.store(true, std::memory_order_release);
            gate.work_go.store(true, std::memory_order_release);
            checkedJoin(t, "churn worker warmup failure");
            return 1;
        }
        gate.work_go.store(true, std::memory_order_release);
        if (!waitAtLeast(gate.done, 1U, "churn worker completion")) {
            abortTimedOutWorkers("churn worker");
        }
        checkedJoin(t, "churn worker completion");
        actual_successful += arg.warmup_successful + arg.successful;
        actual_bytes += arg.warmup_bytes + arg.bytes;
        identities[round].observed_owner =
            spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, identities[round].claim_index).owner;
    }
    const std::size_t occupied_owner_lines_after = countOccupiedOwnerLines(sampler);
    const Counters got = read(sampler) - before;
    const auto spill_after = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, spill);
    const std::uint64_t want = static_cast<std::uint64_t>(k_rounds) * (32 + k_per_round);
    if (actual_successful != want || actual_bytes != want * KBytes) {
        std::fprintf(stderr, "thread churn workload allocations incomplete: successful %llu/%llu bytes %llu/%llu\n",
                     static_cast<unsigned long long>(actual_successful), static_cast<unsigned long long>(want),
                     static_cast<unsigned long long>(actual_bytes), static_cast<unsigned long long>(want) * KBytes);
        return 1;
    }
    if (got.successful != want || got.bytes != want * KBytes) {
        std::fprintf(stderr, "thread churn lost updates: successful %llu/%llu bytes %llu\n",
                     static_cast<unsigned long long>(got.successful), static_cast<unsigned long long>(want),
                     static_cast<unsigned long long>(got.bytes));
        return 1;
    }

    std::vector<std::uint64_t> thread_pointers;
    std::vector<std::size_t> claim_indices;
    std::vector<std::uint64_t> owners;
    thread_pointers.reserve(k_rounds);
    claim_indices.reserve(k_rounds);
    owners.reserve(k_rounds);
    bool identity_error = false;
    for (unsigned round = 0; round < k_rounds; ++round) {
        const auto &identity = identities[round];
        if (identity.thread_pointer == 0 || identity.claim_index >= spill || identity.observed_owner == 0) {
            std::fprintf(stderr, "churn identity %u has incomplete evidence\n", round);
            identity_error = true;
        }
        thread_pointers.push_back(identity.thread_pointer);
        claim_indices.push_back(identity.claim_index);
        owners.push_back(identity.observed_owner);
        std::fprintf(stderr, "churn_identity round=%u tp=0x%016llx index=%zu sampled_worker_line_owner=0x%016llx\n",
                     round, static_cast<unsigned long long>(identity.thread_pointer), identity.claim_index,
                     static_cast<unsigned long long>(identity.observed_owner));
    }
    const auto distinct_count = [](auto values) {
        std::ranges::sort(values);
        const auto unique_values = std::ranges::unique(values);
        return static_cast<std::size_t>(unique_values.begin() - values.begin());
    };
    const std::size_t distinct_threads = distinct_count(thread_pointers);
    const std::size_t distinct_indices = distinct_count(claim_indices);
    const std::size_t distinct_owners = distinct_count(owners);
    const std::uint64_t spill_hooks = spill_after.hooks - spill_before.hooks;
    if (spill_hooks > got.hooks) {
        std::fprintf(stderr, "thread churn spill hooks %llu exceed aggregate hooks %llu\n",
                     static_cast<unsigned long long>(spill_hooks), static_cast<unsigned long long>(got.hooks));
        return 1;
    }
    std::fprintf(stderr,
                 "thread churn: %u create/join cycles, sums exact, distinct identities=%zu indices=%zu "
                 "sampled_worker_line_owners=%zu, spill hook delta=%llu\n",
                 k_rounds, distinct_threads, distinct_indices, distinct_owners,
                 static_cast<unsigned long long>(spill_hooks));
    std::fprintf(stderr,
                 "owner_census phase=serial_churn mode=serial claimable_lines=%zu forced_seed_lines=0 "
                 "occupied_owner_lines_before=%zu occupied_owner_lines_after=%zu\n",
                 spill, occupied_owner_lines_before, occupied_owner_lines_after);
    if (identity_error || distinct_threads == 0 || distinct_indices == 0 || distinct_owners == 0) {
        return 1;
    }
    return 0;
}

int checkIndexDomain()
{
    const std::size_t spill = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    if (spill != 1023) {
        std::fprintf(stderr, "spill index is %zu, want 1023\n", spill);
        return 1;
    }
    std::vector<unsigned> used(spill, 0);
    for (std::uint64_t i = 1; i <= 20000; ++i) {
        const std::uint64_t pointer = i * 0x1880ULL;
        const std::size_t first = spark::test::AllocationLifecycleTestAccess::shardIndexForThreadPointer(pointer);
        if (first != spark::test::AllocationLifecycleTestAccess::shardIndexForThreadPointer(pointer)) {
            std::fprintf(stderr, "shard index is not deterministic\n");
            return 1;
        }
        if (first >= spill) {
            std::fprintf(stderr, "shard index %zu is inside the spill domain\n", first);
            return 1;
        }
        used[first] += 1;
    }
    std::size_t distinct = 0;
    for (unsigned count : used) {
        distinct += count != 0 ? 1U : 0U;
    }
    if (distinct < 64) {
        std::fprintf(stderr, "shard index collapsed onto %zu lines\n", distinct);
        return 1;
    }
    std::fprintf(stderr, "index domain: 20000 pointers, %zu distinct claim lines, spill never returned\n", distinct);
    return 0;
}

struct RestartEvidence {
    std::uint64_t thread_pointer = 0;
    std::uint64_t foreign_owner = 0;
    std::size_t index = 0;
    spark::test::HotShardLineForTesting foreign_line{};
    spark::test::HotShardLineForTesting spill_line{};
    bool precondition_verified = false;
};

int checkSeamRejections(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
                        spark::test::LinuxAllocationTestControl &control, SeedPlan &plan, std::string &error)
{
    const std::uint64_t identity = thisThreadPointer();
    const std::size_t seeded_index = spark::test::AllocationLifecycleTestAccess::shardIndexForThreadPointer(identity);
    resetSeedPlan(plan, sampler, {identity});
    plan.exercise_rejections = true;
    if (!addSeedEntries(plan, {seeded_index})) {
        return 1;
    }
    const bool started = startSeededSampler(sampler, config, control, plan, error);
    if (!started || !callbackEvidenceValid(plan)) {
        std::fprintf(stderr, "seam rejection session failed its pre-admission callback: %s\n", error.c_str());
        if (started || sampler.backendCleanupPending()) {
            stopSessionSafely(sampler, error, "seam rejection cleanup");
        }
        return 1;
    }

    std::size_t outside_index = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    bool rejected_outside_phase = false;
    for (std::size_t index = 0; index < spark::test::AllocationLifecycleTestAccess::spillShardIndex(); ++index) {
        if (index == seeded_index) {
            continue;
        }
        const auto line = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, index);
        if (line.owner != 0 || line.hooks != 0 || line.successful != 0 || line.bytes != 0) {
            continue;
        }
        std::uint64_t token = 0;
        if (findFakeIdentityForIndex(index, {identity}, {}, token)) {
            outside_index = index;
            rejected_outside_phase =
                !spark::test::AllocationLifecycleTestAccess::occupyShardOwnerForTesting(sampler, index, token);
            if (!rejected_outside_phase) {
                std::fprintf(stderr, "owner seed seam unexpectedly accepted a claim after callback closure\n");
                stopSessionSafely(sampler, error, "seam rejection session");
                return 1;
            }
            break;
        }
    }
    if (!rejected_outside_phase) {
        std::fprintf(stderr, "owner seed seam accepted a valid claim outside its callback phase\n");
        stopSessionSafely(sampler, error, "seam rejection session");
        return 1;
    }
    if (!stopSessionSafely(sampler, error, "seam rejection session")) {
        return 1;
    }
    std::fprintf(stderr,
                 "pre-admission seam rejects invalid/duplicate claims inside the phase and a valid zero-line claim "
                 "after it (index=%zu)\n",
                 outside_index);
    return 0;
}

int checkCallbackFailure(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
                         spark::test::LinuxAllocationTestControl &control, SeedPlan &plan, std::string &error)
{
    const std::uint64_t identity = thisThreadPointer();
    const std::size_t index = spark::test::AllocationLifecycleTestAccess::shardIndexForThreadPointer(identity);
    resetSeedPlan(plan, sampler, {identity});
    plan.force_failure = true;
    if (!addSeedEntries(plan, {index})) {
        return 1;
    }
    if (!armSeedPlan(sampler, control, plan)) {
        return 1;
    }
    if (sampler.start(config, error)) {
        std::fprintf(stderr, "callback-failure arm unexpectedly started\n");
        stopSessionSafely(sampler, error, "callback failure unexpected start");
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (sampler.backendCleanupPending()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "callback-failure cleanup remained pending after 10 seconds\n");
            std::fflush(nullptr);
            std::_Exit(2);
        }
        sched_yield();
    }
    if (error != "allocation pre-admission test setup failed" || !plan.callback_called || !plan.reset_verified ||
        !plan.plan_verified || !plan.seeds_verified || plan.callback_succeeded ||
        plan.seeds_claimed != plan.entries.size()) {
        std::fprintf(stderr, "callback-failure arm returned without the expected bounded setup-failure evidence: %s\n",
                     error.c_str());
        return 1;
    }
    std::fprintf(stderr, "callback-failure arm rejected start and completed backend cleanup\n");
    return 0;
}

int runRestartA(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
                spark::test::LinuxAllocationTestControl &control, SeedPlan &plan, RestartEvidence &evidence,
                std::string &error)
{
    evidence.thread_pointer = thisThreadPointer();
    evidence.index = spark::test::AllocationLifecycleTestAccess::shardIndexForThreadPointer(evidence.thread_pointer);
    resetSeedPlan(plan, sampler, {evidence.thread_pointer});
    if (!addSeedEntries(plan, {evidence.index})) {
        return 1;
    }
    const bool started = startSeededSampler(sampler, config, control, plan, error);
    if (!started || !callbackEvidenceValid(plan)) {
        std::fprintf(stderr, "restart A pre-admission setup failed: %s\n", error.c_str());
        if (started || sampler.backendCleanupPending()) {
            stopSessionSafely(sampler, error, "restart A setup failure");
        }
        return 1;
    }

    evidence.foreign_owner = plan.entries.front().owner;
    const auto foreign_before = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, evidence.index);
    const std::size_t spill = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    const auto spill_before = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, spill);
    const Counters before = read(sampler);
    std::uint64_t actual_successful = 0;
    std::uint64_t actual_bytes = 0;
    runMeasuredPairs(KOps, actual_successful, actual_bytes);
    const Counters got = read(sampler) - before;
    evidence.foreign_line = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, evidence.index);
    evidence.spill_line = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, spill);
    evidence.precondition_verified = actual_successful == KOps && actual_bytes == KOps * KBytes &&
                                     got.successful == KOps && got.bytes == KOps * KBytes &&
                                     evidence.foreign_line.owner == evidence.foreign_owner &&
                                     evidence.foreign_line.hooks == foreign_before.hooks &&
                                     evidence.foreign_line.successful == foreign_before.successful &&
                                     evidence.foreign_line.bytes == foreign_before.bytes &&
                                     evidence.spill_line.successful > spill_before.successful &&
                                     evidence.spill_line.bytes >= spill_before.bytes + KOps * KBytes;
    std::fprintf(stderr, "restart A: foreign owner frozen=%u spill successful delta=%llu bytes delta=%llu\n",
                 evidence.precondition_verified ? 1U : 0U,
                 static_cast<unsigned long long>(evidence.spill_line.successful - spill_before.successful),
                 static_cast<unsigned long long>(evidence.spill_line.bytes - spill_before.bytes));
    if (!evidence.precondition_verified) {
        stopSessionSafely(sampler, error, "restart A failed evidence");
        return 1;
    }
    return stopSessionSafely(sampler, error, "restart A") ? 0 : 1;
}

int runRestartB(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
                spark::test::LinuxAllocationTestControl &control, SeedPlan &plan, const RestartEvidence &evidence,
                std::string &error)
{
    resetSeedPlan(plan, sampler, {evidence.thread_pointer});
    plan.require_restart_precondition = true;
    plan.restart_precondition = evidence.precondition_verified &&
                                evidence.foreign_line.owner == evidence.foreign_owner &&
                                evidence.spill_line.successful != 0;
    const bool started = startSeededSampler(sampler, config, control, plan, error);
    if (!started || !callbackEvidenceValid(plan)) {
        std::fprintf(stderr, "restart B reset callback failed: %s\n", error.c_str());
        if (started || sampler.backendCleanupPending()) {
            stopSessionSafely(sampler, error, "restart B setup failure");
        }
        return 1;
    }
    const auto setup_line = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, evidence.index);
    if (setup_line.owner == evidence.foreign_owner) {
        std::fprintf(stderr, "restart B setup retained the old foreign owner\n");
        stopSessionSafely(sampler, error, "restart B reset failure");
        return 1;
    }
    const Counters setup = read(sampler);
    const Counters before = setup;
    const auto line_before = setup_line;
    std::uint64_t actual_successful = 0;
    std::uint64_t actual_bytes = 0;
    runMeasuredPairs(KOps, actual_successful, actual_bytes);
    const Counters got = read(sampler) - before;
    const auto line_after = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, evidence.index);
    const bool exact = actual_successful == KOps && actual_bytes == KOps * KBytes && got.successful == KOps &&
                       got.bytes == KOps * KBytes && line_after.owner == evidence.thread_pointer &&
                       line_after.successful - line_before.successful == KOps &&
                       line_after.bytes - line_before.bytes == KOps * KBytes;
    if (!exact) {
        std::fprintf(stderr, "restart B natural claim/delta failed: global=%llu/%llu line owner=0x%016llx\n",
                     static_cast<unsigned long long>(got.successful), static_cast<unsigned long long>(got.bytes),
                     static_cast<unsigned long long>(line_after.owner));
    }
    const bool stopped = stopSessionSafely(sampler, error, "restart B");
    std::fprintf(stderr,
                 "restart B reset callback saw all-zero table; post-start setup baseline hooks=%llu successful=%llu "
                 "bytes=%llu exact=%u\n",
                 static_cast<unsigned long long>(setup.hooks), static_cast<unsigned long long>(setup.successful),
                 static_cast<unsigned long long>(setup.bytes), exact ? 1U : 0U);
    return exact && stopped ? 0 : 1;
}

struct OccupancyOptions {
    unsigned percent = 0;
    unsigned threads = 1;
    std::size_t ops = 100000;
    bool contention = false;
};

struct OccupancyWorkerEvidence {
    std::uint64_t thread_pointer = 0;
    std::size_t claim_index = 0;
    std::uint64_t successful = 0;
    std::uint64_t bytes = 0;
    std::uint64_t cpu_ns = 0;
    bool cpu_clock_ok = true;
    bool reader_checkpoint_ok = true;
};

struct OccupancyWorkerArg {
    spark::AllocationSampler *sampler = nullptr;
    Gate *gate = nullptr;
    OccupancyWorkerEvidence *evidence = nullptr;
    std::size_t ops = 0;
    bool contention = false;
};

bool clockNanoseconds(clockid_t clock_id, std::uint64_t &value) noexcept
{
    timespec now{};
    if (clock_gettime(clock_id, &now) != 0) {
        return false;
    }
    value = static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(now.tv_nsec);
    return true;
}

void *occupancyWorker(void *raw)
{
    auto &arg = *static_cast<OccupancyWorkerArg *>(raw);
    auto &evidence = *arg.evidence;
    evidence.thread_pointer = thisThreadPointer();
    evidence.claim_index =
        spark::test::AllocationLifecycleTestAccess::shardIndexForThreadPointer(evidence.thread_pointer);
    arg.gate->arrived.fetch_add(1, std::memory_order_release);
    while (!arg.gate->go.load(std::memory_order_acquire)) {
        sched_yield();
    }
    if (arg.gate->cancel.load(std::memory_order_acquire)) {
        arg.gate->done.fetch_add(1, std::memory_order_release);
        return nullptr;
    }

    std::uint64_t cpu_start = 0;
    std::uint64_t cpu_end = 0;
    evidence.cpu_clock_ok = clockNanoseconds(CLOCK_THREAD_CPUTIME_ID, cpu_start);
    const std::size_t first_batch = arg.contention ? arg.ops / 2 : arg.ops;
    for (std::size_t i = 0; i < first_batch; ++i) {
        void *allocation = allocateTouched();
        if (allocation != nullptr) {
            ++evidence.successful;
            evidence.bytes += KBytes;
            std::free(allocation);
        }
    }
    if (arg.contention) {
        const std::uint64_t iteration = ReaderIterations.load(std::memory_order_acquire);
        if (!waitAtLeast(ReaderIterations, iteration + 1, "occupancy monotonic reader checkpoint")) {
            evidence.reader_checkpoint_ok = false;
            arg.gate->done.fetch_add(1, std::memory_order_release);
            return nullptr;
        }
    }
    for (std::size_t i = first_batch; i < arg.ops; ++i) {
        void *allocation = allocateTouched();
        if (allocation != nullptr) {
            ++evidence.successful;
            evidence.bytes += KBytes;
            std::free(allocation);
        }
    }
    const bool cpu_end_ok = clockNanoseconds(CLOCK_THREAD_CPUTIME_ID, cpu_end);
    evidence.cpu_clock_ok = evidence.cpu_clock_ok && cpu_end_ok && cpu_end >= cpu_start;
    if (evidence.cpu_clock_ok) {
        evidence.cpu_ns = cpu_end - cpu_start;
    }
    arg.gate->done.fetch_add(1, std::memory_order_release);
    return nullptr;
}

template <typename Value>
std::size_t uniqueCount(std::vector<Value> values)
{
    std::sort(values.begin(), values.end());
    return static_cast<std::size_t>(std::unique(values.begin(), values.end()) - values.begin());
}

std::uint64_t parseUnsigned(const char *value, bool &ok)
{
    char *end = nullptr;
    errno = 0;
    const std::uint64_t parsed = std::strtoull(value, &end, 10);
    ok = errno == 0 && end != value && *end == '\0';
    return parsed;
}

bool parseOccupancyOptions(int argc, char **argv, OccupancyOptions &options)
{
    bool have_mode = false;
    bool have_percent = false;
    bool have_threads = false;
    bool have_ops = false;
    for (int index = 1; index < argc; ++index) {
        const char *name = argv[index];
        if (std::string(name) == "--occupancy-diagnostic") {
            continue;
        }
        if (index + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", name);
            return false;
        }
        const char *value = argv[++index];
        bool valid = false;
        const std::uint64_t parsed = parseUnsigned(value, valid);
        if (std::string(name) == "--occupancy-percent") {
            if (!valid || parsed > 100) {
                std::fprintf(stderr, "occupancy percent must be between 0 and 100\n");
                return false;
            }
            options.percent = static_cast<unsigned>(parsed);
            have_percent = true;
        }
        else if (std::string(name) == "--threads") {
            if (!valid || parsed == 0 || parsed > std::numeric_limits<unsigned>::max()) {
                std::fprintf(stderr, "thread count must be a positive unsigned integer\n");
                return false;
            }
            options.threads = static_cast<unsigned>(parsed);
            have_threads = true;
        }
        else if (std::string(name) == "--ops") {
            if (!valid || parsed == 0 || parsed > std::numeric_limits<std::size_t>::max()) {
                std::fprintf(stderr, "operation count must be a positive size_t\n");
                return false;
            }
            options.ops = static_cast<std::size_t>(parsed);
            have_ops = true;
        }
        else if (std::string(name) == "--mode") {
            const std::string mode(value);
            if (mode == "single") {
                options.contention = false;
            }
            else if (mode == "contention") {
                options.contention = true;
            }
            else {
                std::fprintf(stderr, "mode must be single or contention\n");
                return false;
            }
            have_mode = true;
        }
        else {
            std::fprintf(stderr, "unknown occupancy diagnostic option: %s\n", name);
            return false;
        }
    }
    if (!have_percent || !have_mode || (options.contention && options.threads < 8) ||
        (!options.contention && options.threads != 1)) {
        std::fprintf(stderr, "occupancy diagnostic requires --occupancy-percent and --mode; single uses 1 thread and "
                             "contention uses at least 8 threads\n");
        return false;
    }
    if (options.ops > std::numeric_limits<std::uint64_t>::max() / options.threads / KBytes) {
        std::fprintf(stderr, "requested operation count would overflow byte accounting\n");
        return false;
    }
    (void)have_threads;
    (void)have_ops;
    return true;
}

void cancelAndJoinWorkers(Gate &gate, std::vector<pthread_t> &threads, const char *label)
{
    gate.cancel.store(true, std::memory_order_release);
    gate.go.store(true, std::memory_order_release);
    gate.work_go.store(true, std::memory_order_release);
    if (!waitAtLeast(gate.done, static_cast<unsigned>(threads.size()), label)) {
        abortTimedOutWorkers(label);
    }
    checkedJoinAll(threads, label);
}

int runOccupancyDiagnostic(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
                           spark::test::LinuxAllocationTestControl &control, SeedPlan &plan,
                           const OccupancyOptions &options, std::string &error, bool &sampler_started)
{
    sampler_started = false;
    const std::size_t spill = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    const std::size_t shard_count = spark::test::AllocationLifecycleTestAccess::hotShardCount();
    if (spill >= shard_count || shard_count - 1 != spill) {
        std::fprintf(stderr, "occupancy diagnostic requires a terminal spill line\n");
        return 1;
    }
    const std::size_t target_occupied = spill * options.percent / 100;

    Gate gate;
    std::vector<OccupancyWorkerEvidence> evidence(options.threads);
    std::vector<OccupancyWorkerArg> args;
    std::vector<pthread_t> threads;
    args.reserve(options.threads);
    threads.reserve(options.threads);
    for (unsigned index = 0; index < options.threads; ++index) {
        args.push_back({&sampler, &gate, &evidence[index], options.ops, options.contention});
    }
    for (unsigned index = 0; index < options.threads; ++index) {
        pthread_t thread{};
        if (pthread_create(&thread, nullptr, occupancyWorker, &args[index]) != 0) {
            std::fprintf(stderr, "occupancy diagnostic: pthread_create failed\n");
            cancelAndJoinWorkers(gate, threads, "occupancy startup cancellation");
            return 1;
        }
        threads.push_back(thread);
    }
    if (!waitAtLeast(gate.arrived, options.threads, "occupancy worker identities")) {
        cancelAndJoinWorkers(gate, threads, "occupancy startup cancellation");
        return 1;
    }

    std::vector<std::uint64_t> identities;
    std::vector<std::size_t> indices;
    identities.reserve(options.threads);
    indices.reserve(options.threads);
    bool identity_error = false;
    for (const auto &worker_evidence : evidence) {
        identities.push_back(worker_evidence.thread_pointer);
        indices.push_back(worker_evidence.claim_index);
        if (worker_evidence.thread_pointer == 0 || worker_evidence.claim_index >= spill) {
            identity_error = true;
        }
    }
    const std::size_t distinct_identities = uniqueCount(identities);
    const std::size_t distinct_indices = uniqueCount(indices);
    if (identity_error || distinct_identities != options.threads) {
        std::fprintf(stderr, "occupancy diagnostic requires distinct nonzero live thread-pointer identities\n");
        cancelAndJoinWorkers(gate, threads, "occupancy startup cancellation");
        return 1;
    }

    std::vector<std::uint64_t> live_identities;
    live_identities.reserve(options.threads + 2);
    live_identities.push_back(thisThreadPointer());
    live_identities.insert(live_identities.end(), identities.begin(), identities.end());
    std::vector<spark::test::HotShardLineForTesting> before_lines(shard_count);
    pthread_t reader_thread{};
    const bool monitor_lines = options.contention;
    if (monitor_lines) {
        prepareReader(sampler);
        if (pthread_create(&reader_thread, nullptr, reader, &sampler) != 0) {
            std::fprintf(stderr, "occupancy diagnostic: monotonic reader creation failed\n");
            cancelAndJoinWorkers(gate, threads, "occupancy startup cancellation");
            return 1;
        }
        if (!waitAtLeast(ReaderStarted, true, "occupancy monotonic reader startup")) {
            StopReader.store(true, std::memory_order_release);
            ReaderGo.store(true, std::memory_order_release);
            cancelAndJoinWorkers(gate, threads, "occupancy startup cancellation");
            if (!waitAtLeast(ReaderExited, true, "occupancy reader cancellation")) {
                abortTimedOutWorkers("occupancy reader cancellation");
            }
            checkedJoin(reader_thread, "occupancy reader startup cancellation");
            return 1;
        }
        live_identities.push_back(ReaderThreadPointer.load(std::memory_order_acquire));
    }

    std::vector<std::size_t> seed_indices;
    seed_indices.reserve(target_occupied);
    std::vector<std::uint64_t> foreign_owners;
    foreign_owners.reserve(target_occupied);
    for (std::size_t index = 0; index < target_occupied; ++index) {
        seed_indices.push_back(index);
    }
    resetSeedPlan(plan, sampler, live_identities);
    if (!addSeedEntries(plan, seed_indices)) {
        std::fprintf(stderr, "occupancy diagnostic start failed: %s\n", error.c_str());
        cancelAndJoinWorkers(gate, threads, "occupancy startup cancellation");
        if (monitor_lines) {
            StopReader.store(true, std::memory_order_release);
            ReaderGo.store(true, std::memory_order_release);
            if (!waitAtLeast(ReaderExited, true, "occupancy reader cancellation")) {
                abortTimedOutWorkers("occupancy reader cancellation");
            }
            checkedJoin(reader_thread, "occupancy sampler-start cancellation");
        }
        return 1;
    }
    const bool started = startSeededSampler(sampler, config, control, plan, error);
    if (!started) {
        std::fprintf(stderr, "occupancy diagnostic start failed: %s\n", error.c_str());
        sampler_started = sampler.backendCleanupPending();
        cancelAndJoinWorkers(gate, threads, "occupancy startup cancellation");
        if (monitor_lines) {
            StopReader.store(true, std::memory_order_release);
            ReaderGo.store(true, std::memory_order_release);
            if (!waitAtLeast(ReaderExited, true, "occupancy reader cancellation")) {
                abortTimedOutWorkers("occupancy reader cancellation");
            }
            checkedJoin(reader_thread, "occupancy sampler-start cancellation");
        }
        return 1;
    }
    sampler_started = true;

    if (!callbackEvidenceValid(plan) || plan.entries.size() != target_occupied) {
        std::fprintf(stderr, "occupancy diagnostic did not seed the requested pre-admission occupancy\n");
        cancelAndJoinWorkers(gate, threads, "occupancy seed-evidence cancellation");
        if (monitor_lines) {
            StopReader.store(true, std::memory_order_release);
            ReaderGo.store(true, std::memory_order_release);
            if (!waitAtLeast(ReaderExited, true, "occupancy reader cancellation")) {
                abortTimedOutWorkers("occupancy reader cancellation");
            }
            checkedJoin(reader_thread, "occupancy seed-evidence cancellation");
        }
        return 1;
    }
    for (const SeedEntry &entry : plan.entries) {
        if (entry.index >= target_occupied ||
            spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, entry.index).owner != entry.owner) {
            std::fprintf(stderr, "occupancy diagnostic owner mismatch at index %zu\n", entry.index);
            cancelAndJoinWorkers(gate, threads, "occupancy owner-check cancellation");
            if (monitor_lines) {
                StopReader.store(true, std::memory_order_release);
                ReaderGo.store(true, std::memory_order_release);
                if (!waitAtLeast(ReaderExited, true, "occupancy reader cancellation")) {
                    abortTimedOutWorkers("occupancy reader cancellation");
                }
                checkedJoin(reader_thread, "occupancy owner-check cancellation");
            }
            return 1;
        }
        foreign_owners.push_back(entry.owner);
    }

    resetReaderBaselines(sampler);
    std::size_t occupied_owner_lines_before = 0;
    for (std::size_t index = 0; index < shard_count; ++index) {
        before_lines[index] = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, index);
        if (index < spill && before_lines[index].owner != 0) {
            occupied_owner_lines_before += 1;
        }
    }
    const Counters setup = read(sampler);
    const Counters before = setup;

    std::uint64_t wall_start = 0;
    std::uint64_t wall_end = 0;
    const bool wall_clock_ok = clockNanoseconds(CLOCK_MONOTONIC, wall_start);
    if (monitor_lines) {
        ReaderGo.store(true, std::memory_order_release);
    }
    gate.go.store(true, std::memory_order_release);
    if (!waitAtLeast(gate.done, options.threads, "occupancy workers")) {
        StopReader.store(true, std::memory_order_release);
        abortTimedOutWorkers("occupancy workers");
    }
    if (monitor_lines) {
        StopReader.store(true, std::memory_order_release);
        if (!waitAtLeast(ReaderExited, true, "occupancy monotonic reader exit")) {
            abortTimedOutWorkers("occupancy monotonic reader");
        }
        checkedJoin(reader_thread, "occupancy reader exit");
    }
    const bool wall_end_ok = clockNanoseconds(CLOCK_MONOTONIC, wall_end);
    checkedJoinAll(threads, "occupancy worker exit");

    const Counters got = read(sampler) - before;
    const std::uint64_t expected_successful = static_cast<std::uint64_t>(options.threads) * options.ops;
    const std::uint64_t expected_bytes = expected_successful * KBytes;
    std::uint64_t actual_successful = 0;
    std::uint64_t actual_bytes = 0;
    for (const auto &worker_evidence : evidence) {
        actual_successful += worker_evidence.successful;
        actual_bytes += worker_evidence.bytes;
    }
    const bool actual_workload_exact = actual_successful == expected_successful && actual_bytes == expected_bytes;
    const double coverage = actual_workload_exact && actual_successful != 0
                              ? static_cast<double>(got.successful) / static_cast<double>(actual_successful)
                              : 0.0;
    const std::size_t spill_index = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    const auto spill_after = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, spill_index);
    const std::uint64_t spill_hooks = spill_after.hooks - before_lines[spill_index].hooks;
    const std::uint64_t spill_successful = spill_after.successful - before_lines[spill_index].successful;
    const std::uint64_t spill_bytes = spill_after.bytes - before_lines[spill_index].bytes;
    const double spill_ratio = got.hooks == 0 ? 0.0 : static_cast<double>(spill_hooks) / static_cast<double>(got.hooks);
    const bool spill_owner_clear = spill_after.owner == 0;
    std::uint64_t cpu_ns = 0;
    bool cpu_clock_ok = true;
    bool checkpoint_ok = true;
    for (const auto &worker_evidence : evidence) {
        cpu_ns += worker_evidence.cpu_ns;
        cpu_clock_ok = cpu_clock_ok && worker_evidence.cpu_clock_ok;
        checkpoint_ok = checkpoint_ok && worker_evidence.reader_checkpoint_ok;
    }

    bool counters_monotone = true;
    bool frozen_owners_held = true;
    std::size_t occupied_owner_lines_after = 0;
    for (std::size_t index = 0; index < shard_count; ++index) {
        const auto after = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, index);
        const auto &prior = before_lines[index];
        if (index < spill_index && after.owner != 0) {
            occupied_owner_lines_after += 1;
        }
        if (after.hooks < prior.hooks || after.successful < prior.successful || after.bytes < prior.bytes) {
            counters_monotone = false;
        }
        if (index < target_occupied && (after.owner != foreign_owners[index] || after.hooks != prior.hooks ||
                                        after.successful != prior.successful || after.bytes != prior.bytes)) {
            frozen_owners_held = false;
        }
    }
    const bool reader_ok = !monitor_lines || (ReaderIterations.load(std::memory_order_acquire) > 0 &&
                                              !SawDecrease.load(std::memory_order_acquire));
    const bool byte_consistent = got.successful <= actual_successful && got.bytes >= got.successful * KBytes &&
                                 got.bytes <= actual_bytes && spill_successful <= got.successful &&
                                 spill_bytes <= got.bytes && spill_hooks <= got.hooks;
    const bool exact_single = options.contention || (actual_workload_exact && got.successful == expected_successful &&
                                                     got.bytes == expected_bytes);
    const bool threaded_coverage =
        !options.contention || (actual_workload_exact && coverage >= 0.90 && coverage <= 1.0);
    const bool time_ok = wall_clock_ok && wall_end_ok && wall_end >= wall_start && cpu_clock_ok;
    const bool passed = distinct_identities == options.threads && target_occupied == foreign_owners.size() &&
                        counters_monotone && frozen_owners_held && reader_ok && checkpoint_ok && byte_consistent &&
                        actual_workload_exact && exact_single && threaded_coverage && got.hooks >= got.successful &&
                        spill_owner_clear && time_ok;

    const std::uint64_t wall_ns = wall_clock_ok && wall_end_ok && wall_end >= wall_start ? wall_end - wall_start : 0;
    const bool include_speed = !options.contention;
    const double wall_ns_per_op = include_speed && expected_successful != 0
                                    ? static_cast<double>(wall_ns) / static_cast<double>(expected_successful)
                                    : 0.0;
    const double cpu_ns_per_op = include_speed && expected_successful != 0
                                   ? static_cast<double>(cpu_ns) / static_cast<double>(expected_successful)
                                   : 0.0;
    const double calls_per_sec = include_speed && wall_ns != 0 ? static_cast<double>(expected_successful) *
                                                                     1000000000.0 / static_cast<double>(wall_ns)
                                                               : 0.0;

    std::printf("record_type,occupancy_percent,occupied_lines,claimable_lines,mode,threads,ops_per_thread,"
                "setup_hooks,setup_successful,setup_bytes,"
                "expected_successful,actual_successful,observed_successful,coverage,expected_bytes,actual_bytes,"
                "observed_bytes,hooks,spill_hooks,spill_successful,spill_bytes,spill_ratio,spill_owner_clear,wall_ns,"
                "wall_ns_per_op,worker_cpu_ns,"
                "worker_cpu_ns_per_op,"
                "calls_per_sec,distinct_thread_pointers,distinct_claim_indices,unique_foreign_owners,reader_iterations,"
                "monotonic,counters_monotone,frozen_owners_held,checkpoint_ok,passed\n");
    std::printf(
        "data,%u,%zu,%zu,%s,%u,%zu,%llu,%llu,%llu,%llu,%llu,%llu,%.9f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.9f,%u,",
        options.percent, target_occupied, spill, options.contention ? "contention" : "single", options.threads,
        options.ops, static_cast<unsigned long long>(setup.hooks), static_cast<unsigned long long>(setup.successful),
        static_cast<unsigned long long>(setup.bytes), static_cast<unsigned long long>(expected_successful),
        static_cast<unsigned long long>(actual_successful), static_cast<unsigned long long>(got.successful), coverage,
        static_cast<unsigned long long>(expected_bytes), static_cast<unsigned long long>(actual_bytes),
        static_cast<unsigned long long>(got.bytes), static_cast<unsigned long long>(got.hooks),
        static_cast<unsigned long long>(spill_hooks), static_cast<unsigned long long>(spill_successful),
        static_cast<unsigned long long>(spill_bytes), spill_ratio, spill_owner_clear ? 1U : 0U);
    if (include_speed) {
        std::printf("%llu,%.3f,%llu,%.3f,%.3f,", static_cast<unsigned long long>(wall_ns), wall_ns_per_op,
                    static_cast<unsigned long long>(cpu_ns), cpu_ns_per_op, calls_per_sec);
    }
    else {
        std::printf(",,,,,");
    }
    std::printf("%zu,%zu,%zu,%llu,%u,%u,%u,%u,%u\n", distinct_identities, distinct_indices, uniqueCount(foreign_owners),
                static_cast<unsigned long long>(ReaderIterations.load()), reader_ok ? 1U : 0U,
                counters_monotone ? 1U : 0U, frozen_owners_held ? 1U : 0U, checkpoint_ok ? 1U : 0U, passed ? 1U : 0U);

    std::fprintf(stderr,
                 "occupancy %u%% forced_seed_lines=%zu claimable_lines=%zu, mode=%s threads=%u identities=%zu "
                 "indices=%zu actual_successful=%llu/%llu actual_bytes=%llu/%llu coverage=%.6f observed_bytes=%llu "
                 "spill_hooks=%llu spill_ratio=%.6f%s\n",
                 options.percent, target_occupied, spill, options.contention ? "contention" : "single", options.threads,
                 distinct_identities, distinct_indices, static_cast<unsigned long long>(actual_successful),
                 static_cast<unsigned long long>(expected_successful), static_cast<unsigned long long>(actual_bytes),
                 static_cast<unsigned long long>(expected_bytes), coverage, static_cast<unsigned long long>(got.bytes),
                 static_cast<unsigned long long>(spill_hooks), spill_ratio, passed ? " passed" : " failed");
    if (options.contention && coverage < 1.0) {
        std::fprintf(stderr, "contention coverage is a workload coverage threshold, not an exact admitted-callback "
                             "conservation proof\n");
    }
    std::fprintf(stderr,
                 "owner_census phase=occupancy occupancy_percent=%u mode=%s threads=%u ops_per_thread=%zu "
                 "claimable_lines=%zu forced_seed_lines=%zu occupied_owner_lines_before=%zu "
                 "occupied_owner_lines_after=%zu\n",
                 options.percent, options.contention ? "contention" : "single", options.threads, options.ops, spill,
                 target_occupied, occupied_owner_lines_before, occupied_owner_lines_after);
    return passed ? 0 : 1;
}

int runOccupancyCommand(int argc, char **argv)
{
    OccupancyOptions options;
    if (!parseOccupancyOptions(argc, argv, options)) {
        return 2;
    }
    spark::test::LinuxAllocationTestControl control;
    SeedPlan plan;
    spark::AllocationSampler sampler;
    spark::AllocationSamplerConfig config;
    config.session_seed = spark::currentNativeThreadId();
    config.all_threads = true;
    config.count_only = true;
    std::string error;
    bool sampler_started = false;
    int result = runOccupancyDiagnostic(sampler, config, control, plan, options, error, sampler_started);
    if (sampler_started || sampler.backendCleanupPending()) {
        if (!stopSessionSafely(sampler, error, "occupancy diagnostic")) {
            result = 1;
        }
    }
    if (!shutdownSamplerSafely(sampler, error, "occupancy diagnostic")) {
        result = 1;
    }
    return result;
}

struct LongRunOptions {
    std::size_t identities = 2048;
    std::size_t ops = 8;
};

struct LongRunGate {
    std::atomic<bool> ready{false};
    std::atomic<bool> go{false};
    std::atomic<bool> cancel{false};
    std::atomic<bool> finish{false};
    std::atomic<bool> ops_done{false};
    std::atomic<unsigned> done{0};
};

struct LongRunEvidence {
    std::uint64_t thread_pointer = 0;
    std::size_t claim_index = 0;
    std::uint64_t successful = 0;
    std::uint64_t bytes = 0;
};

struct LongRunWorkerArg {
    LongRunGate *gate = nullptr;
    LongRunEvidence *evidence = nullptr;
    std::size_t ops = 0;
};

struct StackMapping {
    void *mapping = MAP_FAILED;
    std::size_t mapping_size = 0;
    void *stack = nullptr;
    std::size_t stack_size = 0;
};

void *longRunWorker(void *raw)
{
    auto &arg = *static_cast<LongRunWorkerArg *>(raw);
    arg.evidence->thread_pointer = thisThreadPointer();
    arg.evidence->claim_index =
        spark::test::AllocationLifecycleTestAccess::shardIndexForThreadPointer(arg.evidence->thread_pointer);
    arg.gate->ready.store(true, std::memory_order_release);
    while (!arg.gate->go.load(std::memory_order_acquire)) {
        sched_yield();
    }
    if (arg.gate->cancel.load(std::memory_order_acquire)) {
        arg.gate->done.fetch_add(1, std::memory_order_release);
        return nullptr;
    }
    for (std::size_t i = 0; i < arg.ops; ++i) {
        void *allocation = allocateTouched();
        if (allocation != nullptr) {
            ++arg.evidence->successful;
            arg.evidence->bytes += KBytes;
            std::free(allocation);
        }
    }
    arg.gate->ops_done.store(true, std::memory_order_release);
    while (!arg.gate->finish.load(std::memory_order_acquire)) {
        sched_yield();
    }
    arg.gate->done.fetch_add(1, std::memory_order_release);
    return nullptr;
}

bool parseLongRunOptions(int argc, char **argv, LongRunOptions &options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string name(argv[index]);
        if (name == "--churn-long-run") {
            continue;
        }
        if (index + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", name.c_str());
            return false;
        }
        bool valid = false;
        const std::uint64_t parsed = parseUnsigned(argv[++index], valid);
        if (!valid || parsed == 0 || parsed > std::numeric_limits<std::size_t>::max()) {
            std::fprintf(stderr, "invalid value for %s\n", name.c_str());
            return false;
        }
        if (name == "--identities") {
            options.identities = static_cast<std::size_t>(parsed);
        }
        else if (name == "--ops") {
            options.ops = static_cast<std::size_t>(parsed);
        }
        else {
            std::fprintf(stderr, "unknown long-run option: %s\n", name.c_str());
            return false;
        }
    }
    const std::size_t spill = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    if (options.identities <= spill || options.identities > 4096 ||
        options.ops > std::numeric_limits<std::uint64_t>::max() / options.identities / KBytes) {
        std::fprintf(stderr, "long-run identities must be 1024..4096 and total byte accounting must fit uint64\n");
        return false;
    }
    return true;
}

int runChurnLongRun(spark::AllocationSampler &sampler, const LongRunOptions &options)
{
    const std::int64_t page_result = sysconf(_SC_PAGESIZE);
    if (page_result <= 0) {
        std::fprintf(stderr, "long-run could not determine the system page size\n");
        return 1;
    }
    const auto page_size = static_cast<std::size_t>(page_result);
    const auto minimum_stack = static_cast<std::size_t>(PTHREAD_STACK_MIN);
    const std::size_t desired_stack = std::max<std::size_t>(128 * 1024, minimum_stack);
    const std::size_t stack_size = ((desired_stack + page_size - 1) / page_size) * page_size;
    const std::size_t mapping_size = stack_size + page_size;

    std::vector<StackMapping> mappings;
    std::vector<LongRunEvidence> evidence(options.identities);
    std::vector<std::uint64_t> thread_pointers;
    std::vector<std::size_t> claim_indices;
    std::vector<std::uint64_t> observed_owners;
    mappings.reserve(options.identities);
    thread_pointers.reserve(options.identities);
    claim_indices.reserve(options.identities);
    observed_owners.reserve(options.identities);

    std::uint64_t observed_successful = 0;
    std::uint64_t observed_bytes = 0;
    std::uint64_t observed_hooks = 0;
    std::uint64_t actual_successful = 0;
    std::uint64_t actual_bytes = 0;
    std::uint64_t spill_hooks = 0;
    std::uint64_t spill_successful = 0;
    std::uint64_t spill_bytes = 0;
    bool spill_owner_clear = true;
    std::size_t interval_failures = 0;
    const std::size_t spill = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    const std::size_t occupied_owner_lines_before = countOccupiedOwnerLines(sampler);

    for (std::size_t index = 0; index < options.identities; ++index) {
        void *mapping = mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) {
            std::fprintf(stderr, "long-run stack mmap failed at identity %zu\n", index);
            interval_failures += 1;
            break;
        }
        if (mprotect(mapping, page_size, PROT_NONE) != 0) {
            (void)munmap(mapping, mapping_size);
            std::fprintf(stderr, "long-run stack guard setup failed at identity %zu\n", index);
            interval_failures += 1;
            break;
        }
        StackMapping stack_mapping{.mapping = mapping,
                                   .mapping_size = mapping_size,
                                   .stack = static_cast<unsigned char *>(mapping) + page_size,
                                   .stack_size = stack_size};
        mappings.push_back(stack_mapping);

        pthread_attr_t attributes;
        if (pthread_attr_init(&attributes) != 0) {
            std::fprintf(stderr, "long-run pthread_attr_init failed at identity %zu\n", index);
            interval_failures += 1;
            break;
        }
        const int set_stack_result = pthread_attr_setstack(&attributes, stack_mapping.stack, stack_mapping.stack_size);
        if (set_stack_result != 0) {
            (void)pthread_attr_destroy(&attributes);
            std::fprintf(stderr, "long-run pthread_attr_setstack failed at identity %zu: %d\n", index,
                         set_stack_result);
            interval_failures += 1;
            break;
        }

        LongRunGate gate;
        LongRunWorkerArg arg{.gate = &gate, .evidence = &evidence[index], .ops = options.ops};
        pthread_t thread{};
        const int create_result = pthread_create(&thread, &attributes, longRunWorker, &arg);
        (void)pthread_attr_destroy(&attributes);
        if (create_result != 0) {
            std::fprintf(stderr, "long-run pthread_create failed at identity %zu: %d\n", index, create_result);
            interval_failures += 1;
            break;
        }
        if (!waitAtLeast(gate.ready, true, "long-run worker readiness")) {
            gate.cancel.store(true, std::memory_order_release);
            gate.finish.store(true, std::memory_order_release);
            gate.go.store(true, std::memory_order_release);
            if (!waitAtLeast(gate.done, 1U, "long-run worker drain")) {
                abortTimedOutWorkers("long-run worker");
            }
            checkedJoin(thread, "long-run readiness failure");
            interval_failures += 1;
            break;
        }

        const Counters before = read(sampler);
        const auto spill_before = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, spill);
        gate.go.store(true, std::memory_order_release);
        if (!waitAtLeast(gate.ops_done, true, "long-run worker allocation completion")) {
            abortTimedOutWorkers("long-run worker");
        }
        const Counters after = read(sampler);
        const auto spill_after = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, spill);
        const auto line =
            spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, evidence[index].claim_index);
        gate.finish.store(true, std::memory_order_release);
        if (!waitAtLeast(gate.done, 1U, "long-run worker exit")) {
            abortTimedOutWorkers("long-run worker");
        }
        checkedJoin(thread, "long-run worker completion");

        const Counters delta = after - before;
        actual_successful += evidence[index].successful;
        actual_bytes += evidence[index].bytes;
        observed_hooks += delta.hooks;
        observed_successful += delta.successful;
        observed_bytes += delta.bytes;
        spill_hooks += spill_after.hooks - spill_before.hooks;
        spill_successful += spill_after.successful - spill_before.successful;
        spill_bytes += spill_after.bytes - spill_before.bytes;
        spill_owner_clear = spill_owner_clear && spill_after.owner == 0;
        thread_pointers.push_back(evidence[index].thread_pointer);
        claim_indices.push_back(evidence[index].claim_index);
        observed_owners.push_back(line.owner);
        if (evidence[index].thread_pointer == 0 || evidence[index].claim_index >= spill || line.owner == 0 ||
            evidence[index].successful != options.ops || evidence[index].bytes != options.ops * KBytes ||
            delta.successful != options.ops || delta.bytes != options.ops * KBytes || delta.hooks < delta.successful) {
            interval_failures += 1;
        }
    }
    const std::size_t occupied_owner_lines_after = countOccupiedOwnerLines(sampler);

    for (const auto &mapping : mappings) {
        (void)munmap(mapping.mapping, mapping.mapping_size);
    }
    const std::size_t identities_done = thread_pointers.size();
    const std::size_t distinct_identities = uniqueCount(thread_pointers);
    const std::size_t distinct_indices = uniqueCount(claim_indices);
    const std::size_t distinct_owners = uniqueCount(observed_owners);
    const std::uint64_t expected_successful = static_cast<std::uint64_t>(identities_done) * options.ops;
    const std::uint64_t expected_bytes = expected_successful * KBytes;
    const bool actual_workload_exact = actual_successful == expected_successful && actual_bytes == expected_bytes;
    const double coverage = actual_workload_exact && actual_successful != 0
                              ? static_cast<double>(observed_successful) / static_cast<double>(actual_successful)
                              : 0.0;
    const bool complete = identities_done == options.identities && distinct_identities == identities_done &&
                          actual_workload_exact && identities_done > spill && interval_failures == 0 &&
                          observed_successful == expected_successful && observed_bytes == expected_bytes &&
                          spill_hooks <= observed_hooks && spill_successful <= observed_successful &&
                          spill_bytes <= observed_bytes && spill_owner_clear;

    for (std::size_t index = 0; index < identities_done; ++index) {
        std::fprintf(stderr,
                     "long_run_identity sequence=%zu tp=0x%016llx claim_index=%zu "
                     "sampled_worker_line_owner=0x%016llx\n",
                     index, static_cast<unsigned long long>(evidence[index].thread_pointer),
                     evidence[index].claim_index, static_cast<unsigned long long>(observed_owners[index]));
    }
    std::printf("record_type,requested_identities,completed_identities,ops_per_identity,expected_successful,"
                "actual_successful,observed_successful,coverage,expected_bytes,actual_bytes,observed_bytes,hooks,"
                "spill_hooks,spill_successful,"
                "spill_bytes,spill_ratio,spill_owner_clear,"
                "distinct_thread_pointers,distinct_claim_indices,distinct_observed_owners,passed\n");
    std::printf(
        "data,%zu,%zu,%zu,%llu,%llu,%llu,%.9f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.9f,%u,%zu,%zu,%zu,%u\n",
        options.identities, identities_done, options.ops, static_cast<unsigned long long>(expected_successful),
        static_cast<unsigned long long>(actual_successful), static_cast<unsigned long long>(observed_successful),
        coverage, static_cast<unsigned long long>(expected_bytes), static_cast<unsigned long long>(actual_bytes),
        static_cast<unsigned long long>(observed_bytes), static_cast<unsigned long long>(observed_hooks),
        static_cast<unsigned long long>(spill_hooks), static_cast<unsigned long long>(spill_successful),
        static_cast<unsigned long long>(spill_bytes),
        observed_hooks == 0 ? 0.0 : static_cast<double>(spill_hooks) / static_cast<double>(observed_hooks),
        spill_owner_clear ? 1U : 0U, distinct_identities, distinct_indices, distinct_owners, complete ? 1U : 0U);
    std::fprintf(stderr,
                 "synthetic custom-stack churn: completed=%zu distinct identities=%zu indices=%zu "
                 "sampled_worker_line_owners=%zu "
                 "coverage=%.6f bytes=%llu spill_hooks=%llu; not a server-thread lifetime model%s\n",
                 identities_done, distinct_identities, distinct_indices, distinct_owners, coverage,
                 static_cast<unsigned long long>(observed_bytes), static_cast<unsigned long long>(spill_hooks),
                 complete ? " passed" : " failed");
    std::fprintf(stderr,
                 "owner_census phase=custom_stack_churn mode=custom_stack claimable_lines=%zu forced_seed_lines=0 "
                 "occupied_owner_lines_before=%zu occupied_owner_lines_after=%zu\n",
                 spill, occupied_owner_lines_before, occupied_owner_lines_after);
    return complete ? 0 : 1;
}

int runChurnLongRunCommand(int argc, char **argv)
{
    LongRunOptions options;
    if (!parseLongRunOptions(argc, argv, options)) {
        return 2;
    }
    spark::test::LinuxAllocationTestControl control;
    SeedPlan plan;
    spark::AllocationSampler sampler;
    spark::AllocationSamplerConfig config;
    config.session_seed = spark::currentNativeThreadId();
    config.all_threads = true;
    config.count_only = true;
    std::string error;
    resetSeedPlan(plan, sampler, {thisThreadPointer()});
    const bool started = startSeededSampler(sampler, config, control, plan, error);
    if (!started || !callbackEvidenceValid(plan)) {
        std::fprintf(stderr, "long-run sampler start failed: %s\n", error.c_str());
        if (started || sampler.backendCleanupPending()) {
            stopSessionSafely(sampler, error, "long-run setup failure");
        }
        shutdownSamplerSafely(sampler, error, "long-run setup failure");
        return 1;
    }
    int result = runChurnLongRun(sampler, options);
    if (!stopSessionSafely(sampler, error, "long-run sampler")) {
        result = 1;
    }
    if (!shutdownSamplerSafely(sampler, error, "long-run sampler")) {
        result = 1;
    }
    return result;
}

// Arm A1: foreign-owned lines remain frozen while the matching workers allocate.
struct QuarantineSlot {
    std::atomic<bool> ready{false};
    std::uint64_t thread_pointer = 0;
    std::size_t index = 0;
};

QuarantineSlot QuarantineSlots[KThreads];

struct QuarantineArg {
    Gate *gate;
    unsigned worker = 0;
    std::size_t ops = 0;
    std::uint64_t successful = 0;
    std::uint64_t bytes = 0;
};

void *quarantineWorker(void *raw)
{
    auto &arg = *static_cast<QuarantineArg *>(raw);
    QuarantineSlot &slot = QuarantineSlots[arg.worker];
    const std::uint64_t tp = thisThreadPointer();
    const std::size_t index = spark::test::AllocationLifecycleTestAccess::shardIndexForThreadPointer(tp);
    slot.thread_pointer = tp;
    slot.index = index;
    arg.gate->arrived.fetch_add(1, std::memory_order_release);
    slot.ready.store(true, std::memory_order_release);
    while (!arg.gate->go.load(std::memory_order_acquire)) {
        sched_yield();
    }
    if (arg.gate->cancel.load(std::memory_order_acquire)) {
        arg.gate->done.fetch_add(1, std::memory_order_release);
        return nullptr;
    }
    for (std::size_t i = 0; i < arg.ops; ++i) {
        void *p = allocateTouched();
        if (p != nullptr) {
            ++arg.successful;
            arg.bytes += KBytes;
            std::free(p);
        }
    }
    arg.gate->done.fetch_add(1, std::memory_order_release);
    return nullptr;
}

int checkQuarantineFreeze(spark::AllocationSampler &sampler, const spark::AllocationSamplerConfig &config,
                          spark::test::LinuxAllocationTestControl &control, SeedPlan &plan, std::string &error)
{
    for (auto &quarantine_slot : QuarantineSlots) {
        quarantine_slot.ready.store(false, std::memory_order_relaxed);
        quarantine_slot.thread_pointer = 0;
        quarantine_slot.index = 0;
    }

    const std::size_t spill = spark::test::AllocationLifecycleTestAccess::spillShardIndex();

    Gate gate;
    std::vector<QuarantineArg> args;
    std::vector<pthread_t> threads;
    args.reserve(KThreads);
    threads.reserve(KThreads);
    for (unsigned i = 0; i < KThreads; ++i) {
        args.push_back({&gate, i, KOps});
    }
    for (unsigned i = 0; i < KThreads; ++i) {
        pthread_t t{};
        if (pthread_create(&t, nullptr, quarantineWorker, &args[i]) != 0) {
            std::fprintf(stderr, "quarantine freeze: pthread_create failed\n");
            cancelAndJoinWorkers(gate, threads, "quarantine creation cancellation");
            return 1;
        }
        threads.push_back(t);
    }

    for (auto &quarantine_slot : QuarantineSlots) {
        if (!waitAtLeast(quarantine_slot.ready, true, "quarantine worker readiness")) {
            cancelAndJoinWorkers(gate, threads, "quarantine readiness cancellation");
            return 1;
        }
    }
    waitFor(gate.arrived, KThreads);

    std::vector<std::size_t> indices;
    std::vector<std::uint64_t> live{thisThreadPointer()};
    for (auto &quarantine_slot : QuarantineSlots) {
        live.push_back(quarantine_slot.thread_pointer);
        const std::size_t index = quarantine_slot.index;
        std::size_t slot = 0;
        while (slot < indices.size() && indices[slot] != index) {
            ++slot;
        }
        if (slot == indices.size()) {
            indices.push_back(index);
        }
    }
    std::vector<spark::test::HotShardLineForTesting> pre;
    pre.reserve(indices.size());
    std::vector<std::uint64_t> owners;
    owners.reserve(indices.size());
    resetSeedPlan(plan, sampler, live);
    if (!addSeedEntries(plan, indices)) {
        std::fprintf(stderr, "quarantine freeze: seed plan construction failed\n");
        cancelAndJoinWorkers(gate, threads, "quarantine plan cancellation");
        return 1;
    }
    const bool started = startSeededSampler(sampler, config, control, plan, error);
    if (!started || !callbackEvidenceValid(plan)) {
        std::fprintf(stderr, "quarantine freeze: pre-admission setup failed: %s\n", error.c_str());
        cancelAndJoinWorkers(gate, threads, "quarantine pre-admission cancellation");
        if (started || sampler.backendCleanupPending()) {
            stopSessionSafely(sampler, error, "quarantine setup failure");
        }
        return 1;
    }
    for (std::size_t index : indices) {
        pre.push_back(spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, index));
        auto entry = std::ranges::find_if(plan.entries,
                                          [index](const SeedEntry &candidate) { return candidate.index == index; });
        if (entry == plan.entries.end()) {
            cancelAndJoinWorkers(gate, threads, "quarantine plan mismatch");
            stopSessionSafely(sampler, error, "quarantine plan mismatch");
            return 1;
        }
        owners.push_back(entry->owner);
    }
    const Counters before = read(sampler);
    const auto spill_before = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, spill);
    auto finish = [&](int result) {
        return stopSessionSafely(sampler, error, "quarantine freeze") ? result : 1;
    };

    gate.go.store(true, std::memory_order_release);
    if (!waitAtLeast(gate.done, KThreads, "quarantine worker completion")) {
        abortTimedOutWorkers("quarantine worker completion");
    }
    checkedJoinAll(threads, "quarantine worker completion");

    std::uint64_t actual_successful = 0;
    std::uint64_t actual_bytes = 0;
    for (const QuarantineArg &arg : args) {
        actual_successful += arg.successful;
        actual_bytes += arg.bytes;
    }
    const std::uint64_t expected_successful = static_cast<std::uint64_t>(KThreads) * KOps;
    if (actual_successful != expected_successful || actual_bytes != expected_successful * KBytes) {
        std::fprintf(
            stderr, "quarantine freeze: workload allocations incomplete: successful %llu/%llu bytes %llu/%llu\n",
            static_cast<unsigned long long>(actual_successful), static_cast<unsigned long long>(expected_successful),
            static_cast<unsigned long long>(actual_bytes),
            static_cast<unsigned long long>(expected_successful) * KBytes);
        return finish(1);
    }

    for (std::size_t j = 0; j < indices.size(); ++j) {
        const auto post = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, indices[j]);
        if (post.hooks != pre[j].hooks || post.successful != pre[j].successful || post.bytes != pre[j].bytes) {
            std::fprintf(stderr,
                         "quarantine freeze: foreign-owned line %zu moved: hooks %llu successful %llu bytes %llu\n",
                         indices[j], static_cast<unsigned long long>(post.hooks - pre[j].hooks),
                         static_cast<unsigned long long>(post.successful - pre[j].successful),
                         static_cast<unsigned long long>(post.bytes - pre[j].bytes));
            return finish(1);
        }
        if (post.owner != owners[j]) {
            std::fprintf(stderr, "quarantine freeze: claim index %zu lost its foreign owner\n", indices[j]);
            return finish(1);
        }
    }

    const Counters got = read(sampler) - before;
    const auto spill_after = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, spill);
    if (got.bytes == 0) {
        std::fprintf(stderr, "quarantine freeze: no observed bytes, the arm captured nothing\n");
        return finish(1);
    }
    if (spill_after.owner != 0) {
        std::fprintf(stderr, "quarantine freeze: the spill line accepted owner %llu\n",
                     static_cast<unsigned long long>(spill_after.owner));
        return finish(1);
    }
    const std::uint64_t spill_successful = spill_after.successful - spill_before.successful;
    if (got.successful < spill_successful) {
        std::fprintf(stderr, "quarantine freeze: total %llu below spill %llu\n",
                     static_cast<unsigned long long>(got.successful),
                     static_cast<unsigned long long>(spill_successful));
        return finish(1);
    }
    std::fprintf(stderr,
                 "quarantine freeze: %zu foreign-owned claim lines frozen, spill %llu of total %llu successful\n",
                 indices.size(), static_cast<unsigned long long>(spill_successful),
                 static_cast<unsigned long long>(got.successful));
    return finish(0);
}

// Arm A3: census at quiesce - shape and absence, with the per-line ratio kept as smoke only.
int checkCensusAtQuiesce(spark::AllocationSampler &sampler)
{
    const std::size_t count = spark::test::AllocationLifecycleTestAccess::hotShardCount();
    const std::size_t spill = spark::test::AllocationLifecycleTestAccess::spillShardIndex();
    std::uint64_t hooks = 0;
    std::uint64_t successful = 0;
    std::uint64_t bytes = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto line = spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, index);
        hooks += line.hooks;
        successful += line.successful;
        bytes += line.bytes;
        if (index >= spill) {
            continue;
        }
        if (line.owner == 0 && (line.hooks != 0 || line.successful != 0 || line.bytes != 0)) {
            std::fprintf(stderr, "census: counts sit on unowned claim line %zu\n", index);
            return 1;
        }
    }
    if (hooks != sampler.hookCalls()) {
        std::fprintf(stderr, "census: line hooks %llu != aggregate %llu\n", static_cast<unsigned long long>(hooks),
                     static_cast<unsigned long long>(sampler.hookCalls()));
        return 1;
    }
    if (successful != sampler.successfulAllocationCalls()) {
        std::fprintf(stderr, "census: line successful %llu != aggregate %llu\n",
                     static_cast<unsigned long long>(successful),
                     static_cast<unsigned long long>(sampler.successfulAllocationCalls()));
        return 1;
    }
    if (bytes != sampler.observedBytes()) {
        std::fprintf(stderr, "census: line bytes %llu != aggregate %llu\n", static_cast<unsigned long long>(bytes),
                     static_cast<unsigned long long>(sampler.observedBytes()));
        return 1;
    }
    if (spark::test::AllocationLifecycleTestAccess::hotShardLine(sampler, spill).owner != 0) {
        std::fprintf(stderr, "census: the spill line is owned at quiesce\n");
        return 1;
    }
    std::fprintf(stderr, "census at quiesce: %zu lines sum to the aggregates, no counts on an ownerless line\n", count);
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc > 1) {
        const std::string command(argv[1]);
        if (command == "--occupancy-diagnostic") {
            return runOccupancyCommand(argc, argv);
        }
        if (command == "--churn-long-run") {
            return runChurnLongRunCommand(argc, argv);
        }
        std::fprintf(stderr, "unknown command mode: %s\n", argv[1]);
        return 2;
    }

    spark::test::LinuxAllocationTestControl control;
    SeedPlan plan;
    spark::AllocationSampler sampler;
    spark::AllocationSamplerConfig config;
    config.session_seed = spark::currentNativeThreadId();
    config.all_threads = true;
    config.count_only = true;

    std::string error;
    int failures = 0;

    Counters per_pair{};
    failures += checkIndexDomain();
    failures += checkFakeIdentityConstruction();
    failures += checkCallbackFailure(sampler, config, control, plan, error);
    failures += checkSeamRejections(sampler, config, control, plan, error);

    resetSeedPlan(plan, sampler, {thisThreadPointer()});
    const bool initial_started = startSeededSampler(sampler, config, control, plan, error);
    if (!initial_started || !callbackEvidenceValid(plan)) {
        std::fprintf(stderr, "initial sampler start failed: %s\n", error.c_str());
        if (initial_started || sampler.backendCleanupPending()) {
            stopSessionSafely(sampler, error, "initial sampler setup failure");
        }
        shutdownSamplerSafely(sampler, error, "initial sampler setup failure");
        return 1;
    }
    failures += calibrate(sampler, per_pair);
    failures += runBatch(sampler, config, control, plan, false, "natural claim");
    failures += checkCensusAtQuiesce(sampler);
    if (!stopSessionSafely(sampler, error, "initial sampler")) {
        failures += 1;
    }

    failures += runBatch(sampler, config, control, plan, true, "forced spill");
    failures += runMonotoneWhileSpilling(sampler, config, control, plan);

    resetSeedPlan(plan, sampler, {thisThreadPointer()});
    const bool churn_started = startSeededSampler(sampler, config, control, plan, error);
    if (!churn_started || !callbackEvidenceValid(plan)) {
        std::fprintf(stderr, "churn sampler start failed: %s\n", error.c_str());
        if (churn_started || sampler.backendCleanupPending()) {
            stopSessionSafely(sampler, error, "churn sampler setup failure");
        }
        failures += 1;
    }
    else {
        failures += checkChurn(sampler);
        if (!stopSessionSafely(sampler, error, "churn sampler")) {
            failures += 1;
        }
    }
    failures += checkQuarantineFreeze(sampler, config, control, plan, error);
    RestartEvidence restart_evidence;
    failures += runRestartA(sampler, config, control, plan, restart_evidence, error);
    failures += runRestartB(sampler, config, control, plan, restart_evidence, error);
    if (!shutdownSamplerSafely(sampler, error, "ownership test")) {
        return 1;
    }

    if (failures != 0) {
        std::fprintf(stderr, "shard ownership oracle: %d check(s) failed\n", failures);
        return 1;
    }
    std::fprintf(stderr, "shard ownership oracle: all checks passed\n");
    return 0;
}
