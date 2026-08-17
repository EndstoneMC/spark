#include "core/stats/network_monitor.h"

#include <algorithm>
#include <cmath>
#include <utility>

#ifdef _WIN32
// clang-format off: iphlpapi.h requires windows.h types
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
// clang-format on
#else
#include <fstream>
#include <iterator>
#include <sstream>
#endif

#include <string>

namespace spark {

// ---- NetworkInterfaceInfo ----

bool NetworkInterfaceInfo::isZero() const
{
    return rx_bytes == 0 && rx_packets == 0 && rx_errors == 0 && tx_bytes == 0 && tx_packets == 0 && tx_errors == 0;
}

NetworkInterfaceInfo NetworkInterfaceInfo::subtract(const NetworkInterfaceInfo &other) const
{
    if (other.isZero()) {
        return *this;
    }
    NetworkInterfaceInfo diff;
    diff.name = name;
    diff.rx_bytes = rx_bytes - other.rx_bytes;
    diff.rx_packets = rx_packets - other.rx_packets;
    diff.rx_errors = rx_errors - other.rx_errors;
    diff.tx_bytes = tx_bytes - other.tx_bytes;
    diff.tx_packets = tx_packets - other.tx_packets;
    diff.tx_errors = tx_errors - other.tx_errors;
    return diff;
}

// ---- DoubleRollingAverage ----

DoubleRollingAverage::DoubleRollingAverage(std::size_t window_size) : capacity_(window_size)
{
    samples_.reserve(window_size);
}

void DoubleRollingAverage::add(double value)
{
    if (count_ < capacity_) {
        samples_.push_back(value);
        ++count_;
    }
    else {
        total_ -= samples_[head_];
        samples_[head_] = value;
        head_ = (head_ + 1) % capacity_;
    }
    total_ += value;
}

double DoubleRollingAverage::mean() const
{
    if (count_ == 0) {
        return 0.0;
    }
    return total_ / static_cast<double>(count_);
}

double DoubleRollingAverage::max() const
{
    if (count_ == 0) {
        return 0.0;
    }
    return *std::max_element(samples_.begin(), std::next(samples_.begin(), static_cast<std::ptrdiff_t>(count_)));
}

double DoubleRollingAverage::min() const
{
    if (count_ == 0) {
        return 0.0;
    }
    return *std::min_element(samples_.begin(), std::next(samples_.begin(), static_cast<std::ptrdiff_t>(count_)));
}

std::vector<double> DoubleRollingAverage::sortedCopy() const
{
    std::vector<double> s(samples_.begin(), std::next(samples_.begin(), static_cast<std::ptrdiff_t>(count_)));
    std::ranges::sort(s);
    return s;
}

double DoubleRollingAverage::percentile(double p) const
{
    if (count_ == 0) {
        return 0.0;
    }
    auto s = sortedCopy();
    int rank = static_cast<int>(std::ceil(p * static_cast<double>(s.size() - 1)));
    return s[static_cast<std::size_t>(rank)];
}

double DoubleRollingAverage::median() const
{
    return percentile(0.5);
}

double DoubleRollingAverage::percentile95() const
{
    return percentile(0.95);
}

// ---- NetworkInterfaceAverages ----

NetworkInterfaceAverages::NetworkInterfaceAverages(std::size_t window_size)
    : rx_bytes_per_second(window_size), tx_bytes_per_second(window_size), rx_packets_per_second(window_size),
      tx_packets_per_second(window_size)
{
}

void NetworkInterfaceAverages::accept(const NetworkInterfaceInfo &info, double poll_interval_seconds)
{
    if (poll_interval_seconds <= 0.0) {
        return;
    }
    double inv = 1.0 / poll_interval_seconds;
    rx_bytes_per_second.add(static_cast<double>(info.rx_bytes) * inv);
    tx_bytes_per_second.add(static_cast<double>(info.tx_bytes) * inv);
    rx_packets_per_second.add(static_cast<double>(info.rx_packets) * inv);
    tx_packets_per_second.add(static_cast<double>(info.tx_packets) * inv);
}

// ---- NetworkMonitor ----

NetworkMonitor::NetworkMonitor() : poll_fn_(pollNetworkInterfaces), now_fn_(Clock::now) {}

NetworkMonitor::NetworkMonitor(PollFn poll_fn, NowFn now_fn) : poll_fn_(std::move(poll_fn)), now_fn_(std::move(now_fn))
{
}

bool NetworkMonitor::shouldIgnore(const std::string &name)
{
    // Match upstream spark: ignore virtual eth adapters and container bridge networks.
    if (name.starts_with("veth")) {
        return true;
    }
    if (name.starts_with("br-")) {
        return true;
    }
    return false;
}

bool NetworkMonitor::poll()
{
    std::map<std::string, NetworkInterfaceInfo> current = poll_fn_();
    const Clock::time_point now = now_fn_();

    if (first_poll_) {
        previous_ = current;
        previous_poll_time_ = now;
        first_poll_ = false;
        return false;
    }

    const double elapsed_seconds = std::chrono::duration<double>(now - previous_poll_time_).count();
    previous_poll_time_ = now;
    if (elapsed_seconds <= 0.0 || !std::isfinite(elapsed_seconds)) {
        previous_ = current;
        return false;
    }

    bool accepted = false;
    std::erase_if(averages_, [&current](const auto &entry) { return current.find(entry.first) == current.end(); });
    for (const auto &[name, info] : current) {
        if (shouldIgnore(name)) {
            continue;
        }
        auto prev_it = previous_.find(name);
        if (prev_it == previous_.end()) {
            continue;
        }
        const NetworkInterfaceInfo &previous = prev_it->second;
        if (info.rx_bytes < previous.rx_bytes || info.rx_packets < previous.rx_packets ||
            info.rx_errors < previous.rx_errors || info.tx_bytes < previous.tx_bytes ||
            info.tx_packets < previous.tx_packets || info.tx_errors < previous.tx_errors) {
            continue;
        }
        auto it = averages_.find(name);
        if (it == averages_.end()) {
            it = averages_.emplace(name, NetworkInterfaceAverages(kWindowSize)).first;
        }
        it->second.accept(info.subtract(previous), elapsed_seconds);
        accepted = true;
    }

    previous_ = current;
    return accepted;
}

std::map<std::string, NetworkInterfaceSnapshot> NetworkMonitor::snapshot() const
{
    std::map<std::string, NetworkInterfaceSnapshot> out;
    for (const auto &[name, avg] : averages_) {
        if (avg.rx_bytes_per_second.samples() == 0) {
            continue;
        }
        NetworkInterfaceSnapshot s;
        s.rx_bytes_per_second.present = true;
        s.rx_bytes_per_second.mean = avg.rx_bytes_per_second.mean();
        s.rx_bytes_per_second.max = avg.rx_bytes_per_second.max();
        s.rx_bytes_per_second.min = avg.rx_bytes_per_second.min();
        s.rx_bytes_per_second.median = avg.rx_bytes_per_second.median();
        s.rx_bytes_per_second.percentile95 = avg.rx_bytes_per_second.percentile95();

        s.tx_bytes_per_second.present = true;
        s.tx_bytes_per_second.mean = avg.tx_bytes_per_second.mean();
        s.tx_bytes_per_second.max = avg.tx_bytes_per_second.max();
        s.tx_bytes_per_second.min = avg.tx_bytes_per_second.min();
        s.tx_bytes_per_second.median = avg.tx_bytes_per_second.median();
        s.tx_bytes_per_second.percentile95 = avg.tx_bytes_per_second.percentile95();

        s.rx_packets_per_second.present = true;
        s.rx_packets_per_second.mean = avg.rx_packets_per_second.mean();
        s.rx_packets_per_second.max = avg.rx_packets_per_second.max();
        s.rx_packets_per_second.min = avg.rx_packets_per_second.min();
        s.rx_packets_per_second.median = avg.rx_packets_per_second.median();
        s.rx_packets_per_second.percentile95 = avg.rx_packets_per_second.percentile95();

        s.tx_packets_per_second.present = true;
        s.tx_packets_per_second.mean = avg.tx_packets_per_second.mean();
        s.tx_packets_per_second.max = avg.tx_packets_per_second.max();
        s.tx_packets_per_second.min = avg.tx_packets_per_second.min();
        s.tx_packets_per_second.median = avg.tx_packets_per_second.median();
        s.tx_packets_per_second.percentile95 = avg.tx_packets_per_second.percentile95();

        out[name] = s;
    }
    return out;
}

std::map<std::string, NetworkInterfaceInfo> NetworkMonitor::systemTotals() const
{
    return previous_;
}

// ---- Platform-specific polling ----

#ifndef _WIN32

namespace {

// Parse /proc/net/dev, matching upstream spark's NetworkInterfaceInfo.read().
std::map<std::string, NetworkInterfaceInfo> readProcNetDev(const std::vector<std::string> &lines)
{
    if (lines.size() < 3) {
        return {};
    }

    // Header line 1: "Inter-|   Receive ..."
    // Header line 2: " face |bytes packets errs ..."
    const std::string &header = lines[1];
    std::size_t bar1 = header.find('|');
    std::size_t bar2 = header.find('|', bar1 == std::string::npos ? 0 : bar1 + 1);
    if (bar1 == std::string::npos || bar2 == std::string::npos) {
        return {};
    }

    // Split the RX and TX field name portions.
    std::string rx_header = header.substr(bar1 + 1, bar2 - bar1 - 1);
    std::string tx_header = header.substr(bar2 + 1);

    auto split_fields = [](const std::string &s) {
        std::vector<std::string> fields;
        std::istringstream iss(s);
        std::string field;
        while (iss >> field) {
            fields.push_back(field);
        }
        return fields;
    };

    std::vector<std::string> rx_fields = split_fields(rx_header);
    std::vector<std::string> tx_fields = split_fields(tx_header);
    const std::size_t rx_count = rx_fields.size();
    const std::size_t expected = rx_count + tx_fields.size();

    auto index_of = [](const std::vector<std::string> &v, const std::string &name) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i] == name) {
                return i;
            }
        }
        return std::string::npos;
    };

    const std::size_t f_rx_bytes = index_of(rx_fields, "bytes");
    const std::size_t f_rx_packets = index_of(rx_fields, "packets");
    const std::size_t f_rx_errors = index_of(rx_fields, "errs");
    const std::size_t tx_bytes = index_of(tx_fields, "bytes");
    const std::size_t tx_packets = index_of(tx_fields, "packets");
    const std::size_t tx_errors = index_of(tx_fields, "errs");

    if (f_rx_bytes == std::string::npos || f_rx_packets == std::string::npos || f_rx_errors == std::string::npos ||
        tx_bytes == std::string::npos || tx_packets == std::string::npos || tx_errors == std::string::npos) {
        return {};
    }
    const std::size_t f_tx_bytes = rx_count + tx_bytes;
    const std::size_t f_tx_packets = rx_count + tx_packets;
    const std::size_t f_tx_errors = rx_count + tx_errors;

    std::map<std::string, NetworkInterfaceInfo> result;
    for (std::size_t i = 2; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        // Interface name is everything before the colon, trimmed.
        std::size_t start = line.find_first_not_of(" \t");
        std::size_t end = line.find_last_not_of(" \t", colon - 1);
        if (start == std::string::npos || end == std::string::npos) {
            continue;
        }
        std::string name = line.substr(start, end - start + 1);

        // Values are after the colon.
        std::istringstream iss(line.substr(colon + 1));
        std::vector<std::uint64_t> values;
        std::uint64_t v = 0;
        while (iss >> v) {
            values.push_back(v);
        }

        if (values.size() != expected) {
            continue;
        }

        NetworkInterfaceInfo info;
        info.name = name;
        info.rx_bytes = values[f_rx_bytes];
        info.rx_packets = values[f_rx_packets];
        info.rx_errors = values[f_rx_errors];
        info.tx_bytes = values[f_tx_bytes];
        info.tx_packets = values[f_tx_packets];
        info.tx_errors = values[f_tx_errors];
        result[name] = info;
    }
    return result;
}

}  // namespace

