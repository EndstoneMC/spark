#ifndef ENDSTONE_SPARK_THREAD_GROUPER_H
#define ENDSTONE_SPARK_THREAD_GROUPER_H

#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "core/profiler/profile_mode.h"

namespace spark {

// Groups threads for the exported call-tree view.  Matches upstream spark's
// ThreadGrouper: BY_POOL extracts a pool name from thread names ending in a
// number, BY_NAME keeps each thread separate, AS_ONE merges everything.
class ThreadGrouper {
public:
    explicit ThreadGrouper(ThreadGrouperMode mode) : mode_(mode) {}

    std::string group(std::uint64_t tid, std::string_view name)
    {
        if (mode_ == ThreadGrouperMode::AsOne) {
            seen_.insert(tid);
            return "root";
        }
        if (mode_ == ThreadGrouperMode::ByName) {
            return std::string(name);
        }
        // ByPool
        auto it = cache_.find(tid);
        if (it != cache_.end()) {
            return it->second;
        }
        static const std::regex kPoolPattern(R"(^(.*?)[-# ]+\d+$)");
        std::string n(name);
        std::smatch m;
        std::string g = std::regex_match(n, m, kPoolPattern) ? m[1].str() : n;
        // trim trailing spaces
        while (!g.empty() && g.back() == ' ') g.pop_back();
        cache_[tid] = g;
        pool_members_[g].insert(tid);
        return g;
    }

    std::string label(std::string_view g) const
    {
        if (mode_ == ThreadGrouperMode::AsOne) {
            return "All (x" + std::to_string(seen_.size()) + ")";
        }
        if (mode_ == ThreadGrouperMode::ByPool) {
            std::string gs(g);
            auto it = pool_members_.find(gs);
            if (it != pool_members_.end() && !it->second.empty()) {
                return gs + " (x" + std::to_string(it->second.size()) + ")";
            }
        }
        return std::string(g);
    }

private:
    ThreadGrouperMode mode_;
    std::unordered_map<std::uint64_t, std::string> cache_;
    std::unordered_map<std::string, std::unordered_set<std::uint64_t>> pool_members_;
    std::unordered_set<std::uint64_t> seen_;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_THREAD_GROUPER_H
