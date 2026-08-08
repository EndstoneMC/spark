#ifndef ENDSTONE_SPARK_PROTO_READER_H
#define ENDSTONE_SPARK_PROTO_READER_H

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spark {

// Minimal read-only protobuf (proto3) decoder for the small ws schema.
class ProtoReader {
public:
    explicit ProtoReader(std::string_view data) : data_(data) {}

    bool eof() const { return pos_ >= data_.size(); }

    // Read the next field tag + wire type. Returns false at end of message.
    bool nextField(int &field, int &wire_type)
    {
        if (pos_ >= data_.size()) {
            return false;
        }
        auto tag = readVarint();
        field = static_cast<int>(tag >> 3);
        wire_type = static_cast<int>(tag & 0x07);
        return true;
    }

    std::uint64_t readVarint()
    {
        std::uint64_t value = 0;
        int shift = 0;
        while (pos_ < data_.size()) {
            auto byte = static_cast<unsigned char>(data_[pos_++]);
            value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
            if (!(byte & 0x80)) {
                break;
            }
            shift += 7;
        }
        return value;
    }

    std::int32_t readInt32() { return static_cast<std::int32_t>(readVarint()); }
    std::int64_t readInt64() { return static_cast<std::int64_t>(readVarint()); }
    bool readBool() { return readVarint() != 0; }

    std::string_view readString()
    {
        auto len = readVarint();
        if (pos_ + len > data_.size()) {
            len = data_.size() - pos_;
        }
        std::string_view result(data_.data() + pos_, len);
        pos_ += len;
        return result;
    }

    std::string readBytes()
    {
        auto sv = readString();
        return std::string(sv.data(), sv.size());
    }

    ProtoReader readMessage()
    {
        auto len = readVarint();
        if (pos_ + len > data_.size()) {
            len = data_.size() - pos_;
        }
        ProtoReader sub(data_.substr(pos_, len));
        pos_ += len;
        return sub;
    }

    void skip(int wire_type)
    {
        switch (wire_type) {
        case 0:
            readVarint();
            break;
        case 1:
            pos_ += 8;
            break;
        case 2: {
            auto len = readVarint();
            pos_ += len;
        } break;
        case 5:
            pos_ += 4;
            break;
        default:
            pos_ = data_.size();
            break;
        }
        if (pos_ > data_.size()) {
            pos_ = data_.size();
        }
    }

private:
    std::string_view data_;
    std::size_t pos_ = 0;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_PROTO_READER_H
