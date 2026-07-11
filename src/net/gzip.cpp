#include "net/gzip.h"

#include <stdexcept>

#include <zlib.h>

namespace spark {

std::string gzipCompress(const std::string &input)
{
    z_stream zs{};
    // windowBits 15 + 16 selects a gzip wrapper.
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("gzip: deflateInit2 failed");
    }

    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
    zs.avail_in = static_cast<uInt>(input.size());

    std::string out;
    char buffer[16384];
    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef *>(buffer);
        zs.avail_out = sizeof(buffer);
        ret = deflate(&zs, Z_FINISH);
        out.append(buffer, sizeof(buffer) - zs.avail_out);
    } while (ret == Z_OK);

    deflateEnd(&zs);
    if (ret != Z_STREAM_END) {
        throw std::runtime_error("gzip: deflate failed");
    }
    return out;
}

}  // namespace spark
