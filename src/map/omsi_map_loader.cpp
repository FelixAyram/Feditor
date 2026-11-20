#include "omsi/vfs_overlay.hpp"
#include "omsi/fs_cow.hpp"
#include <cwctype>
#include <unordered_set>
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include "omsi/map_types.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"
#include "omsi/map_attachment.hpp"
#include "omsi/map_tile_layout.hpp"
#include "omsi/utf8_win32.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>

namespace omsi::map {
namespace {

template <typename T>
T parse_value(const std::string& s) {
    try {
        if constexpr (std::is_same_v<T, std::int32_t>) return std::stoi(s);
        else if constexpr (std::is_same_v<T, f4f23ma::f32>) return std::stof(s);
        else return T{};
    } catch (...) {
        if constexpr (std::is_same_v<T, std::int32_t>) return 0;
        else if constexpr (std::is_same_v<T, f4f23ma::f32>) return 0.f;
        else return T{};
    }
}

std::vector<std::string> flatten_section(const cfg::CfgSection& sec) {
    std::vector<std::string> out;
    for (const auto& inst : sec)
        for (const auto& line : inst) out.push_back(line);
    return out;
}

void discover_tiles_from_disk(GlobalMap& map) {
    if (!map.tiles.empty()) return;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(map.root, ec)) {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        if (name.rfind("tile_", 0) != 0) continue;
        const auto stem = entry.path().stem().string();
        const auto coords = stem.substr(5);
        const auto us = coords.find('_');
        if (us == std::string::npos) continue;
        MapTileRef ref{};
        ref.grid_x = std::stoi(coords.substr(0, us));
        ref.grid_y = std::stoi(coords.substr(us + 1));
        ref.tile_path = entry.path();
        map.tiles.push_back(std::move(ref));
    }
}

struct TileParseCacheEntry {
    std::filesystem::file_time_type mtime{};
    std::uintmax_t size = 0;
    MapTileData tile;
};
std::mutex g_tile_parse_mu;
std::unordered_map<std::string, TileParseCacheEntry> g_tile_parse_cache;

std::string tile_parse_key(const std::filesystem::path& tile_map, const std::filesystem::path& omsi_root) {
    auto s = tile_map.lexically_normal().string() + "|" + omsi_root.lexically_normal().string();
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

void invalidate_map_tile_cache(const std::filesystem::path& tile_map) {
    std::lock_guard lock(g_tile_parse_mu);
    if (tile_map.empty()) {
        g_tile_parse_cache.clear();
        return;
    }
    auto needle = tile_map.lexically_normal().string();
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    needle += '|';
    for (auto it = g_tile_parse_cache.begin(); it != g_tile_parse_cache.end();) {
        if (it->first.rfind(needle, 0) == 0)
            it = g_tile_parse_cache.erase(it);
        else
            ++it;
    }
}

std::vector<std::filesystem::path> discover_maps(const std::filesystem::path& maps_dir) {
    std::vector<std::filesystem::path> maps;
    if (!std::filesystem::is_directory(maps_dir)) return maps;
    for (const auto& entry : std::filesystem::directory_iterator(maps_dir)) {
        if (!entry.is_directory()) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() >= 9 && _stricmp(name.c_str() + name.size() - 9, "_original") == 0) continue;
        if (name.size() >= 7 && _stricmp(name.c_str() + name.size() - 7, "_actual") == 0) continue;
        const auto global_cfg = entry.path() / "global.cfg";
        if (omsi::vfs::exists(global_cfg)) maps.push_back(global_cfg);
    }
    std::sort(maps.begin(), maps.end());
    return maps;
}

std::optional<GlobalMap> load_global_map(const std::filesystem::path& global_cfg) {
    const auto doc = cfg::load_cfg_file(global_cfg);
    if (!doc) return std::nullopt;

    GlobalMap map{};
    map.root = global_cfg.parent_path();
    map.global_cfg_path = global_cfg;

    if (doc->contains("[name]") && !cfg::cfg_section(*doc, "[name]").empty())
        map.name = cfg::cfg_section(*doc, "[name]")[0][0];
    if (doc->contains("[friendlyname]") && !cfg::cfg_section(*doc, "[friendlyname]").empty())
        map.friendly_name = cfg::cfg_section(*doc, "[friendlyname]")[0][0];

    map.world_coordinates = doc->contains("[worldcoordinates]");

    if (doc->contains("[groundtex]")) {
        for (const auto& inst : cfg::cfg_section(*doc, "[groundtex]")) {
            if (inst.empty()) continue;
            GroundTex tex{};
            tex.diffuse = inst[0];
            if (inst.size() > 1) tex.detail = inst[1];
            // Blender: scale = 1 / float(tex[3]); last meaningful size field is often index 4.
            if (inst.size() > 2) tex.flag = std::atoi(inst[2].c_str());
            if (inst.size() > 4) tex.main_rep = (std::max)(0, std::atoi(inst[3].c_str()));
            if (inst.size() > 4) tex.scale = parse_value<f4f23ma::f32>(inst[4]);
            else if (inst.size() > 3) tex.scale = parse_value<f4f23ma::f32>(inst[3]);
            if (tex.scale < 0.01f) tex.scale = 60.f;
            map.ground_textures.push_back(std::move(tex));
        }
        if (!map.ground_textures.empty()) {
            log::info("groundtex[0]=" + map.ground_textures.front().diffuse.string() +
                      " scale=" + std::to_string(map.ground_textures.front().scale));
        }
    }

    if (doc->contains("[mapcam]")) {
        const auto lines = flatten_section(cfg::cfg_section(*doc, "[mapcam]"));
        if (lines.size() >= 8) {
            map.mapcam.valid = true;
            map.mapcam.grid_x = parse_value<std::int32_t>(lines[0]);
            map.mapcam.grid_y = parse_value<std::int32_t>(lines[1]);
            // File order is OMSI engine Y-up: (x, height, y). Store Z-up: (x, y, height).
            const auto fx = parse_value<f4f23ma::f32>(lines[2]);
            const auto fy_up = parse_value<f4f23ma::f32>(lines[3]);
            const auto fz = parse_value<f4f23ma::f32>(lines[4]);
            map.mapcam.position.x = fx;
            map.mapcam.position.y = fz;
            map.mapcam.position.z = fy_up;
            map.mapcam.yaw_deg = parse_value<f4f23ma::f32>(lines[5]);
            map.mapcam.pitch_deg = parse_value<f4f23ma::f32>(lines[6]);
            map.mapcam.distance = parse_value<f4f23ma::f32>(lines[7]);
            log::info("mapcam tile=(" + std::to_string(map.mapcam.grid_x) + "," +
                      std::to_string(map.mapcam.grid_y) + ") zup=(" +
                      std::to_string(map.mapcam.position.x) + "," + std::to_string(map.mapcam.position.y) + "," +
                      std::to_string(map.mapcam.position.z) + ") yaw=" + std::to_string(map.mapcam.yaw_deg) +
                      " pitch=" + std::to_string(map.mapcam.pitch_deg) +
                      " dist=" + std::to_string(map.mapcam.distance));
        }
    }

    // TMap.loadGlobalFile: [entrypoints] — see omsi-tt-viewer map_processor.js + Test_Lat30 fixture.
    // Classic maps store world position in cfg; [worldcoordinates] maps use tile-local + category index.
    // Keep blank name lines (cfg_document skips empties).
    {
        const auto raw = cfg::read_cfg_lines(global_cfg);
        std::string section;
        std::vector<std::string> ep_lines;
        for (auto line : raw) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && line.front() == '[' && line.back() == ']') {
                if (section == "[entrypoints]") break;
                section = line;
                continue;
            }
            if (section == "[entrypoints]") ep_lines.push_back(line);
        }
        if (!ep_lines.empty()) {
            std::size_t i = 0;
            while (i < ep_lines.size() && ep_lines[i].empty()) ++i;
            if (i < ep_lines.size()) {
                const int count = parse_value<std::int32_t>(ep_lines[i++]);
                map.entrypoints.reserve(static_cast<std::size_t>(count));
                for (int n = 0; n < count && i + 11 < ep_lines.size(); ++n) {
                    EntryPoint ep{};
                    ep.type = parse_value<std::int32_t>(ep_lines[i++]);
                    ep.id = parse_value<std::int32_t>(ep_lines[i++]);
                    ep.link_id = parse_value<std::int32_t>(ep_lines[i++]);
                    // global.cfg entrypoints: engine Y-up (x, height, y) → Z-up (x, y, height).
                    // .map [object] uses Z-up (x, y, height) already — do not remap those.
                    const auto fx = parse_value<f4f23ma::f32>(ep_lines[i++]);
                    const auto fy_up = parse_value<f4f23ma::f32>(ep_lines[i++]);
                    const auto fz = parse_value<f4f23ma::f32>(ep_lines[i++]);
                    ep.position_local.x = fx;
                    ep.position_local.y = fz;
                    ep.position_local.z = fy_up;
                    ep.rotation_quat.x = parse_value<f4f23ma::f32>(ep_lines[i++]);
                    ep.rotation_quat.y = parse_value<f4f23ma::f32>(ep_lines[i++]);
                    ep.rotation_quat.z = parse_value<f4f23ma::f32>(ep_lines[i++]);
                    ep.rotation_quat.w = parse_value<f4f23ma::f32>(ep_lines[i++]);
                    ep.category = parse_value<std::int32_t>(ep_lines[i++]);
                    ep.name = ep_lines[i++];
                    map.entrypoints.push_back(std::move(ep));
                }
            }
        }
    }

  // [map] tile_x tile_y tile_path.map
    if (doc->contains("[map]")) {
        for (const auto& inst : cfg::cfg_section(*doc, "[map]")) {
            if (inst.size() < 3) continue;
            MapTileRef ref{};
            ref.grid_x = parse_value<std::int32_t>(inst[0]);
            ref.grid_y = parse_value<std::int32_t>(inst[1]);
            ref.tile_path = map.root / inst[2];
            map.tiles.push_back(std::move(ref));
        }
    }

    discover_tiles_from_disk(map);
    resolve_entry_world_positions(map);
    if (!map.entrypoints.empty()) {
        const auto& ep0 = map.entrypoints.front();
        log::info("entry[0] local=(" + std::to_string(ep0.position_local.x) + "," +
                  std::to_string(ep0.position_local.y) + "," + std::to_string(ep0.position_local.z) +
                  ") world=(" + std::to_string(ep0.position.x) + "," + std::to_string(ep0.position.y) + "," +
                  std::to_string(ep0.position.z) + ") category=" + std::to_string(ep0.category));
    }
    {
        const auto ug = map.root / "unsched_vehgroups.txt";
        const auto lines = cfg::read_cfg_lines(ug);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            auto t = lines[i];
            while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) t.pop_back();
            if (t != "[group]") continue;
            GlobalMap::UnschedVehGroup g{};
            if (i + 1 < lines.size()) {
                g.name = lines[i + 1];
                while (!g.name.empty() && (g.name.back() == '\r' || g.name.back() == ' ')) g.name.pop_back();
            }
            if (i + 2 < lines.size()) g.path_mode = parse_value<std::int32_t>(lines[i + 2]);
            if (!g.name.empty()) map.unsched_vehgroups.push_back(std::move(g));
        }
        const auto td = map.root / "unsched_trafficdens.txt";
        const auto td_lines = cfg::read_cfg_lines(td);
        GlobalMap::UnschedTrafficDens* cur_td = nullptr;
        for (std::size_t i = 0; i < td_lines.size(); ++i) {
            auto t = td_lines[i];
            while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) t.pop_back();
            if (t == "[group]") {
                GlobalMap::UnschedTrafficDens d{};
                if (i + 1 < td_lines.size()) {
                    d.group_name = td_lines[i + 1];
                    while (!d.group_name.empty() && (d.group_name.back() == '\r' || d.group_name.back() == ' '))
                        d.group_name.pop_back();
                }
                if (i + 2 < td_lines.size()) d.factor = parse_value<f4f23ma::f32>(td_lines[i + 2]);
                map.unsched_trafficdens.push_back(std::move(d));
                cur_td = &map.unsched_trafficdens.back();
                i += 2;
                continue;
            }
            if (!cur_td) continue;
            if (t == "[set_day_of_week]" && i + 1 < td_lines.size()) {
                cur_td->day_mask = parse_value<std::int32_t>(td_lines[i + 1]);
                ++i;
            } else if (t == "[trafficdensity]" && i + 2 < td_lines.size()) {
                (void)parse_value<f4f23ma::f32>(td_lines[i + 1]);
                cur_td->hour_values.push_back(parse_value<f4f23ma::f32>(td_lines[i + 2]));
                i += 2;
            }
        }
    }
    return map;
}

