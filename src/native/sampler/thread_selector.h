#ifndef ENDSTONE_SPARK_THREAD_SELECTOR_H
#define ENDSTONE_SPARK_THREAD_SELECTOR_H

#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace spark {

// Shared thread-name selection semantics for execution and allocation profiles.
// Regular expressions are compiled before sampling starts; matches are performed
// only from sampler/aggregator service threads.
class ThreadSelector {
public:
    bool configure(bool all_threads, bool regex_threads, const std::vector<std::string> &patterns, std::string &error);

    bool matches(std::string_view thread_name) const;
    bool selectsAll() const noexcept { return all_threads_; }

private:
    bool all_threads_ = false;
    bool regex_threads_ = false;
    std::vector<std::string> patterns_;
    std::vector<std::regex> regexes_;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_THREAD_SELECTOR_H
