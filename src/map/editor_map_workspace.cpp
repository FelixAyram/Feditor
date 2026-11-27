#include "omsi/fs_cow.hpp"
#include "omsi/editor_map_workspace.hpp"

#include "editor/editor_ground.hpp"
#include "omsi/cfg_document.hpp"
#include "omsi/editor_omsi_tools.hpp"
#include "omsi/logger.hpp"
#include "omsi/map_geo_tools.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace omsi::map {

// Forward: defined below (must run on every tile/global write).
std::vector<std::string> ensure_map_file_version_14(std::vector<std::string> lines);

namespace {

bool ends_with_ci(const std::string& s, const std::string& suf) {
    if (s.size() < suf.size()) return false;
    for (std::size_t i = 0; i < suf.size(); ++i) {
        const char a = static_cast<char>(tolower(static_cast<unsigned char>(s[s.size() - suf.size() + i])));
        const char b = static_cast<char>(tolower(static_cast<unsigned char>(suf[i])));
        if (a != b) return false;
    }
    return true;
}

// Fast path: robocopy /MT (MS docs + SoftZone/windows-guru): parallel small-file copy + skip identical.
// Fallback: thread-pool CopyFileW, then sequential std::filesystem.
#ifdef _WIN32
bool robocopy_dir(const std::filesystem::path& src, const std::filesystem::path& dst, const bool mirror,
                  const WorkspaceProgressFn& progress, float progress_lo, float progress_hi,
                  const char* phase) {
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) return false;
    std::filesystem::create_directories(dst, ec);

    // /MT:16 — multithread (MS robocopy). /R:1 /W:1 — fail fast.
    // /NFL /NDL /NJH /NJS /NP — quiet (console I/O kills throughput on thousands of files).
    // /COPY:DAT — data+attrs+timestamps (enough for map tiles; skip ACL copy overhead).
    // Default robocopy skips identical size+time → incremental sync on reopen.
    // /MIR when destination must match source exactly (edit *_actual reset).
    std::wstring cmd = L"robocopy \"";
    cmd += src.wstring();
    cmd += L"\" \"";
    cmd += dst.wstring();
    cmd += L"\" ";
    cmd += mirror ? L"/MIR " : L"/E ";
    cmd += L"/MT:16 /R:1 /W:1 /COPY:DAT /DCOPY:DA /NFL /NDL /NJH /NJS /NP /NC /NS";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        log::warn("robocopy CreateProcess failed — falling back");
        return false;
    }
    CloseHandle(pi.hThread);
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const DWORD wait = WaitForSingleObject(pi.hProcess, 80);
        const auto elapsed =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
        if (progress) {
            // Pulse within band while robocopy runs (no per-file callbacks with /NFL).
            const float wave = 0.5f + 0.5f * std::sin(elapsed * 2.2f);
            const float frac = progress_lo + (progress_hi - progress_lo) * (0.15f + 0.75f * wave);
            progress(frac, std::string(phase) + " (robocopy /MT:16)…");
        }
        if (wait != WAIT_TIMEOUT) break;
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    // Robocopy: 0–7 = success with bit flags; >=8 = failure.
    if (code >= 8) {
        log::warn("robocopy exit=" + std::to_string(code) + " src=" + src.string());
        return false;
    }
    log::info("robocopy ok exit=" + std::to_string(code) + " " + std::string(phase));
    if (progress) progress(progress_hi, std::string(phase) + " — listo");
    return true;
}

bool robocopy_cmd(const std::wstring& cmd_suffix, const WorkspaceProgressFn& progress, float progress_lo,
                  float progress_hi, const char* phase) {
    std::wstring cmd = L"robocopy ";
    cmd += cmd_suffix;
    cmd += L" /MT:16 /R:1 /W:1 /COPY:DAT /DCOPY:DA /NFL /NDL /NJH /NJS /NP /NC /NS";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const DWORD wait = WaitForSingleObject(pi.hProcess, 80);
        if (progress) {
            const auto elapsed =
                std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
            const float wave = 0.5f + 0.5f * std::sin(elapsed * 2.2f);
            const float frac = progress_lo + (progress_hi - progress_lo) * (0.15f + 0.75f * wave);
            progress(frac, std::string(phase) + " (robocopy)…");
        }
        if (wait != WAIT_TIMEOUT) break;
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code < 8;
}
#endif

std::string ascii_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_omsi_map_workspace_root_file(const std::string& filename) {
    const auto l = ascii_lower(filename);
    if (l == "global.cfg" || l == ".omsi_f4_editor.cfg") return true;
    if (l == "humans.txt" || l == "drivers.txt" || l == "holidays.txt" || l == "timezone.txt") return true;
    if (l == "registrations.txt" || l == "signalroutes.cfg") return true;
    if (l == "unsched_vehgroups.txt" || l == "unsched_trafficdens.txt") return true;
    if (l.rfind("ailists", 0) == 0 && l.size() > 4 && l.substr(l.size() - 4) == ".cfg") return true;
    if (l.rfind("parklist_p", 0) == 0) return true;
    if (l.rfind("tile_", 0) == 0) {
        if (l.size() > 4 && l.substr(l.size() - 4) == ".map") return true;
        if (l.find(".map.") != std::string::npos) return true;
    }
    return false;
}

bool is_omsi_map_workspace_subdir(const std::string& dirname) {
    const auto l = ascii_lower(dirname);
    return l == "texture" || l == "ttdata";
}

void prune_map_workspace_root(const std::filesystem::path& map_dir) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(map_dir, ec)) {
        if (ec) break;
        const auto u8n = e.path().filename().u8string(); // string() throws on non-ANSI names
        const std::string name(u8n.begin(), u8n.end());
        if (e.is_directory()) {
            if (!is_omsi_map_workspace_subdir(name)) std::filesystem::remove_all(e.path(), ec);
            continue;
        }
        if (!is_omsi_map_workspace_root_file(name)) std::filesystem::remove(e.path(), ec);
    }
}

void copy_map_workspace_file(const std::filesystem::path& src, const std::filesystem::path& dst,
                             const bool overwrite) {
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
#ifdef _WIN32
    CopyFileW(src.wstring().c_str(), dst.wstring().c_str(), overwrite ? FALSE : TRUE);
#else
    const auto opts = overwrite ? std::filesystem::copy_options::overwrite_existing
                                : std::filesystem::copy_options::skip_existing;
    std::filesystem::copy_file(src, dst, opts, ec);
#endif
}

std::string ws_u8(const std::filesystem::path& p) {
    const auto u = p.u8string(); // (path::string() throws on non-ANSI names)
    return std::string(u.begin(), u.end());
}