namespace {
std::mutex g_chrono_mu;
std::vector<std::filesystem::path> g_chrono_dirs;

std::string chrono_trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}
bool chrono_ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}
std::wstring chrono_upper(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towupper(c));
    return s;
}
// depth first, a folder's subfolders before its own Chrono.cfg, names as Windows sorts them (no case)
void collect_chrono_cfgs(const std::filesystem::path& dir, std::vector<std::filesystem::path>& out) {
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> entries;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) entries.push_back(e);
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return chrono_upper(a.path().filename().wstring()) < chrono_upper(b.path().filename().wstring());
    });
    for (const auto& e : entries)
        if (e.is_directory(ec)) collect_chrono_cfgs(e.path(), out);
    for (const auto& e : entries)
        if (!e.is_directory(ec) && chrono_upper(e.path().filename().wstring()) == L"CHRONO.CFG") out.push_back(e.path());
}

struct ChronoChange {
    bool is_spline = false;
    std::int32_t id = 0;
    bool del = false;
    std::string new_type;
    bool has_relabel = false;
    std::vector<std::string> relabel;
    std::vector<PathRuleEntry> rules;
};

std::vector<ChronoChange> parse_chrono_changes(const std::filesystem::path& file) {
    std::vector<ChronoChange> out;
    const auto lines = cfg::read_cfg_lines(file);
    bool on_sel = false; // the last record keyword was a [selobject] / [selspline]
    auto next_tok = [&](std::size_t& i) -> std::string {
        while (i < lines.size()) {
            auto t = chrono_trim(lines[i++]);
            if (t.empty()) continue;
            if (t.front() == '[') {
                --i;
                return {};
            }
            return t;
        }
        return {};
    };
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto line = chrono_trim(lines[i]);
        if (line.empty() || line.front() != '[') continue;
        std::size_t j = i + 1;
        if (chrono_ieq(line, "[selobject]") || chrono_ieq(line, "[selspline]")) {
            const auto id_s = next_tok(j);
            if (id_s.empty()) continue;
            ChronoChange c;
            c.is_spline = chrono_ieq(line, "[selspline]");
            try {
                c.id = std::stoi(id_s);
            } catch (...) {
                continue;
            }
            out.push_back(std::move(c));
            on_sel = true;
        } else if (chrono_ieq(line, "[object]") || chrono_ieq(line, "[spline]") || chrono_ieq(line, "[spline_h]") ||
                   chrono_ieq(line, "[attachObj]") || chrono_ieq(line, "[splineAttachement]") ||
                   chrono_ieq(line, "[splineAttachement_repeater]")) {
            on_sel = false;
        } else if (!on_sel || out.empty()) {
            continue;
        } else if (chrono_ieq(line, "[delete]")) {
            out.back().del = true;
        } else if (chrono_ieq(line, "[typ]")) {
            out.back().new_type = next_tok(j);
        } else if (chrono_ieq(line, "[relabel]")) {
            const auto n_s = next_tok(j);
            int n = 0;
            try {
                n = std::clamp(std::stoi(n_s), 0, 256);
            } catch (...) {
            }
            out.back().has_relabel = true;
            out.back().relabel.clear();
            for (int k = 0; k < n && j < lines.size(); ++k) out.back().relabel.push_back(chrono_trim(lines[j++]));
        } else if (chrono_ieq(line, "[rule]") || chrono_ieq(line, "[kill_rule]")) {
            const auto pi_s = next_tok(j);
            const auto kw = next_tok(j);
            if (pi_s.empty() || kw.empty()) continue;
            PathRuleEntry r{};
            r.kill = chrono_ieq(line, "[kill_rule]");
            try {
                r.path_index = std::stoi(pi_s);
            } catch (...) {
                continue;
            }
            r.keyword = kw;
            try {
                if (chrono_ieq(kw, "speedlimit") || chrono_ieq(kw, "priority")) {
                    const auto v = next_tok(j);
                    r.value = v.empty() ? 0.f : std::stof(v);
                } else if (chrono_ieq(kw, "trafficdensity")) {
                    const auto v = next_tok(j);
                    const auto g = next_tok(j);
                    r.value = v.empty() ? 0.f : std::stof(v);
                    r.extra = g.empty() ? 0 : std::stoi(g);
                }
            } catch (...) {
            }
            out.back().rules.push_back(std::move(r));
        }
    }
    return out;
}

