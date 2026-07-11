#ifndef ENDSTONE_SPARK_ARGUMENTS_H
#define ENDSTONE_SPARK_ARGUMENTS_H

#include <map>
#include <set>
#include <string>
#include <vector>

namespace spark {

// A spark-style command argument parser: a leading sub-command followed by
// `--flag [value]` options (a flag may repeat). Endstone hands commands a single
// rest-of-line string, so we tokenize it ourselves.
class Arguments {
public:
    explicit Arguments(const std::vector<std::string> &tokens);

    // Tokenize a raw rest-of-line string on whitespace.
    static std::vector<std::string> tokenize(const std::string &line);

    const std::string &subCommand() const
    {
        return sub_;
    }
    const std::vector<std::string> &raw() const
    {
        return raw_;
    }

    bool boolFlag(const std::string &name) const;
    long intFlag(const std::string &name, long fallback = -1) const;
    double doubleFlag(const std::string &name, double fallback = -1.0) const;
    std::vector<std::string> stringFlag(const std::string &name) const;

private:
    std::vector<std::string> raw_;
    std::string sub_;
    std::multimap<std::string, std::string> values_;
    std::set<std::string> present_;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_ARGUMENTS_H