// Copy-on-write workspace sync: every file the map needs is LINKED (hard link -> symbolic link ->
// copy, see omsi::fsutil). Unchanged files that already are the same file are skipped, so a big map
// opens almost at once and the copies take next to no disk space. Writers detach a file
// (omsi::fsutil::break_link) the first time they write it: the live map / snapshot never changes.
void sync_map_workspace_dir_fallback(const std::filesystem::path& src, const std::filesystem::path& dst,
                                   const bool mirror, const WorkspaceProgressFn& progress, float progress_lo,
                                   float progress_hi, const char* phase) {
    std::error_code ec;
    std::filesystem::create_directories(dst, ec);
    std::vector<std::filesystem::path> rels;
    for (const auto& e : std::filesystem::recursive_directory_iterator(src, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        const auto rel = std::filesystem::relative(e.path(), src, ec);
        if (ec || rel.empty()) continue;
        std::string gen = ws_u8(rel);
        for (char& c : gen)
            if (c == '\\') c = '/';
        const auto slash = gen.find('/');
        if (slash == std::string::npos) {
            if (!is_omsi_map_workspace_root_file(ws_u8(rel.filename()))) continue;
        } else {
            if (!is_omsi_map_workspace_subdir(gen.substr(0, slash))) continue;
        }
        rels.push_back(rel);
    }
    const std::size_t total = rels.size();
    std::size_t linked = 0;
    for (std::size_t i = 0; i < total; ++i) {
        if (omsi::fsutil::link_or_copy(src / rels[i], dst / rels[i])) ++linked;
        if (progress && total > 0 && ((i & 63) == 0 || i + 1 == total)) {
            const float t = static_cast<float>(i + 1) / static_cast<float>(total);
            progress(progress_lo + (progress_hi - progress_lo) * t,
                     std::string(phase) + " (" + std::to_string(i + 1) + "/" + std::to_string(total) + ", enlaces)");
        }
    }
    if (mirror) {
        prune_map_workspace_root(dst);
        // texture\ and TTData\: drop what the source no longer has (robocopy /MIR did the same)
        std::unordered_set<std::wstring> keep;
        for (const auto& r : rels) {
            std::wstring k = r.wstring();
            for (auto& c : k) c = static_cast<wchar_t>(towlower(c));
            keep.insert(k);
        }
        std::vector<std::filesystem::path> extra;
        for (const auto& e : std::filesystem::recursive_directory_iterator(dst, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            const auto rel = std::filesystem::relative(e.path(), dst, ec);
            if (ec || rel.empty() || !rel.has_parent_path()) continue; // root files: prune_map_workspace_root
            std::wstring k = rel.wstring();
            for (auto& c : k) c = static_cast<wchar_t>(towlower(c));
            if (!keep.count(k)) extra.push_back(e.path());
        }
        for (const auto& x : extra) std::filesystem::remove(x, ec);
    }
    log::info(std::string(phase) + ": " + std::to_string(linked) + "/" + std::to_string(total) +
              " archivos enlazados (copy-on-write) " + ws_u8(src) + " -> " + ws_u8(dst));
    if (progress) progress(progress_hi, std::string(phase) + " — listo");
}

void sync_map_workspace_dir(const std::filesystem::path& src, const std::filesystem::path& dst, const bool mirror,
                            const WorkspaceProgressFn& progress, float progress_lo, float progress_hi,
                            const char* phase) {
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) return;
    std::filesystem::create_directories(dst, ec);
    if (progress) progress(progress_lo, std::string(phase) + "…");
    // links instead of robocopy copies (robocopy_cmd kept for other callers)
    sync_map_workspace_dir_fallback(src, dst, mirror, progress, progress_lo, progress_hi, phase);
}

void copy_dir_recursive(const std::filesystem::path& src, const std::filesystem::path& dst,
                        const bool overwrite_existing = false, const WorkspaceProgressFn& progress = {},
                        float progress_lo = 0.f, float progress_hi = 1.f, const char* phase = "Copiando",
                        const bool mirror = false) {
    // OMSI map workspace: only files TMap.loadGlobalFile / tiles need (not whole folder /MIR).
    sync_map_workspace_dir(src, dst, mirror || overwrite_existing, progress, progress_lo, progress_hi, phase);
}

// Map-local .omsi_f4_editor.cfg holds [geo]/aerial keys. UI/vista live next to the exe
// (options::editor_ui_prefs_path). Still mirror geo cfg into *_actual after wipe.
void restore_editor_cfg_from_live(const std::filesystem::path& actual_dir,
                                  const std::filesystem::path& live_map_dir) {
    if (actual_dir.empty() || live_map_dir.empty() || actual_dir == live_map_dir) return;
    const auto src = live_map_dir / ".omsi_f4_editor.cfg";
    const auto dst = actual_dir / ".omsi_f4_editor.cfg";
    std::error_code ec;
    if (!std::filesystem::exists(src)) return;
    std::filesystem::create_directories(actual_dir, ec);
    omsi::fsutil::break_link(dst); // copy-on-write workspace: never write through a link
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec) log::info("editor geo cfg restored into actual from " + src.string());
}

std::string rel_omsi(const std::filesystem::path& abs, const std::filesystem::path& omsi_root) {
    std::error_code ec;
    auto rel = std::filesystem::relative(abs, omsi_root, ec);
    if (ec) return abs.generic_string();
    auto s = rel.generic_string();
    for (char& c : s)
        if (c == '/') c = '\\';
    return s;
}

std::string fmt_f(float v) {
    // OMSI Val() rejects "-0"; IEEE -0.f == 0.f so write a plain 0.
    if (v == 0.f) return "0";
    char buf[64]{};
    // OMSI stores full float precision; %.6g truncated values break spline chains / stations.
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    return buf;
}

bool map_file_is_utf16_le(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    unsigned char b[2]{};
    in.read(reinterpret_cast<char*>(b), 2);
    return in.gcount() == 2 && b[0] == 0xFF && b[1] == 0xFE;
}

bool global_cfg_prefers_utf16(const std::filesystem::path& map_dir) {
    if (map_dir.empty()) return true;
    const auto global_cfg = map_dir / "global.cfg";
    if (std::filesystem::exists(global_cfg) && map_file_is_utf16_le(global_cfg)) return true;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(map_dir, ec)) {
        if (ec || !e.is_regular_file()) continue;
        const auto fn = e.path().filename().string();
        if (fn.rfind("tile_", 0) != 0) continue;
        if (e.path().extension() != ".map") continue;
        if (map_file_is_utf16_le(e.path())) return true;
    }
    return false;
}

// OMSI Unit130 text reader stops only on CR (0x0D), then skips one more unit (LF).
// UTF-16 LE maps MUST use CRLF (0D 00 0A 00). LF-only files never split lines →
// [version] is never seen, TMap.version stays -1 (< 0x0B) → "previous OMSI version" dialog.
bool write_text_lines(const std::filesystem::path& path, const std::vector<std::string>& lines, bool utf16_le) {
    omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (utf16_le) {
        f.put(static_cast<char>(0xFF));
        f.put(static_cast<char>(0xFE));
        for (const auto& line : lines) {
            for (unsigned char c : line) {
                f.put(static_cast<char>(c));
                f.put('\0');
            }
            f.put('\r');
            f.put('\0');
            f.put('\n');
            f.put('\0');
        }
    } else {
        // ANSI / UTF-8 body: still CRLF so OMSI AssignFile/ReadLn path matches stock maps.
        for (std::size_t i = 0; i < lines.size(); ++i) {
            f.write(lines[i].data(), static_cast<std::streamsize>(lines[i].size()));
            if (i + 1 < lines.size()) {
                f.put('\r');
                f.put('\n');
            }
        }
    }
    return true;
}

