// Cloudflare quick tunnel + WebSocket<->TCP bridges for co-op rooms (see include/omsi/coop_tunnel.hpp).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <winhttp.h>

#include "omsi/coop_tunnel.hpp"
#include "omsi/logger.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")

namespace omsi::coop {
namespace fs = std::filesystem;

namespace {

// Pinned cloudflared release (same as openOMSI) and the SHA-256 GitHub publishes for it.
constexpr const wchar_t* kCfUrlPath =
    L"/cloudflare/cloudflared/releases/download/2026.9.3/cloudflared-windows-amd64.exe";
constexpr const char* kCfSha256 = "f096265ec2fcbe9bb6e2d64268db167ced3fcbb83d894bdb9e2fcdb26f2ea7e2";

bool hash_bytes(const wchar_t* alg, const void* data, std::size_t n, std::vector<unsigned char>& out) {
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&a, alg, nullptr, 0) != 0) return false;
    DWORD len = 0, got = 0;
    BCryptGetProperty(a, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&len), sizeof(len), &got, 0);
    out.assign(len, 0);
    bool ok = BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(h, reinterpret_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(n), 0) == 0 &&
              BCryptFinishHash(h, out.data(), len, 0) == 0;
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}

std::string hex(const std::vector<unsigned char>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (unsigned char c : v) {
        s.push_back(d[c >> 4]);
        s.push_back(d[c & 15]);
    }
    return s;
}

std::string base64(const unsigned char* p, std::size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (std::size_t i = 0; i < n; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(p[i]) << 16) | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o.push_back(t[(v >> 18) & 63]);
        o.push_back(t[(v >> 12) & 63]);
        o.push_back(i + 1 < n ? t[(v >> 6) & 63] : '=');
        o.push_back(i + 2 < n ? t[v & 63] : '=');
    }
    return o;
}

fs::path own_bin_dir() {
    PWSTR p = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) base = fs::path(p);
    if (p) CoTaskMemFree(p);
    return base / "OMSI_F4" / "bin";
}

fs::path find_cloudflared() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fs::path dir = fs::path(exe).parent_path();
    std::error_code ec;
    for (const auto& c : {dir / "cloudflared.exe", dir / "tools" / "cloudflared.exe"})
        if (fs::is_regular_file(c, ec)) return c;
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"cloudflared.exe", nullptr, MAX_PATH, buf, nullptr) > 0) return fs::path(buf);
    const fs::path own = own_bin_dir() / "cloudflared.exe";
    if (fs::is_regular_file(own, ec) && fs::is_regular_file(own_bin_dir() / "cloudflared.verified", ec)) return own;
    return {};
}

// HTTPS GET into memory (WinHTTP follows GitHub's redirect to its download host)
bool https_get(const wchar_t* host, const wchar_t* path, std::string& body, std::size_t max_bytes) {
    HINTERNET s = WinHttpOpen(L"OMSI-F4-editor", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"OMSI-F4-editor", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE)
                    : nullptr;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                            &sz, WINHTTP_NO_HEADER_INDEX);
        if (code == 200) {
            ok = true;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) {
                    ok = false;
                    break;
                }
                if (avail == 0) break;
                const std::size_t at = body.size();
                body.resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(r, body.data() + at, avail, &got)) {
                    ok = false;
                    break;
                }
                body.resize(at + got);
                if (body.size() > max_bytes) {
                    ok = false;
                    break;
                }
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

fs::path ensure_cloudflared(std::string& status) {
    if (auto p = find_cloudflared(); !p.empty()) return p;
    status = "Descargando cloudflared de Cloudflare (solo la primera vez, ~20 MB)...";
    log::info("tunnel: fetching cloudflared");
    std::string data;
    if (!https_get(L"github.com", kCfUrlPath, data, 200u << 20)) {
        status = "No se pudo descargar cloudflared (sin internet o bloqueado)";
        return {};
    }
    std::vector<unsigned char> h;
    if (!hash_bytes(BCRYPT_SHA256_ALGORITHM, data.data(), data.size(), h) || hex(h) != kCfSha256) {
        status = "cloudflared descargado no coincide con su SHA-256 publicado: no se instala";
        log::warn("tunnel: cloudflared sha256 mismatch " + hex(h));
        return {};
    }
    const fs::path dir = own_bin_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path tmp = dir / "cloudflared.part";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    fs::rename(tmp, dir / "cloudflared.exe", ec);
    std::ofstream(dir / "cloudflared.verified") << "2026.9.3 " << kCfSha256 << "\n";
    log::info("tunnel: cloudflared installed (" + std::to_string(data.size()) + " bytes)");
    return dir / "cloudflared.exe";
}

