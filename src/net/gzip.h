#ifndef ENDSTONE_SPARK_GZIP_H
#define ENDSTONE_SPARK_GZIP_H

#include <string>

namespace spark {

// gzip-compress a buffer (zlib). Throws std::runtime_error on failure.
std::string gzipCompress(const std::string &input);

}  // namespace spark

#endif  // ENDSTONE_SPARK_GZIP_H
