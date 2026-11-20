#include "omsi/options.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"
#include <vector>
#include <cwctype>
#include <algorithm>
#include "omsi/utf8_win32.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fstream>
#include <sstream>

namespace omsi::options {
namespace {

std::string first_line(const cfg::CfgDocument& doc, const char* section) {
    const std::string key(section);
    if (!doc.contains(key) || doc.at(key).empty() || doc.at(key)[0].empty()) return {};
    return doc.at(key)[0][0];
}

f4f23ma::f32 parse_f32(const std::string& s, const f4f23ma::f32 fallback) {
    try {
        return std::stof(s);
    } catch (...) {
        return fallback;
    }
}

std::size_t parse_size(const std::string& s, const std::size_t fallback) {
    try {
        return static_cast<std::size_t>(std::stoull(s));
    } catch (...) {
        return fallback;
    }
}

std::filesystem::path session_cfg_path(const std::filesystem::path& omsi_root) {
    return omsi_root / ".omsi_f4_engine.cfg";
}

void apply_last_map_string(OmsiOptions& opts, const std::string& last_map) {
    if (last_map.empty()) return;
    std::filesystem::path p = last_map;
    if (!p.is_absolute()) p = opts.omsi_root / p;
    opts.last_map_cfg = std::move(p);
}

} // namespace

OmsiOptions load_options(const std::filesystem::path& omsi_root) {
    OmsiOptions opts{};
    opts.omsi_root = omsi_root;

    const auto path = omsi_root / "options.cfg";
    const auto doc = cfg::load_cfg_file(path);
    if (!doc) {
        log::warn("options.cfg not found, using defaults: " + path.string());
    } else {
        apply_last_map_string(opts, first_line(*doc, "[last_map]"));
        opts.language = first_line(*doc, "[language]");
        opts.tile_dist_max = parse_f32(first_line(*doc, "[performance_tiledistmax]"), opts.tile_dist_max);
        opts.max_object_dist = parse_f32(first_line(*doc, "[performance_maxObjDist]"), opts.max_object_dist);
        opts.dyn_tile_red = parse_f32(first_line(*doc, "[performance_dyn_tile_red]"), opts.dyn_tile_red);
        opts.tex_max_256 = doc->contains("[texmax256]");
    }

    // Editor session overrides OMSI [last_map] and adds entrypoint memory.
    if (const auto session = cfg::load_cfg_file(session_cfg_path(omsi_root))) {
        const auto sess_map = first_line(*session, "[last_map]");
        if (!sess_map.empty()) apply_last_map_string(opts, sess_map);
        opts.last_entrypoint_index =
            parse_size(first_line(*session, "[last_entrypoint]"), opts.last_entrypoint_index);
    }

    log::info("options last_map=" + opts.last_map_cfg.string() +
              " last_entry=" + std::to_string(opts.last_entrypoint_index) +
              " tile_dist=" + std::to_string(opts.tile_dist_max) +
              " texmax256=" + std::string(opts.tex_max_256 ? "1" : "0"));
    return opts;
}

std::optional<std::filesystem::path> resolve_last_map(const OmsiOptions& opts) {
    if (opts.last_map_cfg.empty()) return std::nullopt;
    if (!std::filesystem::exists(opts.last_map_cfg)) return std::nullopt;
    return opts.last_map_cfg;
}

void save_editor_session(const std::filesystem::path& omsi_root, const std::filesystem::path& global_cfg,
                         const std::size_t entrypoint_index) {
    if (omsi_root.empty() || global_cfg.empty()) return;
    std::string rel = global_cfg.string();
    const auto root_s = omsi_root.lexically_normal().string();
    auto norm = global_cfg.lexically_normal().string();
    // Prefer path relative to omsi_root when possible.
    if (norm.size() > root_s.size()) {
        const auto prefix = root_s + "\\";
        const auto prefix2 = root_s + "/";
        if (norm.rfind(prefix, 0) == 0) rel = norm.substr(prefix.size());
        else if (norm.rfind(prefix2, 0) == 0) rel = norm.substr(prefix2.size());
        else {
            std::error_code ec;
            const auto rel_p = std::filesystem::relative(global_cfg, omsi_root, ec);
            if (!ec && !rel_p.empty() && !rel_p.string().starts_with("..")) rel = rel_p.string();
        }
    }
    const auto path = session_cfg_path(omsi_root);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        log::warn("could not write editor session: " + path.string());
        return;
    }
    out << "[last_map]\r\n" << rel << "\r\n\r\n";
    out << "[last_entrypoint]\r\n" << entrypoint_index << "\r\n";
    log::info("saved editor session map=" + rel + " entry=" + std::to_string(entrypoint_index));
}

