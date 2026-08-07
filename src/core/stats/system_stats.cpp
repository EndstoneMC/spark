#include "core/stats/system_stats.h"

#include <algorithm>
#include <chrono>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <string>
#else
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace spark {

CpuUsage cpuUsageBetween(const CpuSnapshot &before, const CpuSnapshot &after)
{
    CpuUsage usage;
    if (!before.valid || !after.valid || after.wall_ms <= before.wall_ms) {
        return usage;
    }

    const double wall_seconds =
        static_cast<double>(after.wall_ms - before.wall_ms) / 1000.0;
    if (after.process_ticks >= before.process_ticks &&
        after.process_ticks_per_second > 0.0 && after.cpu_threads > 0) {
        const double process_seconds =
            static_cast<double>(after.process_ticks - before.process_ticks) /
            after.process_ticks_per_second;
        usage.process = std::clamp(
            process_seconds /
                (wall_seconds * static_cast<double>(after.cpu_threads)),
            0.0, 1.0);
        usage.process_valid = true;
    }
    if (after.system_total >= before.system_total &&
        after.system_busy >= before.system_busy) {
        const unsigned long long total_delta =
            after.system_total - before.system_total;
        if (total_delta > 0) {
            usage.system = std::clamp(
                static_cast<double>(after.system_busy - before.system_busy) /
                    static_cast<double>(total_delta),
                0.0, 1.0);
            usage.system_valid = true;
        }
    }
    return usage;
}

#if !defined(_WIN32)

namespace {

std::int64_t steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ProcessStats gatherProcessStats()
{
    ProcessStats stats;
    std::ifstream f("/proc/self/statm");
    long pages_total = 0, pages_resident = 0;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0 && f >> pages_total >> pages_resident) {
        stats.virtual_bytes =
            static_cast<std::int64_t>(pages_total) * page_size;
        stats.rss_bytes =
            static_cast<std::int64_t>(pages_resident) * page_size;
        stats.virtual_present = true;
        stats.rss_present = true;
    }

    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "Threads:") {
            long threads = 0;
            if (status >> threads && threads > 0 &&
                threads <= (std::numeric_limits<int>::max)()) {
                stats.threads = static_cast<int>(threads);
                stats.threads_present = true;
            }
            break;
        }
        std::getline(status, key);
    }
    return stats;
}

CpuSnapshot captureCpuSnapshot()
{
    CpuSnapshot s;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    long ticks_per_second = sysconf(_SC_CLK_TCK);
    s.cpu_threads = ncpu > 0 ? static_cast<int>(ncpu) : 1;
    s.process_ticks_per_second =
        ticks_per_second > 0 ? static_cast<double>(ticks_per_second) : 100.0;
    bool process_valid = false;
    bool system_valid = false;

    // Process CPU: utime + stime from /proc/self/stat (fields after the ')').
    {
        std::ifstream f("/proc/self/stat");
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::size_t rparen = content.find_last_of(')');
        if (rparen != std::string::npos) {
            std::istringstream iss(content.substr(rparen + 1));
            std::string tok;
            int index = 0;
            unsigned long long utime = 0, stime = 0;
            while (iss >> tok) {
                if (index == 11) {
                    utime = std::strtoull(tok.c_str(), nullptr, 10);  // field 14
                }
                else if (index == 12) {
                    stime = std::strtoull(tok.c_str(), nullptr, 10);  // field 15
                    process_valid = true;
                    break;
                }
                ++index;
            }
            s.process_ticks = utime + stime;
        }
    }

    // System CPU: aggregate line of /proc/stat.
    {
        std::ifstream f("/proc/stat");
        std::string label;
        f >> label;  // "cpu"
        unsigned long long value = 0, total = 0, idle_all = 0;
        int index = 0;
        while (index < 8 && f >> value) {
            total += value;
            if (index == 3 || index == 4) {  // idle + iowait
                idle_all += value;
            }
            ++index;
        }
        s.system_total = total;
        s.system_busy = total > idle_all ? total - idle_all : 0;
        system_valid = label == "cpu" && index >= 4 && total > 0;
    }

    s.wall_ms = steadyNowMs();
    s.valid = process_valid && system_valid;
    return s;
}

