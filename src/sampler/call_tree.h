#ifndef ENDSTONE_SPARK_CALL_TREE_H
#define ENDSTONE_SPARK_CALL_TREE_H

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "sampler/types.h"

namespace spark {

// Aggregated sampling stack for a single thread. Mirrors spark's ThreadNode/
// StackTraceNode tree: each node accumulates a sample count per time window.
class CallTree {
public:
    struct Node {
        FrameKey key{};
        std::map<std::int32_t, std::uint64_t> times;  // window -> sample count
        std::unordered_map<FrameKey, std::unique_ptr<Node>, FrameKeyHash> children;
    };

    // Log one sample (frames ordered leaf..root) with the given weight into a window.
    void log(const std::vector<FrameKey> &frames, std::int32_t window, std::uint64_t weight = 1);

    const Node &root() const
    {
        return root_;
    }

    bool empty() const
    {
        return root_.times.empty();
    }

    // Total samples logged (== root's summed counts across windows).
    std::uint64_t sampleCount() const;

private:
    static constexpr int kMaxDepth = 300;  // spark.maxStackDepth default
    Node root_;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_CALL_TREE_H
