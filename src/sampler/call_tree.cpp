#include "sampler/call_tree.h"

namespace spark {

void CallTree::log(const std::vector<FrameKey> &frames, std::int32_t window, std::uint64_t weight)
{
    if (frames.empty()) {
        return;
    }

    root_.times[window] += weight;

    Node *node = &root_;
    int depth = 0;
    // frames are leaf..root; descend the tree root->leaf, i.e. reverse order.
    for (auto it = frames.rbegin(); it != frames.rend() && depth < kMaxDepth; ++it, ++depth) {
        auto &child = node->children[*it];
        if (!child) {
            child = std::make_unique<Node>();
            child->key = *it;
        }
        node = child.get();
        node->times[window] += weight;
    }
}

std::uint64_t CallTree::sampleCount() const
{
    std::uint64_t total = 0;
    for (const auto &[window, count] : root_.times) {
        total += count;
    }
    return total;
}

}  // namespace spark
