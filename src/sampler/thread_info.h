#ifndef ENDSTONE_SPARK_THREAD_INFO_H
#define ENDSTONE_SPARK_THREAD_INFO_H

#include <cstdint>
#include <string>
#include <vector>

namespace spark {

struct ThreadInfo {
    std::uint64_t id = 0;
    std::string name;
};

std::uint64_t currentNativeThreadId();
std::string nativeThreadName(std::uint64_t id);
std::vector<ThreadInfo> enumerateProcessThreads();

}  // namespace spark

#endif  // ENDSTONE_SPARK_THREAD_INFO_H