bool ieq_tag(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

bool is_rewrite_entity_section(std::string_view tag) {
    if (ieq_tag(tag, "[object]") || ieq_tag(tag, "[spline]") || ieq_tag(tag, "[spline_h]") ||
        ieq_tag(tag, "[rule]") || ieq_tag(tag, "[kill_rule]") || ieq_tag(tag, "[attachObj]") ||
        ieq_tag(tag, "[varparent]") || ieq_tag(tag, "[selobject]") || ieq_tag(tag, "[selspline]"))
        return true;
    if (tag.size() >= 19) {
        std::string lower(tag);
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.rfind("[splineattachement", 0) == 0 || lower.rfind("[splineattachment", 0) == 0)
            return true;
    }
    return false;
}

bool is_object_nr_label(const std::string& line) {
    std::string t = line;
    while (!t.empty() && (t.back() == '\r' || t.back() == ' ' || t.back() == '\t')) t.pop_back();
    std::size_t i = 0;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
    if (t.size() - i < 9) return false;
    const char* pref = "object nr";
    for (std::size_t k = 0; k < 9; ++k) {
        const auto c = static_cast<unsigned char>(t[i + k]);
        if (std::tolower(c) != static_cast<unsigned char>(pref[k])) return false;
    }
    i += 9;
    if (i < t.size() && t[i] == '.') ++i;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
    if (i >= t.size()) return false;
    if (t[i] == '-' || t[i] == '+') ++i;
    if (i >= t.size() || t[i] < '0' || t[i] > '9') return false;
    for (; i < t.size(); ++i)
        if (t[i] < '0' || t[i] > '9') return false;
    return true;
}

bool is_section_header(const std::string& line, std::string& out_tag) {
    std::string t = line;
    while (!t.empty() && (t.back() == '\r' || t.back() == ' ' || t.back() == '\t')) t.pop_back();
    std::size_t i = 0;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
    if (i >= t.size() || t[i] != '[') return false;
    const auto end = t.find(']', i);
    if (end == std::string::npos) return false;
    out_tag = t.substr(i, end - i + 1);
    return true;
}

bool line_is_blank(const std::string& s) {
    for (char c : s) {
        if (c != ' ' && c != '\t' && c != '\r') return false;
    }
    return true;
}

// OMSI ReadLn does not skip blanks. "[version]\n\n14" reports "error in line 6".
// Stock tiles (Grundorf): value on the next line after each [tag]; at most one
// extra blank *between* sections, never between a tag and its first field.
// WARNING: strips ALL blank lines first — safe only for header/meta sections, NEVER for
// [object]/[attachObj]/[splineAttachement] bodies (empty stringvars are significant).
std::vector<std::string> compact_omsi_map_blanks(std::vector<std::string> lines) {
    std::vector<std::string> body;
    body.reserve(lines.size());
    for (auto& raw : lines) {
        if (!line_is_blank(raw)) body.push_back(std::move(raw));
    }
    std::vector<std::string> out;
    out.reserve(body.size() + 16);
    for (const auto& raw : body) {
        std::string hdr;
        const bool tag = is_section_header(raw, hdr);
        const bool nr = is_object_nr_label(raw);
        if (!out.empty()) {
            const bool prev_nr = is_object_nr_label(out.back());
            if (nr)
                out.emplace_back();
            else if (tag && !prev_nr)
                out.emplace_back();
        }
        out.push_back(raw);
    }
    return out;
}

std::vector<std::string> normalize_omsi_new_tile_map_lines(std::vector<std::string> lines) {
    lines = compact_omsi_map_blanks(std::move(lines));
    auto has_section = [&](const char* tag) {
        std::string t;
        for (const auto& line : lines) {
            if (is_section_header(line, t) && t == tag) return true;
        }
        return false;
    };
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (!has_section("[variable_terrainlightmap]")) {
        if (!lines.empty()) lines.emplace_back();
        lines.emplace_back("[variable_terrainlightmap]");
        lines.emplace_back();
    }
    if (!has_section("[variable_terrain]")) {
        lines.emplace_back("[variable_terrain]");
        lines.emplace_back();
    }
    return lines;
}

bool finalize_new_tile_map_file(const std::filesystem::path& tile_path, const std::filesystem::path& map_dir) {
    if (!std::filesystem::exists(tile_path)) return false;
    const bool utf16 = map_file_is_utf16_le(tile_path) || global_cfg_prefers_utf16(map_dir);
    auto lines = ensure_map_file_version_14(normalize_omsi_new_tile_map_lines(cfg::read_cfg_lines(tile_path)));
    if (lines.empty()) return false;
    if (!write_text_lines(tile_path, lines, utf16)) return false;
    if (utf16) log::info("tile .map UTF-16 LE: " + tile_path.filename().string());
    return true;
}

bool ensure_global_cfg_utf16_body(const std::filesystem::path& map_dir) {
    if (map_dir.empty()) return false;
    const auto global_cfg = map_dir / "global.cfg";
    if (!std::filesystem::exists(global_cfg)) return false;
    if (map_file_is_utf16_le(global_cfg)) return true;
    auto lines = ensure_map_file_version_14(cfg::read_cfg_lines(global_cfg));
    if (lines.empty()) return false;
    if (!write_text_lines(global_cfg, lines, true)) return false;
    log::info("global.cfg re-encoded UTF-16 LE: " + global_cfg.string());
    return true;
}

} // namespace

// OMSI TMap / TMapKachel: version < 0x0B triggers the "previous OMSI version" editor dialog
// (Unit126 loadGlobalFile / loadMapFile). Stock editor always saves version 0x0E (14).
// Keep [version] immediately followed by 14 — never a blank line in between (StrToInt fails).
constexpr int kOmsiMapFileVersion = 14;

std::vector<std::string> ensure_map_file_version_14(std::vector<std::string> lines) {
    std::size_t ver_i = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string hdr;
        if (is_section_header(lines[i], hdr) && (hdr == "[version]" || hdr == "version")) {
            ver_i = i;
            break;
        }
        // Accept exact tag without relying on is_section_header casing edge-cases.
        std::string s = lines[i];
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        std::string low;
        low.reserve(s.size());
        for (unsigned char c : s) low.push_back(static_cast<char>(std::tolower(c)));
        if (low == "[version]") {
            ver_i = i;
            break;
        }
    }
    const std::string ver_s = std::to_string(kOmsiMapFileVersion);
    if (ver_i == static_cast<std::size_t>(-1)) {
        // Insert after optional "File created..." header / leading blanks.
        std::size_t ins = 0;
        while (ins < lines.size() && line_is_blank(lines[ins])) ++ins;
        if (ins < lines.size() && lines[ins].rfind("File created", 0) == 0) {
            ++ins;
            while (ins < lines.size() && line_is_blank(lines[ins])) ++ins;
        }
        std::vector<std::string> out;
        out.reserve(lines.size() + 4);
        out.insert(out.end(), lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(ins));
        if (!out.empty() && !line_is_blank(out.back())) out.emplace_back();
        out.push_back("[version]");
        out.push_back(ver_s);
        out.emplace_back();
        out.insert(out.end(), lines.begin() + static_cast<std::ptrdiff_t>(ins), lines.end());
        return out;
    }
    // Ensure value is on the next line (no blanks between tag and value).
    std::size_t j = ver_i + 1;
    while (j < lines.size() && line_is_blank(lines[j])) ++j;
    if (j >= lines.size() || lines[j].empty() || lines[j][0] == '[') {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(ver_i + 1), ver_s);
    } else {
        // Replace whatever number is there with 14 if it is a pure integer version field.
        bool all_digit = true;
        for (char c : lines[j]) {
            if (c < '0' || c > '9') {
                all_digit = false;
                break;
            }
        }
        if (all_digit) lines[j] = ver_s;
        // Remove blank lines between [version] and value
        if (j > ver_i + 1) {
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(ver_i + 1),
                        lines.begin() + static_cast<std::ptrdiff_t>(j));
        }
    }
    return lines;
}


