// Cooperative editing session: TCP host/guest (star), UDP LAN presence, local profile / friends /
// chat history, map-folder version check + transfer. See include/omsi/coop_session.hpp.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlobj.h>
#include <natupnp.h>
#include <oleauto.h>

#include "omsi/coop_session.hpp"
#include "omsi/coop_tunnel.hpp"
#include "omsi/fs_cow.hpp"
#include "omsi/vfs_overlay.hpp"
#include "omsi/map_packager.hpp"
#include <unordered_set>
#include <memory>
#include "omsi/logger.hpp"
#include "omsi/utf8_win32.hpp"

#include "../../third_party/miniz/miniz.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <cwctype>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace omsi::coop {
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kProto = 1;
constexpr std::uint16_t kDefaultPort = 47123;
constexpr std::uint16_t kLanPort = 47124;
constexpr int kMaxUsers = 8;
constexpr std::uint32_t kMaxFrame = 8u * 1024u * 1024u;
constexpr std::size_t kFileChunk = 256u * 1024u;

enum Msg : std::uint8_t {
    M_Hello = 1,
    M_Welcome,
    M_Users,
    M_Chat,
    M_Pose,
    M_Op,
    M_Kick,
    M_FileReq,
    M_FileData,
    M_FileEnd,
    M_Ready,
    M_Bye,
    M_Reject,
    M_Profile, // guest -> host: new name / colour (host rebroadcasts the user list)
    M_AssetReq,  // guest -> host: .sco / .sli it lacks
    M_AssetList, // host -> guest: every file those need (rel, size)
    M_AssetGet,  // guest -> host: the ones it does not have
    M_AssetData, // host -> guest: file chunk (goes to RAM)
    M_AssetEnd,  // host -> guest: batch done
};
constexpr std::uint64_t kAssetMax = 96ull << 20;

bool asset_ext_ok(const std::string& rel) {
    const auto dot = rel.rfind('.');
    if (dot == std::string::npos) return false;
    std::string e = rel.substr(dot);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* k[] = {".sco", ".sli", ".o3d", ".x", ".dds", ".bmp", ".png", ".tga", ".jpg", ".jpeg",
                              ".cfg", ".txt", ".osc"};
    for (const char* x : k)
        if (e == x) return true;
    return false;
}

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
std::int64_t unix_now() { return static_cast<std::int64_t>(std::time(nullptr)); }

fs::path u8p(const std::string& s) { return fs::path(omsi::utf8_to_wide(s)); } // never throws

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// ---- byte stream ----
struct W {
    std::string b;
    void u8(std::uint8_t v) { b.push_back(static_cast<char>(v)); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v & 0xffffffffu));
        u32(static_cast<std::uint32_t>(v >> 32));
    }
    void f32(float v) {
        std::uint32_t u;
        std::memcpy(&u, &v, 4);
        u32(u);
    }
    void str(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        b += s;
    }
};
struct R {
    const std::string& b;
    std::size_t p = 0;
    bool ok = true;
    explicit R(const std::string& s) : b(s) {}
    bool need(std::size_t n) {
        if (!ok || p + n > b.size()) ok = false;
        return ok;
    }
    std::uint8_t u8() { return need(1) ? static_cast<std::uint8_t>(b[p++]) : 0; }
    std::uint32_t u32() {
        if (!need(4)) return 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[p + i])) << (8 * i);
        p += 4;
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::uint64_t u64() {
        const std::uint64_t lo = u32();
        const std::uint64_t hi = u32();
        return lo | (hi << 32);
    }
    float f32() {
        const std::uint32_t u = u32();
        float v;
        std::memcpy(&v, &u, 4);
        return v;
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (!need(n)) return {};
        std::string s = b.substr(p, n);
        p += n;
        return s;
    }
};

std::string frame(std::uint8_t type, const std::string& payload) {
    W w;
    w.u32(static_cast<std::uint32_t>(payload.size() + 1));
    w.u8(type);
    w.b += payload;
    return w.b;
}

// ---- invite code: Crockford base32 of ipv4(4) port(2) secret(4) ----
const char* kB32 = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
std::string b32_encode(const std::uint8_t* d, std::size_t n) {
    std::string out;
    std::uint32_t buf = 0;
    int bits = 0;
    for (std::size_t i = 0; i < n; ++i) {
        buf = (buf << 8) | d[i];
        bits += 8;
        while (bits >= 5) {
            out.push_back(kB32[(buf >> (bits - 5)) & 31]);
            bits -= 5;
        }
    }
    if (bits > 0) out.push_back(kB32[(buf << (5 - bits)) & 31]);
    return out;
}
bool b32_decode(const std::string& s, std::vector<std::uint8_t>& out) {
    std::uint32_t buf = 0;
    int bits = 0;
    for (char c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (c == '-' || c == ' ') continue;
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        const char* q = std::strchr(kB32, c);
        if (!q || !*q) return false;
        buf = (buf << 5) | static_cast<std::uint32_t>(q - kB32);
        bits += 5;
        if (bits >= 8) {
            out.push_back(static_cast<std::uint8_t>((buf >> (bits - 8)) & 0xff));
            bits -= 8;
        }
    }
    return true;
}

// ---- map folder manifest ----
struct ManEntry {
    std::string rel; // backslashes, original case
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
};
std::vector<ManEntry> compute_manifest(const fs::path& dir, const std::atomic<bool>* cancel = nullptr) {
    std::vector<ManEntry> out;
    std::error_code ec;
    std::vector<char> buf(1 << 20);
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (cancel && *cancel) break;
        if (!e.is_regular_file(ec)) continue;
        ManEntry m;
        m.rel = utf8(fs::relative(e.path(), dir, ec));
        for (char& c : m.rel)
            if (c == '/') c = '\\';
        m.size = e.file_size(ec);
        std::ifstream in(e.path(), std::ios::binary);
        mz_ulong crc = MZ_CRC32_INIT;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = in.gcount();
            if (n <= 0) break;
            crc = mz_crc32(crc, reinterpret_cast<const unsigned char*>(buf.data()), static_cast<size_t>(n));
        }
        m.crc = static_cast<std::uint32_t>(crc);
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const ManEntry& a, const ManEntry& b) { return a.rel < b.rel; });
    return out;
}

bool safe_rel(const std::string& r) {
    if (r.empty() || r.size() > 400) return false;
    if (r.find("..") != std::string::npos || r.find(':') != std::string::npos) return false;
    if (r[0] == '\\' || r[0] == '/') return false;
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

fs::path app_dir() {
    PWSTR p = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) base = fs::path(p);
    if (p) CoTaskMemFree(p);
    if (base.empty()) base = fs::temp_directory_path();
    const auto d = base / "OMSI_F4" / "coop";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

std::string new_guid() {
    std::random_device rd;
    std::mt19937_64 g((static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^ static_cast<std::uint64_t>(unix_now()));
    char s[40];
    std::snprintf(s, sizeof(s), "%016llx%016llx", static_cast<unsigned long long>(g()),
                  static_cast<unsigned long long>(g()));
    return s;
}

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\n') o += "\\n";
        else if (c == '\r') continue;
        else if (c == '|') o += "\\p";
        else if (c == '\\') o += "\\\\";
        else o.push_back(c);
    }
    return o;
}
std::string unesc(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            o.push_back(n == 'n' ? '\n' : n == 'p' ? '|' : n);
        } else {
            o.push_back(s[i]);
        }
    }
    return o;
}
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            v.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    v.push_back(cur);
    return v;
}

void set_nonblocking(SOCKET s) {
    u_long one = 1;
    ioctlsocket(s, FIONBIO, &one);
}

} // namespace

// =====================================================================================================
struct Session::Impl {
    std::recursive_mutex mu; // guards everything below unless noted

    // profile / friends / chats
    Profile prof;
    std::vector<Friend> friend_list;
    std::map<std::string, std::vector<ChatLine>> chats; // channel -> lines (loaded lazily)
    std::map<std::string, int> unread;
    fs::path dir;

    // room
    Role role = Role::None;
    int my_id = -1;
    std::uint32_t secret = 0;
    std::uint16_t port = kDefaultPort;
    fs::path omsi_root;
    fs::path map_dir; // host: live map; guest: the folder being edited
    std::string room_map; // map folder name
    std::vector<User> users;
    std::string status;
    std::vector<MapOp> ops_in;
    std::map<std::pair<int, std::int32_t>, MapOp> op_log; // host: (0 obj / 1 spl, id) -> last op
    std::map<int, Pose> poses;

