#include "core/stats/network_monitor.h"

#include <algorithm>
#include <cmath>
#include <utility>

#if defined(_WIN32)
// clang-format off: iphlpapi.h requires windows.h types
#include <windows.h>
#include <iphlpapi.h>
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
    return *std::max_element(samples_.begin(), samples_.begin() + count_);
}

double DoubleRollingAverage::min() const
{
    if (count_ == 0) {
        return 0.0;
    }
    return *std::min_element(samples_.begin(), samples_.begin() + count_);
}

std::vector<double> DoubleRollingAverage::sortedCopy() const
{
    std::vector<double> s(samples_.begin(), samples_.begin() + count_);
    std::sort(s.begin(), s.end());
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

NetworkMonitor::NetworkMonitor() : poll_fn_(pollNetworkInterfaces) {}

NetworkMonitor::NetworkMonitor(PollFn poll_fn) : poll_fn_(std::move(poll_fn)) {}

bool NetworkMonitor::shouldIgnore(const std::string &name)
{
    // Match upstream spark: ignore virtual eth adapters and container bridge networks.
    if (name.rfind("veth", 0) == 0) {
        return true;
    }
    if (name.rfind("br-", 0) == 0) {
        return true;
    }
    return false;
}

bool NetworkMonitor::poll()
{
    std::map<std::string, NetworkInterfaceInfo> current = poll_fn_();

    if (first_poll_) {
        previous_ = current;
        first_poll_ = false;
        return false;
    }

    if (current.empty() && previous_.empty()) {
        return false;
    }

    // Compute diff and submit to rolling averages.
    for (const auto &[name, info] : current) {
        if (shouldIgnore(name)) {
            continue;
        }
        auto it = averages_.find(name);
        if (it == averages_.end()) {
            it = averages_.emplace(name, NetworkInterfaceAverages(kWindowSize)).first;
        }
        auto prev_it = previous_.find(name);
        NetworkInterfaceInfo diff = prev_it != previous_.end() ? info.subtract(prev_it->second) : info;
        it->second.accept(diff, static_cast<double>(kPollIntervalSeconds));
    }

    previous_ = current;
    return true;
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

#if !defined(_WIN32)

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

    auto splitFields = [](const std::string &s) {
        std::vector<std::string> fields;
        std::istringstream iss(s);
        std::string field;
        while (iss >> field) {
            fields.push_back(field);
        }
        return fields;
    };

    std::vector<std::string> rx_fields = splitFields(rx_header);
    std::vector<std::string> tx_fields = splitFields(tx_header);
    int rx_count = static_cast<int>(rx_fields.size());
    int tx_count = static_cast<int>(tx_fields.size());
    int expected = rx_count + tx_count;

    auto indexOf = [](const std::vector<std::string> &v, const std::string &name) {
        for (int i = 0; i < static_cast<int>(v.size()); ++i) {
            if (v[i] == name) {
                return i;
            }
        }
        return -1;
    };

    int f_rx_bytes = indexOf(rx_fields, "bytes");
    int f_rx_packets = indexOf(rx_fields, "packets");
    int f_rx_errors = indexOf(rx_fields, "errs");
    int f_tx_bytes = rx_count + indexOf(tx_fields, "bytes");
    int f_tx_packets = rx_count + indexOf(tx_fields, "packets");
    int f_tx_errors = rx_count + indexOf(tx_fields, "errs");

    if (f_rx_bytes < 0 || f_rx_packets < 0 || f_rx_errors < 0 || f_tx_bytes < 0 || f_tx_packets < 0 ||
        f_tx_errors < 0) {
        return {};
    }

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
        std::vector<long long> values;
        long long v;
        while (iss >> v) {
            values.push_back(v);
        }

        if (static_cast<int>(values.size()) != expected) {
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

std::map<std::string, NetworkInterfaceInfo> pollNetworkInterfaces()
{
    ULONG table_size = 0;
    if (GetIfTable(nullptr, &table_size, FALSE) != ERROR_INSUFFICIENT_BUFFER || table_size == 0) {
        return {};
    }

    std::vector<BYTE> buffer(table_size);
    PMIB_IFTABLE table = reinterpret_cast<PMIB_IFTABLE>(buffer.data());
    if (GetIfTable(table, &table_size, FALSE) != NO_ERROR) {
        return {};
    }

    std::map<std::string, NetworkInterfaceInfo> result;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_IFROW &row = table->table[i];
        if (row.dwType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }

        // Convert wide interface name to narrow string.
        std::string name;
        for (int j = 0; j < MAX_INTERFACE_NAME_LEN && row.wszName[j] != L'\0'; ++j) {
            name.push_back(static_cast<char>(row.wszName[j]));
        }
        if (name.empty()) {
            continue;
        }

        NetworkInterfaceInfo info;
        info.name = name;
        info.rx_bytes = static_cast<std::int64_t>(row.dwInOctets);
        info.tx_bytes = static_cast<std::int64_t>(row.dwOutOctets);
        info.rx_packets = static_cast<std::int64_t>(row.dwInUcastPkts + row.dwInNUcastPkts);
        info.tx_packets = static_cast<std::int64_t>(row.dwOutUcastPkts + row.dwOutNUcastPkts);
        info.rx_errors = static_cast<std::int64_t>(row.dwInErrors);
        info.tx_errors = static_cast<std::int64_t>(row.dwOutErrors);
        result[name] = info;
    }

    return result;
}

#endif

}  // namespace spark