bool ensure_global_cfg_utf16(const std::filesystem::path& map_dir) {
    return ensure_global_cfg_utf16_body(map_dir);
}

std::optional<EditorMapWorkspace> ensure_editor_map_workspace(const std::filesystem::path& global_cfg,
                                                              WorkspaceProgressFn progress) {
    if (global_cfg.empty() || !std::filesystem::exists(global_cfg)) return std::nullopt;
    EditorMapWorkspace ws{};
    const auto map_dir = global_cfg.parent_path();
    const auto parent = map_dir.parent_path();
    const std::string name = map_dir.filename().string();
    if (progress) progress(0.02f, "Preparando copia de edición…");

    if (ends_with_ci(name, "_actual")) {
        ws.actual_dir = map_dir;
        ws.actual_global_cfg = global_cfg;
        const auto base = name.substr(0, name.size() - 7);
        ws.original_dir = parent / (base + "_original");
        if (!std::filesystem::exists(ws.original_dir)) {
            const auto legacy = parent / base;
            if (std::filesystem::exists(legacy))
                copy_dir_recursive(legacy, ws.original_dir, false, progress, 0.05f, 0.45f,
                                   "Creando *_original");
        }
        // Keep *_actual as-is (Ctrl+S writes here). Only refresh *_original from live.
        const auto live_map = parent / base;
        if (std::filesystem::exists(live_map / "global.cfg")) {
            copy_dir_recursive(live_map, ws.original_dir, true, progress, 0.20f, 0.50f,
                               "Sincronizando *_original", true);
            restore_editor_cfg_from_live(ws.actual_dir, live_map);
        }
        // Stay below 1.0 — caller closes the loading dialog after editor boot.
        if (progress) progress(0.54f, "Workspace listo");
        ensure_global_cfg_utf16(ws.actual_dir);
        log::info("editor workspace actual=" + ws.actual_dir.string());
        return ws;
    }
    if (ends_with_ci(name, "_original")) {
        const auto base = name.substr(0, name.size() - 9);
        ws.original_dir = map_dir;
        ws.actual_dir = parent / (base + "_actual");
        const auto live_map = parent / base;
        if (!std::filesystem::exists(ws.actual_dir))
            copy_dir_recursive(ws.original_dir, ws.actual_dir, true, progress, 0.1f, 0.52f,
                               "Copiando a *_actual");
        restore_editor_cfg_from_live(ws.actual_dir, live_map);
        ws.actual_global_cfg = ws.actual_dir / "global.cfg";
        if (progress) progress(0.54f, "Workspace listo");
        ensure_global_cfg_utf16(ws.actual_dir);
        log::info("editor workspace from original → actual=" + ws.actual_dir.string());
        return ws;
    }

    ws.original_dir = parent / (name + "_original");
    ws.actual_dir = parent / (name + "_actual");
    // Live map → *_original: robocopy /MIR skips identical files (fast reopen).
    copy_dir_recursive(map_dir, ws.original_dir, true, progress, 0.05f, 0.30f, "Sincronizando *_original",
                       true);
    // Reset *_actual to match *_original via /MIR (no wipe+full-copy — much faster when mostly same).
    if (progress) progress(0.32f, "Sincronizando *_actual…");
    copy_dir_recursive(ws.original_dir, ws.actual_dir, true, progress, 0.34f, 0.52f, "Sincronizando *_actual",
                       true);
    // Re-apply map-local geo cfg from live map (wiped with *_actual above).
    restore_editor_cfg_from_live(ws.actual_dir, map_dir);
    ws.actual_global_cfg = ws.actual_dir / "global.cfg";
    if (!std::filesystem::exists(ws.actual_global_cfg)) {
        log::warn("editor workspace missing global.cfg in actual");
        return std::nullopt;
    }
    if (progress) progress(0.54f, "Workspace listo");
    ensure_global_cfg_utf16(ws.actual_dir);
    log::info("editor workspace original=" + ws.original_dir.string() +
              " actual=" + ws.actual_dir.string());
    return ws;
}

std::filesystem::path workspace_global_cfg_for_read(const EditorMapWorkspace& ws) {
    if (!ws.actual_global_cfg.empty() && std::filesystem::exists(ws.actual_global_cfg))
        return ws.actual_global_cfg;
    return ws.original_dir / "global.cfg";
}

