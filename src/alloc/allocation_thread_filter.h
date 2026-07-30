#ifndef ENDSTONE_SPARK_ALLOCATION_THREAD_FILTER_H
#define ENDSTONE_SPARK_ALLOCATION_THREAD_FILTER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "sampler/thread_selector.h"

namespace spark {

struct AllocationThreadSelection {
    std::uint64_t profile_thread_id = 0;
    std::string display_name;
    bool selected = false;
    bool name_available = false;
};

// Resolves allocation-origin thread names and applies the same matching rules as
// the execution sampler. This object is used only by allocation aggregators, never
// allocator hooks. A monotonic session identity is the cache key; an OS TID is
// only used for the initial name query.
class AllocationThreadFilter {
public:
    AllocationThreadFilter(std::uint64_t maximum_named_roots,
                           std::size_t maximum_cached_identities);

    bool configure(bool all_threads, bool regex_threads,
                   const std::vector<std::string> &patterns, std::string &error);
    void clear();

    AllocationThreadSelection resolve(std::uint64_t session_thread_id,
                                      std::uint64_t os_thread_id);

    std::uint64_t nameFailures() const noexcept
    {
        return name_failures_.load(std::memory_order_relaxed);
    }
    std::uint64_t cacheDrops() const noexcept
    {
        return cache_drops_.load(std::memory_order_relaxed);
    }
    bool selectsAll() const noexcept { return selector_.selectsAll(); }

private:
    std::uint64_t maximum_named_roots_;
    std::size_t maximum_cached_identities_;
    ThreadSelector selector_;
    std::unordered_map<std::uint64_t, AllocationThreadSelection> identities_;
    std::atomic<std::uint64_t> name_failures_{0};
    std::atomic<std::uint64_t> cache_drops_{0};
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_ALLOCATION_THREAD_FILTER_H