    // version (guest)
    VersionState vstate = VersionState::None;
    std::string vsummary;
    std::vector<ManEntry> host_manifest;
    std::vector<std::string> to_fetch;
    std::uint64_t fetch_total = 0, fetch_done = 0;
    fs::path fetch_dir;      // where received files go
    fs::path reload_cfg;     // set when the editor must open another map
    bool host_copy = false;  // guest edits in maps\<map>_coop
    bool want_ready = false; // send READY once the right map is loaded
    std::ofstream fetch_out;
    std::string fetch_rel;
    bool fetch_ram = false;   // received files go to RAM (omsi::vfs), never to disk
    bool ram_mode = false;    // the editor works on the RAM overlay
    bool reload_force = false;
    std::unordered_map<std::string, std::string> ram_files; // rel -> bytes while receiving
    // assets (guest: asked / receiving; host: what each peer was already offered)
    std::unordered_set<std::string> asset_asked;
    std::vector<std::string> asset_queue;
    std::unordered_map<std::string, std::string> asset_rx;
    int asset_epoch = 0;
    int asset_pending = 0;
    std::size_t asset_files = 0, asset_bytes = 0;
    bool assets_vfs_owned = false;
    std::map<int, std::unordered_set<std::string>> asset_listed;

    // network (net thread)
    struct Peer {
        SOCKET s = INVALID_SOCKET;
        int id = -1;
        std::string in;
        std::string out;
        bool hello = false;
        bool closing = false;
        std::string guid, name;
        float color[3] = {1, 1, 1};
        std::string ip;
    };
    std::vector<Peer> peers; // host: guests; guest: [0] = host
    SOCKET listen_s = INVALID_SOCKET;
    SOCKET udp_s = INVALID_SOCKET;
    std::thread net;
    std::atomic<bool> stop{false};
    struct Inbound {
        int peer_id;
        std::uint8_t type;
        std::string payload;
    };
    std::vector<Inbound> inbox;
    std::vector<int> dropped; // peers that disconnected
    std::map<std::string, LanPeer> lan;
    double last_lan_tx = 0.0;
    bool wsa = false;

    // Cloudflare quick tunnel (host) / WebSocket bridge (guest): internet through any router, CGNAT too
    std::shared_ptr<HostTunnel> tunnel;
    std::thread tunnel_thread;
    std::unique_ptr<ClientBridge> bridge;

    // UPnP (internet without a server): port mapping on the router + its public IP
    std::thread upnp_thread;
    std::string upnp_status;
    std::string public_ip;
    std::uint16_t upnp_port = 0;
    bool upnp_mapped = false;

    // host: manifest of the live map folder, hashed in the background when the room opens
    std::thread host_hasher;
    std::atomic<bool> host_man_ready{false};
    std::vector<ManEntry> host_man;
    std::vector<int> pending_welcome;

    // background hashing
    std::thread hasher;
    std::atomic<bool> hash_done{false};
    std::vector<ManEntry> hashed;

    Impl() {
        WSADATA wd;
        wsa = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
        dir = app_dir();
        load_profile();
        open_lan();
        stop = false;
        net = std::thread([this] { net_loop(); });
    }
    ~Impl() {
        stop = true;
        if (net.joinable()) net.join();
        if (hasher.joinable()) hasher.join();
        if (host_hasher.joinable()) host_hasher.join();
        if (upnp_thread.joinable()) upnp_thread.join();
        upnp_unmap_sync();
        if (tunnel_thread.joinable()) tunnel_thread.join();
        if (tunnel) tunnel->stop();
        if (bridge) bridge->stop();
        close_all();
        if (udp_s != INVALID_SOCKET) closesocket(udp_s);
        if (wsa) WSACleanup();
    }