bool workspace_prepare_global_cfg_for_write(EditorMapWorkspace& ws) {
    if (ws.actual_dir.empty() || ws.original_dir.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(ws.actual_dir, ec);
    ws.actual_global_cfg = ws.actual_dir / "global.cfg";
    if (std::filesystem::exists(ws.actual_global_cfg)) {
        ensure_global_cfg_utf16(ws.actual_dir);
        return true;
    }
    const auto src = ws.original_dir / "global.cfg";
    if (!std::filesystem::exists(src)) {
        log::warn("workspace_prepare_global_cfg: missing live global.cfg");
        return false;
    }
    omsi::fsutil::break_link(ws.actual_global_cfg); // copy-on-write workspace: never write through a link
    std::filesystem::copy_file(src, ws.actual_global_cfg, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        log::warn("workspace_prepare_global_cfg copy failed: " + ec.message());
        return false;
    }
    ensure_global_cfg_utf16(ws.actual_dir);
    log::info("overlay COW global.cfg → " + ws.actual_global_cfg.string());
    return true;
}

namespace {

bool path_under(const std::filesystem::path& child, const std::filesystem::path& root, std::filesystem::path& rel_out) {
    std::error_code ec;
    if (child.empty() || root.empty()) return false;
    rel_out = std::filesystem::relative(child.lexically_normal(), root.lexically_normal(), ec);
    if (ec || rel_out.empty()) return false;
    if (rel_out.begin()->string() == "..") return false;
    return true;
}

} // namespace

std::filesystem::path workspace_overlay_write_path(const EditorMapWorkspace& ws,
                                                  const std::filesystem::path& map_file) {
    if (ws.actual_dir.empty()) return {};
    std::filesystem::path rel;
    if (path_under(map_file, ws.actual_dir, rel)) return (ws.actual_dir / rel).lexically_normal();
    if (!ws.original_dir.empty() && path_under(map_file, ws.original_dir, rel)) {
        const auto out = (ws.actual_dir / rel).lexically_normal();
        std::error_code ec;
        std::filesystem::create_directories(out.parent_path(), ec);
        return out;
    }
    return {};
}

std::filesystem::path live_map_dir_from_folder(const std::filesystem::path& map_dir) {
    if (map_dir.empty()) return {};
    std::string name = map_dir.filename().string();
    if (ends_with_ci(name, "_actual")) name = name.substr(0, name.size() - 7);
    else if (ends_with_ci(name, "_original")) name = name.substr(0, name.size() - 9);
    if (name.empty()) return map_dir;
    return map_dir.parent_path() / name;
}

std::filesystem::path workspace_live_map_dir(const EditorMapWorkspace& ws) {
    if (!ws.actual_dir.empty()) return live_map_dir_from_folder(ws.actual_dir);
    if (!ws.original_dir.empty()) return live_map_dir_from_folder(ws.original_dir);
    return {};
}

std::filesystem::path workspace_live_tile_path(const EditorMapWorkspace& ws,
                                               const std::filesystem::path& tile_path) {
    const auto live = workspace_live_map_dir(ws);
    if (live.empty() || tile_path.empty()) return {};
    std::filesystem::path rel;
    if (!ws.actual_dir.empty() && path_under(tile_path, ws.actual_dir, rel))
        return (live / rel).lexically_normal();
    if (!ws.original_dir.empty() && path_under(tile_path, ws.original_dir, rel))
        return (live / rel).lexically_normal();
    if (path_under(tile_path, live, rel)) return (live / rel).lexically_normal();
    return (live / tile_path.filename()).lexically_normal();
}

bool backup_file_timestamped_bak(const std::filesystem::path& src) {
    std::error_code ec;
    if (src.empty() || !std::filesystem::exists(src, ec)) return true;
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char ts[32]{};
    std::snprintf(ts, sizeof(ts), "%04d%02d%02d-%02d%02d%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    std::filesystem::path bak = src;
    bak += ".";
    bak += ts;
    bak += ".bak";
    omsi::fsutil::break_link(bak); // copy-on-write workspace: never write through a link
    std::filesystem::copy_file(src, bak, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        log::warn("bak copy failed: " + src.string() + " -> " + bak.string() + " " + ec.message());
        return false;
    }
    return true;
}

std::filesystem::path workspace_ensure_tile_write_path(const EditorMapWorkspace& ws,
                                                       const std::filesystem::path& logical_tile_path) {
    if (ws.actual_dir.empty()) return logical_tile_path;
    auto out = workspace_overlay_write_path(ws, logical_tile_path);
    if (out.empty()) return logical_tile_path;
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    if (!std::filesystem::exists(out, ec)) {
        // COW: copy original tile into *_actual so we never mutate the frozen snapshot.
        std::filesystem::path src = logical_tile_path;
        if (!ws.original_dir.empty()) {
            std::filesystem::path rel;
            // prefer original sibling of logical
            if (path_under(logical_tile_path, ws.actual_dir, rel)) {
                src = (ws.original_dir / rel).lexically_normal();
            } else if (path_under(logical_tile_path, ws.original_dir, rel)) {
                src = (ws.original_dir / rel).lexically_normal();
            } else {
                const auto live = live_map_dir_from_folder(ws.actual_dir);
                if (path_under(logical_tile_path, live, rel))
                    src = (ws.original_dir / rel).lexically_normal();
            }
        }
        if (std::filesystem::exists(src, ec)) {
            omsi::fsutil::break_link(out); // copy-on-write workspace: never write through a link
            std::filesystem::copy_file(src, out, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
                log::warn("COW tile copy failed: " + src.string() + " -> " + out.string() + " " + ec.message());
            else
                log::info("COW tile → " + out.string());
        }
    }
    return out;
}

std::filesystem::path workspace_tile_map_for_read(const EditorMapWorkspace* ws,
                                                 const std::filesystem::path& logical_tile_path) {
    if (!ws || ws->actual_dir.empty()) return logical_tile_path;
    const auto overlay = workspace_overlay_write_path(*ws, logical_tile_path);
    if (!overlay.empty() && std::filesystem::exists(overlay)) return overlay;
    return logical_tile_path;
}

void rebuild_default_objects_from_file_order(MapTileData& tile) {
    tile.objects_from_file_order.clear();
    tile.objects_from_file_order.reserve(tile.objects.size() + tile.attachments.size() +
                                        tile.spline_attachments.size());
    for (std::size_t i = 0; i < tile.objects.size(); ++i)
        tile.objects_from_file_order.push_back({MapObjectsFromFileKind::Object, i});
    for (std::size_t i = 0; i < tile.attachments.size(); ++i)
        tile.objects_from_file_order.push_back({MapObjectsFromFileKind::Attach, i});
    for (std::size_t i = 0; i < tile.spline_attachments.size(); ++i)
        tile.objects_from_file_order.push_back({MapObjectsFromFileKind::SplineAttach, i});
}

void remap_objects_from_file_order(MapTileData& tile, const MapTileData& before_sync) {
    auto find_object = [&](const std::int32_t id) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < tile.objects.size(); ++i)
            if (tile.objects[i].id == id) return i;
        return std::nullopt;
    };
    auto find_attach = [&](const std::int32_t child_id) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < tile.attachments.size(); ++i)
            if (tile.attachments[i].child_id == child_id) return i;
        return std::nullopt;
    };
    auto find_spline_attach = [&](const std::int32_t instance_id) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < tile.spline_attachments.size(); ++i)
            if (tile.spline_attachments[i].instance_id == instance_id) return i;
        return std::nullopt;
    };
    auto pack_key = [](const MapObjectsFromFileKind kind, const std::int32_t id) -> std::int64_t {
        return (static_cast<std::int64_t>(static_cast<int>(kind)) << 32) |
               static_cast<std::uint32_t>(id);
    };

    std::vector<MapObjectsFromFileRef> out;
    std::unordered_set<std::int64_t> seen;
    if (!before_sync.objects_from_file_order.empty()) {
        out.reserve(before_sync.objects_from_file_order.size() + tile.objects.size() +
                    tile.attachments.size() + tile.spline_attachments.size());
        for (const auto& ref : before_sync.objects_from_file_order) {
            std::optional<std::size_t> ni;
            std::int32_t key_id = 0;
            switch (ref.kind) {
            case MapObjectsFromFileKind::Object:
                if (ref.index >= before_sync.objects.size()) continue;
                key_id = before_sync.objects[ref.index].id;
                ni = find_object(key_id);
                break;
            case MapObjectsFromFileKind::Attach:
                if (ref.index >= before_sync.attachments.size()) continue;
                key_id = before_sync.attachments[ref.index].child_id;
                ni = find_attach(key_id);
                break;
            case MapObjectsFromFileKind::SplineAttach:
                if (ref.index >= before_sync.spline_attachments.size()) continue;
                key_id = before_sync.spline_attachments[ref.index].instance_id;
                ni = find_spline_attach(key_id);
                break;
            }
            if (!ni) continue;
            const auto key = pack_key(ref.kind, key_id);
            if (seen.count(key)) continue;
            seen.insert(key);
            out.push_back({ref.kind, *ni});
        }
    }
    auto append_new = [&](const MapObjectsFromFileKind kind, const std::int32_t id,
                          const std::size_t index) {
        const auto key = pack_key(kind, id);
        if (seen.count(key)) return;
        seen.insert(key);
        out.push_back({kind, index});
    };
    for (std::size_t i = 0; i < tile.objects.size(); ++i)
        append_new(MapObjectsFromFileKind::Object, tile.objects[i].id, i);
    for (std::size_t i = 0; i < tile.attachments.size(); ++i)
        append_new(MapObjectsFromFileKind::Attach, tile.attachments[i].child_id, i);
    for (std::size_t i = 0; i < tile.spline_attachments.size(); ++i)
        append_new(MapObjectsFromFileKind::SplineAttach, tile.spline_attachments[i].instance_id, i);
    if (out.empty())
        rebuild_default_objects_from_file_order(tile);
    else
        tile.objects_from_file_order = std::move(out);
}

