#include "omsi/map_packager.hpp"

#include "gfx/dx12/dx12_texture.hpp"
#include "o3d/o3d_loader.hpp"
#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"
#include "omsi/utf8_win32.hpp"

#include "../../third_party/miniz/miniz.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace omsi::pack {
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path from_utf8(const std::string& s) {
    return fs::path(omsi::utf8_to_wide(s)); // UTF-8 or ANSI cfg text, never throws
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string ext_of(const std::string& s) {
    const auto dot = s.rfind('.');
    if (dot == std::string::npos || dot + 1 >= s.size()) return {};
    const auto slash = s.find_last_of("\\/");
    if (slash != std::string::npos && slash > dot) return {};
    return lower(s.substr(dot));
}

bool is_texture_ext(const std::string& e) {
    return e == ".dds" || e == ".bmp" || e == ".png" || e == ".tga" || e == ".jpg" || e == ".jpeg";
}

bool is_asset_ext(const std::string& e) {
    static const char* k[] = {".sco", ".sli", ".o3d", ".x",   ".dds", ".bmp", ".png", ".tga", ".jpg", ".jpeg",
                              ".cfg", ".osc", ".txt", ".wav", ".ogg", ".bus", ".ovh", ".hum", ".oft", ".cti",
                              ".ctc", ".hof", ".ttp", ".ttr", ".ttl", ".opl"};
    for (const char* x : k)
        if (e == x) return true;
    return false;
}

// text files whose lines name further files
bool is_expandable_ext(const std::string& e) {
    return e == ".sco" || e == ".sli" || e == ".bus" || e == ".ovh" || e == ".cfg" || e == ".hum" || e == ".hof" ||
           e == ".cti" || e == ".ctc" || e == ".x";
}

bool exists_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string key_of(const fs::path& p) {
    return lower(utf8(p.lexically_normal()));
}

// relative path below root in OMSI form (lower case, backslashes); empty when outside
std::string rel_key(const fs::path& p, const fs::path& root) {
    std::error_code ec;
    const auto r = fs::relative(p, root, ec);
    if (ec || r.empty()) return {};
    std::string s = lower(utf8(r));
    if (s.rfind("..", 0) == 0) return {};
    for (char& c : s)
        if (c == '/') c = '\\';
    return s;
}

// ---- protobuf (just enough for Steam depot manifests) ----
bool read_varint(const std::uint8_t*& p, const std::uint8_t* end, std::uint64_t& out) {
    out = 0;
    int shift = 0;
    while (p < end && shift < 64) {
        const std::uint8_t b = *p++;
        out |= static_cast<std::uint64_t>(b & 0x7f) << shift;
        if ((b & 0x80) == 0) return true;
        shift += 7;
    }
    return false;
}

bool skip_field(const std::uint8_t*& p, const std::uint8_t* end, int wt) {
    std::uint64_t v = 0;
    switch (wt) {
    case 0: return read_varint(p, end, v);
    case 1: if (end - p < 8) return false; p += 8; return true;
    case 2: if (!read_varint(p, end, v) || static_cast<std::uint64_t>(end - p) < v) return false; p += v; return true;
    case 5: if (end - p < 4) return false; p += 4; return true;
    default: return false;
    }
}

std::uint32_t rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Returns false when the names are encrypted or the file is unreadable.
bool parse_depot_manifest(const fs::path& file, std::vector<std::pair<std::string, std::uint64_t>>& out,
                          bool& encrypted) {
    encrypted = false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::vector<std::uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    constexpr std::uint32_t kPayload = 0x71F617D0u, kMetadata = 0x1F4812BEu;
    std::vector<std::pair<std::string, std::uint64_t>> names;
    std::size_t off = 0;
    bool have_payload = false;
    while (off + 8 <= buf.size()) {
        const std::uint32_t magic = rd32(&buf[off]);
        const std::uint32_t len = rd32(&buf[off + 4]);
        off += 8;
        if (off + len > buf.size()) break;
        const std::uint8_t* p = buf.data() + off;
        const std::uint8_t* end = p + len;
        if (magic == kPayload) {
            have_payload = true;
            while (p < end) {
                std::uint64_t key = 0;
                if (!read_varint(p, end, key)) break;
                const int field = static_cast<int>(key >> 3), wt = static_cast<int>(key & 7);
                if (field == 1 && wt == 2) {
                    std::uint64_t ml = 0;
                    if (!read_varint(p, end, ml) || static_cast<std::uint64_t>(end - p) < ml) break;
                    const std::uint8_t* q = p;
                    const std::uint8_t* qe = p + ml;
                    std::string name;
                    std::uint64_t size = 0, flags = 0;
                    while (q < qe) {
                        std::uint64_t k2 = 0;
                        if (!read_varint(q, qe, k2)) break;
                        const int f2 = static_cast<int>(k2 >> 3), w2 = static_cast<int>(k2 & 7);
                        if (f2 == 1 && w2 == 2) {
                            std::uint64_t sl = 0;
                            if (!read_varint(q, qe, sl) || static_cast<std::uint64_t>(qe - q) < sl) break;
                            name.assign(reinterpret_cast<const char*>(q), static_cast<std::size_t>(sl));
                            q += sl;
                        } else if (f2 == 2 && w2 == 0) {
                            read_varint(q, qe, size);
                        } else if (f2 == 3 && w2 == 0) {
                            read_varint(q, qe, flags);
                        } else if (!skip_field(q, qe, w2)) {
                            break;
                        }
                    }
                    p += ml;
                    if (!name.empty() && (flags & 64u) == 0) { // 64 = directory
                        name = lower(name);
                        for (char& c : name)
                            if (c == '/') c = '\\';
                        names.emplace_back(std::move(name), size);
                    }
                } else if (!skip_field(p, end, wt)) {
                    break;
                }
            }
        } else if (magic == kMetadata) {
            while (p < end) {
                std::uint64_t key = 0;
                if (!read_varint(p, end, key)) break;
                const int field = static_cast<int>(key >> 3), wt = static_cast<int>(key & 7);
                if (field == 4 && wt == 0) {
                    std::uint64_t v = 0;
                    read_varint(p, end, v);
                    encrypted = v != 0;
                } else if (!skip_field(p, end, wt)) {
                    break;
                }
            }
        }
        off += len;
    }
    if (!have_payload || encrypted) return false;
    out.insert(out.end(), names.begin(), names.end());
    return true;
}

// ---- Valve KeyValues (.acf), tokens only ----
std::vector<std::string> kv_tokens(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::string> t;
    for (std::size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == '"') {
            const auto j = s.find('"', i + 1);
            if (j == std::string::npos) break;
            t.push_back(s.substr(i + 1, j - i - 1));
            i = j + 1;
        } else if (c == '{' || c == '}') {
            t.emplace_back(1, c);
            ++i;
        } else {
            ++i;
        }
    }
    return t;
}

