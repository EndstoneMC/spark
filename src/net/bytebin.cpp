#include "net/bytebin.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include <unistd.h>

namespace spark {

namespace {

std::string shellQuote(const std::string &s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        }
        else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string toLower(std::string s)
{
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

}  // namespace

UploadResult uploadToBytebin(const std::string &gzipped_body, const std::string &bytebin_url,
                             const std::string &content_type, const std::string &user_agent)
{
    UploadResult result;

    char tmpl[] = "/tmp/endstone-spark-XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) {
        result.error = "failed to create temp file";
        return result;
    }
    std::string tmpfile = tmpl;
    ssize_t written = ::write(fd, gzipped_body.data(), gzipped_body.size());
    ::close(fd);
    if (written != static_cast<ssize_t>(gzipped_body.size())) {
        ::unlink(tmpfile.c_str());
        result.error = "failed to write temp file";
        return result;
    }

    std::string url = bytebin_url;
    if (url.empty() || url.back() != '/') {
        url += '/';
    }
    url += "post";

    // Capture curl's stderr so failures are actionable, and force common bin dirs onto
    // PATH — a server process may be launched with a minimal environment.
    char err_template[] = "/tmp/endstone-spark-err-XXXXXX";
    int err_fd = ::mkstemp(err_template);
    std::string err_file;
    if (err_fd >= 0) {
        ::close(err_fd);
        err_file = err_template;
    }

    std::string cmd = "PATH=\"$PATH:/usr/bin:/usr/local/bin:/bin\" curl -s -S -X POST --data-binary @" +
                      shellQuote(tmpfile) + " -H " + shellQuote("Content-Type: " + content_type) + " -H " +
                      shellQuote("Content-Encoding: gzip") + " -H " + shellQuote("User-Agent: " + user_agent) +
                      " -D - -o /dev/null " + shellQuote(url);
    if (!err_file.empty()) {
        cmd += " 2>" + shellQuote(err_file);
    }

    FILE *pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        ::unlink(tmpfile.c_str());
        if (!err_file.empty()) {
            ::unlink(err_file.c_str());
        }
        result.error = "failed to run curl";
        return result;
    }
    std::string headers;
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        headers.append(buffer, n);
    }
    int status = ::pclose(pipe);
    ::unlink(tmpfile.c_str());

    std::string curl_err;
    if (!err_file.empty()) {
        std::ifstream ef(err_file);
        curl_err.assign(std::istreambuf_iterator<char>(ef), std::istreambuf_iterator<char>());
        ::unlink(err_file.c_str());
        while (!curl_err.empty() && (curl_err.back() == '\n' || curl_err.back() == '\r' || curl_err.back() == ' ')) {
            curl_err.pop_back();
        }
    }

    if (status != 0) {
        result.error = curl_err.empty() ? "curl failed (is curl installed and reachable?)" : ("curl: " + curl_err);
        return result;
    }

    std::string key;
    std::size_t pos = 0;
    while (pos < headers.size()) {
        std::size_t eol = headers.find('\n', pos);
        std::string line = headers.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? headers.size() : eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (toLower(line).rfind("location:", 0) == 0) {
            std::string value = line.substr(9);
            std::size_t start = value.find_first_not_of(" \t");
            if (start != std::string::npos) {
                value = value.substr(start);
            }
            std::size_t slash = value.find_last_of('/');
            key = slash == std::string::npos ? value : value.substr(slash + 1);
        }
    }

    if (key.empty()) {
        result.error = "bytebin did not return a content key";
        return result;
    }
    result.ok = true;
    result.key = key;
    return result;
}

}  // namespace spark
