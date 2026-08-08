#ifndef ENDSTONE_SPARK_BASE64_H
#define ENDSTONE_SPARK_BASE64_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spark {

// Standard base64 encoding (RFC 4648) with padding.
std::string base64Encode(const std::uint8_t *data, std::size_t length);
std::string base64Encode(std::string_view data);

// Standard base64 decoding. Returns empty vector on invalid input.
std::vector<std::uint8_t> base64Decode(std::string_view input);

}  // namespace spark

#endif  // ENDSTONE_SPARK_BASE64_H
