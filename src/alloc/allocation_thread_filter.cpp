#include "alloc/allocation_thread_filter.h"

#include <optional>
#include <utility>

#include "sampler/thread_info.h"

namespace spark {

AllocationThreadFilter::AllocationThreadFilter(
    std::uint64_t maximum_named_roots, std::size_t maximum_cached_identities)
    : maximum_named_roots_(maximum_named_roots),
      maximum_cached_identities_(maximum_cached_identities)
{
}

bool AllocationThreadFilter::configure(
    bool all_threads, bool regex_threads,
    const std::vector<std::string> &patterns, std::string &error)
{
    clear();
    return selector_.configure(all_threads, regex_threads, patterns, error);
}

void AllocationThreadFilter::clear()
{
    identities_.clear();
    name_failures_.store(0, std::memory_order_relaxed);
    cache_drops_.store(0, std::memory_order_relaxed);
}

AllocationThreadSelection AllocationThreadFilter::resolve(
    std::uint64_t session_thread_id, std::uint64_t os_thread_id)
{
    auto cached = identities_.find(session_thread_id);
    if (cached != identities_.end()) {
        return cached->second;
    }

    AllocationThreadSelection result;
    result.profile_thread_id =
        session_thread_id <= maximum_named_roots_ ? session_thread_id : 0;

    if (identities_.size() >= maximum_cached_identities_) {
        cache_drops_.fetch_add(1, std::memory_order_relaxed);
        result.selected = selector_.selectsAll();
        result.display_name =
            result.profile_thread_id == 0
                ? "<other threads>"
                : "Thread " + std::to_string(os_thread_id) + " (#" +
                      std::to_string(os_thread_id) + ", session #" +
                      std::to_string(session_thread_id) + ")";
        return result;
    }

    std::optional<std::string> native_name = tryNativeThreadName(os_thread_id);
    result.name_available = native_name.has_value();
    if (!result.name_available) {
        name_failures_.fetch_add(1, std::memory_order_relaxed);
    }

    result.selected =
        selector_.selectsAll() ||
        (native_name && selector_.matches(*native_name));

    if (result.profile_thread_id == 0) {
        result.display_name = "<other threads>";
    }
    else {
        const std::string name =
            native_name ? std::move(*native_name)
                        : "Thread " + std::to_string(os_thread_id);
        result.display_name =
            name + " (#" + std::to_string(os_thread_id) + ", session #" +
            std::to_string(session_thread_id) + ")";
    }

    identities_.emplace(session_thread_id, result);
    return result;
}

}  // namespace spark
