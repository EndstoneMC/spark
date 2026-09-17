#include <cassert>
#include <cstdio>
#include <string>

#include "core/profiler/thread_grouper.h"

using namespace spark;  // NOLINT(google-build-using-namespace)

static void test_by_name()  // NOLINT(misc-use-anonymous-namespace)
{
    ThreadGrouper g(ThreadGrouperMode::ByName);
    assert(g.group(1, "Server thread") == "Server thread");
    assert(g.group(2, "Worker-1") == "Worker-1");
    assert(g.label("Server thread") == "Server thread");
    assert(g.label("Worker-1") == "Worker-1");
    const auto first = g.groupKey(10, "Worker");
    const auto second = g.groupKey(20, "Worker");
    assert(first != second);
    assert(first < second);
    assert(g.groupKey(100, "A") < first);
    assert(g.label(first.first) == "Worker");
    assert(g.label(second.first) == "Worker");
    std::printf("  by_name: OK\n");
}

static void test_by_pool()  // NOLINT(misc-use-anonymous-namespace)
{
    ThreadGrouper g(ThreadGrouperMode::ByPool);

    // Threads with numeric suffixes are grouped by pool name.
    assert(g.group(1, "Worker-1") == "Worker");
    assert(g.group(2, "Worker-2") == "Worker");
    assert(g.group(3, "Worker-3") == "Worker");
    assert(g.groupKey(1, "Worker-1") == g.groupKey(2, "Worker-2"));

    // Thread without numeric suffix stays as-is.
    assert(g.group(4, "Server thread") == "Server thread");

    // Same tid returns cached group.
    assert(g.group(1, "Worker-1") == "Worker");

    // Labels include member count for pool groups.
    assert(g.label("Worker") == "Worker (x3)");
    assert(g.label("Server thread") == "Server thread");

    std::printf("  by_pool: OK\n");
}

static void test_by_pool_separators()  // NOLINT(misc-use-anonymous-namespace)
{
    ThreadGrouper g(ThreadGrouperMode::ByPool);

    // '-' separator
    assert(g.group(10, "Pool-A-1") == "Pool-A");
    // '#' separator
    assert(g.group(11, "Pool#1") == "Pool");
    // ' ' separator
    assert(g.group(12, "Pool 1") == "Pool");
    // Multiple trailing spaces trimmed
    assert(g.group(13, "Pool  1") == "Pool");

    std::printf("  by_pool_separators: OK\n");
}

static void test_as_one()  // NOLINT(misc-use-anonymous-namespace)
{
    ThreadGrouper g(ThreadGrouperMode::AsOne);
    assert(g.group(1, "Server thread") == "root");
    assert(g.group(2, "Worker-1") == "root");
    assert(g.group(3, "Worker-2") == "root");
    assert(g.groupKey(1, "Server thread") == g.groupKey(2, "Worker-1"));

    // Label shows total thread count.
    assert(g.label("root") == "All (x3)");

    std::printf("  as_one: OK\n");
}

static void test_by_pool_no_match()  // NOLINT(misc-use-anonymous-namespace)
{
    ThreadGrouper g(ThreadGrouperMode::ByPool);
    // Names without a trailing number are not pooled.
    assert(g.group(1, "AsyncChatThread") == "AsyncChatThread");
    assert(g.group(2, "main") == "main");
    assert(g.label("AsyncChatThread") == "AsyncChatThread");
    assert(g.label("main") == "main");

    std::printf("  by_pool_no_match: OK\n");
}

static void test_native_labels()  // NOLINT(misc-use-anonymous-namespace)
{
    ThreadGrouper execution(ThreadGrouperMode::ByPool);
    assert(execution.groupKeyForNativeLabel(101, "Worker-1 (#101)", NativeThreadLabelKind::Execution).first ==
           "Worker");
    assert(execution.groupKeyForNativeLabel(202, "Worker-2 (#202)", NativeThreadLabelKind::Execution).first ==
           "Worker");
    assert(execution.label("Worker") == "Worker (x2)");

    ThreadGrouper allocation(ThreadGrouperMode::ByPool);
    assert(allocation.groupKeyForNativeLabel(10, "Worker-1 (#254551, session #10)", NativeThreadLabelKind::Allocation)
               .first == "Worker");
    assert(allocation.groupKeyForNativeLabel(20, "Worker-2 (#254551, session #20)", NativeThreadLabelKind::Allocation)
               .first == "Worker");
    assert(allocation.label("Worker") == "Worker (x2)");

    ThreadGrouper reused_os_id(ThreadGrouperMode::ByPool);
    assert(reused_os_id.groupKeyForNativeLabel(30, "Worker-1 (#254551, session #30)", NativeThreadLabelKind::Allocation)
               .first == "Worker");
    assert(reused_os_id.groupKeyForNativeLabel(31, "Worker-2 (#254551, session #31)", NativeThreadLabelKind::Allocation)
               .first == "Worker");
    assert(reused_os_id.label("Worker") == "Worker (x2)");

    ThreadGrouper by_name(ThreadGrouperMode::ByName);
    const auto first = by_name.groupKeyForNativeLabel(1, "Worker-1 (#1)", NativeThreadLabelKind::Execution);
    const auto second = by_name.groupKeyForNativeLabel(2, "Worker-2 (#2)", NativeThreadLabelKind::Execution);
    assert(first.first == "Worker-1 (#1)");
    assert(second.first == "Worker-2 (#2)");
    assert(first != second);

    ThreadGrouper as_one(ThreadGrouperMode::AsOne);
    const auto one = as_one.groupKeyForNativeLabel(1, "Worker-1 (#1)", NativeThreadLabelKind::Execution);
    const auto two = as_one.groupKeyForNativeLabel(2, "Worker-2 (#2)", NativeThreadLabelKind::Execution);
    assert(one == two);
    assert(as_one.label("root") == "All (x2)");

    ThreadGrouper opaque(ThreadGrouperMode::ByPool);
    assert(opaque.groupKeyForNativeLabel(1, "Worker-1 (#99)", NativeThreadLabelKind::Execution).first ==
           "Worker-1 (#99)");
    assert(opaque.groupKeyForNativeLabel(1, "Worker-1 (#bad)", NativeThreadLabelKind::Execution).first ==
           "Worker-1 (#bad)");
    assert(opaque.groupKeyForNativeLabel(1, "Worker-1 (#99", NativeThreadLabelKind::Execution).first ==
           "Worker-1 (#99");
    assert(opaque.groupKeyForNativeLabel(1, "Worker-1 (#254551 session #1)", NativeThreadLabelKind::Allocation).first ==
           "Worker-1 (#254551 session #1)");
    assert(
        opaque.groupKeyForNativeLabel(1, "Worker-1 (#254551, session #99)", NativeThreadLabelKind::Allocation).first ==
        "Worker-1 (#254551, session #99)");
    assert(opaque.groupKeyForNativeLabel(123, "Worker-1(#77) (#123)", NativeThreadLabelKind::Execution).first ==
           "Worker-1(#77) (#123)");

    std::printf("  native_labels: OK\n");
}

int main()
{
    std::printf("thread_grouper_test:\n");
    test_by_name();
    test_by_pool();
    test_by_pool_separators();
    test_as_one();
    test_by_pool_no_match();
    test_native_labels();
    std::printf("All thread_grouper tests passed.\n");
    return 0;
}