// depot id -> manifest id from "InstalledDepots"
std::vector<std::pair<std::string, std::string>> installed_depots(const fs::path& acf, std::string* installdir) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto t = kv_tokens(acf);
    for (std::size_t i = 0; i + 1 < t.size(); ++i)
        if (installdir && lower(t[i]) == "installdir") *installdir = t[i + 1];
    for (std::size_t i = 0; i + 1 < t.size(); ++i) {
        if (lower(t[i]) != "installeddepots" || t[i + 1] != "{") continue;
        int depth = 0;
        std::string depot;
        for (std::size_t j = i + 1; j < t.size(); ++j) {
            if (t[j] == "{") {
                ++depth;
                continue;
            }
            if (t[j] == "}") {
                if (--depth <= 0) break;
                continue;
            }
            if (depth == 1 && j + 1 < t.size() && t[j + 1] == "{") depot = t[j];
            if (depth == 2 && lower(t[j]) == "manifest" && j + 1 < t.size()) out.emplace_back(depot, t[j + 1]);
        }
        break;
    }
    return out;
}

fs::path steam_root_dir() {
#ifdef _WIN32
    wchar_t buf[1024];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &sz) ==
        ERROR_SUCCESS)
        return fs::path(buf);
#endif
    return {};
}

#ifdef _WIN32
// Runs tools\map_dependency_scanner\f4_pack_scan.py (the Python scanner) and writes its TSV.
bool run_python_scan(const PackOptions& opt, PackProgress& pr, const fs::path& tsv, std::string& err) {
    const fs::path script = opt.scanner_dir / "f4_pack_scan.py";
    if (opt.scanner_dir.empty() || !exists_file(script)) {
        err = "no se encontro f4_pack_scan.py";
        return false;
    }
    // py launcher first; a python.exe under WindowsApps is the Store stub, not Python
    auto find_exe = [](const wchar_t* name) -> std::wstring {
        wchar_t buf[MAX_PATH];
        const DWORD n = SearchPathW(nullptr, name, nullptr, MAX_PATH, buf, nullptr);
        if (n == 0 || n >= MAX_PATH) return {};
        std::wstring p(buf, n);
        std::wstring low = p;
        for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
        if (low.find(L"\\windowsapps\\") != std::wstring::npos) return {};
        return p;
    };
    std::wstring py = find_exe(L"py.exe");
    std::wstring args;
    if (!py.empty()) args = L" -3";
    else py = find_exe(L"python.exe");
    if (py.empty()) {
        err = "Python no esta instalado";
        return false;
    }
    auto q = [](const fs::path& p) { return L"\"" + p.wstring() + L"\""; };
    std::wstring cmd = L"\"" + py + L"\"" + args + L" " + q(script) + L" --omsi-root " + q(opt.omsi_root) +
                       L" --map " + q(opt.global_cfg) + L" -o " + q(tsv);
    if (!opt.ai_vehicles) cmd += L" --no-ai-buses";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        err = "CreatePipe";
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    const std::wstring cwd = opt.scanner_dir.wstring();
    const BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        err = "no se pudo ejecutar Python";
        return false;
    }
    std::string out, tail;
    char buf[4096];
    bool cancelled = false;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            if (ReadFile(rd, buf, (std::min)(avail, static_cast<DWORD>(sizeof(buf))), &got, nullptr) && got > 0) {
                out.append(buf, got);
                std::size_t nl;
                while ((nl = out.find('\n')) != std::string::npos) {
                    std::string line = trim(out.substr(0, nl));
                    out.erase(0, nl + 1);
                    if (line.rfind("PROGRESS ", 0) == 0) {
                        std::lock_guard<std::mutex> lk(pr.mu);
                        pr.stage = "Escaner (Python): " + line.substr(9);
                    } else if (!line.empty()) {
                        tail = line; // last error / warning line
                    }
                }
            }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) break;
        if (pr.cancel) {
            TerminateProcess(pi.hProcess, 1);
            cancelled = true;
            break;
        }
        // indeterminate progress while Python scans
        pr.frac = (std::min)(0.28f, pr.frac.load() + 0.0015f);
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    if (cancelled) {
        err = "Cancelado";
        return false;
    }
    if (code != 0 || !exists_file(tsv)) {
        err = "el escaner Python fallo" + (tail.empty() ? std::string() : ": " + tail);
        return false;
    }
    return true;
}
#endif

} // namespace