SystemStats gatherSystemStats(const std::string &disk_path)
{
    SystemStats s;

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu > 0 && ncpu <= (std::numeric_limits<int>::max)()) {
        s.cpu_threads = static_cast<int>(ncpu);
        s.cpu_present = true;
    }

    // CPU model.
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("model name", 0) == 0) {
                std::size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::size_t start = line.find_first_not_of(" \t", colon + 1);
                    if (start != std::string::npos) {
                        s.cpu_model = line.substr(start);
                        s.cpu_present = true;
                    }
                }
                break;
            }
        }
    }

    // Physical + swap memory (/proc/meminfo, kB).
    {
        std::ifstream f("/proc/meminfo");
        std::string line;
        long long mem_total = 0, mem_avail = 0, swap_total = 0, swap_free = 0;
        bool have_mem_total = false, have_mem_available = false;
        bool have_swap_total = false, have_swap_free = false;
        while (std::getline(f, line)) {
            std::istringstream iss(line);
            std::string key;
            long long value = 0;
            iss >> key >> value;
            if (key == "MemTotal:") {
                mem_total = value;
                have_mem_total = true;
            }
            else if (key == "MemAvailable:") {
                mem_avail = value;
                have_mem_available = true;
            }
            else if (key == "SwapTotal:") {
                swap_total = value;
                have_swap_total = true;
            }
            else if (key == "SwapFree:") {
                swap_free = value;
                have_swap_free = true;
            }
        }
        if (have_mem_total && have_mem_available && mem_total >= mem_avail) {
            s.mem_total = mem_total * 1024;
            s.mem_used = (mem_total - mem_avail) * 1024;
            s.memory_present = true;
        }
        if (have_swap_total && have_swap_free && swap_total >= swap_free) {
            s.swap_total = swap_total * 1024;
            s.swap_used = (swap_total - swap_free) * 1024;
            s.swap_present = true;
        }
    }

    // Disk.
    {
        struct statvfs st{};
        if (statvfs(disk_path.c_str(), &st) == 0) {
            s.disk_total = static_cast<std::int64_t>(st.f_blocks) * static_cast<std::int64_t>(st.f_frsize);
            s.disk_used =
                static_cast<std::int64_t>(st.f_blocks - st.f_bfree) * static_cast<std::int64_t>(st.f_frsize);
            s.disk_present = true;
        }
    }

    // OS.
    {
        struct utsname u{};
        if (uname(&u) == 0) {
            s.os_name = u.sysname;
            s.os_version = u.release;
            s.os_arch = u.machine;
            s.os_present = true;
        }
    }

    s.present = s.cpu_present || s.memory_present || s.swap_present ||
                s.disk_present || s.os_present;
    return s;
}

#else  // _WIN32

namespace {

constexpr double kWindowsTicksPerSecond = 10000000.0;

std::int64_t steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

unsigned long long fileTimeTicks(const FILETIME &time)
{
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

std::string nativeArchitecture(WORD architecture)
{
    switch (architecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        return "x86_64";
    case PROCESSOR_ARCHITECTURE_ARM64:
        return "aarch64";
    case PROCESSOR_ARCHITECTURE_INTEL:
        return "x86";
    default:
        return "unknown";
    }
}

}  // namespace

CpuSnapshot captureCpuSnapshot()
{
    CpuSnapshot s;
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    s.cpu_threads =
        system_info.dwNumberOfProcessors > 0
            ? static_cast<int>(system_info.dwNumberOfProcessors)
            : 1;
    s.process_ticks_per_second = kWindowsTicksPerSecond;

    FILETIME creation{}, exit{}, process_kernel{}, process_user{};
    FILETIME idle{}, system_kernel{}, system_user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &process_kernel, &process_user) ||
        !GetSystemTimes(&idle, &system_kernel, &system_user)) {
        return s;
    }

    s.process_ticks = fileTimeTicks(process_kernel) + fileTimeTicks(process_user);
    s.system_total = fileTimeTicks(system_kernel) + fileTimeTicks(system_user);
    unsigned long long idle_ticks = fileTimeTicks(idle);
    s.system_busy = s.system_total > idle_ticks ? s.system_total - idle_ticks : 0;
    s.wall_ms = steadyNowMs();
    s.valid = true;
    return s;
}

