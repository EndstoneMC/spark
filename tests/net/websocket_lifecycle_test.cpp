#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <curl/curl.h>

#include "core/ws/viewer_socket.h"
#include "net/websocket.h"

namespace spark {

struct WebSocketClientTestAccess {
    enum class SendStep {
        Idle,
        Progress,
        Retry,
        Fatal
    };

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

    static void enqueue(WebSocketClient &client, const std::string &message)
    {
        client.running_.store(true);
        client.send(message);
    }

    static SendStep processSend(WebSocketClient &client,
                                const std::function<std::pair<int, std::size_t>(std::string_view)> &send_function)
    {
        const auto step = client.processNextSend([&send_function](const char *data, std::size_t size) {
            const auto [code, sent] = send_function(std::string_view(data, size));
            return WebSocketClient::SendAttempt{.code = code, .sent = sent};
        });
        switch (step) {
        case WebSocketClient::SendStep::Idle:
            return SendStep::Idle;
        case WebSocketClient::SendStep::Progress:
            return SendStep::Progress;
        case WebSocketClient::SendStep::Retry:
            return SendStep::Retry;
        case WebSocketClient::SendStep::Fatal:
            return SendStep::Fatal;
        }
        return SendStep::Fatal;
    }

    static std::string pendingSend(const WebSocketClient &client)
    {
        return client.pending_send_.value_or(std::string()).substr(client.pending_send_offset_);
    }

    static std::size_t queued(const WebSocketClient &client) { return client.send_queue_.size(); }

    static void receiveFailure(WebSocketClient &client, int code) { client.handleReceiveFailure(code); }
};

struct ViewerSocketTestAccess {
    static void markOpen(ViewerSocket &socket)
    {
        socket.prepareOpen();
        socket.open_.store(true);
    }

    static void terminate(ViewerSocket &socket, WebSocketClient::TerminationKind kind, const std::string &detail = {})
    {
        socket.onTransportClosed({.kind = kind, .detail = detail});
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

    {
        spark::WebSocketClient full_send;
        spark::WebSocketClientTestAccess::enqueue(full_send, "alpha");
        assert(spark::WebSocketClientTestAccess::processSend(full_send, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        assert(spark::WebSocketClientTestAccess::pendingSend(full_send).empty());
        assert(spark::WebSocketClientTestAccess::queued(full_send) == 0);
        full_send.close();
    }

    {
        spark::WebSocketClient retry_send;
        spark::WebSocketClientTestAccess::enqueue(retry_send, "retry");
        assert(spark::WebSocketClientTestAccess::processSend(retry_send, [](std::string_view) {
                   return std::pair{CURLE_AGAIN, std::size_t{0}};
               }) == spark::WebSocketClientTestAccess::SendStep::Retry);
        assert(spark::WebSocketClientTestAccess::pendingSend(retry_send) == "retry");
        assert(spark::WebSocketClientTestAccess::processSend(retry_send, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        assert(spark::WebSocketClientTestAccess::pendingSend(retry_send).empty());
        retry_send.close();
    }

    {
        spark::WebSocketClient partial_send;
        spark::WebSocketClientTestAccess::enqueue(partial_send, "partial");
        assert(spark::WebSocketClientTestAccess::processSend(partial_send, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size() - 3};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        assert(spark::WebSocketClientTestAccess::pendingSend(partial_send) == "ial");
        assert(spark::WebSocketClientTestAccess::processSend(partial_send, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        assert(spark::WebSocketClientTestAccess::pendingSend(partial_send).empty());
        partial_send.close();
    }

    {
        spark::WebSocketClient ordered_send;
        spark::WebSocketClientTestAccess::enqueue(ordered_send, "AAAA");
        spark::WebSocketClientTestAccess::enqueue(ordered_send, "BB");
        spark::WebSocketClientTestAccess::enqueue(ordered_send, "C");
        std::vector<std::string> attempts;
        auto send = [&attempts](std::string_view data) {
            attempts.emplace_back(data);
            const std::size_t sent = attempts.size() == 1 ? 2 : data.size();
            return std::pair{CURLE_OK, sent};
        };
        while (spark::WebSocketClientTestAccess::processSend(ordered_send, send) !=
               spark::WebSocketClientTestAccess::SendStep::Idle) {
        }
        assert((attempts == std::vector<std::string>{"AAAA", "AA", "BB", "C"}));
        ordered_send.close();
    }

    {
        spark::WebSocketClient failed_send;
        spark::WebSocketClientTestAccess::enqueue(failed_send, "failure");
        assert(spark::WebSocketClientTestAccess::processSend(failed_send, [](std::string_view) {
                   return std::pair{CURLE_SEND_ERROR, std::size_t{0}};
               }) == spark::WebSocketClientTestAccess::SendStep::Fatal);
        const auto termination = failed_send.termination();
        assert(termination.kind == spark::WebSocketClient::TerminationKind::SendError);
        assert(!termination.detail.empty());
        failed_send.close();
    }

    {
        spark::WebSocketClient failed_receive;
        spark::WebSocketClientTestAccess::receiveFailure(failed_receive, CURLE_RECV_ERROR);
        const auto termination = failed_receive.termination();
        assert(termination.kind == spark::WebSocketClient::TerminationKind::ReceiveError);
        assert(!termination.detail.empty());
    }

    spark::ViewerSocket viewer({}, {});
    spark::ViewerSocketTestAccess::markOpen(viewer);
    spark::ViewerSocketTestAccess::terminate(viewer, spark::WebSocketClient::TerminationKind::RemoteClose);
    assert(!viewer.isOpen());
    assert(viewer.closeReason() == spark::ViewerSocket::CloseReason::RemoteClose);
    assert(!viewer.takeDiagnostic().empty());

    spark::ViewerSocketTestAccess::markOpen(viewer);
    assert(viewer.isOpen());
    assert(viewer.closeReason() == spark::ViewerSocket::CloseReason::None);
    viewer.close();
    assert(!viewer.isOpen());
    assert(viewer.closeReason() == spark::ViewerSocket::CloseReason::LocalClose);
    assert(viewer.takeDiagnostic().empty());
    return 0;
}
