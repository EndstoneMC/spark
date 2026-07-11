#include "stats/system_stats.h"

#include <chrono>

#if !defined(_WIN32)
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

#if !defined(_WIN32)

namespace {

std::int64_t steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

std::int64_t processRssBytes()
{
    std::ifstream f("/proc/self/statm");
    long pages_total = 0, pages_res = 0;  // statm: field0 = VmSize, field1 = VmRSS (in pages)
    if (f >> pages_total >> pages_res) {
        return static_cast<std::int64_t>(pages_res) * sysconf(_SC_PAGESIZE);
    }
    return 0;
}

std::int64_t processVirtualBytes()
{
    std::ifstream f("/proc/self/statm");
    long pages_total = 0;
    if (f >> pages_total) {
        return static_cast<std::int64_t>(pages_total) * sysconf(_SC_PAGESIZE);
    }
    return 0;
}

CpuSnapshot captureCpuSnapshot()
{
    CpuSnapshot s;

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
    }

    s.wall_ms = steadyNowMs();
    s.valid = true;
    return s;
}

SystemStats gatherSystemStats(const CpuSnapshot &baseline, const std::string &disk_path)
{
    SystemStats s;
    s.present = true;

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) {
        ncpu = 1;
    }
    long clk = sysconf(_SC_CLK_TCK);
    if (clk < 1) {
        clk = 100;
    }
    s.cpu_threads = static_cast<int>(ncpu);

    CpuSnapshot now = captureCpuSnapshot();
    if (baseline.valid && now.valid) {
        double wall_s = static_cast<double>(now.wall_ms - baseline.wall_ms) / 1000.0;
        if (wall_s > 0.0) {
            double proc_cpu_s = static_cast<double>(now.process_ticks - baseline.process_ticks) / clk;
            s.cpu_process = proc_cpu_s / (wall_s * static_cast<double>(ncpu));
        }
        unsigned long long dt = now.system_total - baseline.system_total;
        unsigned long long db = now.system_busy - baseline.system_busy;
        if (dt > 0) {
            s.cpu_system = static_cast<double>(db) / static_cast<double>(dt);
        }
    }
    s.cpu_process = s.cpu_process < 0 ? 0 : (s.cpu_process > 1 ? 1 : s.cpu_process);
    s.cpu_system = s.cpu_system < 0 ? 0 : (s.cpu_system > 1 ? 1 : s.cpu_system);

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
        while (std::getline(f, line)) {
            std::istringstream iss(line);
            std::string key;
            long long value = 0;
            iss >> key >> value;
            if (key == "MemTotal:") {
                mem_total = value;
            }
            else if (key == "MemAvailable:") {
                mem_avail = value;
            }
            else if (key == "SwapTotal:") {
                swap_total = value;
            }
            else if (key == "SwapFree:") {
                swap_free = value;
            }
        }
        s.mem_total = mem_total * 1024;
        s.mem_used = (mem_total - mem_avail) * 1024;
        s.swap_total = swap_total * 1024;
        s.swap_used = (swap_total - swap_free) * 1024;
    }

    // Disk.
    {
        struct statvfs st{};
        if (statvfs(disk_path.c_str(), &st) == 0) {
            s.disk_total = static_cast<std::int64_t>(st.f_blocks) * static_cast<std::int64_t>(st.f_frsize);
            s.disk_used =
                static_cast<std::int64_t>(st.f_blocks - st.f_bfree) * static_cast<std::int64_t>(st.f_frsize);
        }
    }

    // OS.
    {
        struct utsname u{};
        if (uname(&u) == 0) {
            s.os_name = u.sysname;
            s.os_version = u.release;
            s.os_arch = u.machine;
        }
    }

    return s;
}

#else  // _WIN32

CpuSnapshot captureCpuSnapshot()
{
    return {};
}

SystemStats gatherSystemStats(const CpuSnapshot &, const std::string &)
{
    return {};  // TODO(windows): GetSystemInfo / GlobalMemoryStatusEx / GetDiskFreeSpaceEx
}

std::int64_t processRssBytes()
{
    return 0;
}

#endif

}  // namespace spark