bool write_tile_map_file(const std::filesystem::path& tile_map_path, const MapTileData& tile,
                         const std::filesystem::path& omsi_root,
                         const std::filesystem::path& read_fallback) {
    const auto source_for_kept =
        std::filesystem::exists(tile_map_path)
            ? tile_map_path
            : (!read_fallback.empty() && std::filesystem::exists(read_fallback) ? read_fallback
                                                                                : tile_map_path);
    const bool utf16_le =
        map_file_is_utf16_le(source_for_kept) || global_cfg_prefers_utf16(tile_map_path.parent_path());
    std::vector<std::string> kept;
    if (std::filesystem::exists(source_for_kept)) {
        const auto all_lines = cfg::read_cfg_lines(source_for_kept);
        std::string tag;
        bool skip = false;
        for (const auto& line : all_lines) {
            if (is_object_nr_label(line)) continue;
            if (is_section_header(line, tag)) {
                skip = is_rewrite_entity_section(tag);
                if (skip) continue;
            }
            if (skip) continue;
            kept.push_back(line);
        }
        kept = compact_omsi_map_blanks(std::move(kept));
    }

    std::vector<std::string> out;
    out.insert(out.end(), kept.begin(), kept.end());
    if (!kept.empty() && !kept.back().empty()) out.emplace_back();

    auto emit_rules = [&](const std::vector<PathRuleEntry>& rules) {
        auto ieq = [](std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }
            return true;
        };
        for (const auto& r : rules) {
            out.push_back(r.kill ? "[kill_rule]" : "[rule]");
            out.push_back(std::to_string(r.path_index));
            out.push_back(r.keyword);
            if (ieq(r.keyword, "speedlimit")) {
                out.push_back(fmt_f(r.value));
                out.push_back("0"); // OMSI always writes trailing int after speedlimit
            } else if (ieq(r.keyword, "priority")) {
                // SetRule case 6 stores a byte; clamp so OMSI does not BoundErr on >255.
                int pv = static_cast<int>(r.value + (r.value >= 0.f ? 0.5f : -0.5f));
                if (pv < 0) pv = 0;
                if (pv > 255) pv = 255;
                out.push_back(std::to_string(pv));
                out.push_back("0");
            } else if (ieq(r.keyword, "trafficdensity")) {
                // OMSI Unit126 write: value (float) then UVG group index (IntToStr).
                // Example: trafficdensity / 1.000 / 4  → density 1.0 for group index 4.
                out.push_back(fmt_f(r.value));
                out.push_back(std::to_string(r.extra));
            }
            out.emplace_back();
        }
    };

    auto emit_object = [&](const MapObjectInst& o) {
        out.push_back("[object]");
        out.push_back(std::to_string(o.flags));
        out.push_back(rel_omsi(o.sco_path, omsi_root));
        out.push_back(std::to_string(o.id));
        out.push_back(fmt_f(o.position.x));
        out.push_back(fmt_f(o.position.y));
        out.push_back(fmt_f(o.position.z));
        out.push_back(fmt_f(o.rotation_deg.x));
        out.push_back(fmt_f(o.rotation_deg.y));
        out.push_back(fmt_f(o.rotation_deg.z));
        // OMSI [object]: after the 3 rotations comes the label COUNT, then the labels. The type line
        // only exists for the tree form (4 + texture/height/aspect). Writing a "0" type for every object
        // made Omsi.exe read 0 labels and skip the real count + texts (street signs lost their names).
        if (o.obj_type == 4) out.push_back(std::to_string(o.obj_type));
        if (o.obj_type == 4) {
            out.push_back(o.tree_texture);
            out.push_back(fmt_f(o.tree_height));
            out.push_back(fmt_f(o.tree_aspect));
        }
        {
            // Trees: type-4 block already has tex/h/aspect — do not echo them again as strings.
            std::size_t skip = 0;
            if (o.obj_type == 4 && o.strings.size() >= 3) {
                const auto& s0 = o.strings[0];
                const auto dot = s0.rfind('.');
                if (dot != std::string::npos) {
                    std::string ext = s0.substr(dot);
                    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (ext == ".dds" || ext == ".png" || ext == ".bmp" || ext == ".tga" || ext == ".jpg" ||
                        ext == ".jpeg")
                        skip = 3;
                }
            }
            const int raw_n =
                o.string_line_count > 0 ? o.string_line_count : static_cast<int>(o.strings.size());
            const int n = (std::max)(0, raw_n - static_cast<int>(skip));
            out.push_back(std::to_string(n));
            for (int i = 0; i < n; ++i) {
                const std::size_t idx = skip + static_cast<std::size_t>(i);
                if (idx < o.strings.size()) out.push_back(o.strings[idx]);
                else out.push_back("");
            }
        }
        out.emplace_back();
        // OMSI [object] AlignTerrain → [spline_terrain_align] (cut only if .sco.hole exists at runtime).
        if (o.align_terrain) {
            out.push_back("[spline_terrain_align]");
            out.emplace_back();
        }
        emit_rules(o.path_rules);
        if (o.varparent_id != 0) {
            out.push_back("[varparent]");
            out.push_back(std::to_string(o.varparent_id));
            out.emplace_back();
        }
    };

    auto emit_attach = [&](const MapAttachObjectInst& a) {
        out.push_back("[attachObj]");
        out.push_back("0");
        out.push_back(rel_omsi(a.sco_path, omsi_root));
        out.push_back(std::to_string(a.child_id));
        out.push_back(std::to_string(a.parent_id));
        out.push_back(fmt_f(a.extra_rot_y));
        out.push_back(std::to_string(a.attach_point_index));
        out.push_back(fmt_f(a.extra_rot_x));
        out.push_back(fmt_f(a.extra_rot_y2));
        out.push_back(fmt_f(a.extra_rot_z));
        out.push_back(std::to_string(a.options_flag));
        if (a.options_flag != 0) {
            const int n = a.object_text_line_count > 0 ? a.object_text_line_count
                                                         : static_cast<int>(a.object_text_lines.size());
            if (n > 0) {
                for (int i = 0; i < n; ++i) {
                    if (i < static_cast<int>(a.object_text_lines.size()))
                        out.push_back(a.object_text_lines[static_cast<std::size_t>(i)]);
                    else
                        out.push_back("");
                }
            } else if (!a.label.empty()) {
                out.push_back(a.label);
            }
        }
        out.emplace_back();
        if (a.align_terrain) {
            out.push_back("[spline_terrain_align]");
            out.emplace_back();
        }
        if (a.varparent_id != 0) {
            out.push_back("[varparent]");
            out.push_back(std::to_string(a.varparent_id));
            out.emplace_back();
        }
    };

    auto emit_spline_attach = [&](const MapSplineAttachmentInst& sa) {
        out.push_back(sa.is_repeater ? "[splineAttachement_repeater]" : "[splineAttachement]");
        if (sa.is_repeater) {
            // 0 / head / head / sco / id / spline index (map_attachment.cpp parse_spline_attachment_at_tag_line)
            out.push_back("0");
            out.push_back(std::to_string(sa.repeater_head[0]));
            out.push_back(std::to_string(sa.repeater_head[1]));
            out.push_back(rel_omsi(sa.sco_path, omsi_root));
            out.push_back(std::to_string(sa.instance_id));
            out.push_back(std::to_string(sa.spline_index));
        } else {
            out.push_back("0");
            out.push_back(rel_omsi(sa.sco_path, omsi_root));
            out.push_back(std::to_string(sa.instance_id));
            out.push_back(std::to_string(sa.spline_index));
        }
        out.push_back(fmt_f(sa.offset_x));
        out.push_back(fmt_f(sa.offset_y));
        out.push_back(fmt_f(sa.offset_along));
        out.push_back(fmt_f(sa.rotation_y));
        out.push_back(fmt_f(sa.rotation_x));
        out.push_back(fmt_f(sa.rotation_z));
        out.push_back(fmt_f(sa.repeat_step));
        out.push_back(fmt_f(sa.repeat_range));
        out.push_back("0");
        {
            const int n = sa.object_text_line_count > 0 ? sa.object_text_line_count
                                                        : static_cast<int>(sa.object_text_lines.size());
            out.push_back(std::to_string(n));
            for (int i = 0; i < n; ++i) {
                if (i < static_cast<int>(sa.object_text_lines.size())) out.push_back(sa.object_text_lines[static_cast<std::size_t>(i)]);
                else out.push_back("");
            }
        }
        if (sa.align_terrain) {
            out.push_back("[spline_terrain_align]");
        }
        if (sa.varparent_id != 0) {
            out.push_back("[varparent]");
            out.push_back(std::to_string(sa.varparent_id));
        }
        out.emplace_back();
    };

    for (const auto& s : tile.splines) {
        out.push_back(s.is_spline_h ? "[spline_h]" : "[spline]");
        out.push_back(std::to_string(s.flags));
        out.push_back(rel_omsi(s.sli_path, omsi_root));
        out.push_back(std::to_string(s.id));
        out.push_back(std::to_string(s.prev_id));
        out.push_back(std::to_string(s.next_id));
        // File Y-up: x, height, y
        out.push_back(fmt_f(s.position.x));
        out.push_back(fmt_f(s.position.z));
        out.push_back(fmt_f(s.position.y));
        out.push_back(fmt_f(s.heading_deg));
        out.push_back(fmt_f(s.length));
        out.push_back(fmt_f(s.radius));
        out.push_back(fmt_f(s.start_grad));
        out.push_back(fmt_f(s.end_grad));
        if (s.is_spline_h) {
            out.push_back(fmt_f(s.delta_height));
            out.push_back(fmt_f(s.cant_start));
            out.push_back(fmt_f(s.cant_end));
            out.push_back(fmt_f(s.skew_start));
            out.push_back(fmt_f(s.skew_end));
        } else {
            out.push_back(fmt_f(s.cant_start));
            out.push_back(fmt_f(s.cant_end));
            out.push_back(fmt_f(s.skew_start));
            out.push_back(fmt_f(s.skew_end));
        }
        // Always emit local_length so [spline]/[spline_h] keep a fixed 13/14 float trailer
        // (validators pad missing zeros; OMSI treats absent as 0).
        out.push_back(fmt_f(s.local_length));
        if (s.mirror) out.push_back("mirror");
        out.emplace_back();
        if (s.terrain_align != 0) {
            out.push_back("[spline_terrain_align_2]");
            out.push_back(std::to_string(static_cast<int>(s.terrain_align)));
        }
        emit_rules(s.path_rules);
    }

    MapTileData tile_copy = tile;
    if (tile_copy.objects_from_file_order.empty() && !read_fallback.empty() &&
        std::filesystem::exists(read_fallback)) {
        if (const auto parsed = load_map_tile(read_fallback, omsi_root))
            remap_objects_from_file_order(tile_copy, *parsed);
    }
    if (tile_copy.objects_from_file_order.empty()) rebuild_default_objects_from_file_order(tile_copy);

    std::size_t obj_nr = 0;
    for (const auto& ref : tile_copy.objects_from_file_order) {
        out.push_back("Object Nr. " + std::to_string(obj_nr++));
        switch (ref.kind) {
        case MapObjectsFromFileKind::Object:
            if (ref.index < tile_copy.objects.size()) emit_object(tile_copy.objects[ref.index]);
            break;
        case MapObjectsFromFileKind::Attach:
            if (ref.index < tile_copy.attachments.size()) emit_attach(tile_copy.attachments[ref.index]);
            break;
        case MapObjectsFromFileKind::SplineAttach:
            if (ref.index < tile_copy.spline_attachments.size())
                emit_spline_attach(tile_copy.spline_attachments[ref.index]);
            break;
        }
    }

    {
        std::error_code ec;
        std::filesystem::create_directories(tile_map_path.parent_path(), ec);
        // Do NOT run compact_omsi_map_blanks on the full tile: it strips every blank line and
        // destroys empty stringvar slots after the count (OMSI still needs those N lines).
        // compact is only applied to non-entity "kept" header sections above.
        out = ensure_map_file_version_14(std::move(out));
        if (!write_text_lines(tile_map_path, out, utf16_le)) {
            log::warn("write_tile_map_file failed: " + tile_map_path.string());
            return false;
        }
    }
    invalidate_map_tile_cache(tile_map_path);
    log::info("wrote tile map " + tile_map_path.filename().string() + " objects=" +
              std::to_string(tile.objects.size()) + " splines=" + std::to_string(tile.splines.size()));
    return true;
}

