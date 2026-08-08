#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
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
};

}  // namespace spark

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
    return 0;
}