void apply_chrono_rules(std::vector<PathRuleEntry>& rules, const std::vector<PathRuleEntry>& changes) {
    for (const auto& c : changes) {
        if (c.kill) {
            rules.erase(std::remove_if(rules.begin(), rules.end(),
                                       [&](const PathRuleEntry& r) {
                                           return !r.kill && r.path_index == c.path_index &&
                                                  chrono_ieq(r.keyword, c.keyword) && r.extra == c.extra;
                                       }),
                        rules.end());
        } else {
            rules.push_back(c);
        }
    }
}
} // namespace

std::vector<ChronoScenario> list_chrono_scenarios(const std::filesystem::path& map_root) {
    std::vector<ChronoScenario> out;
    std::filesystem::path chrono_dir = map_root / "Chrono";
    std::error_code ec;
    if (!std::filesystem::is_directory(chrono_dir, ec)) {
        for (const auto& e : std::filesystem::directory_iterator(map_root, ec))
            if (e.is_directory(ec) && chrono_ieq(e.path().filename().string(), "Chrono")) {
                chrono_dir = e.path();
                break;
            }
    }
    std::vector<std::filesystem::path> cfgs;
    collect_chrono_cfgs(chrono_dir, cfgs);
    for (const auto& c : cfgs) {
        ChronoScenario sc;
        sc.dir = c.parent_path();
        sc.name = sc.dir.filename().string();
        const auto lines = cfg::read_cfg_lines(c);
        for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
            const auto t = chrono_trim(lines[i]);
            auto val = [&]() {
                for (std::size_t j = i + 1; j < lines.size(); ++j) {
                    const auto v = chrono_trim(lines[j]);
                    if (v.empty()) continue;
                    try {
                        return std::stoi(v);
                    } catch (...) {
                        return 0;
                    }
                }
                return 0;
            };
            if (chrono_ieq(t, "[startdate]")) sc.start_date = val();
            else if (chrono_ieq(t, "[enddate]")) sc.end_date = val();
        }
        out.push_back(std::move(sc));
    }
    return out;
}

void set_active_chrono_dirs(std::vector<std::filesystem::path> dirs) {
    {
        std::lock_guard lk(g_chrono_mu);
        if (dirs == g_chrono_dirs) return;
        g_chrono_dirs = std::move(dirs);
    }
    invalidate_map_tile_cache();
}

std::vector<std::filesystem::path> active_chrono_dirs() {
    std::lock_guard lk(g_chrono_mu);
    return g_chrono_dirs;
}

namespace {
std::optional<MapTileData> load_map_tile_base(const std::filesystem::path& tile_map,
                                              const std::filesystem::path& omsi_root);
}

std::optional<MapTileData> load_map_tile(const std::filesystem::path& tile_map,
                                         const std::filesystem::path& omsi_root) {
    auto tile = load_map_tile_base(tile_map, omsi_root);
    const auto dirs = active_chrono_dirs();
    if (!tile || dirs.empty()) return tile;
    const auto fname = tile_map.filename();
    for (const auto& d : dirs) {
        const auto patch_path = d / fname;
        if (!omsi::vfs::exists(patch_path)) continue;
        const auto patch = load_map_tile_base(patch_path, omsi_root);
        if (!patch) continue;
        auto& t = *tile;
        const std::size_t n_obj = t.objects.size(), n_att = t.attachments.size(), n_sa = t.spline_attachments.size();
        const std::int32_t n_spl = static_cast<std::int32_t>(t.splines.size());
        t.splines.insert(t.splines.end(), patch->splines.begin(), patch->splines.end());
        t.objects.insert(t.objects.end(), patch->objects.begin(), patch->objects.end());
        t.attachments.insert(t.attachments.end(), patch->attachments.begin(), patch->attachments.end());
        for (auto sa : patch->spline_attachments) {
            sa.spline_index += n_spl; // (counts its spline in the combined list)
            t.spline_attachments.push_back(std::move(sa));
        }
        for (auto ref : patch->objects_from_file_order) {
            ref.index += ref.kind == MapObjectsFromFileKind::Object   ? n_obj
                         : ref.kind == MapObjectsFromFileKind::Attach ? n_att
                                                                      : n_sa;
            t.objects_from_file_order.push_back(ref);
        }
        t.splines_from_file_order.insert(t.splines_from_file_order.end(), patch->splines_from_file_order.begin(),
                                         patch->splines_from_file_order.end());
        std::unordered_set<std::int32_t> deleted;
        for (const auto& c : parse_chrono_changes(patch_path)) {
            if (c.is_spline) {
                for (auto& sp : t.splines) {
                    if (sp.id != c.id) continue;
                    if (!c.new_type.empty()) sp.sli_path = omsi::join_omsi_path(omsi_root, c.new_type);
                    apply_chrono_rules(sp.path_rules, c.rules);
                    if (c.del) sp.sli_path.clear(); // (kept in the list: attachments index splines by position)
                }
                continue;
            }
            for (auto& o : t.objects) {
                if (o.id != c.id) continue;
                if (!c.new_type.empty())
                    o.sco_path = omsi::join_omsi_path(omsi_root, omsi::repair_omsi_path_mojibake(c.new_type));
                if (c.has_relabel) {
                    o.strings = c.relabel;
                    o.string_line_count = static_cast<std::int32_t>(c.relabel.size());
                }
                apply_chrono_rules(o.path_rules, c.rules);
            }
            for (auto& a : t.attachments) {
                if (a.child_id != c.id) continue;
                if (!c.new_type.empty())
                    a.sco_path = omsi::join_omsi_path(omsi_root, omsi::repair_omsi_path_mojibake(c.new_type));
            }
            for (auto& sa : t.spline_attachments) {
                if (sa.instance_id != c.id) continue;
                if (!c.new_type.empty())
                    sa.sco_path = omsi::join_omsi_path(omsi_root, omsi::repair_omsi_path_mojibake(c.new_type));
                if (c.has_relabel) {
                    sa.object_text_lines = c.relabel;
                    sa.object_text_line_count = static_cast<std::int32_t>(c.relabel.size());
                }
            }
            if (c.del) deleted.insert(c.id);
        }
        if (!deleted.empty()) {
            // (objects_from_file_order indexes stay valid: deleted entries are moved far away, not erased)
            for (auto& o : t.objects)
                if (deleted.count(o.id)) o.sco_path.clear();
            for (auto& a : t.attachments)
                if (deleted.count(a.child_id)) a.sco_path.clear();
            for (auto& sa : t.spline_attachments)
                if (deleted.count(sa.instance_id)) sa.sco_path.clear();
        }
    }
    return tile;
}

