#include "net/bytebin.h"

#include <curl/curl.h>

#include <cctype>
#include <memory>
#include <mutex>
#include <string_view>

namespace spark {

namespace {

using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
using CurlHeaders = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

std::once_flag curl_init_flag;
CURLcode curl_init_result = CURLE_FAILED_INIT;

std::string trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

bool startsWithIgnoreCase(std::string_view value, std::string_view prefix) {
  if (value.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(value[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

std::size_t captureHeader(char *data, std::size_t size, std::size_t count,
                          void *user_data) {
  const std::size_t length = size * count;
  std::string_view line(data, length);
  if (startsWithIgnoreCase(line, "location:")) {
    auto *location = static_cast<std::string *>(user_data);
    *location = trim(line.substr(9));
  }
  return length;
}

std::size_t discardBody(char *, std::size_t size, std::size_t count, void *) {
  return size * count;
}

std::string contentKeyFromLocation(std::string_view location) {
  while (!location.empty() && location.back() == '/') {
    location.remove_suffix(1);
  }
  const auto slash = location.find_last_of('/');
  return std::string(
      slash == std::string_view::npos ? location : location.substr(slash + 1));
}

} // namespace

UploadResult uploadToBytebin(const std::string &gzipped_body,
                             const std::string &bytebin_url,
                             const std::string &content_type,
                             const std::string &user_agent) {
  UploadResult result;

  std::call_once(curl_init_flag, [] {
    curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
  });
  if (curl_init_result != CURLE_OK) {
    result.error = std::string("failed to initialize libcurl: ") +
                   curl_easy_strerror(curl_init_result);
    return result;
  }

  CurlHandle curl(curl_easy_init(), curl_easy_cleanup);
  if (!curl) {
    result.error = "failed to create libcurl request";
    return result;
  }

  std::string url = bytebin_url;
  if (url.empty() || url.back() != '/') {
    url += '/';
  }
  url += "post";

  curl_slist *raw_headers = nullptr;
  const auto append_header = [&raw_headers](const char *value) {
    curl_slist *updated = curl_slist_append(raw_headers, value);
    if (updated == nullptr) {
      return false;
    }
    raw_headers = updated;
    return true;
  };
  const std::string content_type_header = "Content-Type: " + content_type;
  if (!append_header(content_type_header.c_str()) ||
      !append_header("Content-Encoding: gzip")) {
    curl_slist_free_all(raw_headers);
    result.error = "failed to allocate HTTP headers";
    return result;
  }
  CurlHeaders headers(raw_headers, curl_slist_free_all);

  std::string location;
  char error_buffer[CURL_ERROR_SIZE]{};
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, gzipped_body.data());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(gzipped_body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, user_agent.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, captureHeader);
  curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &location);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, discardBody);
  curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error_buffer);

  const CURLcode request_result = curl_easy_perform(curl.get());
  long status = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
  if (request_result != CURLE_OK) {
    if (status >= 400) {
      result.error = "bytebin returned HTTP " + std::to_string(status);
    } else {
      const char *message = error_buffer[0] == '\0'
                                ? curl_easy_strerror(request_result)
                                : error_buffer;
      result.error = std::string("libcurl: ") + message;
    }
    return result;
  }
  if (status < 200 || status >= 300) {
    result.error = "bytebin returned HTTP " + std::to_string(status);
    return result;
  }

  result.key = contentKeyFromLocation(location);
  if (result.key.empty()) {
    result.error = "bytebin did not return a content key";
    return result;
  }

  result.ok = true;
  return result;
}

} // namespace spark
