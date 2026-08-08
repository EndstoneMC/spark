#ifndef ENDSTONE_SPARK_VIEWER_SOCKET_H
#define ENDSTONE_SPARK_VIEWER_SOCKET_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/ws/crypto.h"
#include "core/ws/ws_proto.h"
#include "net/websocket.h"

namespace spark {

// Manages a live viewer WebSocket connection and pushes payload IDs on window rotation.
class ViewerSocket {
public:
    // Called to produce sampler data and upload it to bytebin.
    // The channel_info_proto is non-empty only for the initial upload;
    // window rotates pass an empty string. Returns the bytebin key.
    using UploadCallback = std::function<std::string(const std::string &channel_info_proto)>;

    struct Config {
        std::string bytesocks_host;
        std::string bytebin_url;
        std::string viewer_url;
        std::string user_agent = "spark-plugin";
    };

    ViewerSocket(Config config, Crypto::KeyPair key_pair);
    ~ViewerSocket();

    // Create the WebSocket channel, connect, and upload initial data.
    // Returns the viewer URL on success, or empty string on failure.
    std::string open(const UploadCallback &upload);

    // Called on window rotation: upload new data and send the payload ID.
    // The upload callback runs on the caller's thread.  For the main-thread
    // path, prefer enqueueUpdate() + sendUpdate() to move I/O off-thread.
    void processWindowRotate(const UploadCallback &upload);

    // Send a pre-uploaded payload ID to all connected viewers.  Thread-safe
    // against tick(); designed to be called from a worker thread after the
    // upload completes.
    void sendUpdate(const std::string &bytebin_key);

    // Close the socket and signal the viewer.
    void close();

    bool isOpen() const { return open_.load(); }

    // Get the SocketChannelInfo for embedding in the initial sampler data upload.
    SocketChannelInfo channelInfo() const;

    // Called on each tick to check timeouts and process queued messages.
    // Returns false if the socket should be closed due to timeout.
    bool tick();

    // Sets the trusted-keys check callback for live viewer authentication.
    using IsKeyTrustedCallback = std::function<bool(const std::vector<std::uint8_t> &)>;
    void setIsKeyTrustedCallback(IsKeyTrustedCallback cb) { is_key_trusted_ = std::move(cb); }

    // Get the pending public key for a client, or empty if not found.
    std::vector<std::uint8_t> pendingKey(const std::string &client_id) const;

    // Re-send a connect response with state=ACCEPTED for a previously-untrusted client.
    void sendClientTrusted(const std::string &client_id);

private:
    void onMessage(const std::string &data);

    Config config_;
    Crypto::KeyPair key_pair_;
    std::unique_ptr<WebSocketClient> ws_;

    std::atomic<bool> open_{false};
    const std::int64_t open_time_ms_;
    std::atomic<std::int64_t> last_ping_ms_{0};
    std::string last_payload_id_;
    std::mutex payload_mutex_;  // protects last_payload_id_ across threads
    std::string channel_id_;

    IsKeyTrustedCallback is_key_trusted_;

    // Pending client keys awaiting trust approval.
    std::map<std::string, std::vector<std::uint8_t>> pending_keys_;

    // Incoming messages are queued for processing on the main thread.
    std::mutex queue_mutex_;
    std::vector<std::string> incoming_queue_;

    static constexpr std::int64_t kInitialTimeoutMs = 60000;      // 60s
    static constexpr std::int64_t kEstablishedTimeoutMs = 30000;  // 30s
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_VIEWER_SOCKET_H