std::string trycloudflare_url(const std::string& line) {
    const auto s = line.find("https://");
    if (s == std::string::npos) return {};
    std::size_t e = s;
    while (e < line.size() && !std::isspace(static_cast<unsigned char>(line[e])) && line[e] != '|' && line[e] != '"') ++e;
    const std::string u = line.substr(s, e - s);
    const std::string suf = ".trycloudflare.com";
    if (u.size() > suf.size() && u.compare(u.size() - suf.size(), suf.size(), suf) == 0 &&
        u.find("api.trycloudflare.com") == std::string::npos)
        return u;
    return {};
}

void set_nb(SOCKET s, bool nb) {
    u_long v = nb ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
}

bool send_all(SOCKET s, const char* p, std::size_t n) {
    while (n > 0) {
        const int r = send(s, p, static_cast<int>((std::min)(n, std::size_t(1 << 20))), 0);
        if (r <= 0) {
            if (r < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            return false;
        }
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

std::string ws_frame(unsigned char opcode, const char* p, std::size_t n) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | opcode));
    if (n < 126) {
        f.push_back(static_cast<char>(n));
    } else if (n < 65536) {
        f.push_back(126);
        f.push_back(static_cast<char>((n >> 8) & 0xff));
        f.push_back(static_cast<char>(n & 0xff));
    } else {
        f.push_back(127);
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((static_cast<std::uint64_t>(n) >> (8 * i)) & 0xff));
    }
    f.append(p, n);
    return f;
}

// One tunnelled visitor: HTTP upgrade, then WebSocket frames <-> raw bytes of a TCP connection to the room.
void serve_ws_client(SOCKET ws, std::uint16_t room_port, std::atomic<bool>* stop) {
    set_nb(ws, false);
    std::string req;
    char buf[65536];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
        const int r = recv(ws, buf, sizeof(buf), 0);
        if (r <= 0) {
            closesocket(ws);
            return;
        }
        req.append(buf, static_cast<std::size_t>(r));
    }
    std::string key;
    {
        std::string low = req;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto k = low.find("sec-websocket-key:");
        if (k != std::string::npos) {
            auto v0 = k + 18;
            while (v0 < req.size() && req[v0] == ' ') ++v0;
            const auto v1 = req.find("\r\n", v0);
            key = req.substr(v0, v1 - v0);
        }
    }
    if (key.empty()) {
        const char* r = "HTTP/1.1 200 OK\r\nContent-Length: 18\r\nConnection: close\r\n\r\nOMSI F4 coop room\n";
        send_all(ws, r, std::strlen(r));
        closesocket(ws);
        return;
    }
    std::vector<unsigned char> h;
    const std::string src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    hash_bytes(BCRYPT_SHA1_ALGORITHM, src.data(), src.size(), h);
    const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: " + base64(h.data(), h.size()) + "\r\n\r\n";
    if (!send_all(ws, resp.data(), resp.size())) {
        closesocket(ws);
        return;
    }
    std::string pending = req.substr(req.find("\r\n\r\n") + 4); // frames that came with the request
    SOCKET room = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(room_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(room, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(room);
        closesocket(ws);
        return;
    }
    BOOL nd = TRUE;
    setsockopt(room, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
    setsockopt(ws, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
    std::string msg; // reassembled fragmented message
    bool alive = true;
    auto parse = [&]() {
        for (;;) {
            if (pending.size() < 2) return;
            const auto b0 = static_cast<unsigned char>(pending[0]), b1 = static_cast<unsigned char>(pending[1]);
            std::size_t hl = 2;
            std::uint64_t len = b1 & 0x7f;
            if (len == 126) {
                if (pending.size() < 4) return;
                len = (static_cast<std::uint64_t>(static_cast<unsigned char>(pending[2])) << 8) |
                      static_cast<unsigned char>(pending[3]);
                hl = 4;
            } else if (len == 127) {
                if (pending.size() < 10) return;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<unsigned char>(pending[2 + i]);
                hl = 10;
            }
            const bool masked = (b1 & 0x80) != 0;
            const std::size_t mh = masked ? 4 : 0;
            if (len > (64u << 20)) {
                alive = false;
                return;
            }
            if (pending.size() < hl + mh + len) return;
            std::string payload = pending.substr(hl + mh, static_cast<std::size_t>(len));
            if (masked)
                for (std::size_t i = 0; i < payload.size(); ++i) payload[i] ^= pending[hl + (i & 3)];
            pending.erase(0, hl + mh + static_cast<std::size_t>(len));
            const unsigned op = b0 & 0x0f;
            if (op == 0x8) {
                alive = false;
                return;
            }
            if (op == 0x9) {
                const auto pong = ws_frame(0xA, payload.data(), payload.size());
                send_all(ws, pong.data(), pong.size());
                continue;
            }
            if (op == 0x1 || op == 0x2 || op == 0x0) {
                msg += payload;
                if (b0 & 0x80) {
                    if (!send_all(room, msg.data(), msg.size())) alive = false;
                    msg.clear();
                }
            }
        }
    };
    parse();
    while (alive && !*stop) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ws, &rd);
        FD_SET(room, &rd);
        timeval tv{0, 200000};
        if (select(0, &rd, nullptr, nullptr, &tv) == SOCKET_ERROR) break;
        if (FD_ISSET(ws, &rd)) {
            const int r = recv(ws, buf, sizeof(buf), 0);
            if (r <= 0) break;
            pending.append(buf, static_cast<std::size_t>(r));
            parse();
        }
        if (FD_ISSET(room, &rd)) {
            const int r = recv(room, buf, sizeof(buf), 0);
            if (r <= 0) break;
            const auto f = ws_frame(0x2, buf, static_cast<std::size_t>(r));
            if (!send_all(ws, f.data(), f.size())) break;
        }
    }
    const char close_frame[2] = {static_cast<char>(0x88), 0};
    send(ws, close_frame, 2, 0);
    closesocket(room);
    closesocket(ws);
}

} // namespace

