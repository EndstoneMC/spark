#ifndef ENDSTONE_SPARK_PROTO_WRITER_H
#define ENDSTONE_SPARK_PROTO_WRITER_H

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace spark {

// Minimal write-only protobuf (proto3) encoder. We only ever serialize, and the
// spark schema we target is small and fixed, so this replaces a protobuf runtime
// dependency. Validated against the real .proto with `protoc --decode`.
class ProtoWriter {
public:
    enum WireType : std::uint8_t { Varint = 0, Fixed64 = 1, LengthDelimited = 2 };

    explicit ProtoWriter(std::string &out) : out_(out) {}

    void varint(int field, std::uint64_t value)
    {
        tag(field, Varint);
        putVarint(value);
    }

    void int32(int field, std::int32_t value)
    {
        tag(field, Varint);
        putVarint(static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));  // sign-extend
    }

    void int64(int field, std::int64_t value)
    {
        tag(field, Varint);
        putVarint(static_cast<std::uint64_t>(value));
    }

    void boolean(int field, bool value)
    {
        tag(field, Varint);
        putVarint(value ? 1 : 0);
    }

    void real(int field, double value)
    {
        tag(field, Fixed64);
        std::uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        putFixed64(bits);
    }

    void string(int field, std::string_view value)
    {
        tag(field, LengthDelimited);
        putVarint(value.size());
        out_.append(value.data(), value.size());
    }

    // Embed an already-serialized submessage.
    void message(int field, std::string_view bytes)
    {
        tag(field, LengthDelimited);
        putVarint(bytes.size());
        out_.append(bytes.data(), bytes.size());
    }

    // proto3 packs repeated scalar numeric fields by default; match that.
    void packedInt32(int field, const std::vector<std::int32_t> &values)
    {
        if (values.empty()) {
            return;
        }
        std::string payload;
        ProtoWriter inner(payload);
        for (std::int32_t v : values) {
            inner.putVarint(static_cast<std::uint64_t>(static_cast<std::int64_t>(v)));
        }
        message(field, payload);
    }

    void packedDouble(int field, const std::vector<double> &values)
    {
        if (values.empty()) {
            return;
        }
        std::string payload;
        payload.reserve(values.size() * 8);
        for (double v : values) {
            std::uint64_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            for (int i = 0; i < 8; ++i) {
                payload.push_back(static_cast<char>((bits >> (8 * i)) & 0xff));
            }
        }
        message(field, payload);
    }

private:
    void tag(int field, WireType type)
    {
        putVarint((static_cast<std::uint64_t>(field) << 3) | static_cast<std::uint64_t>(type));
    }

    void putVarint(std::uint64_t value)
    {
        while (value >= 0x80) {
            out_.push_back(static_cast<char>((value & 0x7f) | 0x80));
            value >>= 7;
        }
        out_.push_back(static_cast<char>(value));
    }

    void putFixed64(std::uint64_t bits)
    {
        for (int i = 0; i < 8; ++i) {
            out_.push_back(static_cast<char>((bits >> (8 * i)) & 0xff));
        }
    }

    std::string &out_;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_PROTO_WRITER_H
