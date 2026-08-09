#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "net/websocket.h"

namespace spark {

struct WebSocketClientTestAccess {
    static void startExitedWorker(WebSocketClient &client, std::mutex &mutex, std::condition_variable &cv, bool &exited)
    {
        client.running_.store(true);
        client.thread_ = std::thread([&client, &mutex, &cv, &exited]() {
            client.running_.store(false);
            {
                std::scoped_lock lock(mutex);
                exited = true;
            }
            cv.notify_one();
        });
    }

    static bool joinable(const WebSocketClient &client) { return client.thread_.joinable(); }

    static bool running(const WebSocketClient &client) { return client.running_.load(); }

    static bool startThrowingCallbackWorker(WebSocketClient &client, std::atomic<std::uint64_t> &cleanup_count)
    {
        client.host_ = "localhost";
        client.channel_id_ = "test";
        client.user_agent_ = "test";
        client.incoming_message_for_testing_ = "message";
        client.resource_cleanup_count_for_testing_ = &cleanup_count;
        client.message_cb_ = [](const std::string &) {
            throw std::runtime_error("injected");
        };
        client.local_close_requested_.store(false);
        client.running_.store(true);
        return client.startReceiveWorker();
    }

    static void startLocalCloseWorker(WebSocketClient &client, std::atomic<bool> &close_attempted)
    {
        client.running_.store(true);
        client.local_close_requested_.store(false);
        client.thread_ = std::thread([&client, &close_attempted] {
            while (client.running_.load()) {
                std::this_thread::yield();
            }
            close_attempted.store(client.local_close_requested_.exchange(false));
        });
    }
};

}  // namespace spark

namespace {

bool waitForExit(const spark::WebSocketClient &client)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (spark::WebSocketClientTestAccess::running(client) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return !spark::WebSocketClientTestAccess::running(client);
}

}  // namespace

int main()
{
    spark::WebSocketClient client;
    std::mutex mutex;
    std::condition_variable cv;
    bool exited = false;
    spark::WebSocketClientTestAccess::startExitedWorker(client, mutex, cv, exited);

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(2), [&exited]() { return exited; }));
    }

    assert(spark::WebSocketClientTestAccess::joinable(client));
    client.close();
    assert(!spark::WebSocketClientTestAccess::joinable(client));
    client.close();

    std::atomic<bool> close_attempted{false};
    spark::WebSocketClientTestAccess::startLocalCloseWorker(client, close_attempted);
    client.close();
    assert(close_attempted.load());
    assert(!spark::WebSocketClientTestAccess::joinable(client));
    client.close();

    std::atomic<std::uint64_t> cleanup_count{0};
    assert(spark::WebSocketClientTestAccess::startThrowingCallbackWorker(client, cleanup_count));
    assert(waitForExit(client));
    assert(cleanup_count.load(std::memory_order_acquire) == 2);
    client.close();
    assert(!spark::WebSocketClientTestAccess::joinable(client));

    assert(spark::WebSocketClientTestAccess::startThrowingCallbackWorker(client, cleanup_count));
    assert(waitForExit(client));
    assert(cleanup_count.load(std::memory_order_acquire) == 4);
    client.close();
    assert(!spark::WebSocketClientTestAccess::joinable(client));
    return 0;
}
