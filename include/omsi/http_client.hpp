#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace omsi::http {

struct HttpResponse {
    int status = 0;
    std::vector<std::uint8_t> body;
    std::string error;
    // From Content-Length or Content-Range total (bytes A-B/TOTAL). 0 if unknown.
    std::uint64_t content_length = 0;
};

[[nodiscard]] HttpResponse http_get(const std::wstring& url);
// Optional extra request headers (e.g. L"Range: bytes=0-1023\r\n").
[[nodiscard]] HttpResponse http_get(const std::wstring& url, const std::wstring& extra_headers);
[[nodiscard]] HttpResponse http_post_json(const std::wstring& url, const std::string& json_body);

} // namespace omsi::http