namespace {
std::optional<MapTileData> load_map_tile_base(const std::filesystem::path& tile_map,
                                              const std::filesystem::path& omsi_root) {
    std::error_code ec_time, ec_size;
    const auto mtime = std::filesystem::last_write_time(tile_map, ec_time);
    const auto fsize = std::filesystem::file_size(tile_map, ec_size);
    const auto key = tile_parse_key(tile_map, omsi_root);
    if (!ec_time && !ec_size) {
        std::lock_guard lock(g_tile_parse_mu);
        if (const auto it = g_tile_parse_cache.find(key); it != g_tile_parse_cache.end()) {
            if (it->second.mtime == mtime && it->second.size == fsize) return it->second.tile;
        }
    }

    const auto lines0 = cfg::read_cfg_lines(tile_map);
    if (lines0.empty() && !omsi::vfs::exists(tile_map)) return std::nullopt;

    MapTileData tile{};
    tile.path = tile_map;

    const auto stem = tile_map.stem().string();
    if (stem.rfind("tile_", 0) == 0) {
        const auto coords = stem.substr(5);
        const auto us = coords.find('_');
        if (us != std::string::npos) {
            tile.grid_x = std::stoi(coords.substr(0, us));
            tile.grid_y = std::stoi(coords.substr(us + 1));
        }
    }

    tile.has_terrain = omsi::vfs::exists(tile_map.string() + ".terrain");

    // Single file-order pass for entities (OMSI TMapKachel): never group-by-tag then concatenate.
    // [spline]/[spline_h] share one index space for splineAttachement.spline_index.
    auto trim_l = [](std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        std::size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        return s.substr(i);
    };
    auto ieq_l = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const auto ca = static_cast<unsigned char>(a[i]);
            const auto cb = static_cast<unsigned char>(b[i]);
            if (std::tolower(ca) != std::tolower(cb)) return false;
        }
        return true;
    };
    auto is_object_nr = [](const std::string& t) {
        if (t.size() < 9) return false;
        std::size_t j = 0;
        while (j < t.size() && (t[j] == ' ' || t[j] == '\t')) ++j;
        const char* pref = "object nr";
        for (std::size_t k = 0; k < 9; ++k) {
            if (j + k >= t.size()) return false;
            if (std::tolower(static_cast<unsigned char>(t[j + k])) != static_cast<unsigned char>(pref[k]))
                return false;
        }
        return true;
    };
    auto is_mirror_tok = [](const std::string& tok) {
        return tok.size() == 6 && (tok[0] == 'm' || tok[0] == 'M') && (tok[1] == 'i' || tok[1] == 'I') &&
               (tok[2] == 'r' || tok[2] == 'R') && (tok[3] == 'r' || tok[3] == 'R') &&
               (tok[4] == 'o' || tok[4] == 'O') && (tok[5] == 'r' || tok[5] == 'R');
    };
    auto looks_like_texture = [](const std::string& s) {
        if (s.size() < 4) return false;
        const auto dot = s.rfind('.');
        if (dot == std::string::npos) return false;
        std::string ext = s.substr(dot);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext == ".dds" || ext == ".png" || ext == ".bmp" || ext == ".tga" || ext == ".jpg" ||
               ext == ".jpeg";
    };
    auto push_object = [&](const std::vector<std::string>& inst) {
        if (inst.size() < 9) return;
        MapObjectInst obj{};
        obj.flags = parse_value<std::int32_t>(inst[0]);
        obj.sco_path = omsi::join_omsi_path(omsi_root, omsi::repair_omsi_path_mojibake(inst[1]));
        obj.id = parse_value<std::int32_t>(inst[2]);
        obj.position.x = parse_value<f4f23ma::f32>(inst[3]);
        obj.position.y = parse_value<f4f23ma::f32>(inst[4]);
        obj.position.z = parse_value<f4f23ma::f32>(inst[5]);
        obj.rotation_deg.x = parse_value<f4f23ma::f32>(inst[6]);
        obj.rotation_deg.y = parse_value<f4f23ma::f32>(inst[7]);
        obj.rotation_deg.z = parse_value<f4f23ma::f32>(inst[8]);
        // Unity OMSICfgReader: for normal objects inst[9] is extra-string count.
        // Trees use obj_type=4 then texture/height/aspect, THEN optional string count.
        // Bug: treating the type "4" as nstr pulled tex/h/a into strings and re-emitted them twice.
        if (inst.size() > 9) {
            const int v9 = parse_value<std::int32_t>(inst[9]);
            int nstr = 0;
            std::size_t str_start = 10;
            if (v9 == 0 && inst.size() > 10) {
                const int v10 = parse_value<std::int32_t>(inst[10]);
                if (v10 > 0 && v10 <= 32 && inst.size() >= 11 + static_cast<std::size_t>(v10)) {
                    nstr = v10;
                    str_start = 11;
                    obj.obj_type = 0;
                }
            } else if (v9 == 4 && inst.size() >= 13 && looks_like_texture(inst[10])) {
                obj.obj_type = 4;
                obj.tree_texture = inst[10];
                obj.tree_height = parse_value<f4f23ma::f32>(inst[11]);
                obj.tree_aspect = parse_value<f4f23ma::f32>(inst[12]);
                // Optional extras after the tree triple (count @ inst[13]).
                if (inst.size() > 13) {
                    const int after = parse_value<std::int32_t>(inst[13]);
                    if (after >= 0 && after <= 32 &&
                        inst.size() >= 14 + static_cast<std::size_t>(after)) {
                        nstr = after;
                        str_start = 14;
                    }
                }
            } else if (v9 > 0 && v9 <= 32) {
                nstr = v9;
                str_start = 10;
            }
            if (nstr > 0 && nstr <= 32 && inst.size() >= str_start + static_cast<std::size_t>(nstr)) {
                obj.string_line_count = nstr;
                obj.strings.reserve(static_cast<std::size_t>(nstr));
                for (int k = 0; k < nstr; ++k)
                    obj.strings.push_back(inst[str_start + static_cast<std::size_t>(k)]);
                // String-only tree form (no type-4 block): first lines are tex/h/aspect.
                if (obj.obj_type != 4 && !obj.strings.empty() && looks_like_texture(obj.strings[0])) {
                    obj.obj_type = 4;
                    obj.tree_texture = obj.strings[0];
                    if (obj.strings.size() > 1)
                        obj.tree_height = parse_value<f4f23ma::f32>(obj.strings[1]);
                    if (obj.strings.size() > 2)
                        obj.tree_aspect = parse_value<f4f23ma::f32>(obj.strings[2]);
                    // Keep only true extras beyond the tree triple.
                    if (obj.strings.size() >= 3) {
                        obj.strings.erase(obj.strings.begin(),
                                          obj.strings.begin() + 3);
                        obj.string_line_count = static_cast<std::int32_t>(obj.strings.size());
                    }
                }
            } else if (obj.obj_type != 4 && inst.size() >= 13 && looks_like_texture(inst[10])) {
                // Legacy tree block without leading count line.
                obj.obj_type = 4;
                obj.tree_texture = inst[10];
                obj.tree_height = parse_value<f4f23ma::f32>(inst[11]);
                obj.tree_aspect = parse_value<f4f23ma::f32>(inst[12]);
            }
            // Already-duplicated files (type4 tree + strings that repeat tex/h/a): strip the echo.
            if (obj.obj_type == 4 && obj.strings.size() >= 3 && looks_like_texture(obj.strings[0])) {
                obj.strings.erase(obj.strings.begin(), obj.strings.begin() + 3);
                obj.string_line_count = static_cast<std::int32_t>(obj.strings.size());
            }
        }
        tile.objects.push_back(std::move(obj));
    };
    auto push_attach = [&](const std::vector<std::string>& inst) {
        if (inst.size() < 10) return;
        MapAttachObjectInst a{};
        a.sco_path = omsi::join_omsi_path(omsi_root, omsi::repair_omsi_path_mojibake(inst[1]));
        a.child_id = parse_value<std::int32_t>(inst[2]);
        a.parent_id = parse_value<std::int32_t>(inst[3]);
        a.extra_rot_y = parse_value<f4f23ma::f32>(inst[4]);
        a.attach_point_index = parse_value<std::int32_t>(inst[5]);
        a.extra_rot_x = parse_value<f4f23ma::f32>(inst[6]);
        a.extra_rot_y2 = parse_value<f4f23ma::f32>(inst[7]);
        a.extra_rot_z = parse_value<f4f23ma::f32>(inst[8]);
        a.options_flag = parse_value<std::int32_t>(inst[9]);
        // options_flag != 0 → remaining lines ARE the extra strings (lineplate "5"/"92"/"5N").
        // Not a count: "5" with only one following field would be misread as five empty lines.
        // openOMSI read_labels: a count and that many lines, empty ones included
        if (a.options_flag > 0 && inst.size() > 10) {
            const std::size_t end_k = (std::min)(inst.size(), static_cast<std::size_t>(10 + a.options_flag));
            a.object_text_lines.reserve(end_k - 10);
            for (std::size_t k = 10; k < end_k; ++k)
                a.object_text_lines.push_back(extract_object_text_value(inst[k]));
            a.object_text_line_count = static_cast<std::int32_t>(a.object_text_lines.size());
            if (!a.object_text_lines.empty()) a.label = a.object_text_lines.front();
        }
        tile.attachments.push_back(std::move(a));
    };
    auto push_spline = [&](const std::vector<std::string>& inst_in, bool is_h) {
        std::vector<std::string> inst;
        inst.reserve(inst_in.size());
        for (const auto& s : inst_in)
            if (!s.empty()) inst.push_back(s);
        const std::size_t need = is_h ? 18u : 17u;
        if (inst.size() < need) return;
        SplineInst sp{};
        sp.is_spline_h = is_h;
        sp.use_delta_height = is_h;
        sp.flags = parse_value<std::int32_t>(inst[0]);
        sp.sli_path = omsi::join_omsi_path(omsi_root, inst[1]);
        sp.id = parse_value<std::int32_t>(inst[2]);
        // OMSI .map: id, prev, next (successor at end). Pimlico 20047/20091: next=20091, localLength of
        // next = previous length — not id,next,prev.
        sp.prev_id = parse_value<std::int32_t>(inst[3]);
        sp.next_id = parse_value<std::int32_t>(inst[4]);
        sp.position.x = parse_value<f4f23ma::f32>(inst[5]);
        sp.position.z = parse_value<f4f23ma::f32>(inst[6]);
        sp.position.y = parse_value<f4f23ma::f32>(inst[7]);
        sp.heading_deg = parse_value<f4f23ma::f32>(inst[8]);
        sp.length = parse_value<f4f23ma::f32>(inst[9]);
        sp.radius = parse_value<f4f23ma::f32>(inst[10]);
        sp.start_grad = parse_value<f4f23ma::f32>(inst[11]);
        sp.end_grad = parse_value<f4f23ma::f32>(inst[12]);
        if (is_h) {
            sp.delta_height = parse_value<f4f23ma::f32>(inst[13]);
            sp.cant_start = parse_value<f4f23ma::f32>(inst[14]);
            sp.cant_end = parse_value<f4f23ma::f32>(inst[15]);
            sp.skew_start = parse_value<f4f23ma::f32>(inst[16]);
            sp.skew_end = parse_value<f4f23ma::f32>(inst[17]);
        } else {
            sp.cant_start = parse_value<f4f23ma::f32>(inst[13]);
            sp.cant_end = parse_value<f4f23ma::f32>(inst[14]);
            sp.skew_start = parse_value<f4f23ma::f32>(inst[15]);
            sp.skew_end = parse_value<f4f23ma::f32>(inst[16]);
        }
        // Trailing optional localLength (after fixed block). Absent → 0 (chain prefix).
        // along = offsetAlong - localLength. Writer omits field when 0 or ≈ length.
        sp.local_length = 0.f;
        const std::size_t first_extra = is_h ? 18u : 17u;
        if (inst.size() > first_extra) {
            for (std::size_t ti = inst.size(); ti > first_extra; --ti) {
                const auto& tok = inst[ti - 1];
                if (tok.empty() || is_mirror_tok(tok)) continue;
                sp.local_length = parse_value<f4f23ma::f32>(tok);
                break;
            }
        }
        for (const auto& tok : inst) {
            if (is_mirror_tok(tok)) {
                sp.mirror = true;
                break;
            }
        }
        for (const auto& existing : tile.splines) {
            if (existing.id == sp.id) {
                static int s_dup = 0;
                if (s_dup < 20) {
                    log::warn("load_map_tile skip duplicate spline id=" + std::to_string(sp.id) +
                              " in " + tile_map.filename().string());
                    ++s_dup;
                }
                return;
            }
        }
        tile.splines.push_back(std::move(sp));
    };

    {
        const auto& lines = lines0;
        enum class Ent { None, Object, Attach, Spline, SplineH, SplineAttach, SplineAttachRep };
        Ent cur = Ent::None;
        std::size_t entity_tag_line = 0;
        std::vector<std::string> fields;
        auto flush = [&]() {
            switch (cur) {
            case Ent::Object:
                push_object(fields);
                break;
            case Ent::Attach:
                push_attach(fields);
                break;
            case Ent::Spline:
                push_spline(fields, false);
                break;
            case Ent::SplineH:
                push_spline(fields, true);
                break;
            case Ent::SplineAttach:
            case Ent::SplineAttachRep:
                if (auto att = parse_spline_attachment_at_tag_line(lines, entity_tag_line, omsi_root))
                    tile.spline_attachments.push_back(std::move(*att));
                break;
            default:
                break;
            }
            fields.clear();
            cur = Ent::None;
        };
        for (std::size_t li = 0; li < lines.size(); ++li) {
            const auto line = trim_l(lines[li]);
            if (line.empty()) {
                // (an empty label is a label: dropped, the labels after it moved up one place)
                if (cur == Ent::Object || cur == Ent::Attach) fields.push_back("");
                continue;
            }
            if (is_object_nr(line)) continue;
            if (line.size() >= 3 && line.front() == '[' && line.back() == ']') {
                flush();
                entity_tag_line = li;
                if (ieq_l(line, "[object]"))
                    cur = Ent::Object;
                else if (ieq_l(line, "[attachObj]"))
                    cur = Ent::Attach;
                else if (ieq_l(line, "[spline]"))
                    cur = Ent::Spline;
                else if (ieq_l(line, "[spline_h]"))
                    cur = Ent::SplineH;
                else if (ieq_l(line, "[splineAttachement]") || ieq_l(line, "[splineAttachment]"))
                    cur = Ent::SplineAttach;
                else if (ieq_l(line, "[splineAttachement_repeater]") ||
                         ieq_l(line, "[splineAttachment_repeater]"))
                    cur = Ent::SplineAttachRep;
                else
                    cur = Ent::None;
                continue;
            }
            if (cur == Ent::SplineAttach || cur == Ent::SplineAttachRep) continue;
            if (cur != Ent::None) fields.push_back(line);
        }
        flush();
    }
    compute_spline_length_accum(tile.splines);

    // [rule]/[kill_rule] attach to preceding [object]/[spline] or [selobject]/[selspline] target.
    // Layout: pathIndex / keyword / value / extra (Unit126 / TPathRule_Road).
    {
        const auto lines = cfg::read_cfg_lines(tile_map);
        auto trim = [](std::string s) {
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
            std::size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            return s.substr(i);
        };
        auto ieq = [](std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                const auto ca = static_cast<unsigned char>(a[i]);
                const auto cb = static_cast<unsigned char>(b[i]);
                if (std::tolower(ca) != std::tolower(cb)) return false;
            }
            return true;
        };
        MapObjectInst* cur_obj = nullptr;
        SplineInst* cur_sp = nullptr;
        MapAttachObjectInst* cur_attach = nullptr;
        auto clear_cur = [&]() { cur_obj = nullptr; cur_sp = nullptr; cur_attach = nullptr; };
        auto bind_obj = [&](std::int32_t id) {
            clear_cur();
            for (auto& o : tile.objects) {
                if (o.id == id) {
                    cur_obj = &o;
                    return;
                }
            }
        };
        auto bind_sp = [&](std::int32_t id) {
            clear_cur();
            for (auto& s : tile.splines) {
                if (s.id == id) {
                    cur_sp = &s;
                    return;
                }
            }
        };
        auto bind_attach = [&](std::int32_t child_id) {
            clear_cur();
            for (auto& a : tile.attachments) {
                if (a.child_id == child_id) {
                    cur_attach = &a;
                    return;
                }
            }
        };
        auto next_tok = [&](std::size_t& i) -> std::string {
            while (i < lines.size()) {
                auto t = trim(lines[i++]);
                if (t.empty()) continue;
                if (!t.empty() && t.front() == '[') {
                    --i;
                    return {};
                }
                return t;
            }
            return {};
        };
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto line = trim(lines[i]);
            if (line.empty() || line.front() != '[') continue;
            if (ieq(line, "[object]") || ieq(line, "[spline]") || ieq(line, "[spline_h]")) {
                std::size_t j = i + 1;
                (void)next_tok(j); // flags
                (void)next_tok(j); // path
                const auto id_s = next_tok(j);
                if (id_s.empty()) continue;
                const auto id = parse_value<std::int32_t>(id_s);
                if (ieq(line, "[object]")) bind_obj(id);
                else bind_sp(id);
            } else if (ieq(line, "[selobject]")) {
                std::size_t j = i + 1;
                const auto id_s = next_tok(j);
                if (!id_s.empty()) bind_obj(parse_value<std::int32_t>(id_s));
            } else if (ieq(line, "[selspline]")) {
                std::size_t j = i + 1;
                const auto id_s = next_tok(j);
                if (!id_s.empty()) bind_sp(parse_value<std::int32_t>(id_s));
            } else if (ieq(line, "[attachObj]")) {
                std::size_t j = i + 1;
                (void)next_tok(j); // flags
                (void)next_tok(j); // path
                const auto id_s = next_tok(j);
                if (!id_s.empty()) bind_attach(parse_value<std::int32_t>(id_s));
            } else if (ieq(line, "[splineAttachement]") || ieq(line, "[splineAttachement_repeater]") ||
                       ieq(line, "[splineAttachment]") || ieq(line, "[splineAttachment_repeater]")) {
                // SA ids live in a separate parse; mark nothing here — align read in map_attachment.
                clear_cur();
            } else if (ieq(line, "[spline_terrain_align]")) {
                // Under [object] / [attachObj] / [splineAttachement]: AlignTerrain for that SCO.
                // Under [spline]: legacy terrain hole mode = 1.
                if (cur_obj) cur_obj->align_terrain = true;
                else if (cur_attach) cur_attach->align_terrain = true;
                else if (cur_sp) cur_sp->terrain_align = 1;
            } else if (ieq(line, "[spline_terrain_align_2]")) {
                std::size_t j = i + 1;
                const auto v = next_tok(j);
                if (cur_sp) {
                    int n = v.empty() ? 1 : parse_value<int>(v);
                    if (n < 0) n = 0;
                    if (n > 255) n = 255;
                    cur_sp->terrain_align = static_cast<std::uint8_t>(n);
                }
            } else if (ieq(line, "[rule]") || ieq(line, "[kill_rule]")) {
                std::size_t j = i + 1;
                const auto pi_s = next_tok(j);
                const auto kw = next_tok(j);
                if (pi_s.empty() || kw.empty()) continue;
                PathRuleEntry r{};
                r.kill = ieq(line, "[kill_rule]");
                r.path_index = parse_value<std::int32_t>(pi_s);
                r.keyword = kw;
                auto kw_ieq = [&](const char* a) { return ieq(kw, a); };
                if (kw_ieq("speedlimit") || kw_ieq("priority")) {
                    const auto val_s = next_tok(j);
                    r.value = val_s.empty() ? 0.f : parse_value<f4f23ma::f32>(val_s);
                    // priority is SetRule case 6 (byte); path_index is toks[0], never the keyword id.
                    if (kw_ieq("priority")) {
                        if (r.value < 0.f) r.value = 0.f;
                        if (r.value > 255.f) r.value = 255.f;
                    }
                } else if (kw_ieq("trafficdensity")) {
                    // Omsi.exe (TMapKachel.loadMapFile 080 -> TFileObject.AddRuleFromFile): line 3 is the
                    // value (StrToFloat), line 4 (tile version >= 13) the vehicle group (StrToInt) - the
                    // writer has it so too. Read the other way round, "0.500 / 1" became group 0 with
                    // density 1, and every group's rule landed on group 0, one over the other.
                    const auto v_s = next_tok(j);
                    const auto g_s = next_tok(j);
                    r.value = v_s.empty() ? 0.f : parse_value<f4f23ma::f32>(v_s);
                    r.extra = g_s.empty() ? 0 : parse_value<std::int32_t>(g_s);
                }
                if (cur_obj) {
                    cur_obj->path_rules.push_back(std::move(r));
                    std::fprintf(stderr,
                        "[map-rule] attached to obj#%d kw=%s pi=%d val=%.3f kill=%d (rules_now=%zu)\n",
                        (int)cur_obj->id, cur_obj->path_rules.back().keyword.c_str(),
                        (int)cur_obj->path_rules.back().path_index,
                        (double)cur_obj->path_rules.back().value,
                        (int)cur_obj->path_rules.back().kill,
                        cur_obj->path_rules.size());
                } else if (cur_sp) {
                    cur_sp->path_rules.push_back(std::move(r));
                    std::fprintf(stderr,
                        "[map-rule] attached to spl#%d kw=%s pi=%d val=%.3f kill=%d (rules_now=%zu)\n",
                        (int)cur_sp->id, cur_sp->path_rules.back().keyword.c_str(),
                        (int)cur_sp->path_rules.back().path_index,
                        (double)cur_sp->path_rules.back().value,
                        (int)cur_sp->path_rules.back().kill,
                        cur_sp->path_rules.size());
                } else {
                    std::fprintf(stderr,
                        "[map-rule] ORPHAN kw=%s pi=%d (no cur_obj/cur_sp)\n",
                        r.keyword.c_str(), (int)r.path_index);
                }
            }
        }
    }

    // objectsFromFile order + [varparent] (post-block, batch table by Object Nr, Unit126).
    {
        const auto lines = cfg::read_cfg_lines(tile_map);
        auto trim = [](std::string s) {
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
            std::size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            return s.substr(i);
        };
        auto ieq = [](std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                const auto ca = static_cast<unsigned char>(a[i]);
                const auto cb = static_cast<unsigned char>(b[i]);
                if (std::tolower(ca) != std::tolower(cb)) return false;
            }
            return true;
        };
        auto is_int_token = [](const std::string& t) {
            if (t.empty()) return false;
            std::size_t i = 0;
            if (t[0] == '-' || t[0] == '+') ++i;
            if (i >= t.size()) return false;
            for (; i < t.size(); ++i)
                if (t[i] < '0' || t[i] > '9') return false;
            return true;
        };
        auto parse_object_nr = [&](const std::string& line) -> std::optional<std::int32_t> {
            const auto t = trim(line);
            if (t.size() < 9) return std::nullopt;
            std::size_t j = 0;
            while (j < t.size() && (t[j] == ' ' || t[j] == '\t')) ++j;
            const char* pref = "object nr";
            for (std::size_t k = 0; k < 9; ++k) {
                if (j + k >= t.size()) return std::nullopt;
                if (std::tolower(static_cast<unsigned char>(t[j + k])) != static_cast<unsigned char>(pref[k]))
                    return std::nullopt;
            }
            j += 9;
            if (j < t.size() && t[j] == '.') ++j;
            while (j < t.size() && (t[j] == ' ' || t[j] == '\t')) ++j;
            if (j >= t.size() || !is_int_token(t.substr(j))) return std::nullopt;
            return parse_value<std::int32_t>(t.substr(j));
        };
        enum class VpKind { None, Object, Attach, SplineAttach };
        auto assign_vp = [&](VpKind kind, std::size_t idx, std::int32_t code) {
            if (kind == VpKind::Object && idx < tile.objects.size())
                tile.objects[idx].varparent_id = code;
            else if (kind == VpKind::Attach && idx < tile.attachments.size())
                tile.attachments[idx].varparent_id = code;
            else if (kind == VpKind::SplineAttach && idx < tile.spline_attachments.size())
                tile.spline_attachments[idx].varparent_id = code;
        };
        VpKind last_kind = VpKind::None;
        std::size_t last_idx = 0;
        std::size_t obj_idx = 0;
        std::size_t attach_idx = 0;
        std::size_t sa_idx = 0;
        std::unordered_map<std::int32_t, std::int32_t> vp_by_obj_nr;
        tile.objects_from_file_order.clear();
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto line = trim(lines[i]);
            if (line.empty()) continue;
            if (const auto nr = parse_object_nr(line)) {
                const auto it = vp_by_obj_nr.find(*nr);
                if (it != vp_by_obj_nr.end()) {
                    if (*nr >= 0 && static_cast<std::size_t>(*nr) < tile.objects_from_file_order.size()) {
                        const auto& ref = tile.objects_from_file_order[static_cast<std::size_t>(*nr)];
                        assign_vp(ref.kind == MapObjectsFromFileKind::Object       ? VpKind::Object
                                  : ref.kind == MapObjectsFromFileKind::Attach   ? VpKind::Attach
                                  : ref.kind == MapObjectsFromFileKind::SplineAttach ? VpKind::SplineAttach
                                                                                   : VpKind::None,
                                  ref.index, it->second);
                    }
                    vp_by_obj_nr.erase(it);
                }
                continue;
            }
            if (line.front() != '[') continue;
            if (ieq(line, "[object]")) {
                tile.objects_from_file_order.push_back({MapObjectsFromFileKind::Object, obj_idx});
                last_kind = VpKind::Object;
                last_idx = obj_idx++;
            } else if (ieq(line, "[attachObj]")) {
                tile.objects_from_file_order.push_back({MapObjectsFromFileKind::Attach, attach_idx});
                last_kind = VpKind::Attach;
                last_idx = attach_idx++;
            } else if (ieq(line, "[splineAttachement]") || ieq(line, "[splineAttachement_repeater]")) {
                tile.objects_from_file_order.push_back({MapObjectsFromFileKind::SplineAttach, sa_idx});
                last_kind = VpKind::SplineAttach;
                last_idx = sa_idx++;
            } else if (ieq(line, "[varparent]")) {
                std::int32_t code = 0;
                bool got = false;
                std::optional<std::int32_t> batch_nr;
                for (++i; i < lines.size(); ++i) {
                    const auto t = trim(lines[i]);
                    if (t.empty()) continue;
                    if (t.front() == '[') {
                        --i;
                        break;
                    }
                    if (const auto nr = parse_object_nr(t)) {
                        batch_nr = nr;
                        break;
                    }
                    if (!is_int_token(t)) continue;
                    code = parse_value<std::int32_t>(t);
                    got = true;
                    break;
                }
                if (!got) continue;
                if (last_kind == VpKind::Object || last_kind == VpKind::Attach || last_kind == VpKind::SplineAttach)
                    assign_vp(last_kind, last_idx, code);
                else if (batch_nr)
                    vp_by_obj_nr[*batch_nr] = code;
            }
        }
        for (const auto& [nr, code] : vp_by_obj_nr) {
            if (nr < 0 || static_cast<std::size_t>(nr) >= tile.objects_from_file_order.size()) continue;
            const auto& ref = tile.objects_from_file_order[static_cast<std::size_t>(nr)];
            if (ref.kind == MapObjectsFromFileKind::Object)
                assign_vp(VpKind::Object, ref.index, code);
            else if (ref.kind == MapObjectsFromFileKind::Attach)
                assign_vp(VpKind::Attach, ref.index, code);
            else if (ref.kind == MapObjectsFromFileKind::SplineAttach)
                assign_vp(VpKind::SplineAttach, ref.index, code);
        }
    }

    if (!ec_time && !ec_size) {
        std::lock_guard lock(g_tile_parse_mu);
        g_tile_parse_cache[key] = TileParseCacheEntry{mtime, fsize, tile};
    }
    return tile;
}
} // namespace

