#include "native/sampler/call_tree.h"

#include <limits>

namespace spark {

void CallTree::log(const std::vector<FrameKey> &frames, std::int32_t window, std::uint64_t weight)
{
    std::size_t unlimited = std::numeric_limits<std::size_t>::max();
    logBounded(frames, window, weight, unlimited);
}

bool CallTree::logBounded(const std::vector<FrameKey> &frames, std::int32_t window, std::uint64_t weight,
                          std::size_t &remaining_nodes)
{
    if (frames.empty()) {
        return true;
    }

    root_.times[window] += weight;

    Node *node = &root_;
    int depth = 0;
    // frames are leaf..root; descend the tree root->leaf, i.e. reverse order.
    for (auto it = frames.rbegin(); it != frames.rend() && depth < kMaxDepth; ++it, ++depth) {
        auto child = node->children.find(*it);
        if (child == node->children.end()) {
            if (remaining_nodes == 0) {
                return false;
            }
            auto inserted = std::make_unique<Node>();
            inserted->key = *it;
            child = node->children.emplace(*it, std::move(inserted)).first;
            --remaining_nodes;
        }
        node = child->second.get();
        node->times[window] += weight;
    }
    return true;
}

std::uint64_t CallTree::sampleCount() const
{
    std::uint64_t total = 0;
    for (const auto &[window, count] : root_.times) {
        total += count;
    }
    return total;
}

namespace {

bool pruneNode(CallTree::Node &node, std::int32_t minimum_window)  // NOLINT(misc-no-recursion)
{
    node.times.erase(node.times.begin(), node.times.lower_bound(minimum_window));
    for (auto it = node.children.begin(); it != node.children.end();) {
        if (pruneNode(*it->second, minimum_window)) {
            it = node.children.erase(it);
        }
        else {
            ++it;
        }
    }
    return node.times.empty() && node.children.empty();
}

}  // namespace

bool CallTree::pruneBefore(std::int32_t minimum_window)
{
    return pruneNode(root_, minimum_window);
}

namespace {

void mergeNode(CallTree::Node &dst, const CallTree::Node &src)  // NOLINT(misc-no-recursion)
{
    for (const auto &[window, count] : src.times) {
        dst.times[window] += count;
    }
    for (const auto &[key, child] : src.children) {
        auto it = dst.children.find(key);
        if (it == dst.children.end()) {
            auto inserted = std::make_unique<CallTree::Node>();
            inserted->key = key;
            it = dst.children.emplace(key, std::move(inserted)).first;
        }
        mergeNode(*it->second, *child);
    }
}

}  // namespace

void mergeCallTree(CallTree &dst, const CallTree &src)
{
    mergeNode(dst.root(), src.root());
}

}  // namespace spark