const std::uint64_t* SteamFileList::find(const std::string& rel_lower) const {
    auto it = std::lower_bound(files.begin(), files.end(), rel_lower,
                               [](const auto& a, const std::string& b) { return a.first < b; });
    if (it == files.end() || it->first != rel_lower) return nullptr;
    return &it->second;
}

SteamFileList load_steam_file_list(const fs::path& omsi_root) {
    SteamFileList list;
    // Libraries: the one omsi_root sits in (<library>\steamapps\common\OMSI 2) and every library Steam
    // knows (libraryfolders.vdf) - an OMSI copied out of Steam (another drive) still matches the
    // depot lists of the installed game: same relative paths.
    std::vector<fs::path> libs{omsi_root.parent_path().parent_path()};
    const fs::path sr = steam_root_dir();
    if (!sr.empty()) {
        libs.push_back(sr / "steamapps");
        const auto t = kv_tokens(sr / "steamapps" / "libraryfolders.vdf");
        for (std::size_t i = 0; i + 1 < t.size(); ++i)
            if (lower(t[i]) == "path" && t[i + 1] != "{") libs.push_back(fs::path(omsi::utf8_to_wide(t[i + 1])) / "steamapps");
    }
    std::vector<fs::path> caches;
    if (!sr.empty()) caches.push_back(sr / "depotcache");
    for (const auto& l : libs) {
        caches.push_back(l / "depotcache");
        caches.push_back(l.parent_path() / "depotcache");
    }
    // install folder name of OMSI 2 (252530): DLC apps installed into the same folder belong to it
    std::string install;
    for (const auto& l : libs) {
        std::string inst;
        if (exists_file(l / "appmanifest_252530.acf")) {
            (void)installed_depots(l / "appmanifest_252530.acf", &inst);
            if (!inst.empty()) {
                install = lower(inst);
                break;
            }
        }
    }
    if (install.empty()) install = lower(utf8(omsi_root.filename()));
    std::unordered_set<std::string> seen_depots, seen_libs;
    std::error_code ec;
    for (const auto& steamapps : libs) {
      if (!seen_libs.insert(key_of(steamapps)).second) continue;
      for (const auto& ent : fs::directory_iterator(steamapps, ec)) {
        const auto name = lower(utf8(ent.path().filename()));
        if (name.rfind("appmanifest_", 0) != 0 || ext_of(name) != ".acf") continue;
        std::string installdir;
        auto depots = installed_depots(ent.path(), &installdir);
        // OMSI 2 itself (252530) and every app installed into the same folder (DLC apps)
        if (lower(installdir) != install && name != "appmanifest_252530.acf") continue;
        for (const auto& [depot, manifest] : depots) {
            if (!seen_depots.insert(depot).second) continue;
            bool found = false, enc = false;
            for (const auto& c : caches) {
                const auto f = c / (depot + "_" + manifest + ".manifest");
                if (!exists_file(f)) continue;
                found = true;
                if (parse_depot_manifest(f, list.files, enc)) ++list.depots_read;
                else if (enc) ++list.depots_encrypted;
                break;
            }
            if (!found) ++list.depots_missing;
        }
      }
    }
    std::sort(list.files.begin(), list.files.end());
    list.files.erase(std::unique(list.files.begin(), list.files.end(),
                                 [](const auto& a, const auto& b) { return a.first == b.first; }),
                     list.files.end());
    log::info("pack: steam depot files=" + std::to_string(list.files.size()) + " depots read=" +
              std::to_string(list.depots_read) + " missing=" + std::to_string(list.depots_missing) +
              " encrypted=" + std::to_string(list.depots_encrypted));
    return list;
}

