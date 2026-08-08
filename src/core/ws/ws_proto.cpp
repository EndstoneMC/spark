#include "core/ws/ws_proto.h"

#include "core/util/base64.h"
#include "core/ws/crypto.h"
#include "proto/proto_reader.h"
#include "proto/proto_writer.h"

namespace spark {

namespace {

// PacketWrapper field numbers (from spark_ws.proto)
constexpr int KFieldServerPong = 1;
constexpr int KFieldServerConnectResponse = 2;
constexpr int KFieldServerUpdateSampler = 3;
constexpr int KFieldClientPing = 10;
constexpr int KFieldClientConnect = 11;

// RawPacket field numbers
constexpr int KRawVersion = 1;
constexpr int KRawPublicKey = 2;
constexpr int KRawSignature = 3;
constexpr int KRawMessage = 4;

std::string buildRawPacket(std::string_view message, const std::vector<std::uint8_t> &private_key_pkcs8)
{
    auto signature =
        Crypto::sign(private_key_pkcs8, reinterpret_cast<const std::uint8_t *>(message.data()), message.size());

    std::string raw;
    ProtoWriter w(raw);
    w.int32(KRawVersion, Crypto::kVersion);
    // public_key is omitted for outgoing (server-signed) packets;
    // the viewer identifies the server by the channel's pre-announced key.
    if (!signature.empty()) {
        w.string(KRawSignature, std::string_view(reinterpret_cast<const char *>(signature.data()), signature.size()));
    }
    w.string(KRawMessage, message);
    return raw;
}

std::string wrapAndEncode(std::string_view packet_wrapper, const std::vector<std::uint8_t> &private_key_pkcs8)
{
    std::string raw = buildRawPacket(packet_wrapper, private_key_pkcs8);
    return base64Encode(raw);
}

}  // namespace

std::string encodeSocketChannelInfo(const SocketChannelInfo &info)
{
    std::string bytes;
    ProtoWriter w(bytes);
    w.string(1, info.channel_id);
    if (!info.public_key.empty()) {
        w.string(2, std::string_view(reinterpret_cast<const char *>(info.public_key.data()), info.public_key.size()));
    }
    return bytes;
}

bool decodeRawPacket(std::string_view base64_data, WsIncomingPacket &out)
{
    auto raw_bytes = base64Decode(base64_data);
    if (raw_bytes.empty()) {
        return false;
    }

    ProtoReader raw(std::string_view(reinterpret_cast<const char *>(raw_bytes.data()), raw_bytes.size()));
    int version = 0;
    std::vector<std::uint8_t> public_key;
    std::vector<std::uint8_t> signature;
    std::string message;

    int field = 0;
    int wire = 0;
    while (raw.nextField(field, wire)) {
        if (wire != 2 && wire != 0) {
            raw.skip(wire);
            continue;
        }
        if (field == KRawVersion && wire == 0) {
            version = raw.readInt32();
        }
        else if (field == KRawPublicKey && wire == 2) {
            auto sv = raw.readString();
            public_key.assign(reinterpret_cast<const std::uint8_t *>(sv.data()),
                              reinterpret_cast<const std::uint8_t *>(sv.data()) + sv.size());
        }
        else if (field == KRawSignature && wire == 2) {
            auto sv = raw.readString();
            signature.assign(reinterpret_cast<const std::uint8_t *>(sv.data()),
                             reinterpret_cast<const std::uint8_t *>(sv.data()) + sv.size());
        }
        else if (field == KRawMessage && wire == 2) {
            message = std::string(raw.readString());
        }
        else {
            raw.skip(wire);
        }
    }

    if (version != Crypto::kVersion) {
        return false;
    }
    if (message.empty()) {
        return false;
    }

    // Verify signature if both public_key and signature are present.
    out.verified = false;
    if (!public_key.empty() && !signature.empty()) {
        out.verified = Crypto::verify(public_key, reinterpret_cast<const std::uint8_t *>(message.data()),
                                      message.size(), signature.data(), signature.size());
    }
    out.public_key = std::move(public_key);

    // Decode the PacketWrapper inside message.
    ProtoReader wrapper(std::string_view(message.data(), message.size()));
    while (wrapper.nextField(field, wire)) {
        if (wire != 2) {
            wrapper.skip(wire);
            continue;
        }
        if (field == KFieldClientPing) {
            out.type = WsPacketType::ClientPing;
            auto sub = wrapper.readMessage();
            while (sub.nextField(field, wire)) {
                if (field == 1 && wire == 0) {
                    out.ping.ok = sub.readBool();
                }
                else if (field == 2 && wire == 0) {
                    out.ping.data = sub.readInt32();
                }
                else {
                    sub.skip(wire);
                }
            }
        }
        else if (field == KFieldClientConnect) {
            out.type = WsPacketType::ClientConnect;
            auto sub = wrapper.readMessage();
            while (sub.nextField(field, wire)) {
                if (field == 1 && wire == 2) {
                    out.connect.client_id = std::string(sub.readString());
                }
                else if (field == 2 && wire == 2) {
                    out.connect.description = std::string(sub.readString());
                }
                else {
                    sub.skip(wire);
                }
            }
        }
        else {
            wrapper.skip(wire);
        }
    }

    return out.type != WsPacketType::Unknown;
}

std::string encodeServerPong(bool ok, std::int32_t data, const std::vector<std::uint8_t> &private_key_pkcs8)
{
    // ServerPong { bool ok = 1; int32 data = 2; }
    std::string pong;
    ProtoWriter w(pong);
    w.boolean(1, ok);
    w.int32(2, data);

    // PacketWrapper { ServerPong server_pong = 1; }
    std::string wrapper;
    ProtoWriter ww(wrapper);
    ww.message(KFieldServerPong, pong);

    return wrapAndEncode(wrapper, private_key_pkcs8);
}

std::string encodeServerConnectResponse(const std::string &client_id, int state, int sampler_interval,
                                        int statistics_interval, const std::string &last_payload_id,
                                        const std::vector<std::uint8_t> &private_key_pkcs8)
{
    // ServerConnectResponse.Settings { int32 statistics_interval = 1; int32 sampler_interval = 2; }
    std::string settings;
    ProtoWriter ws(settings);
    ws.int32(1, statistics_interval);
    ws.int32(2, sampler_interval);

    // ServerConnectResponse { string client_id = 1; State state = 2; Settings settings = 3; string last_payload_id = 4;
    // }
    std::string resp;
    ProtoWriter wr(resp);
    wr.string(1, client_id);
    wr.int32(2, state);
    wr.message(3, settings);
    if (!last_payload_id.empty()) {
        wr.string(4, last_payload_id);
    }

    // PacketWrapper { ServerConnectResponse server_connect_response = 2; }
    std::string wrapper;
    ProtoWriter ww(wrapper);
    ww.message(KFieldServerConnectResponse, resp);

    return wrapAndEncode(wrapper, private_key_pkcs8);
}

std::string encodeServerUpdateSamplerData(const std::string &payload_id,
                                          const std::vector<std::uint8_t> &private_key_pkcs8)
{
    // ServerUpdateSamplerData { string payload_id = 1; }
    std::string update;
    ProtoWriter wu(update);
    wu.string(1, payload_id);

    // PacketWrapper { ServerUpdateSamplerData server_update_sampler = 3; }
    std::string wrapper;
    ProtoWriter ww(wrapper);
    ww.message(KFieldServerUpdateSampler, update);

    return wrapAndEncode(wrapper, private_key_pkcs8);
}

std::string encodeServerClose(const std::vector<std::uint8_t> &private_key_pkcs8)
{
    return encodeServerPong(false, 0, private_key_pkcs8);
}

}  // namespace spark