// =====================================================================================================
struct HostTunnel::Impl {
    std::mutex mu;
    std::string url, status;
    std::atomic<bool> stop{false};
    SOCKET listen_s = INVALID_SOCKET;
    std::thread accept_thread, log_thread;
    HANDLE proc = nullptr, job = nullptr, err_rd = nullptr;
};

HostTunnel::HostTunnel() : d_(std::make_unique<Impl>()) {}
HostTunnel::~HostTunnel() { stop(); }

std::string HostTunnel::url() {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->url;
}
std::string HostTunnel::status() {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->status;
}

bool HostTunnel::start(std::uint16_t ws_port, std::uint16_t room_port) {
    auto set_status = [&](const std::string& s) {
        std::lock_guard<std::mutex> lk(d_->mu);
        d_->status = s;
    };
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    // 1) WebSocket bridge on localhost
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(ws_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(s, 16) != 0) {
        closesocket(s);
        set_status("Tunel: el puerto " + std::to_string(ws_port) + " esta ocupado");
        return false;
    }
    d_->listen_s = s;
    d_->stop = false;
    Impl* d = d_.get();
    d_->accept_thread = std::thread([d, room_port] {
        while (!d->stop) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(d->listen_s, &rd);
            timeval tv{0, 200000};
            if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET c = accept(d->listen_s, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            std::thread(serve_ws_client, c, room_port, &d->stop).detach();
        }
    });
    // 2) cloudflared in front of it
    std::string st;
    const fs::path exe = ensure_cloudflared(st);
    if (!st.empty()) set_status(st);
    if (exe.empty()) return false;
    set_status("Abriendo el tunel de Cloudflare...");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe.wstring() + L"\" tunnel --no-autoupdate --url http://127.0.0.1:" + std::to_wstring(ws_port);
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    const BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                   nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        set_status("No se pudo ejecutar cloudflared");
        return false;
    }
    // dies with the editor
    d_->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl{};
    jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(d_->job, JobObjectExtendedLimitInformation, &jl, sizeof(jl));
    AssignProcessToJobObject(d_->job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    d_->proc = pi.hProcess;
    d_->err_rd = rd;
    d_->log_thread = std::thread([d] {
        std::string acc;
        char b[2048];
        DWORD got = 0;
        while (ReadFile(d->err_rd, b, sizeof(b), &got, nullptr) && got > 0) {
            acc.append(b, got);
            std::size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                const std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                const std::string u = trycloudflare_url(line);
                if (!u.empty()) {
                    std::lock_guard<std::mutex> lk(d->mu);
                    d->url = u;
                    d->status = "Internet listo por tunel: " + u;
                    log::info("tunnel: reachable at " + u);
                }
            }
        }
    });
    return true;
}

void HostTunnel::stop() {
    d_->stop = true;
    if (d_->proc) {
        TerminateProcess(d_->proc, 0);
        WaitForSingleObject(d_->proc, 2000);
        CloseHandle(d_->proc);
        d_->proc = nullptr;
    }
    if (d_->log_thread.joinable()) d_->log_thread.join();
    if (d_->err_rd) {
        CloseHandle(d_->err_rd);
        d_->err_rd = nullptr;
    }
    if (d_->job) {
        CloseHandle(d_->job);
        d_->job = nullptr;
    }
    if (d_->accept_thread.joinable()) d_->accept_thread.join();
    if (d_->listen_s != INVALID_SOCKET) {
        closesocket(d_->listen_s);
        d_->listen_s = INVALID_SOCKET;
    }
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->url.clear();
}

