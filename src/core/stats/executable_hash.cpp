#include "core/stats/executable_hash.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace spark {
namespace {

constexpr std::array<std::uint32_t, 64> KRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

constexpr std::uint32_t rotateRight(std::uint32_t value, unsigned count)
{
    return (value >> count) | (value << (32U - count));
}

class Sha256 {
public:
    void update(const void *bytes, std::size_t size)
    {
        const auto *data = static_cast<const unsigned char *>(bytes);
        total_bytes_ += static_cast<std::uint64_t>(size);

        if (buffer_size_ != 0) {
            const std::size_t copied = std::min(size, buffer_.size() - buffer_size_);
            std::memcpy(buffer_.data() + buffer_size_, data, copied);
            buffer_size_ += copied;
            data += copied;
            size -= copied;
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_.data());
                buffer_size_ = 0;
            }
        }

        while (size >= buffer_.size()) {
            transform(data);
            data += buffer_.size();
            size -= buffer_.size();
        }
        if (size != 0) {
            std::memcpy(buffer_.data(), data, size);
            buffer_size_ = size;
        }
    }

    std::array<unsigned char, 32> finish()
    {
        const std::uint64_t total_bits = total_bytes_ * 8U;
        buffer_[buffer_size_++] = 0x80U;
        if (buffer_size_ > 56) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_), buffer_.end(), 0);
            transform(buffer_.data());
            buffer_size_ = 0;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_), buffer_.begin() + 56, 0);
        for (unsigned i = 0; i < 8; ++i) {
            buffer_[63 - i] = static_cast<unsigned char>(total_bits >> (i * 8U));
        }
        transform(buffer_.data());

        std::array<unsigned char, 32> digest{};
        for (std::size_t i = 0; i < state_.size(); ++i) {
            digest[i * 4] = static_cast<unsigned char>(state_[i] >> 24U);
            digest[i * 4 + 1] = static_cast<unsigned char>(state_[i] >> 16U);
            digest[i * 4 + 2] = static_cast<unsigned char>(state_[i] >> 8U);
            digest[i * 4 + 3] = static_cast<unsigned char>(state_[i]);
        }
        return digest;
    }

private:
    void transform(const unsigned char *block)
    {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            words[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) |
                       (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) |
                       (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) |
                       static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const std::uint32_t s0 =
                rotateRight(words[i - 15], 7) ^ rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3U);
            const std::uint32_t s1 =
                rotateRight(words[i - 2], 17) ^ rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10U);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];

        for (std::size_t i = 0; i < words.size(); ++i) {
            const std::uint32_t sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + sum1 + choice + KRoundConstants[i] + words[i];
            const std::uint32_t sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = sum0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                           0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::array<unsigned char, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

std::string digestHex(const std::array<unsigned char, 32> &digest)
{
    static constexpr char k_hex[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2);
    for (std::size_t i = 0; i < digest.size(); ++i) {
        result[i * 2] = k_hex[digest[i] >> 4U];
        result[i * 2 + 1] = k_hex[digest[i] & 0x0fU];
    }
    return result;
}

std::filesystem::path currentExecutablePath(std::string &error)
{
#ifdef _WIN32
    std::vector<wchar_t> path(32768);
    const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) {
        error = "GetModuleFileNameW failed with error " + std::to_string(::GetLastError());
        return {};
    }
    if (length >= path.size()) {
        error = "the current executable path exceeds the Windows path limit";
        return {};
    }
    return {std::wstring(path.data(), length)};
#elif defined(__linux__)
    return std::filesystem::path("/proc/self/exe");
#else
    error = "executable hashing is not supported on this platform";
    return {};
#endif
}

}  // namespace

std::string sha256Hex(std::string_view bytes)
{
    Sha256 hash;
    hash.update(bytes.data(), bytes.size());
    return digestHex(hash.finish());
}

std::string currentExecutableSha256(std::string &error)
{
    error.clear();
    const std::filesystem::path path = currentExecutablePath(error);
    if (path.empty()) {
        return {};
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open the current executable for hashing";
        return {};
    }

    Sha256 hash;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            hash.update(buffer.data(), static_cast<std::size_t>(count));
        }
    }
    if (input.bad()) {
        error = "unable to read the current executable for hashing";
        return {};
    }
    return digestHex(hash.finish());
}

}  // namespace spark