    // ---------------- persistence ----------------
    void load_profile() {
        std::ifstream in(dir / "profile.cfg");
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const auto k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "guid") prof.guid = v;
            else if (k == "name") prof.name = unesc(v);
            else if (k == "color") {
                const auto c = split(v, ',');
                for (int i = 0; i < 3 && i < static_cast<int>(c.size()); ++i) prof.color[i] = std::strtof(c[i].c_str(), nullptr);
            }
        }
        if (prof.guid.empty()) {
            prof.guid = new_guid();
            char nm[64];
            std::snprintf(nm, sizeof(nm), "Editor-%s", prof.guid.substr(0, 4).c_str());
            prof.name = nm;
            std::mt19937 g(static_cast<unsigned>(unix_now()));
            std::uniform_real_distribution<float> u(0.2f, 1.f);
            prof.color[0] = u(g);
            prof.color[1] = u(g);
            prof.color[2] = u(g);
            save_profile();
        }
        std::ifstream fin(dir / "friends.cfg");
        while (std::getline(fin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto f = split(line, '|');
            if (f.size() < 5 || f[0].empty()) continue;
            Friend fr;
            fr.guid = f[0];
            fr.name = unesc(f[1]);
            for (int i = 0; i < 3; ++i) fr.color[i] = std::strtof(f[2 + i].c_str(), nullptr);
            friend_list.push_back(fr);
        }
    }
    void save_profile() {
        std::ofstream o(dir / "profile.cfg", std::ios::trunc);
        o << "guid=" << prof.guid << "\nname=" << esc(prof.name) << "\ncolor=" << prof.color[0] << "," << prof.color[1]
          << "," << prof.color[2] << "\n";
    }
    void save_friends() {
        std::ofstream o(dir / "friends.cfg", std::ios::trunc);
        for (const auto& f : friend_list)
            o << f.guid << "|" << esc(f.name) << "|" << f.color[0] << "|" << f.color[1] << "|" << f.color[2] << "\n";
    }
    fs::path chat_file(const std::string& channel) {
        return dir / (channel.empty() ? std::string("chat_sala.log") : "chat_" + channel + ".log");
    }
    std::vector<ChatLine>& chat_lines(const std::string& channel) {
        auto it = chats.find(channel);
        if (it != chats.end()) return it->second;
        auto& v = chats[channel];
        std::ifstream in(chat_file(channel));
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto f = split(line, '|');
            if (f.size() < 4) continue;
            ChatLine c;
            c.t = std::strtoll(f[0].c_str(), nullptr, 10);
            c.from_guid = f[1];
            c.from_name = unesc(f[2]);
            c.text = unesc(f[3]);
            c.system = f.size() > 4 && f[4] == "s";
            v.push_back(std::move(c));
        }
        if (v.size() > 2000) v.erase(v.begin(), v.end() - 2000);
        return v;
    }
    void append_chat(const std::string& channel, const ChatLine& c, bool count_unread) {
        chat_lines(channel).push_back(c);
        std::ofstream o(chat_file(channel), std::ios::app);
        o << c.t << "|" << c.from_guid << "|" << esc(c.from_name) << "|" << esc(c.text) << "|" << (c.system ? "s" : "u")
          << "\n";
        if (count_unread) ++unread[channel];
    }
    void sys_msg(const std::string& text) {
        ChatLine c;
        c.t = unix_now();
        c.from_name = "sala";
        c.text = text;
        c.system = true;
        append_chat("", c, false);
        status = text;
    }

    // ---------------- UPnP ----------------
    static bool is_private_v4(const std::string& ip) {
        unsigned a = 0, b = 0;
        if (std::sscanf(ip.c_str(), "%u.%u", &a, &b) != 2) return false;
        return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 100 && b >= 64 && b <= 127) ||
               a == 127;
    }
    // runs on upnp_thread
    void upnp_map(std::uint16_t port, std::string local_ip) {
        const HRESULT hi = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::string status, ext;
        bool mapped = false;
        IUPnPNAT* nat = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT),
                                       reinterpret_cast<void**>(&nat))) && nat) {
            IStaticPortMappingCollection* col = nullptr;
            if (SUCCEEDED(nat->get_StaticPortMappingCollection(&col)) && col) {
                std::wstring wl(local_ip.begin(), local_ip.end());
                BSTR proto = SysAllocString(L"TCP");
                BSTR client = SysAllocString(wl.c_str());
                BSTR desc = SysAllocString(L"Feditor - sala cooperativa");
                col->Remove(port, proto); // a stale mapping of an earlier run
                IStaticPortMapping* m = nullptr;
                if (SUCCEEDED(col->Add(port, proto, port, client, VARIANT_TRUE, desc, &m)) && m) {
                    mapped = true;
                    BSTR e = nullptr;
                    if (SUCCEEDED(m->get_ExternalIPAddress(&e)) && e) {
                        const std::wstring we(e);
                        ext.assign(we.begin(), we.end());
                        SysFreeString(e);
                    }
                    m->Release();
                }
                SysFreeString(proto);
                SysFreeString(client);
                SysFreeString(desc);
                col->Release();
            } else {
                status = "Tu router no tiene UPnP activo";
            }
            nat->Release();
        } else {
            status = "UPnP no disponible en Windows";
        }
        if (SUCCEEDED(hi)) CoUninitialize();
        std::lock_guard<std::recursive_mutex> lk(mu);
        upnp_mapped = mapped;
        upnp_port = mapped ? port : 0;
        if (mapped && !ext.empty() && !is_private_v4(ext)) {
            public_ip = ext;
            upnp_status = "Internet listo: puerto " + std::to_string(port) + " abierto en el router, IP publica " + ext;
        } else if (mapped) {
            public_ip.clear();
            upnp_status = "El router abrio el puerto pero su IP (" + (ext.empty() ? std::string("?") : ext) +
                          ") no es publica: tu proveedor usa CGNAT. Usa una VPN (Tailscale / ZeroTier / Radmin).";
        } else {
            public_ip.clear();
            upnp_status = (status.empty() ? std::string("El router no acepto abrir el puerto") : status) +
                          ". Para internet: abri el puerto " + std::to_string(port) +
                          " TCP en el router hacia esta PC, o usa Tailscale / ZeroTier / Radmin.";
        }
        log::info("coop: upnp " + upnp_status);
    }
    void upnp_unmap_sync() {
        std::uint16_t port = 0;
        {
            std::lock_guard<std::recursive_mutex> lk(mu);
            if (!upnp_mapped) return;
            port = upnp_port;
            upnp_mapped = false;
            upnp_port = 0;
            public_ip.clear();
        }
        const HRESULT hi = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IUPnPNAT* nat = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT),
                                       reinterpret_cast<void**>(&nat))) && nat) {
            IStaticPortMappingCollection* col = nullptr;
            if (SUCCEEDED(nat->get_StaticPortMappingCollection(&col)) && col) {
                BSTR proto = SysAllocString(L"TCP");
                col->Remove(port, proto);
                SysFreeString(proto);
                col->Release();
            }
            nat->Release();
        }
        if (SUCCEEDED(hi)) CoUninitialize();
    }

    // ---------------- network ----------------
    void open_lan() {
        udp_s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udp_s == INVALID_SOCKET) return;
        BOOL yes = TRUE;
        setsockopt(udp_s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        setsockopt(udp_s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(kLanPort);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(udp_s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            closesocket(udp_s);
            udp_s = INVALID_SOCKET;
            return;
        }
        set_nonblocking(udp_s);
    }
    void close_all() {
        for (auto& p : peers)
            if (p.s != INVALID_SOCKET) closesocket(p.s);
        peers.clear();
        if (listen_s != INVALID_SOCKET) closesocket(listen_s);
        listen_s = INVALID_SOCKET;
    }
    Peer* peer_by_id(int id) {
        for (auto& p : peers)
            if (p.id == id && !p.closing) return &p;
        return nullptr;
    }
    void send_to(int peer_id, std::uint8_t type, const std::string& payload) {
        std::lock_guard<std::recursive_mutex> lk(mu);
        if (auto* p = peer_by_id(peer_id)) p->out += frame(type, payload);
    }
    void send_all(std::uint8_t type, const std::string& payload, int except = -9999) {
        std::lock_guard<std::recursive_mutex> lk(mu);
        const auto f = frame(type, payload);
        for (auto& p : peers)
            if (!p.closing && p.hello && p.id != except) p.out += f;
    }

    void net_loop() {
        while (!stop) {
            fd_set rd, wr;
            FD_ZERO(&rd);
            FD_ZERO(&wr);
            int n = 0;
            {
                std::lock_guard<std::recursive_mutex> lk(mu);
                if (listen_s != INVALID_SOCKET) {
                    FD_SET(listen_s, &rd);
                    ++n;
                }
                if (udp_s != INVALID_SOCKET) {
                    FD_SET(udp_s, &rd);
                    ++n;
                }
                for (auto& p : peers) {
                    if (p.s == INVALID_SOCKET) continue;
                    FD_SET(p.s, &rd);
                    if (!p.out.empty()) FD_SET(p.s, &wr);
                    ++n;
                }
            }
            timeval tv{0, 50000};
            if (n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } else if (select(0, &rd, &wr, nullptr, &tv) == SOCKET_ERROR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            std::lock_guard<std::recursive_mutex> lk(mu);
            lan_tick(n > 0 && udp_s != INVALID_SOCKET && FD_ISSET(udp_s, &rd));
            if (listen_s != INVALID_SOCKET && FD_ISSET(listen_s, &rd)) {
                sockaddr_in a{};
                int al = sizeof(a);
                SOCKET c = accept(listen_s, reinterpret_cast<sockaddr*>(&a), &al);
                if (c != INVALID_SOCKET) {
                    set_nonblocking(c);
                    BOOL nd = TRUE;
                    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
                    Peer p;
                    p.s = c;
                    char ip[64]{};
                    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
                    p.ip = ip;
                    // lowest free id 1..7
                    for (int id = 1; id < kMaxUsers; ++id) {
                        bool used = false;
                        for (const auto& q : peers) used |= q.id == id;
                        if (!used) {
                            p.id = id;
                            break;
                        }
                    }
                    if (p.id < 0) {
                        W w;
                        w.str("Sala llena");
                        const auto f = frame(M_Reject, w.b);
                        send(c, f.data(), static_cast<int>(f.size()), 0);
                        closesocket(c);
                    } else {
                        peers.push_back(std::move(p));
                    }
                }
            }
            for (auto& p : peers) {
                if (p.s == INVALID_SOCKET) continue;
                if (FD_ISSET(p.s, &rd)) {
                    char buf[65536];
                    for (;;) {
                        const int r = recv(p.s, buf, sizeof(buf), 0);
                        if (r > 0) {
                            p.in.append(buf, static_cast<std::size_t>(r));
                            if (r < static_cast<int>(sizeof(buf))) break;
                        } else {
                            if (r == 0 || WSAGetLastError() != WSAEWOULDBLOCK) p.closing = true;
                            break;
                        }
                    }
                    // frames
                    while (p.in.size() >= 5) {
                        std::uint32_t len = 0;
                        for (int i = 0; i < 4; ++i) len |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(p.in[i])) << (8 * i);
                        if (len == 0 || len > kMaxFrame) {
                            p.closing = true;
                            break;
                        }
                        if (p.in.size() < 4 + len) break;
                        Inbound ib;
                        ib.peer_id = p.id;
                        ib.type = static_cast<std::uint8_t>(p.in[4]);
                        ib.payload = p.in.substr(5, len - 1);
                        p.in.erase(0, 4 + len);
                        inbox.push_back(std::move(ib));
                    }
                }
                if (!p.out.empty() && !p.closing && FD_ISSET(p.s, &wr)) {
                    const int s = send(p.s, p.out.data(), static_cast<int>((std::min)(p.out.size(), std::size_t(1 << 20))), 0);
                    if (s > 0) p.out.erase(0, static_cast<std::size_t>(s));
                    else if (WSAGetLastError() != WSAEWOULDBLOCK) p.closing = true;
                }
            }
            for (auto it = peers.begin(); it != peers.end();) {
                if (it->closing && (it->out.empty() || it->s == INVALID_SOCKET)) {
                    if (it->s != INVALID_SOCKET) closesocket(it->s);
                    dropped.push_back(it->id);
                    it = peers.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    void lan_tick(bool readable) {
        if (udp_s == INVALID_SOCKET) return;
        const double t = now_s();
        if (t - last_lan_tx > 3.0) {
            last_lan_tx = t;
            std::string code;
            if (role == Role::Host) {
                const auto addrs = local_addrs();
                if (!addrs.empty()) code = make_code(addrs.front());
            }
            const std::string msg = "F4COOP1|" + prof.guid + "|" + esc(prof.name) + "|" + code + "|" + room_map;
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(kLanPort);
            a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            sendto(udp_s, msg.data(), static_cast<int>(msg.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        }
        if (!readable) return;
        char buf[2048];
        for (;;) {
            sockaddr_in from{};
            int fl = sizeof(from);
            const int r = recvfrom(udp_s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (r <= 0) break;
            const auto f = split(std::string(buf, static_cast<std::size_t>(r)), '|');
            if (f.size() < 4 || f[0] != "F4COOP1" || f[1] == prof.guid) continue;
            LanPeer lp;
            lp.guid = f[1];
            lp.name = unesc(f[2]);
            lp.code = f[3];
            char ip[64]{};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            lp.ip = ip;
            lp.last_seen = t;
            lan[lp.guid] = lp;
        }
    }

    std::vector<std::string> local_addrs() {
        std::vector<std::string> v;
        char host[256];
        if (gethostname(host, sizeof(host)) != 0) return v;
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        if (getaddrinfo(host, nullptr, &hints, &res) != 0) return v;
        for (auto* p = res; p; p = p->ai_next) {
            char ip[64]{};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr, ip, sizeof(ip));
            if (std::strncmp(ip, "127.", 4) != 0 && std::find(v.begin(), v.end(), ip) == v.end()) v.emplace_back(ip);
        }
        freeaddrinfo(res);
        return v;
    }
    std::string make_code(const std::string& addr) {
        if (addr.rfind("tunel:", 0) == 0) {
            // F4T~<words of the trycloudflare host>~<room secret>
            std::string host = addr.substr(6);
            if (host.rfind("https://", 0) == 0) host = host.substr(8);
            const auto dot = host.find(".trycloudflare.com");
            if (dot == std::string::npos) return {};
            std::uint8_t sec[4];
            for (int i = 0; i < 4; ++i) sec[i] = static_cast<std::uint8_t>((secret >> (8 * i)) & 0xff);
            return "F4T~" + host.substr(0, dot) + "~" + b32_encode(sec, 4);
        }
        in_addr ia{};
        if (inet_pton(AF_INET, addr.c_str(), &ia) != 1) return {};
        std::uint8_t d[10];
        std::memcpy(d, &ia, 4);
        d[4] = static_cast<std::uint8_t>(port >> 8);
        d[5] = static_cast<std::uint8_t>(port & 0xff);
        for (int i = 0; i < 4; ++i) d[6 + i] = static_cast<std::uint8_t>((secret >> (8 * i)) & 0xff);
        const std::string s = b32_encode(d, 10); // 16 chars
        return "F4-" + s.substr(0, 4) + "-" + s.substr(4, 4) + "-" + s.substr(8, 4) + "-" + s.substr(12);
    }

    // ---------------- room helpers (main thread, mu held) ----------------
    void broadcast_users() {
        W w;
        w.u32(static_cast<std::uint32_t>(users.size()));
        for (const auto& u : users) {
            w.i32(u.id);
            w.str(u.guid);
            w.str(u.name);
            for (float c : u.color) w.f32(c);
            w.u8(u.host ? 1 : 0);
        }
        send_all(M_Users, w.b);
    }
    void rebuild_users_host() {
        users.clear();
        User h;
        h.id = 0;
        h.guid = prof.guid;
        h.name = prof.name;
        std::memcpy(h.color, prof.color, sizeof(h.color));
        h.host = true;
        users.push_back(h);
        for (const auto& p : peers) {
            if (!p.hello || p.closing) continue;
            User u;
            u.id = p.id;
            u.guid = p.guid;
            u.name = p.name;
            std::memcpy(u.color, p.color, sizeof(u.color));
            users.push_back(u);
        }
    }
    std::string user_name(int id) {
        for (const auto& u : users)
            if (u.id == id) return u.name;
        return "?";
    }

    void handle(const Inbound& ib) {
        R r(ib.payload);
        switch (ib.type) {
        case M_Hello: {
            if (role != Role::Host) return;
            auto* p = peer_by_id(ib.peer_id);
            if (!p) return;
            const std::uint32_t sec = r.u32();
            const std::uint32_t proto = r.u32();
            p->guid = r.str();
            p->name = r.str();
            for (float& c : p->color) c = r.f32();
            if (!r.ok || sec != secret || proto != kProto) {
                W w;
                w.str(sec != secret ? "Codigo de sala invalido" : "Version del editor distinta");
                p->out += frame(M_Reject, w.b);
                p->closing = true;
                return;
            }
            p->hello = true;
            if (host_man_ready) send_welcome(*p);
            else pending_welcome.push_back(p->id); // sent by update() when the hash is done
            rebuild_users_host();
            broadcast_users();
            sys_msg(p->name + " se unio a la sala");
            break;
        }
        case M_Welcome: {
            if (role != Role::Guest) return;
            my_id = r.i32();
            room_map = r.str();
            const std::uint32_t n = r.u32();
            host_manifest.clear();
            for (std::uint32_t i = 0; i < n && r.ok; ++i) {
                ManEntry m;
                m.rel = r.str();
                m.size = r.u64();
                m.crc = r.u32();
                host_manifest.push_back(std::move(m));
            }
            sys_msg("Conectado a la sala (mapa " + room_map + ")");
            start_compare();
            break;
        }
        case M_Users: {
            if (role != Role::Guest) return;
            users.clear();
            const std::uint32_t n = r.u32();
            for (std::uint32_t i = 0; i < n && r.ok; ++i) {
                User u;
                u.id = r.i32();
                u.guid = r.str();
                u.name = r.str();
                for (float& c : u.color) c = r.f32();
                u.host = r.u8() != 0;
                users.push_back(u);
            }
            break;
        }
        case M_Chat: {
            const std::string channel = r.str(); // "" room, else target guid
            ChatLine c;
            c.from_guid = r.str();
            c.from_name = r.str();
            c.text = r.str();
            c.t = static_cast<std::int64_t>(r.u64());
            if (!r.ok) return;
            if (role == Role::Host) {
                // relay
                if (channel.empty()) send_all(M_Chat, ib.payload, ib.peer_id);
                else
                    for (auto& p : peers)
                        if (p.hello && p.guid == channel) p.out += frame(M_Chat, ib.payload);
            }
            if (channel.empty()) append_chat("", c, true);
            else if (channel == prof.guid) append_chat(c.from_guid, c, true); // a DM for me
            break;
        }
        case M_Pose: {
            Pose p;
            p.user = r.i32();
            p.gx = r.i32();
            p.gy = r.i32();
            p.lx = r.f32();
            p.ly = r.f32();
            p.lz = r.f32();
            p.yaw = r.f32();
            p.pitch = r.f32();
            {
                const std::uint8_t fl = r.u8();
                p.walking = (fl & 1u) != 0;
                p.airborne = (fl & 2u) != 0;
            }
            if (!r.ok) return;
            if (role == Role::Host) {
                p.user = ib.peer_id; // trust the socket, not the payload
                W w;
                w.i32(p.user);
                w.i32(p.gx);
                w.i32(p.gy);
                w.f32(p.lx);
                w.f32(p.ly);
                w.f32(p.lz);
                w.f32(p.yaw);
                w.f32(p.pitch);
                w.u8(static_cast<std::uint8_t>((p.walking ? 1u : 0u) | (p.airborne ? 2u : 0u)));
                send_all(M_Pose, w.b, ib.peer_id);
            }
            p.recv_time = now_s();
            if (p.user != my_id) poses[p.user] = p;
            break;
        }
        case M_Op: {
            MapOp op;
            op.kind = static_cast<OpKind>(r.u8());
            op.id = r.i32();
            op.data = r.str();
            op.from_user = r.i32();
            if (!r.ok) return;
            if (role == Role::Host) {
                op.from_user = ib.peer_id;
                log_op(op);
                send_all(M_Op, op_payload(op), ib.peer_id);
            }
            ops_in.push_back(std::move(op));
            break;
        }
        case M_Kick: {
            if (role != Role::Guest) return;
            const std::string why = r.str();
            disconnect();
            sys_msg("Te expulsaron de la sala" + (why.empty() ? std::string() : ": " + why));
            break;
        }
        case M_Reject: {
            const std::string why = r.str();
            disconnect();
            sys_msg("No se pudo entrar: " + why);
            break;
        }
        case M_FileReq: {
            if (role != Role::Host) return;
            auto* p = peer_by_id(ib.peer_id);
            if (!p) return;
            const std::uint32_t n = r.u32();
            for (std::uint32_t i = 0; i < n && r.ok; ++i) {
                const std::string rel = r.str();
                if (!safe_rel(rel)) continue;
                std::ifstream in(map_dir / u8p(rel), std::ios::binary);
                if (!in) continue;
                std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::uint64_t off = 0;
                do {
                    const std::size_t len = (std::min)(kFileChunk, data.size() - static_cast<std::size_t>(off));
                    W w;
                    w.str(rel);
                    w.u64(data.size());
                    w.u64(off);
                    w.str(data.substr(static_cast<std::size_t>(off), len));
                    p->out += frame(M_FileData, w.b);
                    off += len;
                } while (off < data.size());
            }
            p->out += frame(M_FileEnd, {});
            break;
        }
        case M_FileData: {
            if (role != Role::Guest || vstate != VersionState::Transferring) return;
            const std::string rel = r.str();
            const std::uint64_t total = r.u64();
            const std::uint64_t off = r.u64();
            const std::string chunk = r.str();
            if (!r.ok || !safe_rel(rel)) return;
            if (fetch_ram) {
                auto& buf = ram_files[rel];
                if (off == 0 || buf.size() != total) buf.assign(static_cast<std::size_t>(total), '\0');
                if (off + chunk.size() <= buf.size())
                    std::memcpy(buf.data() + static_cast<std::size_t>(off), chunk.data(), chunk.size());
                fetch_done += chunk.size();
                break;
            }
            const fs::path dst = fetch_dir / u8p(rel);
            if (off == 0 || rel != fetch_rel) {
                if (fetch_out.is_open()) fetch_out.close();
                std::error_code ec;
                fs::create_directories(dst.parent_path(), ec);
                omsi::fsutil::break_link(dst); // the _coop copy links the guest's own files: never write through
                fetch_out.open(dst, std::ios::binary | std::ios::trunc);
                fetch_rel = rel;
            }
            fetch_out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            fetch_done += chunk.size();
            if (off + chunk.size() >= total) fetch_out.close();
            break;
        }
        case M_FileEnd: {
            if (role != Role::Guest || vstate != VersionState::Transferring) return;
            if (fetch_out.is_open()) fetch_out.close();
            if (fetch_ram) {
                omsi::vfs::begin(fetch_dir);
                std::size_t bytes = 0;
                for (auto& [rel, data] : ram_files) {
                    bytes += data.size();
                    omsi::vfs::put(fetch_dir / u8p(rel), std::move(data));
                }
                ram_files.clear();
                // files of the guest's copy that the host does not have: hidden
                std::unordered_set<std::string> host_set;
                for (const auto& h : host_manifest) host_set.insert(lower(h.rel));
                for (const auto& m : hashed)
                    if (!host_set.count(lower(m.rel))) omsi::vfs::hide(fetch_dir / u8p(m.rel));
                omsi::vfs::protect_disk_files();
                fetch_ram = false;
                ram_mode = true;
                vstate = VersionState::Reloading;
                reload_cfg = fetch_dir / "global.cfg";
                reload_force = true;
                want_ready = true;
                sys_msg("Version del anfitrion en RAM (" + std::to_string(bytes / (1024 * 1024)) +
                        " MB): tu mapa en disco no se modifica");
                break;
            }
            vstate = VersionState::Reloading;
            reload_cfg = fetch_dir / "global.cfg";
            want_ready = true;
            sys_msg("Mapa recibido; recargando " + utf8(fetch_dir.filename()));
            break;
        }
        case M_Ready: {
            if (role != Role::Host) return;
            // replay the room's edits to the late joiner
            if (auto* p = peer_by_id(ib.peer_id))
                for (const auto& [k, op] : op_log) p->out += frame(M_Op, op_payload(op));
            break;
        }
        case M_AssetReq: {
            if (role != Role::Host) return;
            const std::uint32_t n = r.u32();
            std::vector<std::string> rels;
            for (std::uint32_t i = 0; i < n && i < 512 && r.ok; ++i) {
                std::string rel = r.str();
                if (r.ok && safe_rel(rel) && asset_ext_ok(rel)) rels.push_back(rel);
            }
            const int pid = ib.peer_id;
            const fs::path root = omsi_root;
            if (rels.empty() || root.empty()) {
                W w;
                w.u32(0);
                if (auto* p = peer_by_id(pid)) p->out += frame(M_AssetList, w.b);
                return;
            }
            // dependency walk off the main thread (reads .sco / .o3d files)
            std::thread([this, pid, rels, root] {
                std::vector<std::pair<std::string, std::uint64_t>> list;
                std::unordered_set<std::string> seen;
                for (const auto& rel : rels) {
                    std::vector<fs::path> files;
                    try {
                        files = omsi::pack::asset_closure(root, root / u8p(rel));
                    } catch (...) {
                    }
                    for (const auto& f : files) {
                        std::error_code ec;
                        const auto rr = fs::relative(f, root, ec);
                        if (ec || rr.empty()) continue;
                        const std::string rs = utf8(rr);
                        if (!safe_rel(rs) || !asset_ext_ok(rs) || !seen.insert(lower(rs)).second) continue;
                        const auto sz = fs::file_size(f, ec);
                        if (ec || sz > kAssetMax) continue;
                        list.emplace_back(rs, sz);
                    }
                }
                std::lock_guard<std::recursive_mutex> lk(mu);
                auto& offered = asset_listed[pid];
                W body;
                std::uint32_t cnt = 0;
                for (const auto& [rs, sz] : list) {
                    if (!offered.insert(lower(rs)).second) continue;
                    body.str(rs);
                    body.u64(sz);
                    ++cnt;
                }
                W w;
                w.u32(cnt);
                w.b += body.b;
                log::info("coop assets: anfitrion ofrece " + std::to_string(cnt) + " archivos para " +
                          std::to_string(rels.size()) + " .sco/.sli");
                if (auto* p = peer_by_id(pid)) p->out += frame(M_AssetList, w.b);
            }).detach();
            break;
        }
        case M_AssetGet: {
            if (role != Role::Host) return;
            const std::uint32_t n = r.u32();
            std::vector<std::string> rels;
            for (std::uint32_t i = 0; i < n && i < 8192 && r.ok; ++i) {
                std::string rel = r.str();
                if (r.ok && safe_rel(rel) && asset_ext_ok(rel)) rels.push_back(rel);
            }
            const int pid = ib.peer_id;
            const fs::path root = omsi_root;
            std::thread([this, pid, rels, root] {
                for (const auto& rel : rels) {
                    std::ifstream in(root / u8p(rel), std::ios::binary);
                    if (!in) continue;
                    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    if (data.size() > kAssetMax) continue;
                    std::uint64_t off = 0;
                    do {
                        const std::size_t len = (std::min)(kFileChunk, data.size() - static_cast<std::size_t>(off));
                        W w;
                        w.str(rel);
                        w.u64(data.size());
                        w.u64(off);
                        w.str(data.substr(static_cast<std::size_t>(off), len));
                        {
                            std::lock_guard<std::recursive_mutex> lk(mu);
                            auto* p = peer_by_id(pid);
                            if (!p) return;
                            p->out += frame(M_AssetData, w.b);
                        }
                        off += len;
                    } while (off < data.size());
                    // keep the send queue bounded (slow links / tunnel)
                    for (int i = 0; i < 1500; ++i) {
                        {
                            std::lock_guard<std::recursive_mutex> lk(mu);
                            auto* p = peer_by_id(pid);
                            if (!p) return;
                            if (p->out.size() < (8u << 20)) break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
                std::lock_guard<std::recursive_mutex> lk(mu);
                if (auto* p = peer_by_id(pid)) p->out += frame(M_AssetEnd, {});
            }).detach();
            break;
        }
        case M_AssetList: {
            if (role != Role::Guest) return;
            const std::uint32_t n = r.u32();
            std::vector<std::string> want;
            for (std::uint32_t i = 0; i < n && r.ok; ++i) {
                const std::string rel = r.str();
                const std::uint64_t sz = r.u64();
                if (!r.ok || !safe_rel(rel)) break;
                const fs::path local = omsi_root / u8p(rel);
                std::error_code ec;
                const bool on_disk = fs::is_regular_file(local, ec) && fs::file_size(local, ec) == sz && !ec;
                if (on_disk || omsi::vfs::get(local)) continue;
                want.push_back(rel);
            }
            log::info("coop assets: " + std::to_string(n) + " ofrecidos, faltan " + std::to_string(want.size()));
            if (want.empty()) {
                if (asset_pending > 0) --asset_pending;
                ++asset_epoch;
                break;
            }
            W w;
            w.u32(static_cast<std::uint32_t>(want.size()));
            for (const auto& x : want) w.str(x);
            send_to(0, M_AssetGet, w.b);
            break;
        }
        case M_AssetData: {
            if (role != Role::Guest) return;
            const std::string rel = r.str();
            const std::uint64_t total = r.u64();
            const std::uint64_t off = r.u64();
            const std::string chunk = r.str();
            if (!r.ok || !safe_rel(rel) || total > kAssetMax) return;
            auto& buf = asset_rx[rel];
            if (off == 0 || buf.size() != total) buf.assign(static_cast<std::size_t>(total), '\0');
            if (off + chunk.size() <= buf.size())
                std::memcpy(buf.data() + static_cast<std::size_t>(off), chunk.data(), chunk.size());
            if (off + chunk.size() >= total) {
                if (!omsi::vfs::active()) {
                    omsi::vfs::activate_assets();
                    assets_vfs_owned = true;
                }
                ++asset_files;
                asset_bytes += buf.size();
                omsi::vfs::put(omsi_root / u8p(rel), std::move(buf));
                asset_rx.erase(rel);
            }
            break;
        }
        case M_AssetEnd: {
            if (role != Role::Guest) return;
            log::info("coop assets: lote recibido (" + std::to_string(asset_files) + " archivos en RAM)");
            if (asset_pending > 0) --asset_pending;
            ++asset_epoch;
            break;
        }
        case M_Profile: {
            if (role != Role::Host) return;
            auto* p = peer_by_id(ib.peer_id);
            if (!p) return;
            std::string nm = r.str();
            float col[3];
            for (float& c : col) c = r.f32();
            if (!r.ok) return;
            if (!nm.empty()) p->name = nm.substr(0, 48);
            std::memcpy(p->color, col, sizeof(col));
            rebuild_users_host();
            broadcast_users();
            break;
        }
        case M_Bye: {
            if (auto* p = peer_by_id(ib.peer_id)) p->closing = true;
            if (role == Role::Guest) {
                disconnect();
                sys_msg("El anfitrion cerro la sala");
            }
            break;
        }
        default: break;
        }
    }

    // welcome: id + map folder + manifest of the host's map folder
    void send_welcome(Peer& p) {
        W w;
        w.i32(p.id);
        w.str(room_map);
        w.u32(static_cast<std::uint32_t>(host_man.size()));
        for (const auto& m : host_man) {
            w.str(m.rel);
            w.u64(m.size);
            w.u32(m.crc);
        }
        p.out += frame(M_Welcome, w.b);
    }
    static std::string op_payload(const MapOp& op) {
        W w;
        w.u8(static_cast<std::uint8_t>(op.kind));
        w.i32(op.id);
        w.str(op.data);
        w.i32(op.from_user);
        return w.b;
    }
    void log_op(const MapOp& op) {
        const int grp = (op.kind == OpKind::ObjUpsert || op.kind == OpKind::ObjDelete) ? 0 : 1;
        op_log[{grp, op.id}] = op;
    }

    void disconnect() {
        close_all();
        role = Role::None;
        my_id = -1;
        users.clear();
        poses.clear();
        op_log.clear();
        vstate = VersionState::None;
        want_ready = false;
        host_copy = false;
        fetch_ram = false;
        ram_files.clear();
        asset_asked.clear();
        asset_queue.clear();
        asset_rx.clear();
        asset_pending = 0;
        asset_files = asset_bytes = 0;
        asset_listed.clear();
        if (assets_vfs_owned && !ram_mode) omsi::vfs::end();
        assets_vfs_owned = false;
        if (ram_mode) {
            // back to the guest's own map, read from disk again
            ram_mode = false;
            omsi::vfs::end();
            if (!fetch_dir.empty()) {
                reload_cfg = fetch_dir / "global.cfg";
                reload_force = true;
            }
        }
    }

    // guest: compare the local copy of the room's map with the host's manifest
    void start_compare() {
        vstate = VersionState::Comparing;
        const fs::path local = omsi_root / "maps" / u8p(room_map);
        if (hasher.joinable()) hasher.join();
        hash_done = false;
        hasher = std::thread([this, local] {
            std::error_code ec;
            auto m = fs::is_directory(local, ec) ? compute_manifest(local) : std::vector<ManEntry>{};
            std::lock_guard<std::recursive_mutex> lk(mu);
            hashed = std::move(m);
            hash_done = true;
        });
    }
    void finish_compare() {
        to_fetch.clear();
        fetch_total = 0;
        std::unordered_map<std::string, const ManEntry*> mine;
        for (const auto& m : hashed) mine[lower(m.rel)] = &m;
        int differ = 0, absent = 0;
        for (const auto& h : host_manifest) {
            const auto it = mine.find(lower(h.rel));
            if (it == mine.end()) ++absent;
            else if (it->second->size != h.size || it->second->crc != h.crc) ++differ;
            else continue;
            to_fetch.push_back(h.rel);
            fetch_total += h.size;
        }
        if (to_fetch.empty()) {
            vstate = VersionState::Same;
            vsummary = "Tu mapa es igual al del anfitrion.";
            fetch_dir = omsi_root / "maps" / u8p(room_map);
            reload_cfg = fetch_dir / "global.cfg";
            want_ready = true;
        } else {
            vstate = VersionState::Differs;
            std::ostringstream s;
            if (hashed.empty())
                s << "No tenes el mapa \"" << room_map << "\".";
            else
                s << "Tu version del mapa \"" << room_map << "\" es distinta: " << differ << " archivos cambiados y "
                  << absent << " que te faltan";
            s << " (" << (fetch_total / (1024 * 1024)) << " MB para recibir, solo la carpeta maps).";
            vsummary = s.str();
        }
    }
    void request_files(const fs::path& target) {
        fetch_dir = target;
        fetch_done = 0;
        vstate = VersionState::Transferring;
        W w;
        w.u32(static_cast<std::uint32_t>(to_fetch.size()));
        for (const auto& r : to_fetch) w.str(r);
        send_to(0, M_FileReq, w.b);
    }
};

// =====================================================================================================
Session& Session::get() {
    static Session s;
    return s;
}
Session::Session() : d_(new Impl()) {}
Session::~Session() { delete d_; }

Profile& Session::profile() { return d_->prof; }
void Session::save_profile() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    d_->save_profile();
    if (d_->role == Role::Host) {
        d_->rebuild_users_host();
        d_->broadcast_users();
    } else if (d_->role == Role::Guest) {
        W w;
        w.str(d_->prof.name);
        for (float c : d_->prof.color) w.f32(c);
        d_->send_to(0, M_Profile, w.b);
    }
}
std::vector<Friend> Session::friends() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->friend_list;
}
void Session::add_friend(const std::string& guid, const std::string& name, const float color[3]) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    for (auto& f : d_->friend_list)
        if (f.guid == guid) {
            f.name = name;
            std::memcpy(f.color, color, sizeof(f.color));
            d_->save_friends();
            return;
        }
    Friend f;
    f.guid = guid;
    f.name = name;
    std::memcpy(f.color, color, sizeof(f.color));
    d_->friend_list.push_back(f);
    d_->save_friends();
}
void Session::remove_friend(const std::string& guid) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    d_->friend_list.erase(std::remove_if(d_->friend_list.begin(), d_->friend_list.end(),
                                         [&](const Friend& f) { return f.guid == guid; }),
                          d_->friend_list.end());
    d_->save_friends();
}
bool Session::is_friend(const std::string& guid) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    for (const auto& f : d_->friend_list)
        if (f.guid == guid) return true;
    return false;
}
bool Session::friend_online(const std::string& guid) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    for (const auto& u : d_->users)
        if (u.guid == guid) return true;
    const auto it = d_->lan.find(guid);
    return it != d_->lan.end() && now_s() - it->second.last_seen < 10.0;
}
std::vector<ChatLine> Session::chat(const std::string& channel) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->chat_lines(channel);
}
void Session::send_chat(const std::string& channel, const std::string& text) {
    if (text.empty()) return;
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    ChatLine c;
    c.t = unix_now();
    c.from_guid = d_->prof.guid;
    c.from_name = d_->prof.name;
    c.text = text;
    d_->append_chat(channel, c, false);
    if (d_->role == Role::None) return; // kept in the history; DMs are only delivered inside a room
    W w;
    w.str(channel);
    w.str(c.from_guid);
    w.str(c.from_name);
    w.str(c.text);
    w.u64(static_cast<std::uint64_t>(c.t));
    if (d_->role == Role::Host) {
        if (channel.empty()) d_->send_all(M_Chat, w.b);
        else
            for (auto& p : d_->peers)
                if (p.hello && p.guid == channel) p.out += frame(M_Chat, w.b);
    } else {
        d_->send_to(0, M_Chat, w.b);
    }
}
int Session::unread(const std::string& channel) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    const auto it = d_->unread.find(channel);
    return it == d_->unread.end() ? 0 : it->second;
}
void Session::mark_read(const std::string& channel) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    d_->unread[channel] = 0;
}
bool Session::export_profile(const fs::path& out_dir) {
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    for (const auto& e : fs::directory_iterator(d_->dir, ec))
        fs::copy_file(e.path(), out_dir / e.path().filename(), fs::copy_options::overwrite_existing, ec);
    return !ec;
}
bool Session::import_profile(const fs::path& in_dir) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    std::error_code ec;
    if (!fs::exists(in_dir / "profile.cfg", ec)) return false;
    for (const auto& e : fs::directory_iterator(in_dir, ec))
        fs::copy_file(e.path(), d_->dir / e.path().filename(), fs::copy_options::overwrite_existing, ec);
    d_->friend_list.clear();
    d_->chats.clear();
    d_->prof = Profile{};
    d_->load_profile();
    return true;
}