int find_map_block_line(const std::filesystem::path& map_path, const std::int32_t id, const bool want_spline) {
    auto trim = [](std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        std::size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        return s.substr(i);
    };
    auto ieq = [](const std::string& a, const char* b) {
        if (a.size() != std::strlen(b)) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
            const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
            if (ca != cb) return false;
        }
        return true;
    };
    auto is_object_nr = [&](const std::string& t) {
        if (t.size() < 9) return false;
        const char* pref = "object nr";
        for (std::size_t k = 0; k < 9; ++k) {
            if (std::tolower(static_cast<unsigned char>(t[k])) != static_cast<unsigned char>(pref[k]))
                return false;
        }
        return true;
    };
    const auto lines = cfg::read_cfg_lines(map_path);
    if (lines.empty()) return -1;
    int lineno = 0;
    for (const auto& line : lines) {
        ++lineno;
        const std::string t = trim(line);
        int id_field = 0;
        if (want_spline) {
            if (ieq(t, "[spline]") || ieq(t, "[spline_h]")) id_field = 3;
        } else if (ieq(t, "[object]") || ieq(t, "[attachObj]") || ieq(t, "[attachobj]") ||
                   ieq(t, "[splineAttachement]") || ieq(t, "[splineAttachment]")) {
            id_field = 3;
        } else if (ieq(t, "[splineAttachement_repeater]") || ieq(t, "[splineAttachment_repeater]")) {
            id_field = 5;
        }
        if (id_field <= 0) continue;
        int seen = 0;
        for (int i = lineno; i < static_cast<int>(lines.size()); ++i) {
            const std::string body = trim(lines[static_cast<std::size_t>(i)]);
            if (body.empty() || is_object_nr(body)) continue;
            if (!body.empty() && body.front() == '[') break;
            ++seen;
            if (seen != id_field) continue;
            try {
                if (std::stoi(body) == id) return lineno;
            } catch (...) {
            }
            break;
        }
    }
    return -1;
}

