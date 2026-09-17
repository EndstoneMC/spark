#ifndef ENDSTONE_SPARK_THREAD_GROUPER_H
#define ENDSTONE_SPARK_THREAD_GROUPER_H

#include <charconv>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "core/profiler/profile_mode.h"

namespace spark {

enum class NativeThreadLabelKind : std::uint8_t {
    Execution,
    Allocation,
};

// Groups threads for the exported call-tree view.  Matches upstream spark's
// ThreadGrouper: BY_POOL extracts a pool name from thread names ending in a
// number, BY_NAME keeps each thread separate, AS_ONE merges everything.
class ThreadGrouper {
public:
    using GroupKey = std::pair<std::string, std::uint64_t>;

    explicit ThreadGrouper(ThreadGrouperMode mode) : mode_(mode) {}

    GroupKey groupKey(std::uint64_t tid, std::string_view name)
    {
        return {group(tid, name), mode_ == ThreadGrouperMode::ByName ? tid : 0};
    }

    GroupKey groupKeyForNativeLabel(std::uint64_t tid, std::string_view name, NativeThreadLabelKind kind)
    {
        if (mode_ != ThreadGrouperMode::ByPool) {
            return groupKey(tid, name);
        }
        const std::size_t marker = name.rfind(" (#");
        if (marker == std::string_view::npos) {
            return groupKey(tid, name);
        }
        const auto native_name = nativeName(tid, name, kind);
        if (!native_name || !poolName(*native_name)) {
            return {std::string(name), 0};
        }
        return groupKey(tid, *native_name);
    }

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
        std::string n(name);
        const auto pool = poolName(n);
        if (!pool) {
            return n;
        }
        std::string g = *pool;
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
    static bool decimal(std::string_view value, std::uint64_t &result)
    {
        if (value.empty()) {
            return false;
        }
        for (const char ch : value) {
            if (ch < '0' || ch > '9') {
                return false;
            }
        }
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
    }

    static std::optional<std::string> poolName(std::string_view name)
    {
        static const std::regex kPoolPattern(R"(^(.*?)[-# ]+\d+$)");
        std::string n(name);
        std::smatch m;
        if (!std::regex_match(n, m, kPoolPattern)) {
            return std::nullopt;
        }
        std::string g = m[1].str();
        while (!g.empty() && g.back() == ' ') {
            g.pop_back();
        }
        return g;
    }

    static std::optional<std::string> nativeName(std::uint64_t identity, std::string_view name,
                                                 NativeThreadLabelKind kind)
    {
        const std::size_t marker = name.rfind(" (#");
        if (marker == std::string_view::npos || name.empty() || name.back() != ')') {
            return std::nullopt;
        }
        const std::string_view suffix = name.substr(marker + 3, name.size() - marker - 4);
        std::uint64_t parsed_identity = 0;
        if (kind == NativeThreadLabelKind::Execution) {
            if (!decimal(suffix, parsed_identity) || parsed_identity != identity) {
                return std::nullopt;
            }
            return std::string(name.substr(0, marker));
        }

        constexpr std::string_view kSessionSeparator = ", session #";
        const std::size_t separator = suffix.find(kSessionSeparator);
        if (separator == std::string_view::npos) {
            return std::nullopt;
        }
        std::uint64_t os_identity = 0;
        const std::string_view os_id = suffix.substr(0, separator);
        const std::string_view session_id = suffix.substr(separator + kSessionSeparator.size());
        if (!decimal(os_id, os_identity) || !decimal(session_id, parsed_identity) || parsed_identity != identity) {
            return std::nullopt;
        }
        return std::string(name.substr(0, marker));
    }

    ThreadGrouperMode mode_;
    std::unordered_map<std::uint64_t, std::string> cache_;
    std::unordered_map<std::string, std::unordered_set<std::uint64_t>> pool_members_;
    std::unordered_set<std::uint64_t> seen_;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_THREAD_GROUPER_H
