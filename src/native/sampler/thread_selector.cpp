#include "native/sampler/thread_selector.h"

#include <algorithm>
#include <cctype>

namespace spark {
namespace {

bool equalsIgnoreCase(std::string_view left, std::string_view right)
{
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
}

}  // namespace

bool ThreadSelector::configure(bool all_threads, bool regex_threads, const std::vector<std::string> &patterns,
                               std::string &error)
{
    all_threads_ = all_threads;
    regex_threads_ = regex_threads;
    patterns_ = patterns;
    regexes_.clear();
    error.clear();

    if (!regex_threads_) {
        return true;
    }

    try {
        regexes_.reserve(patterns_.size());
        for (const std::string &pattern : patterns_) {
            regexes_.emplace_back(pattern, std::regex_constants::ECMAScript | std::regex_constants::icase);
        }
    }
    catch (const std::regex_error &regex_error) {
        error = std::string("invalid thread name regex: ") + regex_error.what();
        regexes_.clear();
        return false;
    }
    return true;
}

bool ThreadSelector::matches(std::string_view thread_name) const
{
    if (all_threads_) {
        return true;
    }
    if (regex_threads_) {
        return std::ranges::any_of(regexes_, [&](const std::regex &pattern) {
            return std::regex_match(thread_name.begin(), thread_name.end(), pattern);
        });
    }
    return std::ranges::any_of(patterns_,
                               [&](const std::string &pattern) { return equalsIgnoreCase(thread_name, pattern); });
}

}  // namespace spark
