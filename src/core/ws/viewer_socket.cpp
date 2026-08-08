#include "core/ws/viewer_socket.h"

#include <chrono>
#include <cstring>

#include "core/util/base64.h"
#include "net/bytebin.h"
#include "net/gzip.h"
#include "proto/sampler_data.h"
#include "spark_constants.h"

namespace spark {

namespace {

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ViewerSocket::ViewerSocket(Config config, Crypto::KeyPair key_pair)
    : config_(std::move(config)),
      key_pair_(std::move(key_pair)),
      open_time_ms_(nowMs())
{
}

ViewerSocket::~ViewerSocket()
{
    close();
}

SocketChannelInfo ViewerSocket::channelInfo() const
{
    SocketChannelInfo info;
    info.channel_id = channel_id_;
    info.public_key = key_pair_.public_key_x509;
    return info;
}

std::string ViewerSocket::open(const UploadCallback &upload)
{
    ws_ = std::make_unique<WebSocketClient>();
    ws_->setMessageCallback([this](const std::string &data) { onMessage(data); });

    channel_id_ = ws_->connect(config_.bytesocks_host, config_.user_agent);
    if (channel_id_.empty()) {
        return {};
    }

    // Build SocketChannelInfo proto.
    SocketChannelInfo info;
    info.channel_id = channel_id_;
    info.public_key = key_pair_.public_key_x509;
    std::string channel_info_proto = encodeSocketChannelInfo(info);

    // Upload initial sampler data.
    std::string bytebin_key = upload(channel_info_proto);
    if (bytebin_key.empty()) {
        close();
        return {};
    }

    last_payload_id_ = bytebin_key;
    open_.store(true);
    return config_.viewer_url + bytebin_key;
}

void ViewerSocket::processWindowRotate(const UploadCallback &upload)
{
    if (!open_.load() || !ws_ || !ws_->isOpen()) {
        return;
    }

    auto time = nowMs();
    if ((time - open_time_ms_) > kInitialTimeoutMs && (time - last_ping_ms_.load()) > kEstablishedTimeoutMs) {
        close();
        return;
    }

    // No clients connected yet.
    if (last_ping_ms_.load() == 0) {
        return;
    }

    std::string bytebin_key = upload(std::string());
    if (bytebin_key.empty()) {
        return;
    }

    last_payload_id_ = bytebin_key;
    std::string msg = encodeServerUpdateSamplerData(bytebin_key, key_pair_.private_key_pkcs8);
    ws_->send(msg);
}

void ViewerSocket::close()
{
    if (!open_.exchange(false)) return;
    if (ws_) {
        if (ws_->isOpen()) {
            std::string msg = encodeServerClose(key_pair_.private_key_pkcs8);
            ws_->send(msg);
        }
        ws_->close();
    }
}

bool ViewerSocket::tick()
{
    if (!open_.load()) return false;

    // Process queued incoming messages.
    std::vector<std::string> messages;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        messages.swap(incoming_queue_);
    }
    for (const auto &msg : messages) {
        WsIncomingPacket packet;
        if (!decodeRawPacket(msg, packet)) continue;

        switch (packet.type) {
            case WsPacketType::ClientPing:
                last_ping_ms_.store(nowMs());
                ws_->send(encodeServerPong(!open_.load() ? false : true,
                                           packet.ping.data,
                                           key_pair_.private_key_pkcs8));
                break;
            case WsPacketType::ClientConnect: {
                last_ping_ms_.store(nowMs());
                bool trusted = false;
                if (is_key_trusted_ && !packet.public_key.empty()) {
                    trusted = is_key_trusted_(packet.public_key);
                }
                if (!packet.public_key.empty()) {
                    pending_keys_[packet.connect.client_id] = packet.public_key;
                }
                int state = trusted ? 0 : 1;  // 0=ACCEPTED, 1=UNTRUSTED
                ws_->send(encodeServerConnectResponse(
                    packet.connect.client_id, state,
                    10, 10,  // sampler_interval, statistics_interval
                    last_payload_id_,
                    key_pair_.private_key_pkcs8));
                break;
            }
            default:
                break;
        }
    }

    // Check timeout.
    auto time = nowMs();
    if ((time - open_time_ms_) > kInitialTimeoutMs && (time - last_ping_ms_.load()) > kEstablishedTimeoutMs) {
        close();
        return false;
    }

    return true;
}

void ViewerSocket::onMessage(const std::string &data)
{
    std::lock_guard<std::mutex> lock(queue_mutex_);
    incoming_queue_.push_back(data);
}

std::vector<std::uint8_t> ViewerSocket::pendingKey(const std::string &client_id) const
{
    auto it = pending_keys_.find(client_id);
    if (it == pending_keys_.end()) {
        return {};
    }
    return it->second;
}

void ViewerSocket::sendClientTrusted(const std::string &client_id)
{
    if (!open_.load() || !ws_ || !ws_->isOpen()) {
        return;
    }
    ws_->send(encodeServerConnectResponse(
        client_id, 0,  // 0=ACCEPTED
        10, 10,
        last_payload_id_,
        key_pair_.private_key_pkcs8));
}

}  // namespace spark
