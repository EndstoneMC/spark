#include "sampler/thread_info.h"

#include <algorithm>
#include <charconv>
#include <utility>

#if defined(_WIN32)
// clang-format off
#include <windows.h>
#include <tlhelp32.h>
// clang-format on
#elif defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace spark {

namespace {

std::string fallbackThreadName(std::uint64_t id)
{
    return "Thread " + std::to_string(id);
}

#if defined(_WIN32)

std::string utf8ThreadName(HANDLE thread)
{
    using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
    HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    auto get_description = kernel32 == nullptr
                               ? nullptr
                               : reinterpret_cast<GetThreadDescriptionFn>(
                                     ::GetProcAddress(kernel32, "GetThreadDescription"));
    if (get_description == nullptr) {
        return {};
    }

    PWSTR wide = nullptr;
    if (FAILED(get_description(thread, &wide)) || wide == nullptr) {
        return {};
    }
    int length = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string name;
    if (length > 1) {
        name.resize(static_cast<std::size_t>(length));
        ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), length, nullptr, nullptr);
        name.pop_back();
    }
    ::LocalFree(wide);
    return name;
}

std::optional<std::string> platformThreadName(std::uint64_t id)
{
    HANDLE thread = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(id));
    if (thread == nullptr) {
        return std::nullopt;
    }
    std::string name = utf8ThreadName(thread);
    ::CloseHandle(thread);
    return name.empty() ? std::nullopt
                        : std::optional<std::string>(std::move(name));
}

#elif defined(__linux__)

std::optional<std::string> platformThreadName(std::uint64_t id)
{
    std::string path = "/proc/self/task/" + std::to_string(id) + "/comm";
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::nullopt;
    }
    char buffer[256];
    ssize_t count = ::read(fd, buffer, sizeof(buffer));
    ::close(fd);
    if (count <= 0) {
        return std::nullopt;
    }
    while (count > 0 && (buffer[count - 1] == '\n' || buffer[count - 1] == '\r' || buffer[count - 1] == '\0')) {
        --count;
    }
    return count == 0
               ? std::nullopt
               : std::optional<std::string>(
                     std::string(buffer, static_cast<std::size_t>(count)));
}

#endif

}  // namespace

std::uint64_t currentNativeThreadId()
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#elif defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#else
    return 0;
#endif
}

std::optional<std::string> tryNativeThreadName(std::uint64_t id)
{
    return platformThreadName(id);
}

std::string nativeThreadName(std::uint64_t id)
{
    auto name = tryNativeThreadName(id);
    return name ? std::move(*name) : fallbackThreadName(id);
}

std::vector<ThreadInfo> enumerateProcessThreads()
{
    std::vector<ThreadInfo> threads;
#if defined(_WIN32)
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return threads;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (::Thread32First(snapshot, &entry)) {
        const DWORD process_id = ::GetCurrentProcessId();
        do {
            if (entry.th32OwnerProcessID == process_id) {
                const std::uint64_t id = static_cast<std::uint64_t>(entry.th32ThreadID);
                threads.push_back({id, nativeThreadName(id)});
            }
            entry.dwSize = sizeof(entry);
        } while (::Thread32Next(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
#elif defined(__linux__)
    DIR *directory = ::opendir("/proc/self/task");
    if (directory == nullptr) {
        return threads;
    }
    while (dirent *entry = ::readdir(directory)) {
        std::uint64_t id = 0;
        const char *begin = entry->d_name;
        const char *end = begin;
        while (*end >= '0' && *end <= '9') {
            ++end;
        }
        if (end == begin || *end != '\0') {
            continue;
        }
        auto [parsed_end, error] = std::from_chars(begin, end, id);
        if (error == std::errc{} && parsed_end == end && id != 0) {
            threads.push_back({id, nativeThreadName(id)});
        }
    }
    ::closedir(directory);
#endif

    std::sort(threads.begin(), threads.end(), [](const ThreadInfo &left, const ThreadInfo &right) {
        return left.id < right.id;
    });
    threads.erase(std::unique(threads.begin(), threads.end(), [](const ThreadInfo &left, const ThreadInfo &right) {
                      return left.id == right.id;
                  }),
                  threads.end());
    return threads;
}

}  // namespace spark