std::map<std::string, NetworkInterfaceInfo> pollNetworkInterfaces()
{
    std::ifstream f("/proc/net/dev");
    if (!f.is_open()) {
        return {};
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        lines.push_back(line);
    }
    return readProcNetDev(lines);
}

#else  // _WIN32

namespace {

bool isUsableWindowsInterface(const MIB_IF_ROW2 &row)
{
    if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.OperStatus != IfOperStatusUp) {
        return false;
    }

    const auto &flags = row.InterfaceAndOperStatusFlags;
    return !flags.FilterInterface && !flags.NotMediaConnected && !flags.Paused && !flags.EndPointInterface;
}

std::string wideToUtf8(const wchar_t *value)
{
    if (value == nullptr || *value == L'\0') {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), required, nullptr, nullptr) <= 0) {
        return {};
    }
    result.resize(static_cast<std::size_t>(required - 1));
    return result;
}

std::string windowsInterfaceDisplayName(const MIB_IF_ROW2 &row)
{
    std::string name = wideToUtf8(row.Alias);
    if (!name.empty()) {
        return name;
    }

    char interface_name[IF_MAX_STRING_SIZE + 1]{};
    if (ConvertInterfaceLuidToNameA(&row.InterfaceLuid, interface_name, sizeof(interface_name)) == NO_ERROR) {
        return interface_name;
    }
    return {};
}

}  // namespace

std::map<std::string, NetworkInterfaceInfo> pollNetworkInterfaces()
{
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr) {
        return {};
    }

    std::map<std::string, NetworkInterfaceInfo> result;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2 &row = table->Table[i];
        if (!isUsableWindowsInterface(row)) {
            continue;
        }

        std::string interface_name = windowsInterfaceDisplayName(row);
        if (interface_name.empty()) {
            continue;
        }

        NetworkInterfaceInfo info;
        info.name = std::move(interface_name);
        info.rx_bytes = row.InOctets;
        info.tx_bytes = row.OutOctets;
        info.rx_packets = row.InUcastPkts + row.InNUcastPkts;
        info.tx_packets = row.OutUcastPkts + row.OutNUcastPkts;
        info.rx_errors = row.InErrors;
        info.tx_errors = row.OutErrors;

        // Interface aliases are normally unique. If Windows reports a duplicate
        // alias, keep both entries instead of silently overwriting one.
        if (result.contains(info.name)) {
            info.name += " (" + std::to_string(row.InterfaceIndex) + ")";
        }
        result[info.name] = std::move(info);
    }
    FreeMibTable(table);

    return result;
}

#endif

}  // namespace spark
