#include "omsi/updater.hpp"

#include "omsi/editor_version.hpp"
#include "omsi/logger.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace omsi::updater {
namespace {

std::mutex g_mu;
Status g_status;
std::string g_zip_url;
std::filesystem::path g_zip_path;
std::atomic<bool> g_started{false};
std::atomic<bool> g_dismissed{false};

void set_state(State s, const std::string& err = {}) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_status.state = s;
    if (!err.empty()) g_status.error = err;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// GET over HTTPS (redirects followed). body_out or file_out; progress 0..1 when the length is known.
bool http_get(const std::string& url, std::string* body_out, const std::filesystem::path* file_out) {
    const std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{}, path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
    // (a generic agent: GitHub's API refuses requests without one; nothing about the user is sent)
    HINTERNET ses = WinHttpOpen(L"Feditor-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (req) {
        const wchar_t* hdr = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
        if (WinHttpSendRequest(req, hdr, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD code = 0, sz = sizeof(code);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &code, &sz, WINHTTP_NO_HEADER_INDEX);
            DWORD total = 0;
            sz = sizeof(total);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &total, &sz, WINHTTP_NO_HEADER_INDEX);
            if (code == 200) {
                std::ofstream f;
                if (file_out) f.open(*file_out, std::ios::binary | std::ios::trunc);
                std::vector<char> buf(1 << 16);
                std::uint64_t got = 0;
                ok = !file_out || static_cast<bool>(f);
                for (;;) {
                    DWORD avail = 0, read = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail)) {
                        ok = false;
                        break;
                    }
                    if (avail == 0) break;
                    if (avail > buf.size()) buf.resize(avail);
                    if (!WinHttpReadData(req, buf.data(), avail, &read)) {
                        ok = false;
                        break;
                    }
                    if (body_out) body_out->append(buf.data(), read);
                    if (file_out) f.write(buf.data(), read);
                    got += read;
                    if (total > 0) {
                        std::lock_guard<std::mutex> lk(g_mu);
                        g_status.progress = static_cast<float>(static_cast<double>(got) / total);
                    }
                }
            }
        }
        WinHttpCloseHandle(req);
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

// the string value of "key": "..." after position `from` (JSON escapes \" \\ \n handled)
std::string json_str(const std::string& j, const std::string& key, std::size_t from = 0, std::size_t* at = nullptr) {
    const std::string k = "\"" + key + "\"";
    std::size_t p = j.find(k, from);
    if (p == std::string::npos) return {};
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return {};
    p = j.find('"', p);
    if (p == std::string::npos) return {};
    std::string out;
    for (std::size_t i = p + 1; i < j.size(); ++i) {
        const char ch = j[i];
        if (ch == '\\' && i + 1 < j.size()) {
            const char e = j[++i];
            out += e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e;
            continue;
        }
        if (ch == '"') {
            if (at) *at = i;
            break;
        }
        out += ch;
    }
    return out;
}

std::vector<int> version_parts(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    std::vector<int> out;
    int cur = -1;
    for (const char ch : v) {
        if (ch >= '0' && ch <= '9') {
            cur = (cur < 0 ? 0 : cur) * 10 + (ch - '0');
        } else if (ch == '.') {
            out.push_back(cur < 0 ? 0 : cur);
            cur = -1;
        } else {
            break;
        }
    }
    if (cur >= 0) out.push_back(cur);
    return out;
}

bool newer(const std::string& a, const std::string& b) { // a > b
    const auto x = version_parts(a), y = version_parts(b);
    for (std::size_t i = 0; i < (std::max)(x.size(), y.size()); ++i) {
        const int u = i < x.size() ? x[i] : 0, w = i < y.size() ? y[i] : 0;
        if (u != w) return u > w;
    }
    return false;
}

std::filesystem::path exe_path() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

