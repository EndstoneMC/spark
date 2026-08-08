#ifndef ENDSTONE_SPARK_WS_PROTO_H
#define ENDSTONE_SPARK_WS_PROTO_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spark {

// SocketChannelInfo (spark_sampler.proto field 8 in SamplerData)
struct SocketChannelInfo {
    std::string channel_id;
    std::vector<std::uint8_t> public_key;  // X.509 DER
};

std::string encodeSocketChannelInfo(const SocketChannelInfo &info);

// Packet types (from spark_ws.proto PacketWrapper oneof)
enum class WsPacketType {
    Unknown = 0,
    ServerPong = 1,
    ServerConnectResponse = 2,
    ServerUpdateSamplerData = 3,
    ServerUpdateStatistics = 4,
    ClientPing = 10,
    ClientConnect = 11,
};

// Decoded incoming packet (ClientPing or ClientConnect).
struct WsClientPing {
    bool ok = false;
    std::int32_t data = 0;
};

struct WsClientConnect {
    std::string client_id;
    std::string description;
};

struct WsIncomingPacket {
    WsPacketType type = WsPacketType::Unknown;
    WsClientPing ping;
    WsClientConnect connect;
    bool verified = false;
    std::vector<std::uint8_t> public_key;  // X.509 DER from RawPacket
};

// RawPacket = { version: int32, public_key: bytes, signature: bytes, message: bytes }
// Decode the RawPacket wrapper and the PacketWrapper inside it.
// Returns false if the version is unsupported or the data is malformed.
bool decodeRawPacket(std::string_view base64_data, WsIncomingPacket &out);

// Encode outgoing packets as base64-encoded RawPacket strings (signed).
// These produce the final text to send over the WebSocket.
std::string encodeServerPong(bool ok, std::int32_t data,
                             const std::vector<std::uint8_t> &private_key_pkcs8);

std::string encodeServerConnectResponse(const std::string &client_id,
                                        int state,  // 0=ACCEPTED, 1=UNTRUSTED, 2=REJECTED
                                        int sampler_interval, int statistics_interval,
                                        const std::string &last_payload_id,
                                        const std::vector<std::uint8_t> &private_key_pkcs8);

std::string encodeServerUpdateSamplerData(const std::string &payload_id,
                                          const std::vector<std::uint8_t> &private_key_pkcs8);

// Encode a "close" pong (ok=false, data=0) to signal shutdown.
std::string encodeServerClose(const std::vector<std::uint8_t> &private_key_pkcs8);

}  // namespace spark

#endif  // ENDSTONE_SPARK_WS_PROTO_H
