#include "omsi/vfs_overlay.hpp"
#include "omsi/disk_stats.hpp"

#include "omsi/logger.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <atomic>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omsi::vfs {
namespace fs = std::filesystem;
namespace {

std::atomic<bool> g_active{false};
std::mutex g_mu;
fs::path g_root;
std::wstring g_root_key;
std::unordered_map<std::wstring, std::shared_ptr<const std::string>> g_files;
std::unordered_set<std::wstring> g_dirs; // parent folders of RAM files (exist even when not on disk)
std::unordered_set<std::wstring> g_hidden;
std::size_t g_bytes = 0;
#ifdef _WIN32
std::vector<HANDLE> g_locks;
#endif

std::wstring key_of(const fs::path& p) {
    std::error_code ec;
    fs::path a = p.is_absolute() ? p : fs::absolute(p, ec);
    std::wstring w = a.lexically_normal().wstring();
    for (auto& c : w) {
        if (c == L'/') c = L'\\';
        c = static_cast<wchar_t>(std::towlower(c));
    }
    while (w.size() > 3 && w.back() == L'\\') w.pop_back();
    return w;
}

bool key_under_root(const std::wstring& k) {
    if (g_root_key.empty() || k.size() < g_root_key.size()) return false;
    if (k.compare(0, g_root_key.size(), g_root_key) != 0) return false;
    return k.size() == g_root_key.size() || k[g_root_key.size()] == L'\\';
}

void release_locks_locked() {
#ifdef _WIN32
    for (HANDLE h : g_locks) CloseHandle(h);
    g_locks.clear();
#endif
}

} // namespace

bool active() { return g_active.load(std::memory_order_acquire); }

void begin(const fs::path& root) {
    std::lock_guard<std::mutex> lk(g_mu);
    release_locks_locked();
    // files already received (objects / textures) stay; only the map overlay starts over
    g_hidden.clear();
    g_root = root;
    g_root_key = key_of(root);
    g_active.store(true, std::memory_order_release);
}

void activate_assets() {
    if (active()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_root.clear();
    g_root_key.clear();
    g_active.store(true, std::memory_order_release);
}

void put(const fs::path& path, std::string bytes) {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::wstring k = key_of(path);
    g_hidden.erase(k);
    if (const auto it = g_files.find(k); it != g_files.end()) g_bytes -= it->second->size();
    g_bytes += bytes.size();
    g_files[k] = std::make_shared<const std::string>(std::move(bytes));
    for (std::wstring d = k; ;) {
        const auto slash = d.find_last_of(L'\\');
        if (slash == std::wstring::npos || slash < 3) break;
        d.resize(slash);
        if (!g_dirs.insert(d).second) break;
    }
}

void hide(const fs::path& path) {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::wstring k = key_of(path);
    if (g_files.count(k)) return;
    g_hidden.insert(k);
}

void protect_disk_files() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(g_mu);
    release_locks_locked();
    std::error_code ec;
    if (g_root.empty() || !fs::is_directory(g_root, ec)) return;
    std::size_t failed = 0;
    for (const auto& e : fs::recursive_directory_iterator(g_root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        std::error_code ec2;
        if (!e.is_regular_file(ec2)) continue;
        // read sharing only: other opens for reading work, any open for writing / replace / delete fails
        HANDLE h = CreateFileW(e.path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            ++failed;
            continue;
        }
        g_locks.push_back(h);
        if (g_locks.size() > 200000) break;
    }
    log::info("vfs: " + std::to_string(g_locks.size()) + " archivos propios protegidos contra escritura" +
              (failed ? " (" + std::to_string(failed) + " no se pudieron abrir)" : std::string()));
#endif
}

void end() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_active.store(false, std::memory_order_release);
    release_locks_locked();
    g_files.clear();
    g_dirs.clear();
    g_hidden.clear();
    g_bytes = 0;
    g_root.clear();
    g_root_key.clear();
}

fs::path root() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_root;
}

bool under_root(const fs::path& path) {
    if (!active()) return false;
    const std::wstring k = key_of(path);
    std::lock_guard<std::mutex> lk(g_mu);
    return key_under_root(k);
}

bool write_blocked(const fs::path& path) {
    if (!active() || path.empty()) return false;
    const bool b = under_root(path);
    if (b) {
        const auto u = path.filename().u8string();
        log::warn("vfs: escritura bloqueada (version del anfitrion en RAM): " + std::string(u.begin(), u.end()));
    }
    return b;
}

std::shared_ptr<const std::string> get(const fs::path& path) {
    if (!active() || path.empty()) return nullptr;
    const std::wstring k = key_of(path);
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_files.find(k);
    return it == g_files.end() ? nullptr : it->second;
}

bool hidden(const fs::path& path) {
    if (!active() || path.empty()) return false;
    const std::wstring k = key_of(path);
    std::lock_guard<std::mutex> lk(g_mu);
    return g_hidden.count(k) != 0;
}

bool exists(const fs::path& path, std::error_code& ec) {
    ec.clear();
    if (active() && !path.empty()) {
        const std::wstring k = key_of(path);
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_files.count(k) || g_dirs.count(k) || k == g_root_key) return true;
        if (g_hidden.count(k)) return false;
    }
    return fs::exists(path, ec);
}

bool exists(const fs::path& path) {
    std::error_code ec;
    return omsi::vfs::exists(path, ec);
}

bool read_file(const fs::path& path, std::string& out) {
    if (const auto ram = get(path)) {
        out = *ram;
        return true;
    }
    if (hidden(path)) return false;
    omsi::disk_stats::Timer disk_t(omsi::disk_stats::Vfs, path);
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    disk_t.bytes = out.size();
    return true;
}

std::size_t ram_bytes() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_bytes;
}

std::size_t ram_files() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_files.size();
}

} // namespace omsi::vfs