bool Session::host(const fs::path& omsi_root, const fs::path& live_map_dir, std::uint16_t port, std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->role != Role::None) {
        err = "Ya estas en una sala";
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        err = "No se pudo crear el socket";
        return false;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port ? port : kDefaultPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(s, 8) != 0) {
        closesocket(s);
        err = "El puerto " + std::to_string(port) + " esta ocupado";
        return false;
    }
    set_nonblocking(s);
    std::random_device rd;
    d_->secret = (static_cast<std::uint32_t>(rd()) << 1) ^ static_cast<std::uint32_t>(rd()) ^ 0x5a5a1234u;
    d_->listen_s = s;
    d_->port = port ? port : kDefaultPort;
    d_->role = Role::Host;
    d_->my_id = 0;
    d_->omsi_root = omsi_root;
    d_->map_dir = live_map_dir;
    d_->room_map = utf8(live_map_dir.filename());
    d_->op_log.clear();
    d_->host_man_ready = false;
    d_->pending_welcome.clear();
    if (d_->host_hasher.joinable()) d_->host_hasher.join();
    {
        Impl* impl = d_;
        const fs::path dir = live_map_dir;
        d_->host_hasher = std::thread([impl, dir] {
            auto m = compute_manifest(dir);
            std::lock_guard<std::recursive_mutex> lk2(impl->mu);
            impl->host_man = std::move(m);
            impl->host_man_ready = true;
        });
    }
    {
        // open the port on the router (UPnP) in the background; the router reports the public IP
        std::string local;
        for (const auto& a2 : d_->local_addrs())
            if (Impl::is_private_v4(a2) && a2.rfind("100.", 0) != 0) {
                local = a2;
                break;
            }
        if (local.empty()) {
            const auto la = d_->local_addrs();
            if (!la.empty()) local = la.front();
        }
        d_->upnp_status = "Abriendo el puerto en el router (UPnP)...";
        Impl* impl = d_;
        const std::uint16_t prt = d_->port;
        // chained, never joined under the lock (the UPnP thread takes it at the end)
        std::thread prev = std::move(d_->upnp_thread);
        d_->upnp_thread = std::thread([impl, prt, local, p = std::move(prev)]() mutable {
            if (p.joinable()) p.join();
            impl->upnp_map(prt, local);
        });
    }
    {
        // internet through any router (CGNAT included): Cloudflare quick tunnel, as openOMSI does
        auto t = std::make_shared<HostTunnel>();
        d_->tunnel = t;
        const std::uint16_t room = d_->port;
        std::thread prev = std::move(d_->tunnel_thread);
        d_->tunnel_thread = std::thread([t, room, p = std::move(prev)]() mutable {
            if (p.joinable()) p.join();
            t->start(static_cast<std::uint16_t>(room + 2), room);
        });
    }
    d_->rebuild_users_host();
    d_->sys_msg("Sala creada (mapa " + d_->room_map + ", puerto " + std::to_string(d_->port) + ")");
    log::info("coop: hosting map=" + d_->room_map + " port=" + std::to_string(d_->port));
    return true;
}

