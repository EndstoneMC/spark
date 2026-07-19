#ifndef ENDSTONE_SPARK_TYPES_H
#define ENDSTONE_SPARK_TYPES_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spark {

using ModuleId = std::uint32_t;
inline constexpr ModuleId kInvalidModule = 0xffffffffu;

// A stack frame identified by its module and module-relative address (RVA). This
// is stable across the run and is all we need to aggregate; symbol resolution is
// deferred to export time.
struct FrameKey {
    ModuleId module = kInvalidModule;
    std::uint64_t rva = 0;
    std::uint64_t raw_address = 0;  // runtime PC (for cpptrace resolution); not part of identity

    bool operator==(const FrameKey &o) const noexcept
    {
        return module == o.module && rva == o.rva;
    }
};

struct FrameKeyHash {
    std::size_t operator()(const FrameKey &k) const noexcept
    {
        std::uint64_t h = k.rva + 0x9e3779b97f4a7c15ULL + (static_cast<std::uint64_t>(k.module) << 1);
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ULL;
        h ^= h >> 27;
        h *= 0x94d049bb133111ebULL;
        h ^= h >> 31;
        return static_cast<std::size_t>(h);
    }
};

// Interns module path strings to small ids. Written only by the sampler thread.
class ModuleTable {
public:
    ModuleId intern(std::string_view path)
    {
        for (ModuleId i = 0; i < paths_.size(); ++i) {
            if (paths_[i] == path) {
                return i;
            }
        }
        paths_.emplace_back(path);
        return static_cast<ModuleId>(paths_.size() - 1);
    }

    const std::string &path(ModuleId id) const
    {
        return paths_.at(id);
    }

    std::size_t size() const
    {
        return paths_.size();
    }

private:
    std::vector<std::string> paths_;
};

// One captured stack, ordered leaf (index 0) -> root.
struct Sample {
    std::vector<FrameKey> frames;
    std::uint64_t thread_id = 0;
    std::string thread_name;
    std::int32_t window = 0;
    std::uint64_t tick_id = 0;
    std::uint64_t weight = 1;  // execution sample count or allocation bytes
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_TYPES_H
