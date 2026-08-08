#ifndef ENDSTONE_SPARK_WEBSOCKET_H
#define ENDSTONE_SPARK_WEBSOCKET_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace spark {

// Minimal WebSocket client built on libcurl's experimental WS API.
// Creates a channel via HTTP GET to <host>/create, then connects to
// wss://<host>/<channelId>. A background thread receives messages and
// invokes the callback. Sends are enqueued and processed by the same
// thread to avoid concurrent curl handle access.
class WebSocketClient {
public:
    using MessageCallback = std::function<void(const std::string &)>;
    using CloseCallback = std::function<void()>;

    WebSocketClient();
    ~WebSocketClient();

    // Connect to the given bytesocks host. Returns the channel ID on success,
    // or an empty string on failure.
    std::string connect(const std::string &host, const std::string &user_agent);

    // Enqueue a text message to send. Thread-safe.
    void send(const std::string &message);

    // Close the connection and join the background thread.
    void close();

    bool isOpen() const { return running_.load(); }

    void setMessageCallback(MessageCallback cb) { message_cb_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { close_cb_ = std::move(cb); }

private:
    void runReceiveLoop();

    std::string channel_id_;
    std::string host_;
    std::string user_agent_;

    std::atomic<bool> running_{false};
    std::thread thread_;

    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::queue<std::string> send_queue_;
    std::atomic<bool> has_data_{false};

    MessageCallback message_cb_;
    CloseCallback close_cb_;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_WEBSOCKET_H