bool Session::join(const std::string& code_in, const fs::path& omsi_root, std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->role != Role::None) {
        err = "Ya estas en una sala";
        return false;
    }
    std::string code = code_in;
    while (!code.empty() && (code.back() == ' ' || code.back() == '\r' || code.back() == '\n')) code.pop_back();
    while (!code.empty() && code.front() == ' ') code.erase(code.begin());
    in_addr ia{};
    std::uint16_t port = 0;
    std::uint32_t secret = 0;
    if (code.size() > 4 && (code.rfind("F4T~", 0) == 0 || code.rfind("f4t~", 0) == 0)) {
        // internet room behind a Cloudflare quick tunnel: bridge wss://<words>.trycloudflare.com locally
        const auto t2 = code.find('~', 4);
        if (t2 == std::string::npos) {
            err = "Codigo invalido";
            return false;
        }
        const std::string words = code.substr(4, t2 - 4);
        std::vector<std::uint8_t> sd;
        if (!b32_decode(code.substr(t2 + 1), sd) || sd.size() < 4) {
            err = "Codigo invalido";
            return false;
        }
        for (int i = 0; i < 4; ++i) secret |= static_cast<std::uint32_t>(sd[i]) << (8 * i);
        d_->bridge = std::make_unique<ClientBridge>();
        port = d_->bridge->start(words + ".trycloudflare.com", err);
        if (port == 0) {
            d_->bridge.reset();
            return false;
        }
        inet_pton(AF_INET, "127.0.0.1", &ia);
    } else {
        if (code.size() > 3 && (code[0] == 'F' || code[0] == 'f') && code[1] == '4') code = code.substr(2);
        std::vector<std::uint8_t> d;
        if (!b32_decode(code, d) || d.size() < 10) {
            err = "Codigo invalido";
            return false;
        }
        std::memcpy(&ia, d.data(), 4);
        port = static_cast<std::uint16_t>((d[4] << 8) | d[5]);
        for (int i = 0; i < 4; ++i) secret |= static_cast<std::uint32_t>(d[6 + i]) << (8 * i);
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr = ia;
    // blocking connect with a short timeout
    set_nonblocking(s);
    connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    fd_set wr;
    FD_ZERO(&wr);
    FD_SET(s, &wr);
    timeval tv{4, 0};
    int so_err = 0;
    int so_len = sizeof(so_err);
    const int sel = select(0, nullptr, &wr, nullptr, &tv);
    if (sel > 0) getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &so_len);
    if (sel <= 0 || so_err != 0) {
        closesocket(s);
        char ip[64]{};
        inet_ntop(AF_INET, &ia, ip, sizeof(ip));
        err = std::string("No responde ") + ip + ":" + std::to_string(port) +
              " (misma red / puerto abierto / VPN?)";
        return false;
    }
    BOOL nd = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
    Impl::Peer p;
    p.s = s;
    p.id = 0;
    p.hello = true;
    W w;
    w.u32(secret);
    w.u32(kProto);
    w.str(d_->prof.guid);
    w.str(d_->prof.name);
    for (float c : d_->prof.color) w.f32(c);
    p.out = frame(M_Hello, w.b);
    d_->peers.push_back(std::move(p));
    d_->role = Role::Guest;
    d_->secret = secret;
    d_->port = port;
    d_->omsi_root = omsi_root;
    d_->vstate = VersionState::None;
    d_->status = "Conectando...";
    log::info("coop: joining port=" + std::to_string(port));
    return true;
}