fs::path default_pack_path(const fs::path& omsi_root, const fs::path& global_cfg) {
    std::time_t now = std::time(nullptr);
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M", &t);
    auto name = global_cfg.parent_path().filename();
    return omsi_root / "F4_Packs" / from_utf8(utf8(name) + "_" + stamp + ".zip");
}

static void run_pack_impl(const PackOptions& opt, PackProgress& pr);

void run_pack(const PackOptions& opt, PackProgress& pr) {
    // never take the editor down from the worker thread
    try {
        run_pack_impl(opt, pr);
    } catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> lk(pr.mu);
            pr.result = std::string("Error: ") + e.what();
        }
        log::warn(std::string("pack: exception ") + e.what());
        pr.ok = false;
        pr.frac = 1.f;
        pr.done = true;
    } catch (...) {
        {
            std::lock_guard<std::mutex> lk(pr.mu);
            pr.result = "Error inesperado al empaquetar";
        }
        pr.ok = false;
        pr.frac = 1.f;
        pr.done = true;
    }
}

static void run_pack_impl(const PackOptions& opt, PackProgress& pr) {
    auto set_stage = [&](const std::string& s) {
        std::lock_guard<std::mutex> lk(pr.mu);
        pr.stage = s;
    };
    auto finish = [&](bool ok, const std::string& msg) {
        {
            std::lock_guard<std::mutex> lk(pr.mu);
            pr.result = msg;
        }
        pr.ok = ok;
        pr.frac = 1.f;
        pr.done = true;
        log::info("pack: " + msg);
    };
    const fs::path root = opt.omsi_root;
    const fs::path map_dir = opt.global_cfg.parent_path();
    if (root.empty() || !exists_file(opt.global_cfg)) return finish(false, "Mapa o carpeta de OMSI invalidos");

    // ---- Steam depot list ----
    SteamFileList steam;
    if (opt.exclude_steam) {
        set_stage("Leyendo manifiestos de Steam (contenido original / DLC)...");
        steam = load_steam_file_list(root);
        pr.steam_data = !steam.files.empty();
    }

    std::vector<fs::path> files;
    std::vector<std::string> missing;
    bool py_ok = false;
    std::string py_err;
#ifdef _WIN32
    // ---- the Python scanner (same result as map_dependency_scanner.py) ----
    {
        set_stage("Escaner (Python)...");
        const fs::path tsv = fs::temp_directory_path() / L"omsi_f4_pack_scan.tsv";
        std::error_code rec;
        fs::remove(tsv, rec);
        if (run_python_scan(opt, pr, tsv, py_err)) {
            std::unordered_set<std::string> seen;
            auto want = [&](const std::string& cats) {
                return (opt.map_files && cats.find("map") != std::string::npos) ||
                       (opt.objects && cats.find("objects") != std::string::npos) ||
                       (opt.splines && cats.find("splines") != std::string::npos) ||
                       (opt.ai_vehicles && cats.find("ai") != std::string::npos) ||
                       (opt.depot_vehicles && cats.find("depot") != std::string::npos);
            };
            std::ifstream in(tsv, std::ios::binary);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto t1 = line.find('\t');
                const auto t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
                if (t2 == std::string::npos) continue;
                const std::string rel = line.substr(0, t1);
                const std::string cats = line.substr(t1 + 1, t2 - t1 - 1);
                const bool exists = line.substr(t2 + 1) == "1";
                if (!want(cats)) continue;
                if (!exists) {
                    missing.push_back(rel + "  [" + cats + "]");
                    continue;
                }
                fs::path p = from_utf8(rel);
                if (p.is_relative()) p = root / p;
                if (seen.insert(key_of(p)).second) files.push_back(p.lexically_normal());
            }
            if (opt.map_files) { // the whole map folder, as the map needs it
                std::error_code ec2;
                for (const auto& e : fs::recursive_directory_iterator(map_dir, ec2))
                    if (e.is_regular_file(ec2) && seen.insert(key_of(e.path())).second) files.push_back(e.path());
            }
            py_ok = true;
            pr.frac = 0.3f;
            log::info("pack: python scan files=" + std::to_string(files.size()) + " missing=" + std::to_string(missing.size()));
        } else if (py_err == "Cancelado") {
            return finish(false, "Cancelado");
        } else {
            log::warn("pack: python scan unavailable (" + py_err + "), using the built-in scan");
        }
    }