void compute_spline_length_accum(std::vector<SplineInst>& splines) {
    std::unordered_map<std::int32_t, const SplineInst*> by_id;
    by_id.reserve(splines.size());
    for (const auto& s : splines) by_id[s.id] = &s;
    for (auto& s : splines) {
        if (s.length_accum > 1.e-3f) continue;
        float acc = 0.f;
        std::int32_t pid = s.prev_id;
        for (int guard = 0; pid != 0 && guard < 4096; ++guard) {
            const auto it = by_id.find(pid);
            if (it == by_id.end()) break;
            acc += it->second->length;
            pid = it->second->prev_id;
        }
        s.length_accum = acc;
    }
}


bool update_spline_terrain_align_in_map(const std::filesystem::path& map_path, std::int32_t spline_id,
                                        std::uint8_t terrain_align) {
    if (map_path.empty() || !omsi::vfs::exists(map_path)) return false;
    std::ifstream in(map_path);
    if (!in) return false;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    in.close();

    auto ieq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = a[i], cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) return false;
        }
        return true;
    };
    auto trim = [](std::string_view s) {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
        return s.substr(b, e - b);
    };

    // Find [spline]/[spline_h] block whose first integer token after path is spline_id
    // Structure roughly: tag, path, id, next, prev, x,y,z, heading, length, ...
    int block_start = -1;
    int block_end = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto t = trim(lines[i]);
        if (!ieq(t, "[spline]") && !ieq(t, "[spline_h]")) continue;
        // collect tokens until next [
        std::vector<std::string> toks;
        size_t j = i + 1;
        for (; j < lines.size(); ++j) {
            const auto tj = trim(lines[j]);
            if (tj.empty()) continue;
            if (!tj.empty() && tj.front() == '[') break;
            toks.push_back(std::string(tj));
        }
        // Layout: flags, path, id, prev, next, ...
        if (toks.size() >= 3) {
            try {
                const int id = std::stoi(toks[2]);
                if (id == spline_id) {
                    block_start = static_cast<int>(i);
                    block_end = static_cast<int>(j); // first line of next tag or end
                    break;
                }
            } catch (...) {}
        }
    }
    if (block_start < 0) return false;

    // Remove existing align tags inside this block
    std::vector<std::string> out;
    out.reserve(lines.size() + 2);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > static_cast<size_t>(block_start) && i < static_cast<size_t>(block_end)) {
            const auto t = trim(lines[i]);
            if (ieq(t, "[spline_terrain_align]") || ieq(t, "[spline_terrain_align_2]")) {
                // skip tag and following numeric line if present
                if (i + 1 < lines.size()) {
                    const auto n = trim(lines[i + 1]);
                    if (!n.empty() && n.front() != '[' && (std::isdigit(static_cast<unsigned char>(n.front())) || n.front() == '-'))
                        ++i; // skip value line
                }
                continue;
            }
        }
        out.push_back(lines[i]);
    }
    // Recompute block_end in out is hard — insert tags just before the next [ after block_start in out
    if (terrain_align > 0) {
        // Find insertion point: after block's content tokens, before next section tag
        size_t insert_at = out.size();
        bool seen = false;
        for (size_t i = 0; i < out.size(); ++i) {
            const auto t = trim(out[i]);
            if (!seen) {
                if (ieq(t, "[spline]") || ieq(t, "[spline_h]")) {
                    // check id
                    std::vector<std::string> toks;
                    for (size_t j = i + 1; j < out.size(); ++j) {
                        const auto tj = trim(out[j]);
                        if (tj.empty()) continue;
                        if (tj.front() == '[') break;
                        toks.push_back(std::string(tj));
                    }
                    if (toks.size() >= 3) {
                        try {
                            if (std::stoi(toks[2]) == spline_id) seen = true;
                        } catch (...) {}
                    }
                }
                continue;
            }
            if (!t.empty() && t.front() == '[') {
                insert_at = i;
                break;
            }
        }
        if (terrain_align == 1) {
            out.insert(out.begin() + static_cast<std::ptrdiff_t>(insert_at), "[spline_terrain_align]");
        } else {
            out.insert(out.begin() + static_cast<std::ptrdiff_t>(insert_at), std::to_string(static_cast<int>(terrain_align)));
            out.insert(out.begin() + static_cast<std::ptrdiff_t>(insert_at), "[spline_terrain_align_2]");
        }
    }

    omsi::fsutil::break_link(map_path); // copy-on-write workspace: never write through a link
    std::ofstream o(map_path, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    for (size_t i = 0; i < out.size(); ++i) {
        o << out[i] << "\r\n";
    }
    return true;
}