void Session::leave() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->role == Role::None) return;
    d_->send_all(M_Bye, {});
    // give the net thread a moment to flush
    d_->mu.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    d_->mu.lock();
    d_->disconnect();
    d_->sys_msg("Saliste de la sala");
    {
        Impl* impl = d_;
        std::thread prev = std::move(d_->upnp_thread);
        d_->upnp_thread = std::thread([impl, p = std::move(prev)]() mutable {
            if (p.joinable()) p.join();
            impl->upnp_unmap_sync();
        });
    }
    if (d_->tunnel) {
        auto t = d_->tunnel;
        d_->tunnel.reset();
        std::thread prev = std::move(d_->tunnel_thread);
        d_->tunnel_thread = std::thread([t, p = std::move(prev)]() mutable {
            if (p.joinable()) p.join();
            t->stop();
        });
    }
    if (d_->bridge) {
        d_->bridge->stop();
        d_->bridge.reset();
    }
}
Role Session::role() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->role;
}
int Session::my_id() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->my_id;
}
std::vector<User> Session::users() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->users;
}
void Session::kick(int user_id) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->role != Role::Host) return;
    if (auto* p = d_->peer_by_id(user_id)) {
        W w;
        w.str("el anfitrion te saco");
        p->out += frame(M_Kick, w.b);
        p->closing = true;
        d_->sys_msg(p->name + " fue expulsado");
    }
}
std::string Session::invite_code(const std::string& address) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->role == Role::Host ? d_->make_code(address) : std::string();
}
std::vector<std::string> Session::local_addresses() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    auto v = d_->local_addrs();
    if (!d_->public_ip.empty()) v.insert(v.begin(), d_->public_ip); // internet (UPnP) first
    if (d_->tunnel) {
        const std::string u = d_->tunnel->url();
        if (!u.empty()) v.insert(v.begin(), "tunel:" + u); // internet through the tunnel: works everywhere
    }
    return v;
}
std::string Session::internet_status() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->role != Role::Host) return {};
    std::string s = d_->upnp_status;
    if (d_->tunnel) {
        const std::string t = d_->tunnel->status();
        if (!t.empty()) s += (s.empty() ? "" : "\n") + t;
    }
    return s;
}
std::string Session::public_address() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->public_ip;
}
std::uint16_t Session::port() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->port;
}
std::string Session::status() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->status;
}
std::vector<LanPeer> Session::lan_peers() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    std::vector<LanPeer> v;
    const double t = now_s();
    for (const auto& [g, p] : d_->lan)
        if (t - p.last_seen < 10.0) v.push_back(p);
    return v;
}
std::string Session::map_name() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->room_map;
}

