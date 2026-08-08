#include "core/util/base64.h"

namespace spark {

namespace {

constexpr char KEncodeTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decodeChar(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

}  // namespace

std::string base64Encode(const std::uint8_t *data, std::size_t length)
{
    std::string out;
    out.reserve(((length + 2) / 3) * 4);
    for (std::size_t i = 0; i < length; i += 3) {
        std::uint32_t triple = data[i] << 16;
        if (i + 1 < length) {
            triple |= data[i + 1] << 8;
        }
        if (i + 2 < length) {
            triple |= data[i + 2];
        }

        out.push_back(KEncodeTable[(triple >> 18) & 0x3f]);
        out.push_back(KEncodeTable[(triple >> 12) & 0x3f]);
        if (i + 1 < length) {
            out.push_back(KEncodeTable[(triple >> 6) & 0x3f]);
        }
        else {
            out.push_back('=');
        }
        if (i + 2 < length) {
            out.push_back(KEncodeTable[triple & 0x3f]);
        }
        else {
            out.push_back('=');
        }
    }
    return out;
}

std::string base64Encode(std::string_view data)
{
    return base64Encode(reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
}

std::vector<std::uint8_t> base64Decode(std::string_view input)
{
    std::vector<std::uint8_t> out;
    out.reserve(input.size() / 4 * 3);

    std::uint32_t triple = 0;
    int bits = 0;
    for (char c : input) {
        if (c == '=' || c == '\r' || c == '\n') {
            continue;
        }
        int val = decodeChar(c);
        if (val < 0) {
            continue;
        }
        triple = (triple << 6) | static_cast<std::uint32_t>(val);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((triple >> bits) & 0xff));
        }
    }
    return out;
}

}  // namespace spark