CatalogScanResult scan_catalog_files(const std::filesystem::path& root, const char* ext,
                                     const std::size_t max_files) {
    CatalogScanResult result;
    std::error_code ec;
    if (root.empty() || !std::filesystem::exists(root, ec)) return result;
    const auto t0 = std::chrono::steady_clock::now();
    const auto opts = std::filesystem::directory_options::skip_permission_denied;
    // Prefer increment()-style iteration: skip_permission_denied on the constructor alone still
    // throws/stops on some locked junctions; increment clears and continues.
    std::filesystem::recursive_directory_iterator it(root, opts, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        ++result.entries_walked;
        std::error_code iec;
        if (!it->is_regular_file(iec) || iec) continue;
        const auto p = it->path();
        if (!ends_with_ci(p.extension().string(), ext)) continue;
        result.paths.push_back(p);
        if (max_files > 0 && result.paths.size() >= max_files) {
            result.truncated = true;
            break;
        }
    }
    std::sort(result.paths.begin(), result.paths.end());
    result.paths.erase(std::unique(result.paths.begin(), result.paths.end()), result.paths.end());
    result.walk_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (result.truncated)
        log::warn("scan_catalog_files capped at " + std::to_string(max_files) + " under " + root.string());
    return result;
}

bool delete_map_tile(GlobalMap& global, const std::int32_t grid_x, const std::int32_t grid_y) {
    auto it = std::find_if(global.tiles.begin(), global.tiles.end(), [&](const MapTileRef& t) {
        return t.grid_x == grid_x && t.grid_y == grid_y;
    });
    if (it == global.tiles.end()) return false;
    const auto tile_path = it->tile_path;
    const auto stem = tile_path.filename().string();
    // Rewrite global.cfg without this [map] block.
    {
        auto lines = cfg::read_cfg_lines(global.global_cfg_path);
        std::vector<std::string> out;
        out.reserve(lines.size());
        std::string section;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto& line = lines[i];
            if (!line.empty() && line.front() == '[' && line.back() == ']') {
                section = line;
                out.push_back(line);
                continue;
            }
            if (section == "[map]" && i + 2 < lines.size() && line == std::to_string(grid_x) &&
                lines[i + 1] == std::to_string(grid_y)) {
                // Skip gx, gy, filename (+ blank)
                i += 2;
                if (i + 1 < lines.size() && lines[i + 1].empty()) ++i;
                continue;
            }
            out.push_back(line);
        }
        const bool cfg_utf16 = map_file_is_utf16_le(global.global_cfg_path) ||
                               global_cfg_prefers_utf16(global.global_cfg_path.parent_path());
        out = ensure_map_file_version_14(std::move(out));
        if (!write_text_lines(global.global_cfg_path, out, cfg_utf16)) return false;
    }
    global.tiles.erase(it);
    std::error_code ec;
    std::filesystem::remove(tile_path, ec);
    std::filesystem::remove(std::filesystem::path(tile_path.string() + ".terrain"), ec);
    log::info("deleted tile " + stem);
    return true;
}

namespace {

std::string strip_edit_suffix(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    const char* suf = " - edit";
    const std::size_t n = 7;
    if (s.size() >= n) {
        bool match = true;
        for (std::size_t i = 0; i < n; ++i) {
            const char a = static_cast<char>(tolower(static_cast<unsigned char>(s[s.size() - n + i])));
            const char b = static_cast<char>(tolower(static_cast<unsigned char>(suf[i])));
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) s.resize(s.size() - n);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

std::string read_cfg_single(const std::filesystem::path& cfg, const char* tag) {
    if (const auto doc = cfg::load_cfg_file(cfg)) {
        if (doc->contains(tag) && !cfg::cfg_section(*doc, tag).empty())
            return cfg::cfg_section(*doc, tag)[0][0];
    }
    return {};
}

bool rewrite_cfg_name_fields(const std::filesystem::path& cfg, const std::string& name,
                             const std::string& friendly) {
    auto lines = cfg::read_cfg_lines(cfg);
    if (lines.empty()) return false;
    const bool utf16_le =
        map_file_is_utf16_le(cfg) || global_cfg_prefers_utf16(cfg.parent_path());
    auto replace_section = [&](const char* tag, const std::string& value) {
        for (std::size_t i = 0; i < lines.size(); ++i) {
            std::string t;
            if (!is_section_header(lines[i], t) || t != tag) continue;
            std::size_t j = i + 1;
            while (j < lines.size() && lines[j].empty()) ++j;
            if (j < lines.size()) {
                std::string ht;
                if (!is_section_header(lines[j], ht)) {
                    lines[j] = value;
                    return;
                }
            }
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(i + 1), value);
            return;
        }
        if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
        lines.push_back(tag);
        lines.push_back(value);
        lines.emplace_back();
    };
    replace_section("[name]", name);
    replace_section("[friendlyname]", friendly);
    lines = ensure_map_file_version_14(std::move(lines));
    return write_text_lines(cfg, lines, utf16_le);
}

} // namespace

bool set_global_map_names(const std::filesystem::path& global_cfg, const std::string& name,
                          const std::string& friendly) {
    if (global_cfg.empty()) return false;
    return rewrite_cfg_name_fields(global_cfg, name, friendly);
}

bool commit_editor_workspace(EditorMapWorkspace& ws) {
    if (ws.actual_dir.empty()) return false;
    if (!workspace_prepare_global_cfg_for_write(ws)) return false;
    if (ws.actual_global_cfg.empty() || !std::filesystem::exists(ws.actual_global_cfg)) return false;
    std::string base = read_cfg_single(ws.actual_global_cfg, "[name]");
    if (base.empty()) base = read_cfg_single(ws.actual_global_cfg, "[friendlyname]");
    if (base.empty() && !ws.original_dir.empty()) {
        const auto og = ws.original_dir / "global.cfg";
        base = read_cfg_single(og, "[name]");
        if (base.empty()) base = read_cfg_single(og, "[friendlyname]");
    }
    if (base.empty()) base = ws.actual_dir.filename().string();
    base = strip_edit_suffix(base);
    // Folder Foo_actual → display "Foo"
    if (ends_with_ci(base, "_actual")) base = base.substr(0, base.size() - 7);
    const std::string edited = base + " - edit";
    if (!rewrite_cfg_name_fields(ws.actual_global_cfg, edited, edited)) {
        log::warn("commit_editor_workspace: failed to rewrite names");
        return false;
    }
    log::info("commit editor workspace kept=" + ws.actual_dir.string() + " name=\"" + edited + "\"");
    return true;
}

bool discard_editor_workspace(const EditorMapWorkspace& ws) {
    if (ws.actual_dir.empty() || !std::filesystem::exists(ws.actual_dir)) return true;
    // Never delete original snapshot.
    if (!ws.original_dir.empty()) {
        std::error_code ec;
        if (std::filesystem::equivalent(ws.actual_dir, ws.original_dir, ec)) {
            log::warn("discard_editor_workspace refused: actual==original");
            return false;
        }
    }
    std::error_code ec;
    const auto n = std::filesystem::remove_all(ws.actual_dir, ec);
    if (ec) {
        log::warn("discard_editor_workspace failed: " + ec.message());
        return false;
    }
    log::info("discard editor workspace removed=" + ws.actual_dir.string() + " entries=" +
              std::to_string(static_cast<std::uint64_t>(n)));
    return true;
}

} // namespace omsi::map