std::filesystem::path engine_prefs_path() {
    wchar_t mod[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, mod, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::filesystem::path("omsi_f4_engine_prefs.cfg");
    return std::filesystem::path(mod).parent_path() / "omsi_f4_engine_prefs.cfg";
}

std::filesystem::path editor_ui_prefs_path() {
    wchar_t mod[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, mod, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::filesystem::path("omsi_f4_editor.cfg");
    return std::filesystem::path(mod).parent_path() / "omsi_f4_editor.cfg";
}

bool is_valid_omsi_root(const std::filesystem::path& omsi_root) {
    if (omsi_root.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::is_directory(omsi_root, ec)) return false;
    if (std::filesystem::exists(omsi_root / "Omsi.exe", ec)) return true;
    if (std::filesystem::is_directory(omsi_root / "maps", ec)) return true;
    return false;
}

namespace {
// omsi_f4_engine_prefs.cfg: [omsi_root] <path> and [omsi_recent] <path>... ; paths in UTF-8 (older files: ANSI,
// still read: utf8_to_wide falls back to the system code page)
struct EnginePrefs {
    std::filesystem::path root;
    std::vector<std::filesystem::path> recent;
};
EnginePrefs read_engine_prefs() {
    EnginePrefs p;
    std::ifstream in(engine_prefs_path(), std::ios::binary);
    if (!in) return p;
    std::string line, section;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);
        first = false;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line;
            continue;
        }
        const std::filesystem::path path(omsi::utf8_to_wide(line));
        if (section == "[omsi_root]" && p.root.empty()) p.root = path;
        else if (section == "[omsi_recent]") p.recent.push_back(path);
    }
    return p;
}
std::wstring path_key(const std::filesystem::path& p) {
    std::wstring k = p.lexically_normal().native();
    for (auto& c : k) c = static_cast<wchar_t>(towlower(c == L'/' ? L'\\' : c));
    while (k.size() > 3 && k.back() == L'\\') k.pop_back();
    return k;
}
} // namespace

std::optional<std::filesystem::path> load_saved_omsi_root() {
    const auto prefs = read_engine_prefs();
    if (prefs.root.empty()) return std::nullopt;
    if (!is_valid_omsi_root(prefs.root)) {
        log::warn("saved omsi_root invalid: " + omsi::path_text(prefs.root));
        return std::nullopt;
    }
    return prefs.root.lexically_normal();
}

std::vector<std::filesystem::path> load_recent_omsi_roots() {
    const auto prefs = read_engine_prefs();
    std::vector<std::filesystem::path> out;
    std::vector<std::wstring> seen;
    auto add = [&](const std::filesystem::path& p) {
        if (p.empty() || !is_valid_omsi_root(p)) return;
        const auto k = path_key(p);
        if (std::find(seen.begin(), seen.end(), k) != seen.end()) return;
        seen.push_back(k);
        out.push_back(p.lexically_normal());
    };
    add(prefs.root);
    for (const auto& r : prefs.recent) add(r);
    return out;
}

void save_omsi_root(const std::filesystem::path& omsi_root) {
    if (!is_valid_omsi_root(omsi_root)) return;
    auto prefs = read_engine_prefs();
    std::vector<std::filesystem::path> recent{omsi_root.lexically_normal()};
    std::vector<std::wstring> seen{path_key(omsi_root)};
    auto keep = [&](const std::filesystem::path& p) {
        if (p.empty()) return;
        const auto k = path_key(p);
        if (std::find(seen.begin(), seen.end(), k) != seen.end()) return;
        seen.push_back(k);
        recent.push_back(p.lexically_normal());
    };
    keep(prefs.root);
    for (const auto& r : prefs.recent) keep(r);
    if (recent.size() > 12) recent.resize(12);
    const auto path = engine_prefs_path();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        log::warn("could not write engine prefs: " + omsi::path_text(path));
        return;
    }
    out << "[omsi_root]\r\n" << omsi::wide_to_utf8(omsi_root.lexically_normal().native()) << "\r\n";
    out << "[omsi_recent]\r\n";
    for (const auto& r : recent) out << omsi::wide_to_utf8(r.native()) << "\r\n";
    log::info("saved omsi_root prefs: " + omsi::path_text(omsi_root) + " (recientes: " +
              std::to_string(recent.size()) + ")");
}

} // namespace omsi::options
