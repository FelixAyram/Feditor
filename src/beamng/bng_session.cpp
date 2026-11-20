#include "beamng/bng_session.hpp"

#include <fstream>
#include <cwctype>
#include <algorithm>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bng {

namespace {
std::filesystem::path prefs_path() {
#ifdef _WIN32
    wchar_t mod[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, mod, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::filesystem::path(mod).parent_path() / "omsi_f4_beamng_prefs.cfg";
#endif
    return "omsi_f4_beamng_prefs.cfg";
}
std::mutex g_vfs_mu;
std::shared_ptr<Vfs> g_vfs;
} // namespace

Prefs load_prefs() {
    Prefs p;
    std::ifstream in(prefs_path(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "game") p.game = v;
        else if (k == "install") p.install = std::filesystem::u8path(v);
        else if (k == "user_folder") p.user_folder = std::filesystem::u8path(v);
        else if (k == "last_level") p.last_level = v;
        else if (k == "last_spawn") p.last_spawn = v;
        else if (k == "load_radius") p.load_radius = std::strtof(v.c_str(), nullptr);
        else if (k == "q_mesh") p.q_mesh = std::atoi(v.c_str());
        else if (k == "q_texture") p.q_texture = std::atoi(v.c_str());
        else if (k == "q_shadows") p.q_shadows = std::atoi(v.c_str());
        else if (k == "recent_install" && !v.empty()) p.recent_installs.push_back(std::filesystem::u8path(v));
        else if (k == "recent_user" && !v.empty()) p.recent_users.push_back(std::filesystem::u8path(v));
    }
    if (p.load_radius < 50.f) p.load_radius = 1000.f;
    return p;
}

void save_prefs(const Prefs& p) {
    std::ofstream out(prefs_path(), std::ios::binary | std::ios::trunc);
    auto u8 = [](const std::filesystem::path& x) {
        const auto s = x.u8string();
        return std::string(s.begin(), s.end());
    };
    out << "game=" << p.game << "\n";
    out << "install=" << u8(p.install) << "\n";
    out << "user_folder=" << u8(p.user_folder) << "\n";
    out << "last_level=" << p.last_level << "\n";
    out << "last_spawn=" << p.last_spawn << "\n";
    out << "load_radius=" << p.load_radius << "\n";
    out << "q_mesh=" << p.q_mesh << "\n";
    out << "q_texture=" << p.q_texture << "\n";
    out << "q_shadows=" << p.q_shadows << "\n";
    for (const auto& r : p.recent_installs) out << "recent_install=" << u8(r) << "\n";
    for (const auto& r : p.recent_users) out << "recent_user=" << u8(r) << "\n";
}

void remember_path(std::vector<std::filesystem::path>& list, const std::filesystem::path& p) {
    if (p.empty()) return;
    auto key = [](const std::filesystem::path& x) {
        std::wstring k = x.lexically_normal().native();
        for (auto& c : k) c = static_cast<wchar_t>(towlower(c == L'/' ? L'\\' : c));
        while (k.size() > 3 && k.back() == L'\\') k.pop_back();
        return k;
    };
    const auto pk = key(p);
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::filesystem::path& x) { return key(x) == pk; }),
               list.end());
    list.insert(list.begin(), p.lexically_normal());
    if (list.size() > 12) list.resize(12);
}

void set_active_vfs(std::shared_ptr<Vfs> vfs) {
    std::lock_guard<std::mutex> lk(g_vfs_mu);
    g_vfs = std::move(vfs);
}
std::shared_ptr<Vfs> active_vfs_shared() {
    std::lock_guard<std::mutex> lk(g_vfs_mu);
    return g_vfs;
}
const Vfs* active_vfs() {
    std::lock_guard<std::mutex> lk(g_vfs_mu);
    return g_vfs.get();
}

} // namespace bng