SystemStats gatherSystemStats(const std::string &disk_path)
{
    SystemStats s;

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    if (system_info.dwNumberOfProcessors > 0) {
        s.cpu_threads = static_cast<int>(system_info.dwNumberOfProcessors);
        s.cpu_present = true;
    }

    HKEY cpu_key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_QUERY_VALUE,
                      &cpu_key) == ERROR_SUCCESS) {
        char model[256]{};
        DWORD type = 0;
        DWORD size = sizeof(model);
        if (RegQueryValueExA(cpu_key, "ProcessorNameString", nullptr, &type,
                            reinterpret_cast<BYTE *>(model), &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            model[sizeof(model) - 1] = '\0';
            s.cpu_model = model;
            std::size_t start = s.cpu_model.find_first_not_of(" \t");
            std::size_t end = s.cpu_model.find_last_not_of(" \t");
            s.cpu_model = start == std::string::npos ? std::string() : s.cpu_model.substr(start, end - start + 1);
            s.cpu_present = true;
        }
        RegCloseKey(cpu_key);
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        s.mem_total = static_cast<std::int64_t>(memory.ullTotalPhys);
        unsigned long long physical_used =
            memory.ullTotalPhys > memory.ullAvailPhys ? memory.ullTotalPhys - memory.ullAvailPhys : 0;
        s.mem_used = static_cast<std::int64_t>(physical_used);
        s.memory_present = true;

        // Windows exposes commit limit/availability rather than Linux-style swap
        // counters. Approximate page-file capacity as commit limit beyond physical
        // RAM, and usage as committed bytes beyond currently used physical RAM.
        unsigned long long swap_total =
            memory.ullTotalPageFile > memory.ullTotalPhys ? memory.ullTotalPageFile - memory.ullTotalPhys : 0;
        unsigned long long commit_used = memory.ullTotalPageFile > memory.ullAvailPageFile
                                             ? memory.ullTotalPageFile - memory.ullAvailPageFile
                                             : 0;
        unsigned long long swap_used = commit_used > physical_used ? commit_used - physical_used : 0;
        if (swap_used > swap_total) {
            swap_used = swap_total;
        }
        s.swap_total = static_cast<std::int64_t>(swap_total);
        s.swap_used = static_cast<std::int64_t>(swap_used);
        s.swap_present = true;
    }

    ULARGE_INTEGER free_available{}, disk_total{}, disk_free{};
    const char *path = disk_path.empty() ? "." : disk_path.c_str();
    if (GetDiskFreeSpaceExA(path, &free_available, &disk_total, &disk_free)) {
        s.disk_total = static_cast<std::int64_t>(disk_total.QuadPart);
        s.disk_used = static_cast<std::int64_t>(disk_total.QuadPart - disk_free.QuadPart);
        s.disk_present = true;
    }

    SYSTEM_INFO native_info{};
    GetNativeSystemInfo(&native_info);
    s.os_arch = nativeArchitecture(native_info.wProcessorArchitecture);
    s.os_name = "Windows";
    s.os_present = true;

    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto rtl_get_version = ntdll == nullptr
                               ? nullptr
                               : reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtl_get_version != nullptr) {
        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (rtl_get_version(&version) >= 0) {
            s.os_version = std::to_string(version.dwMajorVersion) + "." +
                           std::to_string(version.dwMinorVersion) + "." + std::to_string(version.dwBuildNumber);
        }
    }

    s.present = s.cpu_present || s.memory_present || s.swap_present ||
                s.disk_present || s.os_present;
    return s;
}

ProcessStats gatherProcessStats()
{
    ProcessStats stats;
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        stats.rss_bytes = static_cast<std::int64_t>(counters.WorkingSetSize);
        stats.rss_present = true;
    }

    SYSTEM_INFO system_info{};
    GetNativeSystemInfo(&system_info);
    std::uintptr_t address =
        reinterpret_cast<std::uintptr_t>(system_info.lpMinimumApplicationAddress);
    const std::uintptr_t maximum =
        reinterpret_cast<std::uintptr_t>(system_info.lpMaximumApplicationAddress);
    std::uint64_t virtual_bytes = 0;
    bool virtual_query_succeeded = false;
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void *>(address), &region,
                         sizeof(region)) == 0) {
            break;
        }
        virtual_query_succeeded = true;
        if (region.State != MEM_FREE) {
            virtual_bytes += static_cast<std::uint64_t>(region.RegionSize);
        }
        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (region.RegionSize == 0 ||
            base > (std::numeric_limits<std::uintptr_t>::max)() -
                       region.RegionSize) {
            break;
        }
        const std::uintptr_t next = base + region.RegionSize;
        if (next <= address) {
            break;
        }
        address = next;
    }
    if (virtual_query_succeeded &&
        virtual_bytes <=
            static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        stats.virtual_bytes = static_cast<std::int64_t>(virtual_bytes);
        stats.virtual_present = true;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        int count = 0;
        const DWORD process_id = GetCurrentProcessId();
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == process_id &&
                    count < (std::numeric_limits<int>::max)()) {
                    ++count;
                }
            } while (Thread32Next(snapshot, &entry));
            stats.threads = count;
            stats.threads_present = count > 0;
        }
        CloseHandle(snapshot);
    }

    return stats;
}

#endif

}  // namespace spark