bool update_sco_align_terrain_in_map(const std::filesystem::path& map_path, std::string_view block_tag,
                                     std::int32_t instance_id, bool align_terrain) {
    if (map_path.empty() || !omsi::vfs::exists(map_path)) return false;
    std::ifstream in(map_path);
    if (!in) return false;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    in.close();
    auto ieq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = a[i], cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) return false;
        }
        return true;
    };
    auto trim = [](std::string_view s) {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
        return s.substr(b, e - b);
    };
    auto is_sa = [&](std::string_view t) {
        return ieq(t, "[splineattachement]") || ieq(t, "[splineattachement_repeater]") ||
               ieq(t, "[splineattachment]") || ieq(t, "[splineattachment_repeater]");
    };
    auto matches_tag = [&](std::string_view t) {
        if (ieq(block_tag, "[splineAttachement]") || ieq(block_tag, "[splineattachement]"))
            return is_sa(t);
        return ieq(t, block_tag);
    };
    // id token index: object/SA non-repeater = 2 (flags, path, id); attachObj = 2 same
    int block_start = -1, block_end = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!matches_tag(trim(lines[i]))) continue;
        std::vector<std::string> toks;
        size_t j = i + 1;
        for (; j < lines.size(); ++j) {
            const auto tj = trim(lines[j]);
            if (tj.empty()) continue;
            if (tj.front() == '[') break;
            toks.push_back(std::string(tj));
        }
        if (toks.size() < 3) continue;
        try {
            if (std::stoi(toks[2]) != instance_id) continue;
        } catch (...) { continue; }
        block_start = static_cast<int>(i);
        block_end = static_cast<int>(j);
        break;
    }
    if (block_start < 0) return false;
    std::vector<std::string> out;
    out.reserve(lines.size() + 1);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > static_cast<size_t>(block_start) && i < static_cast<size_t>(block_end)) {
            if (ieq(trim(lines[i]), "[spline_terrain_align]")) continue;
        }
        // Also strip a lone [spline_terrain_align] immediately after block_end if present
        if (i == static_cast<size_t>(block_end) && ieq(trim(lines[i]), "[spline_terrain_align]"))
            continue;
        out.push_back(lines[i]);
    }
    if (align_terrain) {
        size_t insert_at = out.size();
        bool seen = false;
        for (size_t i = 0; i < out.size(); ++i) {
            const auto t = trim(out[i]);
            if (!seen) {
                if (matches_tag(t)) {
                    std::vector<std::string> toks;
                    for (size_t j = i + 1; j < out.size(); ++j) {
                        const auto tj = trim(out[j]);
                        if (tj.empty()) continue;
                        if (tj.front() == '[') break;
                        toks.push_back(std::string(tj));
                    }
                    if (toks.size() >= 3) {
                        try {
                            if (std::stoi(toks[2]) == instance_id) seen = true;
                        } catch (...) {}
                    }
                }
                continue;
            }
            if (!t.empty() && t.front() == '[') {
                insert_at = i;
                break;
            }
        }
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(insert_at), "[spline_terrain_align]");
    }
    omsi::fsutil::break_link(map_path); // copy-on-write workspace: never write through a link
    std::ofstream o(map_path, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    for (const auto& L : out) o << L << "\r\n";
    return true;
}

