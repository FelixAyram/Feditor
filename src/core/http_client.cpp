#include "omsi/http_client.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace omsi::http {
namespace {

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool crack_url(const std::wstring& url, std::wstring& host, std::wstring& path, INTERNET_PORT& port,
               bool& secure) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    // Heap only. Leave ExtraInfo NULL so ?query stays inside UrlPath (separate ExtraInfo
    // left dw*Length stale → std::string::append "string too long" on DEM batches).
    std::vector<wchar_t> host_buf(256);
    std::vector<wchar_t> path_buf((std::max)(url.size() + 64u, static_cast<std::size_t>(4096)));
    uc.lpszHostName = host_buf.data();
    uc.dwHostNameLength = static_cast<DWORD>(host_buf.size());
    uc.lpszUrlPath = path_buf.data();
    uc.dwUrlPathLength = static_cast<DWORD>(path_buf.size());
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    if (uc.dwHostNameLength >= host_buf.size() || uc.dwUrlPathLength >= path_buf.size()) return false;
    host.assign(host_buf.data(), uc.dwHostNameLength);
    path.assign(path_buf.data(), uc.dwUrlPathLength);
    port = uc.nPort;
    secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

HttpResponse request(const std::wstring& method, const std::wstring& url, const std::string* json_body,
                     const std::wstring* extra_headers = nullptr) {
    HttpResponse out{};
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool secure = true;
    if (!crack_url(url, host, path, port, secure)) {
        out.error = "invalid url";
        return out;
    }

    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) omsi-f4-engine/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        out.error = "WinHttpOpen failed";
        return out;
    }
    // Aerial tiles are tiny; DEM Range extracts (FABDEM ~14 MiB) need a longer receive window.
    const bool dem_range = extra_headers && extra_headers->find(L"Range:") != std::wstring::npos;
    WinHttpSetTimeouts(session, 8000, 8000, dem_range ? 60000 : 15000, dem_range ? 180000 : 30000);
    // S3 / modern APIs need TLS 1.2+ (WinHTTP default can omit it on older stacks).
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        out.error = "WinHttpConnect failed";
        return out;
    }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(connect, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        out.error = "WinHttpOpenRequest failed";
        return out;
    }

    std::wstring hdr_owned;
    LPCWSTR headers = WINHTTP_NO_ADDITIONAL_HEADERS;
    DWORD hdr_len = 0;
    if (json_body) {
        hdr_owned = L"Content-Type: application/json\r\n";
        if (extra_headers && !extra_headers->empty()) hdr_owned += *extra_headers;
        headers = hdr_owned.c_str();
        hdr_len = static_cast<DWORD>(-1);
    } else if (extra_headers && !extra_headers->empty()) {
        headers = extra_headers->c_str();
        hdr_len = static_cast<DWORD>(-1);
    }

    const void* body_ptr = json_body ? json_body->data() : WINHTTP_NO_REQUEST_DATA;
    const DWORD body_len = json_body ? static_cast<DWORD>(json_body->size()) : 0;

    if (!WinHttpSendRequest(req, headers, hdr_len, const_cast<void*>(body_ptr), body_len, body_len, 0)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        out.error = "WinHttpSendRequest failed";
        return out;
    }
    if (!WinHttpReceiveResponse(req, nullptr)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        out.error = "WinHttpReceiveResponse failed";
        return out;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    out.status = static_cast<int>(status);

    // Content-Length / Content-Range total (for ZIP EOCD / surgical extracts).
    {
        wchar_t cr[128]{};
        DWORD cr_sz = sizeof(cr);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, cr, &cr_sz,
                                WINHTTP_NO_HEADER_INDEX)) {
            // bytes 0-0/864903137
            const wchar_t* slash = nullptr;
            for (wchar_t* p = cr; *p; ++p)
                if (*p == L'/') slash = p;
            if (slash && slash[1]) out.content_length = _wcstoui64(slash + 1, nullptr, 10);
        }
        if (out.content_length == 0) {
            DWORD cl = 0;
            DWORD cl_sz = sizeof(cl);
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &cl, &cl_sz, WINHTTP_NO_HEADER_INDEX))
                out.content_length = cl;
        }
    }

    // Cap body for Range probes / runaway servers (48 MiB — FABDEM 1° store member ~14 MiB).
    constexpr std::size_t kMaxBody = 48u * 1024u * 1024u;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        if (out.body.size() >= kMaxBody) break;
        if (out.body.size() + avail > kMaxBody) avail = static_cast<DWORD>(kMaxBody - out.body.size());
        const std::size_t off = out.body.size();
        out.body.resize(off + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, out.body.data() + off, avail, &read)) break;
        out.body.resize(off + read);
        if (read == 0) break;
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return out;
}

} // namespace

HttpResponse http_get(const std::wstring& url) { return request(L"GET", url, nullptr, nullptr); }

HttpResponse http_get(const std::wstring& url, const std::wstring& extra_headers) {
    return request(L"GET", url, nullptr, &extra_headers);
}

HttpResponse http_post_json(const std::wstring& url, const std::string& json_body) {
    return request(L"POST", url, &json_body, nullptr);
}

} // namespace omsi::http