#endif
  if (!py_ok) {
    // ---- dependency walk (built-in, when Python is not available) ----
    set_stage("Buscando dependencias...");
    std::unordered_set<std::string> visited;
    std::deque<fs::path> queue;
    auto add = [&](const fs::path& p) {
        const auto k = key_of(p);
        if (!visited.insert(k).second) return;
        files.push_back(p.lexically_normal());
        if (is_expandable_ext(ext_of(utf8(p.filename())))) queue.push_back(p.lexically_normal());
    };
    auto add_texture = [&](const fs::path& owner, const std::string& name) {
        if (const auto r = gfx::dx12::resolve_texture_path(owner, name, root)) {
            add(*r);
            // seasonal / night variants and the texture's .cfg sidecar
            const fs::path tex = *r;
            if (exists_file(fs::path(tex.wstring() + L".cfg"))) add(fs::path(tex.wstring() + L".cfg"));
            std::error_code ec;
            for (const auto& sub : fs::directory_iterator(tex.parent_path(), ec)) {
                if (!sub.is_directory(ec)) continue;
                const auto v = sub.path() / tex.filename();
                if (exists_file(v)) add(v);
            }
            return true;
        }
        missing.push_back(utf8(owner.filename()) + " -> " + name);
        return false;
    };
    auto resolve = [&](const fs::path& owner, std::string tok) -> fs::path {
        for (char& c : tok)
            if (c == '/') c = '\\';
        const fs::path t = from_utf8(tok);
        const fs::path d = owner.parent_path();
        const fs::path cands[] = {d / t, d / "model" / t, d / "texture" / t, d / "sound" / t, d / "script" / t,
                                  d.parent_path() / t, d.parent_path() / "texture" / t,
                                  d.parent_path() / "sound" / t, d.parent_path() / "script" / t, root / t};
        for (const auto& c : cands)
            if (exists_file(c)) return c;
        std::error_code ec;
        if (fs::is_directory(root / t, ec)) return root / t;
        return {};
    };
    auto add_dir_vehicles = [&](const fs::path& dir) {
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
            const auto x = ext_of(utf8(e.path().filename()));
            if (x == ".bus" || x == ".ovh") add(e.path());
        }
    };
    auto add_token = [&](const fs::path& owner, const std::string& tok_in) {
        std::string tok = trim(tok_in);
        if (tok.size() >= 2 && tok.front() == '"' && tok.back() == '"') tok = tok.substr(1, tok.size() - 2);
        if (tok.empty() || tok.size() > 260) return;
        const auto x = ext_of(tok);
        if (!is_asset_ext(x)) {
            // AI lists name vehicle folders
            if (const auto r = resolve(owner, tok); !r.empty()) {
                std::error_code ec;
                if (fs::is_directory(r, ec)) add_dir_vehicles(r);
            }
            return;
        }
        if (is_texture_ext(x)) {
            add_texture(owner, tok);
            return;
        }
        const auto r = resolve(owner, tok);
        if (!r.empty()) {
            add(r);
        } else if (x == ".sco" || x == ".sli" || x == ".o3d" || x == ".x" || x == ".bus" || x == ".ovh") {
            missing.push_back(utf8(owner.filename()) + " -> " + tok);
        }
    };

    // map folder
    std::vector<fs::path> tile_maps;
    {
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(map_dir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            if (opt.map_files) add(e.path());
            const auto x = ext_of(utf8(e.path().filename()));
            if (x == ".map" && e.path().parent_path() == map_dir) tile_maps.push_back(e.path());
        }
    }
    // tiles -> objects / splines
    if (opt.objects || opt.splines) {
        int ti = 0;
        for (const auto& tm : tile_maps) {
            if (pr.cancel) return finish(false, "Cancelado");
            if ((++ti & 15) == 0) pr.frac = 0.05f * static_cast<float>(ti) / static_cast<float>(tile_maps.size());
            for (const auto& line : cfg::read_cfg_lines(tm)) {
                const auto t = trim(line);
                const auto x = ext_of(t);
                if (x == ".sco" && opt.objects) {
                    if (const auto r = resolve(root / "x", t); !r.empty()) add(r);
                    else missing.push_back(utf8(tm.filename()) + " -> " + t);
                } else if (x == ".sli" && opt.splines) {
                    if (const auto r = resolve(root / "x", t); !r.empty()) add(r);
                    else missing.push_back(utf8(tm.filename()) + " -> " + t);
                }
            }
        }
    }
    // AI vehicles / people
    if (opt.ai_vehicles) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(map_dir, ec)) {
            const auto n = lower(utf8(e.path().filename()));
            if ((n.rfind("ailists", 0) == 0 && ext_of(n) == ".cfg") || n == "humans.txt" || n == "drivers.txt") {
                for (const auto& line : cfg::read_cfg_lines(e.path())) add_token(root / "x", line);
            }
        }
    }
    // depot vehicles
    if (opt.depot_vehicles) {
        const auto lines = cfg::read_cfg_lines(opt.global_cfg);
        for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
            if (lower(trim(lines[i])) != "[standarddepot]") continue;
            const auto t = trim(lines[i + 1]);
            fs::path p = root / from_utf8(t);
            std::error_code ec;
            if (!fs::exists(p, ec)) p = map_dir / from_utf8(t);
            if (fs::is_directory(p, ec)) {
                for (const auto& e : fs::recursive_directory_iterator(p, ec))
                    if (e.is_regular_file(ec)) add(e.path());
            } else if (exists_file(p)) {
                add(p);
            }
        }
    }

    // expand
    std::size_t expanded = 0;
    while (!queue.empty()) {
        if (pr.cancel) return finish(false, "Cancelado");
        const fs::path f = queue.front();
        queue.pop_front();
        ++expanded;
        if ((expanded & 63) == 0) {
            pr.frac = 0.05f + 0.25f * static_cast<float>(expanded) / static_cast<float>(expanded + queue.size());
            std::lock_guard<std::mutex> lk(pr.mu);
            pr.current = utf8(f.filename());
        }
        const auto x = ext_of(utf8(f.filename()));
        std::error_code ec;
        if (fs::file_size(f, ec) > 4u * 1024u * 1024u) continue;
        if (x == ".x") {
            for (const auto& line : cfg::read_cfg_lines(f)) {
                const auto q0 = line.find('"');
                const auto q1 = q0 == std::string::npos ? q0 : line.find('"', q0 + 1);
                if (q1 != std::string::npos) add_token(f, line.substr(q0 + 1, q1 - q0 - 1));
            }
            continue;
        }
        for (const auto& line : cfg::read_cfg_lines(f)) add_token(f, line);
    }
    // meshes -> textures (after the walk: meshes were collected as plain files)
    {
        const std::size_t n = files.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (pr.cancel) return finish(false, "Cancelado");
            if (ext_of(utf8(files[i].filename())) != ".o3d") continue;
            const fs::path o3d_path = files[i];
            if (auto m = o3d::load_mapped(o3d_path)) {
                for (const auto& mat : m->mesh().parse_materials())
                    if (!mat.texture.empty()) add_texture(o3d_path, mat.texture);
            }
        }
    }

  } // !py_ok

    // ---- filter Steam content ----
    set_stage("Filtrando contenido original / DLC de Steam...");
    std::vector<fs::path> pack;
    pack.reserve(files.size());
    std::uint64_t total_bytes = 0;
    for (const auto& f : files) {
        const auto rel = rel_key(f, root);
        if (rel.empty()) continue; // outside OMSI 2
        if (opt.exclude_steam && !steam.files.empty()) {
            if (const auto* sz = steam.find(rel)) {
                // user-edited text files of the map folder (tiles, cfgs) differ from the shipped ones
                std::error_code ec;
                const auto x = ext_of(rel);
                const bool in_map = !rel_key(f, map_dir).empty();
                const bool edited_text = in_map && (x == ".map" || x == ".cfg" || x == ".txt") &&
                                         fs::file_size(f, ec) != *sz;
                if (!edited_text) {
                    ++pr.excluded_steam;
                    continue;
                }
            }
        }
        std::error_code ec;
        total_bytes += fs::file_size(f, ec);
        pack.push_back(f);
    }
    pr.total = static_cast<int>(pack.size());
    pr.missing = static_cast<int>(missing.size());

    // ---- zip ----
    set_stage("Comprimiendo...");
    std::error_code ec;
    fs::create_directories(opt.out_zip.parent_path(), ec);