void Session::update() {
    std::vector<Impl::Inbound> in;
    std::vector<int> gone;
    {
        std::lock_guard<std::recursive_mutex> lk(d_->mu);
        in.swap(d_->inbox);
        gone.swap(d_->dropped);
    }
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    for (const auto& ib : in) d_->handle(ib);
    for (int id : gone) {
        if (d_->role == Role::Host) {
            const std::string nm = d_->user_name(id);
            d_->poses.erase(id);
            d_->rebuild_users_host();
            d_->broadcast_users();
            if (nm != "?") d_->sys_msg(nm + " salio de la sala");
        } else if (d_->role == Role::Guest && id == 0) {
            d_->disconnect();
            d_->sys_msg("Se perdio la conexion con el anfitrion");
        }
    }
    if (d_->role == Role::Host && d_->host_man_ready && !d_->pending_welcome.empty()) {
        for (int id : d_->pending_welcome)
            if (auto* p = d_->peer_by_id(id)) d_->send_welcome(*p);
        d_->pending_welcome.clear();
    }
    if (d_->vstate == VersionState::Comparing && d_->hash_done) {
        if (d_->hasher.joinable()) d_->hasher.join();
        d_->finish_compare();
    }
    // guest: ask the host for missing objects / splines (a few batches in flight)
    if (d_->role == Role::Guest && d_->my_id >= 0 && !d_->asset_queue.empty() && d_->asset_pending < 3) {
        const std::size_t n = (std::min)(d_->asset_queue.size(), static_cast<std::size_t>(48));
        W w;
        w.u32(static_cast<std::uint32_t>(n));
        for (std::size_t i = 0; i < n; ++i) w.str(d_->asset_queue[i]);
        d_->asset_queue.erase(d_->asset_queue.begin(), d_->asset_queue.begin() + static_cast<std::ptrdiff_t>(n));
        d_->send_to(0, M_AssetReq, w.b);
        ++d_->asset_pending;
    }
    // stale avatars
    const double t = now_s();
    for (auto it = d_->poses.begin(); it != d_->poses.end();)
        it = (t - it->second.recv_time > 15.0) ? d_->poses.erase(it) : std::next(it);
}

