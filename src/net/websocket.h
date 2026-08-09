#ifndef ENDSTONE_SPARK_WEBSOCKET_H
#define ENDSTONE_SPARK_WEBSOCKET_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>

namespace spark {

struct WebSocketClientTestAccess;

// WebSocket client on libcurl's WS API. Sends are enqueued to a background thread
// to avoid concurrent curl handle access.
class WebSocketClient {
public:
    enum class TerminationKind {
        None,
        LocalClose,
        RemoteClose,
        SendError,
        ReceiveError,
        WorkerFailure
    };

    struct Termination {
        TerminationKind kind = TerminationKind::None;
        std::string detail;
    };

    using MessageCallback = std::function<void(const std::string &)>;
    using CloseCallback = std::function<void(const Termination &)>;

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
    Termination termination() const;

    void setMessageCallback(MessageCallback cb) { message_cb_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { close_cb_ = std::move(cb); }

private:
    friend struct WebSocketClientTestAccess;

    bool startReceiveWorker();
    void runReceiveLoop();
    void recordTermination(TerminationKind kind, std::string detail = {});
    void notifyTermination() noexcept;

    struct SendAttempt {
        int code = 0;
        std::size_t sent = 0;
    };
    enum class SendStep {
        Idle,
        Progress,
        Retry,
        Fatal
    };
    using SendFunction = std::function<SendAttempt(const char *, std::size_t)>;
    SendStep processNextSend(const SendFunction &send_function);
    void handleReceiveFailure(int code);

    std::string channel_id_;
    std::string host_;
    std::string user_agent_;

    std::atomic<bool> running_{false};
    std::atomic<bool> local_close_requested_{false};
    std::thread thread_;

    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::queue<std::string> send_queue_;
    std::optional<std::string> pending_send_;
    std::size_t pending_send_offset_ = 0;

    mutable std::mutex termination_mutex_;
    Termination termination_;

    MessageCallback message_cb_;
    CloseCallback close_cb_;
    std::string incoming_message_for_testing_;
    std::atomic<std::uint64_t> *resource_cleanup_count_for_testing_ = nullptr;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_WEBSOCKET_H