bool update_object_align_terrain_in_map(const std::filesystem::path& map_path, std::int32_t object_id,
                                        bool align_terrain) {
    return update_sco_align_terrain_in_map(map_path, "[object]", object_id, align_terrain);
}

bool update_sa_tangential_in_map(const std::filesystem::path& map_path, std::int32_t instance_id,
                                 bool tangential) {
    if (map_path.empty() || !omsi::vfs::exists(map_path)) return false;
    std::ifstream in(map_path);
    if (!in) return false;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    in.close();
    auto ieq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = a[i], cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) return false;
        }
        return true;
    };
    auto trim = [](std::string_view s) {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
        return s.substr(b, e - b);
    };
    // Locate [splineAttachement] block; id is 3rd nonempty body token (flags, path, id)
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto tag = trim(lines[i]);
        if (!ieq(tag, "[splineAttachement]") && !ieq(tag, "[splineAttachement_repeater]")) continue;
        std::vector<size_t> body_idx;
        for (size_t j = i + 1; j < lines.size(); ++j) {
            const auto tj = trim(lines[j]);
            if (tj.empty()) continue;
            if (tj.front() == '[') break;
            body_idx.push_back(j);
        }
        // non-repeater: path=1, id=2 → body index 2 is id after flags(0) path(1)
        // Actually body: 0=flags, 1=path, 2=id, 3=spline_idx, 4=ox, 5=oy, 6=along, 7=ry, 8=rx, 9=rz, 10=step, 11=range, 12=tangential
        if (body_idx.size() < 3) continue;
        try {
            if (std::stoi(std::string(trim(lines[body_idx[2]]))) != instance_id) continue;
        } catch (...) { continue; }
        // Ensure we have at least tangential slot (index 12)
        while (body_idx.size() < 13) {
            // insert empty lines before next tag — append after last body line
            const size_t after = body_idx.back() + 1;
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(after), "0");
            body_idx.push_back(after);
            // re-scan would be safer but ids shift only after insert once
            break;
        }
        // Re-find body indices after possible insert
        body_idx.clear();
        for (size_t j = i + 1; j < lines.size(); ++j) {
            const auto tj = trim(lines[j]);
            if (tj.empty()) continue;
            if (tj.front() == '[') break;
            body_idx.push_back(j);
        }
        if (body_idx.size() > 12)
            lines[body_idx[12]] = tangential ? "1" : "0";
        else if (body_idx.size() > 11) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(body_idx[11] + 1),
                         tangential ? "1" : "0");
        }
        omsi::fsutil::break_link(map_path); // copy-on-write workspace: never write through a link
        std::ofstream o(map_path, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        for (const auto& L : lines) o << L << "\r\n";
        return true;
    }
    return false;
}