// The version this build compares with. Testing the update path: FEDITOR_UPDATE_TEST_VERSION pretends an older
// build, only while update_test.flag sits beside the .exe (the installer deletes it, so the restarted editor
// is the real version and does not update again).
std::string local_version() {
    if (const char* e = std::getenv("FEDITOR_UPDATE_TEST_VERSION"); e && *e) {
        std::error_code ec;
        if (std::filesystem::exists(exe_path().parent_path() / L"update_test.flag", ec)) return e;
    }
    return kEditorVersion;
}

} // namespace

void start_check() {
    if (g_started.exchange(true)) return;
    if (const char* e = std::getenv("FEDITOR_NO_UPDATE_CHECK"); e && *e && *e != '0') return;
    set_state(State::Checking);
    std::thread([] {
        std::string body;
        const std::string url = std::string("https://api.github.com/repos/") + kRepo + "/releases/latest";
        if (!http_get(url, &body, nullptr) || body.empty()) {
            set_state(State::Failed, "no se pudo consultar GitHub");
            return;
        }
        const std::string tag = json_str(body, "tag_name");
        // the release's zip (the first .zip asset)
        std::string zip;
        std::size_t pos = 0;
        for (;;) {
            std::size_t at = 0;
            const std::string u = json_str(body, "browser_download_url", pos, &at);
            if (u.empty()) break;
            if (u.size() > 4 && u.compare(u.size() - 4, 4, ".zip") == 0) {
                zip = u;
                break;
            }
            pos = at + 1;
        }
        std::string notes = json_str(body, "body");
        if (notes.size() > 600) notes = notes.substr(0, 600) + "...";
        std::lock_guard<std::mutex> lk(g_mu);
        g_status.latest = (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? tag.substr(1) : tag;
        g_status.notes = notes;
        g_zip_url = zip;
        const std::string mine = local_version();
        g_status.state = (!tag.empty() && !zip.empty() && newer(tag, mine)) ? State::Available : State::UpToDate;
        log::info("updater: latest release " + tag + " (this build " + mine + ")" +
                  (g_status.state == State::Available ? " - update available" : ""));
    }).detach();
}

Status status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_status;
}

void start_download_and_install() {
    std::string url;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_status.state != State::Available) return;
        g_status.state = State::Downloading;
        g_status.progress = 0.f;
        url = g_zip_url;
    }
    std::thread([url] {
        wchar_t tmp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp);
        const std::filesystem::path zip = std::filesystem::path(tmp) / L"feditor_update.zip";
        if (!http_get(url, nullptr, &zip)) {
            set_state(State::Failed, "la descarga fallo");
            return;
        }
        std::lock_guard<std::mutex> lk(g_mu);
        g_zip_path = zip;
        g_status.state = State::ReadyToRestart;
    }).detach();
}

bool launch_installer() {
    std::filesystem::path zip;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_status.state != State::ReadyToRestart) return false;
        zip = g_zip_path;
    }
    const auto exe = exe_path();
    const auto dir = exe.parent_path();
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const auto script = std::filesystem::path(tmp) / L"feditor_update.cmd";
    const DWORD pid = GetCurrentProcessId();
    {
        std::wofstream f(script, std::ios::trunc);
        if (!f) return false;
        f << L"@echo off\r\n"
          << L":wait\r\n"
          << L"tasklist /FI \"PID eq " << pid << L"\" | find \"" << pid << L"\" >nul && (timeout /t 1 >nul & goto wait)\r\n"
          << L"powershell -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" << zip.wstring()
          << L"' -DestinationPath '" << dir.wstring() << L"' -Force\"\r\n"
          << L"del /q \"" << (dir / L"update_test.flag").wstring() << L"\" 2>nul\r\n"
          << L"start \"\" \"" << exe.wstring() << L"\"\r\n";
    }
    std::wstring cmd = L"cmd.exe /c \"" + script.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log::info("updater: installer started, closing the editor");
    return true;
}

void dismiss() { g_dismissed = true; }
bool dismissed() { return g_dismissed; }

} // namespace omsi::updater
