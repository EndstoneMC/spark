#ifndef ENDSTONE_SPARK_CALL_TREE_H
#define ENDSTONE_SPARK_CALL_TREE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "native/sampler/types.h"

namespace spark {

// Aggregated profiling stack. Each node accumulates a mode-dependent weight per
// time window: execution microseconds or allocation bytes.
class CallTree {
public:
    struct Node {
        FrameKey key{};
        std::map<std::int32_t, std::uint64_t> times;  // window -> profile weight
        std::unordered_map<FrameKey, std::unique_ptr<Node>, FrameKeyHash> children;
    };

    // Log one sample (frames ordered leaf..root) with the given weight into a window.
    void log(const std::vector<FrameKey> &frames, std::int32_t window, std::uint64_t weight = 1);
    bool logBounded(const std::vector<FrameKey> &frames, std::int32_t window, std::uint64_t weight,
                    std::size_t &remaining_nodes);

    const Node &root() const { return root_; }
    Node &root() { return root_; }

    bool empty() const { return root_.times.empty(); }

    // Total profile weight logged (execution microseconds or allocation bytes).
    std::uint64_t sampleCount() const;

private:
    static constexpr int kMaxDepth = 300;  // spark.maxStackDepth default
    Node root_;
};

// Merge `src` into `dst`, summing weights at matching nodes.
void mergeCallTree(CallTree &dst, const CallTree &src);

}  // namespace spark

#endif  // ENDSTONE_SPARK_CALL_TREE_H