#ifdef _WIN32
    FILE* fp = _wfopen(opt.out_zip.wstring().c_str(), L"wb");
#else
    FILE* fp = std::fopen(opt.out_zip.string().c_str(), "wb");
#endif
    if (!fp) return finish(false, "No se pudo crear " + utf8(opt.out_zip));
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_cfile(&zip, fp, 0)) {
        std::fclose(fp);
        return finish(false, "No se pudo iniciar el zip");
    }
    const mz_uint level = static_cast<mz_uint>(std::clamp(opt.compression, 0, 9));
    std::uint64_t done_bytes = 0;
    bool aborted = false;
    std::vector<char> buf;
    for (const auto& f : pack) {
        if (pr.cancel) {
            aborted = true;
            break;
        }
        std::ifstream in(f, std::ios::binary);
        if (!in) continue;
        buf.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string name = utf8(fs::relative(f, root, ec));
        for (char& c : name)
            if (c == '\\') c = '/';
        if (!mz_zip_writer_add_mem_ex(&zip, name.c_str(), buf.data(), buf.size(), nullptr, 0, level, 0, 0)) {
            log::warn("pack: add failed " + name);
            continue;
        }
        ++pr.added;
        done_bytes += buf.size();
        pr.bytes_in = done_bytes;
        pr.frac = 0.3f + 0.7f * static_cast<float>(total_bytes ? static_cast<double>(done_bytes) / static_cast<double>(total_bytes) : 1.0);
        std::lock_guard<std::mutex> lk(pr.mu);
        pr.current = name;
    }
    if (!aborted) {
        std::ostringstream info;
        info << "Paquete creado por Feditor\r\nMapa: " << utf8(map_dir.filename()) << "\r\nArchivos: " << pr.added
             << "\r\nExcluidos (originales / DLC de Steam): " << pr.excluded_steam
             << "\r\nFaltantes (referenciados pero no instalados): " << missing.size() << "\r\n";
        if (opt.exclude_steam && steam.files.empty())
            info << "AVISO: no se encontraron manifiestos de Steam; no se pudo excluir contenido original.\r\n";
        if (!missing.empty()) {
            info << "\r\n[faltantes]\r\n";
            for (const auto& m : missing) info << m << "\r\n";
        }
        const std::string s = info.str();
        mz_zip_writer_add_mem(&zip, "F4_pack_info.txt", s.data(), s.size(), level);
        mz_zip_writer_finalize_archive(&zip);
    }
    mz_zip_writer_end(&zip);
    std::fclose(fp);
    if (aborted) {
        fs::remove(opt.out_zip, ec);
        return finish(false, "Cancelado");
    }
    std::ostringstream msg;
    msg << pr.added << " archivos (" << (done_bytes / (1024 * 1024)) << " MB sin comprimir), " << pr.excluded_steam
        << " originales/DLC excluidos, " << missing.size() << " faltantes";
    if (opt.exclude_steam && steam.files.empty()) msg << ". AVISO: sin datos de Steam, no se excluyo nada";
    finish(true, msg.str());
}

