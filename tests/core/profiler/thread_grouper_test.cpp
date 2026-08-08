#include <cassert>
#include <cstdio>
#include <string>

#include "core/profiler/thread_grouper.h"

using namespace spark;

static void test_by_name()
{
    ThreadGrouper g(ThreadGrouperMode::ByName);
    assert(g.group(1, "Server thread") == "Server thread");
    assert(g.group(2, "Worker-1") == "Worker-1");
    assert(g.label("Server thread") == "Server thread");
    assert(g.label("Worker-1") == "Worker-1");
    std::printf("  by_name: OK\n");
}

static void test_by_pool()
{
    ThreadGrouper g(ThreadGrouperMode::ByPool);

    // Threads with numeric suffixes are grouped by pool name.
    assert(g.group(1, "Worker-1") == "Worker");
    assert(g.group(2, "Worker-2") == "Worker");
    assert(g.group(3, "Worker-3") == "Worker");

    // Thread without numeric suffix stays as-is.
    assert(g.group(4, "Server thread") == "Server thread");

    // Same tid returns cached group.
    assert(g.group(1, "Worker-1") == "Worker");

    // Labels include member count for pool groups.
    assert(g.label("Worker") == "Worker (x3)");
    assert(g.label("Server thread") == "Server thread");

    std::printf("  by_pool: OK\n");
}

static void test_by_pool_separators()
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

static void test_as_one()
{
    ThreadGrouper g(ThreadGrouperMode::AsOne);
    assert(g.group(1, "Server thread") == "root");
    assert(g.group(2, "Worker-1") == "root");
    assert(g.group(3, "Worker-2") == "root");

    // Label shows total thread count.
    assert(g.label("root") == "All (x3)");

    std::printf("  as_one: OK\n");
}

static void test_by_pool_no_match()
{
    ThreadGrouper g(ThreadGrouperMode::ByPool);
    // Names without a trailing number are not pooled.
    assert(g.group(1, "AsyncChatThread") == "AsyncChatThread");
    assert(g.group(2, "main") == "main");
    assert(g.label("AsyncChatThread") == "AsyncChatThread");
    assert(g.label("main") == "main");

    std::printf("  by_pool_no_match: OK\n");
}

int main()
{
    std::printf("thread_grouper_test:\n");
    test_by_name();
    test_by_pool();
    test_by_pool_separators();
    test_as_one();
    test_by_pool_no_match();
    std::printf("All thread_grouper tests passed.\n");
    return 0;
}
