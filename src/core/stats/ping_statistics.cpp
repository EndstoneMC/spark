#include "core/stats/ping_statistics.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace spark {

// --- PingSummary ---

PingSummary::PingSummary(std::vector<int> values) : sorted_(std::move(values))
{
    std::ranges::sort(sorted_);
}

int PingSummary::percentile(double p) const
{
    if (sorted_.empty()) {
        return 0;
    }
    // ceil(p * (n-1)), matching upstream PingSummary.java
    double idx = std::ceil(p * static_cast<double>(sorted_.size() - 1));
    auto i = static_cast<std::size_t>(idx);
    if (i >= sorted_.size()) {
        i = sorted_.size() - 1;
    }
    return sorted_[i];
}

int PingSummary::min() const
{
    if (sorted_.empty()) {
        return 0;
    }
    return sorted_.front();
}

int PingSummary::median() const
{
    return percentile(0.50);
}

int PingSummary::percentile95th() const
{
    return percentile(0.95);
}

int PingSummary::max() const
{
    if (sorted_.empty()) {
        return 0;
    }
    return sorted_.back();
}

// --- PingRollingAverage ---

void PingRollingAverage::add(int value)
{
    if (count_ < capacity_) {
        samples_.push_back(value);
        ++count_;
    }
    else {
        samples_[head_] = value;
        head_ = (head_ + 1) % capacity_;
    }
}

std::vector<int> PingRollingAverage::sortedCopy() const
{
    std::vector<int> copy(samples_.begin(), samples_.end());
    std::ranges::sort(copy);
    return copy;
}

double PingRollingAverage::mean() const
{
    if (count_ == 0) {
        return 0.0;
    }
    double sum = 0.0;
    for (int v : samples_) {
        sum += static_cast<double>(v);
    }
    return sum / static_cast<double>(count_);
}

int PingRollingAverage::min() const
{
    if (count_ == 0) {
        return 0;
    }
    return *std::ranges::min_element(samples_);
}

int PingRollingAverage::median() const
{
    auto sorted = sortedCopy();
    if (sorted.empty()) {
        return 0;
    }
    double idx = std::ceil(0.50 * static_cast<double>(sorted.size() - 1));
    auto i = static_cast<std::size_t>(idx);
    if (i >= sorted.size()) {
        i = sorted.size() - 1;
    }
    return sorted[i];
}

int PingRollingAverage::percentile95th() const
{
    auto sorted = sortedCopy();
    if (sorted.empty()) {
        return 0;
    }
    double idx = std::ceil(0.95 * static_cast<double>(sorted.size() - 1));
    auto i = static_cast<std::size_t>(idx);
    if (i >= sorted.size()) {
        i = sorted.size() - 1;
    }
    return sorted[i];
}

int PingRollingAverage::max() const
{
    if (count_ == 0) {
        return 0;
    }
    return *std::ranges::max_element(samples_);
}

// --- PingStatistics ---

PingStatistics::PingStatistics(PlayerPingProvider &provider) : provider_(provider), rolling_average_(kWindowSize) {}

bool PingStatistics::poll()
{
    PingSummary summary = currentSummary();
    if (summary.total() == 0) {
        return false;
    }
    rolling_average_.add(summary.median());
    return true;
}

PingSummary PingStatistics::currentSummary() const
{
    std::map<std::string, int> results = provider_.poll();
    std::vector<int> values;
    values.reserve(results.size());
    for (const auto &[name, ping] : results) {
        if (ping > 0) {
            values.push_back(ping);
        }
    }
    if (values.empty()) {
        return {};
    }
    return PingSummary(std::move(values));
}

PlayerPing PingStatistics::query(const std::string &player_name) const
{
    std::map<std::string, int> results = provider_.poll();
    // Exact match first
    auto it = results.find(player_name);
    if (it != results.end()) {
        return {.name = it->first, .ping = it->second};
    }
    // Case-insensitive match
    for (const auto &[name, ping] : results) {
        if (name.size() == player_name.size()) {
            bool match = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                char a = name[i];
                char b = player_name[i];
                if (a >= 'A' && a <= 'Z') {
                    a = static_cast<char>(a + 32);
                }
                if (b >= 'A' && b <= 'Z') {
                    b = static_cast<char>(b + 32);
                }
                if (a != b) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return {.name = name, .ping = ping};
            }
        }
    }
    return {};
}

}  // namespace spark
