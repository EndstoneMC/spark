#include "net/websocket.h"

#include <chrono>
#include <cstring>
#include <memory>

#include <curl/curl.h>

namespace spark {

namespace {

// HTTP response buffer for the /create request.
struct CreateResponse {
    std::string data;
};

struct CurlEasyDeleter {
    std::atomic<std::uint64_t> *cleanup_count = nullptr;

    void operator()(CURL *curl) const
    {
        if (curl != nullptr) {
            curl_easy_cleanup(curl);
            if (cleanup_count != nullptr) {
                cleanup_count->fetch_add(1, std::memory_order_release);
            }
        }
    }
};

class CurlHeaderList {
public:
    explicit CurlHeaderList(std::atomic<std::uint64_t> *cleanup_count) : cleanup_count_(cleanup_count) {}
    ~CurlHeaderList()
    {
        if (headers_ != nullptr) {
            curl_slist_free_all(headers_);
            if (cleanup_count_ != nullptr) {
                cleanup_count_->fetch_add(1, std::memory_order_release);
            }
        }
    }

    CurlHeaderList(const CurlHeaderList &) = delete;
    CurlHeaderList &operator=(const CurlHeaderList &) = delete;

    void append(const char *value)
    {
        if (curl_slist *updated = curl_slist_append(headers_, value)) {
            headers_ = updated;
        }
    }

    [[nodiscard]] curl_slist *get() const { return headers_; }

private:
    curl_slist *headers_ = nullptr;
    std::atomic<std::uint64_t> *cleanup_count_ = nullptr;
};

using CurlEasyPtr = std::unique_ptr<CURL, CurlEasyDeleter>;

size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *resp = static_cast<CreateResponse *>(userdata);
    resp->data.append(ptr, size * nmemb);
    return size * nmemb;
}

// Parse {"key":"<channelId>"} from the /create response.
std::string parseChannelKey(const std::string &json)
{
    // Simple JSON extraction: find "key":"..." and extract the value.
    auto pos = json.find("\"key\"");
    if (pos == std::string::npos) {
        return {};
    }
    pos = json.find(':', pos + 5);
    if (pos == std::string::npos) {
        return {};
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) {
        return {};
    }
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) {
        return {};
    }
    return json.substr(pos + 1, end - pos - 1);
}

}  // namespace

WebSocketClient::WebSocketClient() = default;

WebSocketClient::~WebSocketClient()
{
    close();
}

std::string WebSocketClient::connect(const std::string &host, const std::string &user_agent)
{
    host_ = host;
    user_agent_ = user_agent;

    // Step 1: Create a channel via HTTP GET.
    CURL *curl = curl_easy_init();
    if (!curl) {
        return {};
    }

    std::string url = "https://" + host + "/create";
    CreateResponse resp;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    if (!user_agent.empty()) {
        std::string ua_header = "User-Agent: " + user_agent;
        headers = curl_slist_append(headers, ua_header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        return {};
    }

    channel_id_ = parseChannelKey(resp.data);
    if (channel_id_.empty()) {
        return {};
    }

    // Step 2: Connect via WebSocket.
    local_close_requested_.store(false);
    running_.store(true);
    if (!startReceiveWorker()) {
        running_.store(false);
        return {};
    }

    // Give the connection a brief moment to establish.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!running_.load()) {
        thread_.join();
        return {};
    }

    return channel_id_;
}

bool WebSocketClient::startReceiveWorker()
{
    try {
        thread_ = std::thread([this] {
            try {
                runReceiveLoop();
            }
            catch (...) {
                running_.store(false);
            }
        });
    }
    catch (...) {
        return false;
    }
    return true;
}

void WebSocketClient::send(const std::string &message)
{
    if (!running_.load()) {
        return;
    }
    {
        std::scoped_lock lock(send_mutex_);
        send_queue_.push(message);
        has_data_.store(true);
    }
    send_cv_.notify_one();
}

void WebSocketClient::close()
{
    if (running_.load()) {
        local_close_requested_.store(true);
    }
    const bool was_running = running_.exchange(false);
    if (!was_running) {
        local_close_requested_.store(false);
    }
    send_cv_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
    if (was_running && close_cb_) {
        close_cb_();
    }
}

void WebSocketClient::runReceiveLoop()
{
    CurlEasyPtr curl(curl_easy_init(), CurlEasyDeleter{resource_cleanup_count_for_testing_});
    if (!curl) {
        running_.store(false);
        return;
    }

    std::string ws_url = "wss://" + host_ + "/" + channel_id_;
    CurlHeaderList headers(resource_cleanup_count_for_testing_);
    if (!user_agent_.empty()) {
        std::string ua_header = "User-Agent: " + user_agent_;
        headers.append(ua_header.c_str());
    }

    curl_easy_setopt(curl.get(), CURLOPT_URL, ws_url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_CONNECT_ONLY, 2L);  // WebSocket mode

    if (!incoming_message_for_testing_.empty() && message_cb_) {
        message_cb_(incoming_message_for_testing_);
    }

    CURLcode rc = curl_easy_perform(curl.get());
    if (rc != CURLE_OK) {
        running_.store(false);
        return;
    }

    // Poll loop: receive messages and send queued messages.
    constexpr auto k_poll_interval_ms = 100L;
    char recv_buf[4096];
    std::string pending_recv;  // buffered partial message

    while (running_.load()) {
        // Send any queued messages.
        if (has_data_.exchange(false)) {
            std::queue<std::string> to_send;
            {
                std::scoped_lock lock(send_mutex_);
                to_send.swap(send_queue_);
            }
            while (!to_send.empty()) {
                const auto &msg = to_send.front();
                size_t sent = 0;
                curl_ws_send(curl.get(), msg.data(), msg.size(), &sent, 0, CURLWS_TEXT);
                to_send.pop();
            }
        }

        // Try to receive.
        size_t recv = 0;
        const struct curl_ws_frame *frame = nullptr;
        CURLcode r = curl_ws_recv(curl.get(), recv_buf, sizeof(recv_buf), &recv, &frame);
        if (r == CURLE_OK && recv > 0 && frame) {
            pending_recv.append(recv_buf, recv);
            if (frame->bytesleft == 0) {
                if (message_cb_ && !pending_recv.empty()) {
                    message_cb_(pending_recv);
                }
                pending_recv.clear();
            }
        }
        else if (r == CURLE_AGAIN) {
            // No data available, wait briefly.
            std::unique_lock<std::mutex> lock(send_mutex_);
            send_cv_.wait_for(lock, std::chrono::milliseconds(k_poll_interval_ms),
                              [this]() { return !running_.load() || has_data_.load(); });
        }
        else {
            // Error or connection closed.
            break;
        }
    }

    if (local_close_requested_.exchange(false)) {
        size_t sent = 0;
        const char *close_msg = "";
        curl_ws_send(curl.get(), close_msg, 0, &sent, 0, CURLWS_CLOSE);
    }

    running_.store(false);
}

}  // namespace spark