bool save_global_entrypoints(const std::filesystem::path& global_cfg,
                             const std::vector<EntryPoint>& entrypoints) {
    if (global_cfg.empty() || !omsi::vfs::exists(global_cfg)) return false;
    if (omsi::vfs::write_blocked(global_cfg)) return false;
    auto raw = cfg::read_cfg_lines(global_cfg);
    if (raw.empty()) return false;

    // Detect existing encoding (OMSI wide-char maps use UTF-16 LE + BOM).
    bool utf16_le = false;
    {
        std::ifstream in(global_cfg, std::ios::binary);
        unsigned char b[2]{};
        if (in && in.read(reinterpret_cast<char*>(b), 2) && in.gcount() == 2 && b[0] == 0xFF && b[1] == 0xFE)
            utf16_le = true;
    }

    std::size_t sec_begin = raw.size();
    std::size_t sec_end = raw.size();
    for (std::size_t i = 0; i < raw.size(); ++i) {
        auto line = raw[i];
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            if (line == "[entrypoints]") {
                sec_begin = i;
                sec_end = raw.size();
                for (std::size_t j = i + 1; j < raw.size(); ++j) {
                    auto l2 = raw[j];
                    while (!l2.empty() && (l2.back() == '\r' || l2.back() == ' ')) l2.pop_back();
                    if (!l2.empty() && l2.front() == '[' && l2.back() == ']') {
                        sec_end = j;
                        break;
                    }
                }
                break;
            }
        }
    }

    auto fmt_f = [](float v) -> std::string {
        if (v == 0.f) return "0";
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
        return buf;
    };

    std::vector<std::string> block;
    block.push_back("[entrypoints]");
    block.push_back(std::to_string(static_cast<int>(entrypoints.size())));
    for (const auto& ep : entrypoints) {
        block.push_back(std::to_string(ep.type));
        block.push_back(std::to_string(ep.id));
        block.push_back(std::to_string(ep.link_id));
        // Engine Y-up in file: x, height, y  (memory is Z-up: x, y, height)
        block.push_back(fmt_f(ep.position_local.x));
        block.push_back(fmt_f(ep.position_local.z));
        block.push_back(fmt_f(ep.position_local.y));
        block.push_back(fmt_f(ep.rotation_quat.x));
        block.push_back(fmt_f(ep.rotation_quat.y));
        block.push_back(fmt_f(ep.rotation_quat.z));
        block.push_back(fmt_f(ep.rotation_quat.w));
        block.push_back(std::to_string(ep.category));
        block.push_back(ep.name);
    }
    block.push_back("");

    std::vector<std::string> out;
    out.reserve(raw.size() + block.size());
    if (sec_begin >= raw.size()) {
        std::size_t insert_at = raw.size();
        for (std::size_t i = 0; i < raw.size(); ++i) {
            auto line = raw[i];
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line == "[map]") { insert_at = i; break; }
        }
        out.insert(out.end(), raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(insert_at));
        out.insert(out.end(), block.begin(), block.end());
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(insert_at), raw.end());
    } else {
        out.insert(out.end(), raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(sec_begin));
        out.insert(out.end(), block.begin(), block.end());
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(sec_end), raw.end());
    }

    omsi::fsutil::break_link(global_cfg); // copy-on-write workspace: never write through a link
    std::ofstream f(global_cfg, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (utf16_le) {
        f.put(static_cast<char>(0xFF));
        f.put(static_cast<char>(0xFE));
        for (const auto& line : out) {
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
        for (std::size_t i = 0; i < out.size(); ++i) {
            f.write(out[i].data(), static_cast<std::streamsize>(out[i].size()));
            f.put('\r');
            f.put('\n');
        }
    }
    return static_cast<bool>(f);
}

// category = index of tile (grid_x, grid_y) in global.cfg [map] order (same as OMSI TEntrypoint).
std::int32_t entrypoint_category_for_tile(const GlobalMap& global, std::int32_t grid_x, std::int32_t grid_y) {
    for (std::size_t i = 0; i < global.tiles.size(); ++i) {
        if (global.tiles[i].grid_x == grid_x && global.tiles[i].grid_y == grid_y)
            return static_cast<std::int32_t>(i);
    }
    return 0;
}

// Memory only — disk write is deferred to editor Save (Ctrl+S / finish_save_job).
bool upsert_global_entrypoint(GlobalMap& global, const EntryPoint& ep) {
    bool found = false;
    for (auto& e : global.entrypoints) {
        if (e.id == ep.id) {
            e = ep;
            found = true;
            break;
        }
    }
    if (!found) global.entrypoints.push_back(ep);
    resolve_entry_world_positions(global);
    return true;
}

bool remove_global_entrypoint_by_object_id(GlobalMap& global, std::int32_t object_id) {
    const auto before = global.entrypoints.size();
    global.entrypoints.erase(
        std::remove_if(global.entrypoints.begin(), global.entrypoints.end(),
                       [object_id](const EntryPoint& e) { return e.id == object_id; }),
        global.entrypoints.end());
    if (global.entrypoints.size() == before) return true;
    resolve_entry_world_positions(global);
    return true;
}

} // namespace omsi::map