// =====================================================================================================
struct ClientBridge::Impl {
    HINTERNET session = nullptr, connect = nullptr, ws = nullptr;
    SOCKET listen_s = INVALID_SOCKET, local = INVALID_SOCKET;
    std::thread th_accept, th_up, th_down;
    std::atomic<bool> stop{false};
};

ClientBridge::ClientBridge() : d_(std::make_unique<Impl>()) {}
ClientBridge::~ClientBridge() { stop(); }

std::uint16_t ClientBridge::start(const std::string& host, std::string& err) {
    std::wstring wh(host.begin(), host.end());
    auto& d = *d_;
    d.session = WinHttpOpen(L"OMSI-F4-editor", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!d.session)
        d.session = WinHttpOpen(L"OMSI-F4-editor", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!d.session) {
        err = "WinHTTP no disponible";
        return 0;
    }
    d.connect = WinHttpConnect(d.session, wh.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = d.connect ? WinHttpOpenRequest(d.connect, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                              : nullptr;
    if (!req || !WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        if (req) WinHttpCloseHandle(req);
        err = "No se pudo conectar al tunel " + host + " (el anfitrion cerro la sala?)";
        return 0;
    }
    DWORD code = 0, sz = sizeof(code);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                        &sz, WINHTTP_NO_HEADER_INDEX);
    if (code != 101) {
        WinHttpCloseHandle(req);
        err = "El tunel respondio " + std::to_string(code) + " (sala cerrada o codigo viejo)";
        return 0;
    }
    d.ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    WinHttpCloseHandle(req);
    if (!d.ws) {
        err = "WebSocket rechazado";
        return 0;
    }
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    d.listen_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int al = sizeof(a);
    if (bind(d.listen_s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(d.listen_s, 1) != 0 ||
        getsockname(d.listen_s, reinterpret_cast<sockaddr*>(&a), &al) != 0) {
        err = "No se pudo abrir el puerto local del puente";
        return 0;
    }
    const std::uint16_t port = ntohs(a.sin_port);
    Impl* dp = d_.get();
    d.th_accept = std::thread([dp] {
        SOCKET c = accept(dp->listen_s, nullptr, nullptr);
        if (c == INVALID_SOCKET) return;
        dp->local = c;
        BOOL nd = TRUE;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
        // local -> websocket
        dp->th_up = std::thread([dp] {
            char b[65536];
            while (!dp->stop) {
                const int r = recv(dp->local, b, sizeof(b), 0);
                if (r <= 0) break;
                if (WinHttpWebSocketSend(dp->ws, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, b, static_cast<DWORD>(r)) !=
                    NO_ERROR)
                    break;
            }
            dp->stop = true;
        });
        // websocket -> local
        dp->th_down = std::thread([dp] {
            std::vector<char> b(1 << 16);
            while (!dp->stop) {
                DWORD got = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
                if (WinHttpWebSocketReceive(dp->ws, b.data(), static_cast<DWORD>(b.size()), &got, &type) != NO_ERROR) break;
                if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
                if (got > 0 && !send_all(dp->local, b.data(), got)) break;
            }
            dp->stop = true;
            shutdown(dp->local, SD_BOTH);
        });
    });
    log::info("tunnel: bridge to " + host + " on 127.0.0.1:" + std::to_string(port));
    return port;
}

void ClientBridge::stop() {
    auto& d = *d_;
    d.stop = true;
    if (d.ws) {
        WinHttpWebSocketClose(d.ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    }
    if (d.local != INVALID_SOCKET) shutdown(d.local, SD_BOTH);
    if (d.listen_s != INVALID_SOCKET) {
        closesocket(d.listen_s);
        d.listen_s = INVALID_SOCKET;
    }
    if (d.th_accept.joinable()) d.th_accept.join();
    if (d.th_up.joinable()) d.th_up.join();
    if (d.th_down.joinable()) d.th_down.join();
    if (d.local != INVALID_SOCKET) {
        closesocket(d.local);
        d.local = INVALID_SOCKET;
    }
    if (d.ws) {
        WinHttpCloseHandle(d.ws);
        d.ws = nullptr;
    }
    if (d.connect) {
        WinHttpCloseHandle(d.connect);
        d.connect = nullptr;
    }
    if (d.session) {
        WinHttpCloseHandle(d.session);
        d.session = nullptr;
    }
}

} // namespace omsi::coop
