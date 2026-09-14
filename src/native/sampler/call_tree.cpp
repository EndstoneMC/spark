#include "native/sampler/call_tree.h"

#include <limits>

namespace spark {

void CallTree::log(const std::vector<FrameKey> &frames, std::int32_t window, std::uint64_t weight)
{
    std::size_t unlimited = std::numeric_limits<std::size_t>::max();
    logBounded(frames, window, weight, unlimited, unlimited);
}

bool CallTree::logBounded(const std::vector<FrameKey> &frames, std::int32_t window, std::uint64_t weight,
                          std::size_t &remaining_nodes)
{
    std::size_t unlimited = std::numeric_limits<std::size_t>::max();
    return logBounded(frames, window, weight, remaining_nodes, unlimited);
}

bool CallTree::logBounded(const std::vector<FrameKey> &frames, std::int32_t window, std::uint64_t weight,
                          std::size_t &remaining_nodes, std::size_t &remaining_time_entries)
{
    if (frames.empty()) {
        return true;
    }

    const StorageUsage required = requiredStorage(frames, window);
    return logBoundedPrevalidated(frames, window, weight, required, remaining_nodes, remaining_time_entries);
}

bool CallTree::logBoundedPrevalidated(const std::vector<FrameKey> &frames, std::int32_t window, std::uint64_t weight,
                                      const StorageUsage &required, std::size_t &remaining_nodes,
                                      std::size_t &remaining_time_entries)
{
    if (frames.empty()) {
        return true;
    }

    if (required.child_nodes > remaining_nodes || required.time_entries > remaining_time_entries) {
        return false;
    }

    remaining_nodes -= required.child_nodes;
    remaining_time_entries -= required.time_entries;
    root_.times[window] += weight;

    Node *node = &root_;
    int depth = 0;
    // frames are leaf..root; descend the tree root->leaf, i.e. reverse order.
    for (auto it = frames.rbegin(); it != frames.rend() && depth < kMaxDepth; ++it, ++depth) {
        auto child = node->children.find(*it);
        if (child == node->children.end()) {
            auto inserted = std::make_unique<Node>();
            inserted->key = *it;
            child = node->children.emplace(*it, std::move(inserted)).first;
        }
        node = child->second.get();
        node->times[window] += weight;
    }
    return true;
}

CallTree::StorageUsage CallTree::requiredStorage(const std::vector<FrameKey> &frames, std::int32_t window) const
{
    StorageUsage required;
    if (frames.empty()) {
        return required;
    }

    const Node *node = &root_;
    if (!node->times.contains(window)) {
        ++required.time_entries;
    }
    int depth = 0;
    bool missing_parent = false;
    // frames are leaf..root; descend the tree root->leaf, i.e. reverse order.
    for (auto it = frames.rbegin(); it != frames.rend() && depth < kMaxDepth; ++it, ++depth) {
        if (missing_parent) {
            ++required.child_nodes;
            ++required.time_entries;
            continue;
        }
        auto child = node->children.find(*it);
        if (child == node->children.end()) {
            ++required.child_nodes;
            ++required.time_entries;
            missing_parent = true;
            continue;
        }
        node = child->second.get();
        if (!node->times.contains(window)) {
            ++required.time_entries;
        }
    }
    return required;
}

namespace {

CallTree::StorageUsage measureNode(const CallTree::Node &node)  // NOLINT(misc-no-recursion)
{
    CallTree::StorageUsage usage;
    usage.time_entries = node.times.size();
    for (const auto &[key, child] : node.children) {
        (void)key;
        ++usage.child_nodes;
        const CallTree::StorageUsage child_usage = measureNode(*child);
        usage.child_nodes += child_usage.child_nodes;
        usage.time_entries += child_usage.time_entries;
    }
    return usage;
}

struct PruneResult {
    CallTree::StorageUsage released;
    bool empty = false;
};

PruneResult pruneNode(CallTree::Node &node, std::int32_t minimum_window)  // NOLINT(misc-no-recursion)
{
    PruneResult result;
    for (auto it = node.times.begin(); it != node.times.lower_bound(minimum_window);) {
        it = node.times.erase(it);
        ++result.released.time_entries;
    }
    for (auto it = node.children.begin(); it != node.children.end();) {
        PruneResult child = pruneNode(*it->second, minimum_window);
        result.released.time_entries += child.released.time_entries;
        result.released.child_nodes += child.released.child_nodes;
        if (child.empty) {
            it = node.children.erase(it);
            ++result.released.child_nodes;
        }
        else {
            ++it;
        }
    }
    result.empty = node.times.empty() && node.children.empty();
    return result;
}

}  // namespace

CallTree::StorageUsage CallTree::storageUsage() const
{
    return measureNode(root_);
}

std::uint64_t CallTree::sampleCount() const
{
    std::uint64_t total = 0;
    for (const auto &[window, count] : root_.times) {
        total += count;
    }
    return total;
}

bool CallTree::pruneBefore(std::int32_t minimum_window)
{
    pruneBeforeWithUsage(minimum_window);
    return root_.times.empty() && root_.children.empty();
}

CallTree::StorageUsage CallTree::pruneBeforeWithUsage(std::int32_t minimum_window)
{
    return pruneNode(root_, minimum_window).released;
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