std::vector<fs::path> asset_closure(const fs::path& omsi_root, const fs::path& asset_file) {
    std::vector<fs::path> files;
    if (!exists_file(asset_file)) return files;
    const fs::path& root = omsi_root;
    std::unordered_set<std::string> visited;
    std::deque<fs::path> queue;
    auto add = [&](const fs::path& p) {
        const auto x = ext_of(utf8(p.filename()));
        if (x == ".wav" || x == ".ogg") return; // not needed to draw it
        const auto k = key_of(p);
        if (!visited.insert(k).second) return;
        files.push_back(p.lexically_normal());
        if (is_expandable_ext(x)) queue.push_back(p.lexically_normal());
    };
    auto add_texture = [&](const fs::path& owner, const std::string& name) {
        if (const auto r = gfx::dx12::resolve_texture_path(owner, name, root)) {
            add(*r);
            const fs::path tex = *r;
            if (exists_file(fs::path(tex.wstring() + L".cfg"))) add(fs::path(tex.wstring() + L".cfg"));
            std::error_code ec;
            for (const auto& sub : fs::directory_iterator(tex.parent_path(), ec)) {
                if (!sub.is_directory(ec)) continue;
                const auto v = sub.path() / tex.filename();
                if (exists_file(v)) add(v);
            }
        }
    };
    auto resolve = [&](const fs::path& owner, std::string tok) -> fs::path {
        for (char& c : tok)
            if (c == '/') c = '\\';
        const fs::path t = from_utf8(tok);
        const fs::path d = owner.parent_path();
        const fs::path cands[] = {d / t, d / "model" / t, d / "texture" / t, d / "script" / t,
                                  d.parent_path() / t, d.parent_path() / "texture" / t, root / t};
        for (const auto& c : cands)
            if (exists_file(c)) return c;
        return {};
    };
    auto add_token = [&](const fs::path& owner, const std::string& tok_in) {
        std::string tok = trim(tok_in);
        if (tok.size() >= 2 && tok.front() == '"' && tok.back() == '"') tok = tok.substr(1, tok.size() - 2);
        if (tok.empty() || tok.size() > 260) return;
        const auto x = ext_of(tok);
        if (!is_asset_ext(x) || x == ".bus" || x == ".ovh" || x == ".hum") return;
        if (is_texture_ext(x)) {
            add_texture(owner, tok);
            return;
        }
        if (const auto r = resolve(owner, tok); !r.empty()) add(r);
    };
    add(asset_file);
    std::size_t guard = 0;
    while (!queue.empty() && ++guard < 4000) {
        const fs::path f = queue.front();
        queue.pop_front();
        const auto x = ext_of(utf8(f.filename()));
        std::error_code ec;
        if (fs::file_size(f, ec) > 4u * 1024u * 1024u) continue;
        if (x == ".x") {
            for (const auto& line : cfg::read_cfg_lines(f)) {
                const auto q0 = line.find('"');
                const auto q1 = q0 == std::string::npos ? q0 : line.find('"', q0 + 1);
                if (q1 != std::string::npos) add_token(f, line.substr(q0 + 1, q1 - q0 - 1));
            }
            continue;
        }
        for (const auto& line : cfg::read_cfg_lines(f)) add_token(f, line);
    }
    const std::size_t n = files.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (ext_of(utf8(files[i].filename())) != ".o3d") continue;
        if (auto m = o3d::load_mapped(files[i]))
            for (const auto& mat : m->mesh().parse_materials())
                if (!mat.texture.empty()) add_texture(files[i], mat.texture);
    }
    return files;
}

} // namespace omsi::pack