void Session::send_op(OpKind kind, std::int32_t id, const std::string& data) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->role == Role::None) return;
    MapOp op;
    op.kind = kind;
    op.id = id;
    op.data = data;
    op.from_user = d_->my_id;
    if (d_->role == Role::Host) {
        d_->log_op(op);
        d_->send_all(M_Op, Impl::op_payload(op));
    } else {
        d_->send_to(0, M_Op, Impl::op_payload(op));
    }
}
std::vector<MapOp> Session::take_ops() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    std::vector<MapOp> v;
    v.swap(d_->ops_in);
    return v;
}
void Session::id_range(std::int32_t& lo, std::int32_t& hi) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->role == Role::Guest && d_->my_id > 0) {
        lo = 100000000 + d_->my_id * 10000000;
        hi = lo + 9999999;
    } else if (d_->role == Role::Host) {
        lo = 1;
        hi = 99999999;
    } else {
        lo = 1;
        hi = 0x7fffffff;
    }
}

void Session::send_pose(const Pose& p) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->role == Role::None) return;
    W w;
    w.i32(d_->my_id);
    w.i32(p.gx);
    w.i32(p.gy);
    w.f32(p.lx);
    w.f32(p.ly);
    w.f32(p.lz);
    w.f32(p.yaw);
    w.f32(p.pitch);
    w.u8(static_cast<std::uint8_t>((p.walking ? 1u : 0u) | (p.airborne ? 2u : 0u)));
    if (d_->role == Role::Host) d_->send_all(M_Pose, w.b);
    else d_->send_to(0, M_Pose, w.b);
}
std::vector<Pose> Session::poses() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    std::vector<Pose> v;
    for (const auto& [id, p] : d_->poses) v.push_back(p);
    return v;
}

VersionState Session::version_state() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->vstate;
}
std::string Session::version_summary() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->vsummary;
}
float Session::transfer_progress() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->fetch_total ? static_cast<float>(static_cast<double>(d_->fetch_done) / static_cast<double>(d_->fetch_total))
                           : 0.f;
}
void Session::choose_update_my_map() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->role != Role::Guest || d_->vstate != VersionState::Differs) return;
    const fs::path live = d_->omsi_root / "maps" / u8p(d_->room_map);
    // back up every file that is about to be overwritten
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
    const fs::path backup = d_->omsi_root / "maps" / u8p(d_->room_map + "_backup_" + stamp);
    std::error_code ec;
    for (const auto& r : d_->to_fetch) {
        const auto src = live / u8p(r);
        if (!fs::exists(src, ec)) continue;
        fs::create_directories((backup / u8p(r)).parent_path(), ec);
        fs::copy_file(src, backup / u8p(r), fs::copy_options::overwrite_existing, ec);
    }
    d_->host_copy = false;
    d_->fetch_ram = false;
    d_->request_files(live);
    d_->sys_msg("Actualizando tu mapa (copia de seguridad en " + utf8(backup.filename()) + ")");
}
void Session::choose_host_version() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->role != Role::Guest || d_->vstate != VersionState::Differs) return;
    const fs::path mine = d_->omsi_root / "maps" / u8p(d_->room_map);
    const fs::path copy = d_->omsi_root / "maps" / u8p(d_->room_map + "_coop");
    std::error_code ec;
    fs::create_directories(copy, ec);
    // start from the local files, as links (no copy, no space): only what differs comes over the network
    // and replaces its link with a real file
    if (fs::is_directory(mine, ec))
        for (const auto& e : fs::recursive_directory_iterator(mine, ec))
            if (e.is_regular_file(ec)) omsi::fsutil::link_or_copy(e.path(), copy / fs::relative(e.path(), mine, ec));
    d_->host_copy = true;
    d_->fetch_ram = false;
    d_->request_files(copy);
    d_->sys_msg("Recibiendo la version del anfitrion en " + utf8(copy.filename()));
}
void Session::choose_host_version_ram() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->role != Role::Guest || d_->vstate != VersionState::Differs) return;
    d_->host_copy = true;
    d_->fetch_ram = true;
    d_->ram_files.clear();
    d_->request_files(d_->omsi_root / "maps" / u8p(d_->room_map));
    d_->sys_msg("Recibiendo la version del anfitrion en memoria (no se escribe nada en disco)");
}
bool Session::take_reload(fs::path& out, bool* force) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->reload_cfg.empty()) return false;
    out = d_->reload_cfg;
    d_->reload_cfg.clear();
    if (force) *force = d_->reload_force;
    d_->reload_force = false;
    return true;
}
void Session::request_assets(const std::vector<fs::path>& files) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->role != Role::Guest || d_->omsi_root.empty()) return;
    std::wstring root = d_->omsi_root.lexically_normal().native();
    for (auto& c : root) c = static_cast<wchar_t>(std::towlower(c == L'/' ? L'\\' : c));
    while (!root.empty() && root.back() == L'\\') root.pop_back();
    for (const auto& f : files) {
        if (f.empty()) continue;
        std::wstring fw = f.lexically_normal().native();
        for (auto& c : fw)
            if (c == L'/') c = L'\\';
        std::wstring fl = fw;
        for (auto& c : fl) c = static_cast<wchar_t>(std::towlower(c));
        if (fl.size() <= root.size() + 1 || fl.compare(0, root.size(), root) != 0 || fl[root.size()] != L'\\') continue;
        const std::string rs = utf8(fs::path(fw.substr(root.size() + 1)));
        if (rs.empty() || !safe_rel(rs) || !asset_ext_ok(rs)) continue;
        if (!d_->asset_asked.insert(lower(rs)).second) continue;
        d_->asset_queue.push_back(rs);
    }
}
int Session::assets_epoch() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->asset_epoch;
}
std::string Session::assets_status() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->role != Role::Guest || (d_->asset_files == 0 && d_->asset_pending == 0)) return {};
    std::ostringstream o;
    o << "Objetos del anfitrion en RAM: " << d_->asset_files << " archivos, " << (d_->asset_bytes / (1024 * 1024))
      << " MB";
    if (d_->asset_pending > 0 || !d_->asset_queue.empty()) o << " (recibiendo...)";
    return o.str();
}
bool Session::editing_in_ram() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->role == Role::Guest && d_->ram_mode;
}
void Session::notify_map_loaded(const fs::path& omsi_root, const fs::path& live_map_dir) {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    if (d_->role == Role::Host) {
        d_->map_dir = live_map_dir;
        return;
    }
    if (d_->role != Role::Guest || !d_->want_ready) return;
    (void)omsi_root;
    std::error_code ec;
    auto same_dir = [&](const fs::path& a, const fs::path& b) {
        if (fs::equivalent(a, b, ec)) return true;
        return lower(utf8(a.lexically_normal())) == lower(utf8(b.lexically_normal()));
    };
    if (!d_->fetch_dir.empty() && same_dir(live_map_dir, d_->fetch_dir)) {
        d_->want_ready = false;
        d_->vstate = VersionState::Ready;
        d_->send_to(0, M_Ready, {});
        d_->sys_msg("Listo: viendo los cambios de la sala en vivo");
    } else if (d_->vstate == VersionState::Same || d_->vstate == VersionState::Reloading) {
        // the editor still has another map open: ask it again
        d_->reload_cfg = d_->fetch_dir / "global.cfg";
    }
}
bool Session::editing_host_copy() {
    std::lock_guard<std::recursive_mutex> lk(d_->mu);
    return d_->role == Role::Guest && d_->host_copy;
}

} // namespace omsi::coop
