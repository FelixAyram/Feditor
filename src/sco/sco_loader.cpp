#include "omsi/vfs_overlay.hpp"
#include "omsi/sco_types.hpp"
#include "omsi/path_utf8.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/crossing_height_deform.hpp"
#include "omsi/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <execution>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace omsi::sco {
namespace {

float parse_f(const std::string& s, float fallback) {
    try {
        return std::stof(s);
    } catch (...) {
        return fallback;
    }
}

int parse_i(const std::string& s, int fallback) {
    try {
        return std::stoi(s);
    } catch (...) {
        return fallback;
    }
}

std::string texture_key(std::string_view name) {
    std::string out(name);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

} // namespace

namespace {
std::mutex g_sco_mu;
std::unordered_map<std::string, std::unique_ptr<SceneryObject>> g_sco_cache;

std::string sco_key(const std::filesystem::path& p) {
    auto s = omsi::path_utf8(p.lexically_normal());
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

void invalidate_sco_cache(const std::filesystem::path& sco_path) {
    std::lock_guard lock(g_sco_mu);
    if (sco_path.empty()) {
        g_sco_cache.clear();
        return;
    }
    g_sco_cache.erase(sco_key(sco_path));
}

std::size_t sco_cache_size() {
    std::lock_guard lock(g_sco_mu);
    return g_sco_cache.size();
}

bool rewrite_sco_paths(const std::filesystem::path& sco_path, const std::vector<ScoPath>& paths) {
    return rewrite_sco_paths_and_traffic(sco_path, paths, std::nullopt);
}

bool rewrite_sco_paths_and_traffic(const std::filesystem::path& sco_path, const std::vector<ScoPath>& paths,
                                   const std::optional<ScoTrafficLightsGroup>& group) {
    auto ieq2 = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    };
    auto trim2 = [](std::string s) {
        const auto a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string{};
        const auto b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    };
    auto fmt = [](float v) {
        std::ostringstream o;
        o.setf(std::ios::fmtflags(0), std::ios::floatfield);
        o << v;
        return o.str();
    };
    auto is_tl_inner = [&](std::string_view t) {
        return ieq2(t, "[traffic_light]") || ieq2(t, "[phase]") || ieq2(t, "[approachdist]") || ieq2(t, "[stop]");
    };
    const auto lines = cfg::read_cfg_lines(sco_path);
    if (lines.empty() && !omsi::vfs::exists(sco_path)) return false;
    std::vector<std::string> out;
    out.reserve(lines.size() + paths.size() * 20);
    bool inserted_paths = false;
    bool inserted_tl = !group.has_value();
    auto emit_paths = [&]() {
        for (const auto& p : paths) {
            out.emplace_back("[path]");
            out.push_back(fmt(p.x));
            out.push_back(fmt(p.y));
            out.push_back(fmt(p.z));
            out.push_back(fmt(p.heading_deg));
            out.push_back(fmt(p.radius));
            out.push_back(fmt(p.length));
            out.push_back(fmt(p.grad_start));
            out.push_back(fmt(p.grad_end));
            out.push_back(std::to_string(p.type));
            out.push_back(fmt(p.width));
            out.push_back(std::to_string(p.direction));
            out.push_back(std::to_string(p.blinker));
            if (p.height_deformed) out.push_back(fmt(p.height_delta));
            if (p.has_use_traffic_light) {
                out.emplace_back("[use_traffic_light]");
                out.push_back(std::to_string(p.use_traffic_light));
            }
            if (p.crossing_problem) out.emplace_back("[crossingproblem]");
            if (p.block_path) out.emplace_back("[blockpath]");
            out.emplace_back();
        }
        inserted_paths = true;
    };
    auto emit_tl = [&]() {
        if (!group) return;
        out.emplace_back("[traffic_lights_group]");
        out.push_back(fmt(group->cycle));
        for (const auto& L : group->lights) {
            out.emplace_back("[traffic_light]");
            out.push_back(L.name);
            for (const auto& ph : L.phases) {
                out.emplace_back("[phase]");
                out.push_back(std::to_string(ph.phase));
                out.push_back(fmt(ph.duration));
            }
            out.emplace_back("[approachdist]");
            out.push_back(fmt(L.approach_dist));
            for (const auto& st : L.stops) {
                out.emplace_back("[stop]");
                out.push_back(std::to_string(st.path_index));
                out.push_back(fmt(st.distance));
            }
        }
        out.emplace_back();
        inserted_tl = true;
    };
    for (std::size_t i = 0; i < lines.size();) {
        const auto t = trim2(lines[i]);
        if (ieq2(t, "[path]") || ieq2(t, "[path_2]")) {
            if (!inserted_paths) emit_paths();
            ++i;
            while (i < lines.size()) {
                const auto t2 = trim2(lines[i]);
                if (t2.empty()) {
                    ++i;
                    break;
                }
                if (t2.front() == '[') {
                    if (ieq2(t2, "[use_traffic_light]")) {
                        i += (i + 1 < lines.size()) ? 2 : 1;
                        continue;
                    }
                    if (ieq2(t2, "[crossingproblem]") || ieq2(t2, "[blockpath]")) {
                        ++i;
                        continue;
                    }
                    break;
                }
                ++i;
            }
            continue;
        }
        if (ieq2(t, "[traffic_lights_group]")) {
            if (!inserted_tl) emit_tl();
            ++i;
            while (i < lines.size()) {
                const auto t2 = trim2(lines[i]);
                if (t2.empty()) {
                    ++i;
                    continue;
                }
                if (t2.front() == '[' && !is_tl_inner(t2)) break;
                ++i;
            }
            continue;
        }
        out.push_back(lines[i]);
        ++i;
    }
    if (!inserted_paths) emit_paths();
    if (!inserted_tl) emit_tl();
    std::ofstream f(sco_path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    for (const auto& L : out) f << L << "\n";
    invalidate_sco_cache(sco_path);
    return true;
}

const char* sco_rendertype_tag(ScoRenderPriority p) {
    switch (p) {
    case ScoRenderPriority::Presurface: return "presurface";
    case ScoRenderPriority::Surface: return "surface";
    case ScoRenderPriority::OnSurface: return "on_surface";
    case ScoRenderPriority::Layer1: return "1";
    case ScoRenderPriority::Layer3: return "3";
    case ScoRenderPriority::Layer4: return "4";
    default: return "";
    }
}

bool rewrite_sco_header(const std::filesystem::path& sco_path, const std::string& friendly_name, int complexity,
                        ScoRenderPriority rendertype, bool absheight, bool onlyeditor, bool nocollision, bool fixed,
                        bool lightmap) {
    auto trim3 = [](std::string s) {
        const auto a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string{};
        const auto b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    };
    auto is_header_flag = [&](const std::string& t) {
        return t == "[absheight]" || t == "[onlyeditor]" || t == "[nocollision]" || t == "[fixed]" ||
               t == "[LightMapMapping]" || t == "[lightmapmapping]";
    };
    auto is_header_kv = [&](const std::string& t) {
        return t == "[friendlyname]" || t == "[complexity]" || t == "[rendertype]";
    };
    const auto lines = cfg::read_cfg_lines(sco_path);
    if (lines.empty() && !omsi::vfs::exists(sco_path)) return false;
    std::vector<std::string> out;
    out.reserve(lines.size() + 12);
    for (std::size_t i = 0; i < lines.size();) {
        const auto t = trim3(lines[i]);
        if (is_header_flag(t)) {
            ++i;
            continue;
        }
        if (is_header_kv(t)) {
            ++i;
            if (i < lines.size()) {
                const auto v = trim3(lines[i]);
                if (!v.empty() && v.front() != '[') ++i; // skip value line
            }
            continue;
        }
        out.push_back(lines[i]);
        ++i;
    }
    std::vector<std::string> hdr;
    if (!friendly_name.empty()) {
        hdr.emplace_back("[friendlyname]");
        hdr.push_back(friendly_name);
    }
    if (complexity != 0) {
        hdr.emplace_back("[complexity]");
        hdr.push_back(std::to_string(complexity));
    }
    const char* rt = sco_rendertype_tag(rendertype);
    if (rt && rt[0]) {
        hdr.emplace_back("[rendertype]");
        hdr.push_back(rt);
    }
    if (absheight) hdr.emplace_back("[absheight]");
    if (onlyeditor) hdr.emplace_back("[onlyeditor]");
    if (nocollision) hdr.emplace_back("[nocollision]");
    if (fixed) hdr.emplace_back("[fixed]");
    if (lightmap) hdr.emplace_back("[LightMapMapping]");

    std::vector<std::string> result;
    result.reserve(out.size() + hdr.size());
    if (!out.empty()) {
        result.push_back(out.front());
        result.insert(result.end(), hdr.begin(), hdr.end());
        result.insert(result.end(), out.begin() + 1, out.end());
    } else {
        result = std::move(hdr);
    }
    std::ofstream f(sco_path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    for (const auto& L : result) f << L << "\n";
    invalidate_sco_cache(sco_path);
    return true;
}

const SceneryObject* peek_sco(const std::filesystem::path& sco_path) {
    const auto key = sco_key(sco_path);
    std::lock_guard lock(g_sco_mu);
    const auto it = g_sco_cache.find(key);
    return it != g_sco_cache.end() ? it->second.get() : nullptr;
}

const SceneryObject* load_sco(const std::filesystem::path& sco_path) {
    const auto key = sco_key(sco_path);
    {
        std::lock_guard lock(g_sco_mu);
        if (const auto it = g_sco_cache.find(key); it != g_sco_cache.end()) return it->second.get();
    }

    const auto doc = cfg::load_cfg_file(sco_path);
    if (!doc) return nullptr;

    SceneryObject obj{};
    obj.sco_path = sco_path;
    const auto base = sco_path.parent_path();
    // The .sco's lines, and after them those of its [model] cfg: a vehicle's parked .sco keeps its
    // [mesh]/[matl]/lights there (Vehicles\X\X_Parked.sco -> model\X_Parked.cfg; mesh paths are
    // relative to that cfg's folder, textures to the vehicle's texture folder as for a .sco)
    std::vector<std::string> src_lines = cfg::read_cfg_lines(sco_path);
    std::filesystem::path model_cfg_dir;
    {
        const std::size_t n0 = src_lines.size();
        for (std::size_t i = 0; i < n0; ++i) {
            if (!ieq(trim(src_lines[i]), "[model]")) continue;
            for (std::size_t j = i + 1; j < n0; ++j) {
                const auto n = trim(src_lines[j]);
                if (n.empty()) continue;
                if (n.front() == '[') break;
                const auto mp = base / std::string(n);
                if (omsi::vfs::exists(mp)) {
                    model_cfg_dir = mp.parent_path();
                    auto more = cfg::read_cfg_lines(mp);
                    src_lines.push_back("");
                    src_lines.insert(src_lines.end(), more.begin(), more.end());
                }
                break;
            }
            break;
        }
    }

    // Omsi.exe compares whole lines: a keyword with leading blanks ("\t[mesh]") is a comment, not a tag. Read
    // as tags (every pass below trims), 2_ampel_bus_T_ohneM's commented yellow / pedestrian lenses were loaded as
    // meshes and lit with the bus light's own phase - two lamps on at once.
    for (auto& ln : src_lines) {
        std::size_t k = 0;
        while (k < ln.size() && (ln[k] == ' ' || ln[k] == '\t')) ++k;
        if (k > 0 && k < ln.size() && ln[k] == '[') ln.insert(ln.begin(), '#');
    }

    if (doc->contains("[friendlyname]") && !cfg::cfg_section(*doc, "[friendlyname]").empty())
        obj.friendly_name = cfg::cfg_section(*doc, "[friendlyname]")[0][0];

    if (doc->contains("[crossing_heightdeformation]") &&
        !cfg::cfg_section(*doc, "[crossing_heightdeformation]").empty() &&
        !cfg::cfg_section(*doc, "[crossing_heightdeformation]")[0].empty())
        obj.crossing_height_deformation_mesh = cfg::cfg_section(*doc, "[crossing_heightdeformation]")[0][0];

    if (doc->contains("[collision_mesh]") && !cfg::cfg_section(*doc, "[collision_mesh]").empty() &&
        !cfg::cfg_section(*doc, "[collision_mesh]")[0].empty())
        obj.collision_mesh = cfg::cfg_section(*doc, "[collision_mesh]")[0][0];
    else if (doc->contains("[collisionmesh]") && !cfg::cfg_section(*doc, "[collisionmesh]").empty() &&
             !cfg::cfg_section(*doc, "[collisionmesh]")[0].empty())
        obj.collision_mesh = cfg::cfg_section(*doc, "[collisionmesh]")[0][0];

    if (doc->contains("[terrainhole]") && !cfg::cfg_section(*doc, "[terrainhole]").empty() &&
        !cfg::cfg_section(*doc, "[terrainhole]")[0].empty())
        obj.terrain_hole_mesh = cfg::cfg_section(*doc, "[terrainhole]")[0][0];

    if (doc->contains("[onlyeditor]")) {
        obj.only_editor_mesh = true;
        obj.no_collision = true;
    }
    if (doc->contains("[nocollision]")) obj.no_collision = true;
    if (doc->contains("[fixed]") || doc->contains("[crashmode_pole]")) obj.fixed = true;
    if (doc->contains("[crashmode_pole]")) {
        obj.crash_pole = true;
        const auto& sec = cfg::cfg_section(*doc, "[crashmode_pole]");
        if (!sec.empty()) {
            const auto& lines = sec[0];
            if (lines.size() >= 1) obj.crash_pole_mass_t = std::strtof(lines[0].c_str(), nullptr);
            if (lines.size() >= 2) obj.crash_pole_load_kn = std::strtof(lines[1].c_str(), nullptr);
        }
        if (obj.crash_pole_mass_t <= 0.f) obj.crash_pole_mass_t = 0.1f;
    }
    if (doc->contains("[surface]")) {
        obj.fixed = true;
        obj.surface = true;
        obj.active = false;
        // OMSI drive-on ground: same draw pass as [rendertype] surface when tag omitted.
        if (obj.render_priority == ScoRenderPriority::Normal)
            obj.render_priority = ScoRenderPriority::Surface;
    }
    if (doc->contains("[boundingbox]")) {
        const auto& bb = cfg::cfg_section(*doc, "[boundingbox]");
        std::vector<float> nums;
        for (const auto& inst : bb) {
            for (const auto& tok : inst) nums.push_back(parse_f(tok, 0.f));
        }
        if (nums.size() >= 6) {
            obj.has_boundingbox = true;
            obj.bbox_w = nums[0];
            obj.bbox_l = nums[1];
            obj.bbox_h = nums[2];
            obj.bbox_off_x = nums[3];
            obj.bbox_off_y = nums[4];
            obj.bbox_off_z = nums[5];
        }
    }
    if (doc->contains("[absheight]")) obj.abs_height = true;
    if (doc->contains("[complexity]") && !cfg::cfg_section(*doc, "[complexity]").empty() &&
        !cfg::cfg_section(*doc, "[complexity]")[0].empty())
        obj.complexity = static_cast<int>(parse_f(cfg::cfg_section(*doc, "[complexity]")[0][0], 0.f));
    if (doc->contains("[LightMapMapping]")) obj.lightmap = true;
    if (doc->contains("[path]")) obj.has_path = true;
    if (doc->contains("[rendertype]") && !cfg::cfg_section(*doc, "[rendertype]").empty() &&
        !cfg::cfg_section(*doc, "[rendertype]")[0].empty()) {
        const auto rt = trim(cfg::cfg_section(*doc, "[rendertype]")[0][0]);
        if (ieq(rt, "presurface")) obj.render_priority = ScoRenderPriority::Presurface;
        else if (ieq(rt, "surface")) obj.render_priority = ScoRenderPriority::Surface;
        else if (ieq(rt, "on_surface")) obj.render_priority = ScoRenderPriority::OnSurface;
        else if (rt == "1") obj.render_priority = ScoRenderPriority::Layer1;
        else if (rt == "3") obj.render_priority = ScoRenderPriority::Layer3;
        else if (rt == "4") obj.render_priority = ScoRenderPriority::Layer4;
        else obj.render_priority = ScoRenderPriority::Normal;
    }
    // Unit128 special kinds (+0x1c4). Order matches decompilation tag checks.
    // OMSI tags are case-insensitive: real busstop .sco files exist as [Busstop] / [BUSSTOP] too.
    auto has_tag_ci = [&](const char* tag) {
        if (doc->contains(tag)) return true;
        for (const auto& kv : *doc)
            if (ieq(kv.first, tag)) return true;
        return false;
    };
    if (has_tag_ci("[busstop]")) obj.special = ScoSpecial::Busstop;
    else if (has_tag_ci("[entrypoint]")) obj.special = ScoSpecial::EntryPoint;
    else if (has_tag_ci("[carpark_p]")) obj.special = ScoSpecial::Carpark;
    else if (has_tag_ci("[trafficlight]")) obj.special = ScoSpecial::TrafficLight;
    else if (has_tag_ci("[signal]")) obj.special = ScoSpecial::Signal;
    else if (has_tag_ci("[helparrow]")) obj.special = ScoSpecial::HelpArrow;
    else if (has_tag_ci("[switch]")) obj.special = ScoSpecial::Switch;

    // Sequential [LOD] + [mesh] (OMSI screensize bands). Flattened list keeps lod_screensize per mesh.
    {
        const auto& lines = src_lines;
        float cur_lod_ss = 1.f;
        int cur_lod_i = 0;
        bool saw_lod_tag = false;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            auto t = trim(lines[i]);
            if (t.empty() || t.front() != '[') continue;
            if (ieq(t, "[LOD]") || ieq(t, "[lod]")) {
                float v = 0.f;
                for (std::size_t j = i + 1; j < lines.size(); ++j) {
                    auto n = trim(lines[j]);
                    if (n.empty()) continue;
                    if (n.front() == '[') break;
                    try {
                        v = std::stof(n);
                    } catch (...) {
                        v = 0.f;
                    }
                    break;
                }
                if (saw_lod_tag) ++cur_lod_i;
                saw_lod_tag = true;
                cur_lod_ss = v;
                continue;
            }
            if (!ieq(t, "[mesh]")) continue;
            std::string rel;
            for (std::size_t j = i + 1; j < lines.size(); ++j) {
                auto n = trim(lines[j]);
                if (n.empty()) continue;
                if (n.front() == '[') break;
                rel = n;
                break;
            }
            if (rel.empty()) continue;
            SceneryMeshRef ref{};
            ref.mesh_file = base / rel;
            if (!omsi::vfs::exists(ref.mesh_file)) {
                const auto model_path = base / "model" / rel;
                if (omsi::vfs::exists(model_path)) ref.mesh_file = model_path;
                else if (!model_cfg_dir.empty() && omsi::vfs::exists(model_cfg_dir / rel))
                    ref.mesh_file = model_cfg_dir / rel;
            }
            const auto ext = omsi::path_utf8(ref.mesh_file.extension());
            ref.is_o3d = (ext == ".o3d" || ext == ".O3D");
            ref.lod_screensize = cur_lod_ss;
            ref.lod_index = cur_lod_i;
            obj.meshes.push_back(std::move(ref));
        }
    }

    // Sequential [matl] + modifiers (same order as model.cfg / DecalF01.sco).
    {
        const auto& lines = src_lines;
        std::string tag;
        model_cfg::ModelCfgMaterial* cur = nullptr;
        int mesh_i = -1;
        // after [matl_item] the modifiers belong to the next item of the [matl_change] (its variant), not
        // to the base: only a "NightlightA" item's light map is kept (the night look the renderer lays on)
        bool in_item = false;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto& raw = lines[i];
            if (raw.empty()) continue;
            if (raw.front() == '[') {
                const auto line = trim(raw);
                if (line.empty() || line.back() != ']') continue;
                tag = line;
                if (ieq(tag, "[mesh]")) {
                    ++mesh_i;
                    cur = nullptr;
                    in_item = false;
                }
                if (ieq(tag, "[matl_item]")) in_item = cur != nullptr;
                // [matl_transmap] with no file (3_ggr_7): the texture's own alpha masks the envmap
                if (cur && !in_item && ieq(tag, "[matl_transmap]") && !cur->transmap_texture)
                    cur->transmap_texture = std::string{};
                continue;
            }
            const auto line = trim(raw);
            if (line.empty()) continue;
            if (in_item && !(ieq(tag, "[matl]") || ieq(tag, "[matl_change]"))) {
                if (cur && ieq(tag, "[matl_lightmap]")) {
                    std::string cv;
                    for (char c : cur->change_var)
                        if (c != ' ' && c != '\t') cv.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                    if (cv == "nightlighta" && !cur->lightmap_texture) cur->lightmap_texture = std::string(line);
                }
                tag.clear();
                continue;
            }
            if (ieq(tag, "[matl]") || ieq(tag, "[matl_change]")) {
                in_item = false;
                model_cfg::ModelCfgMaterial mat{};
                mat.is_matl_change = ieq(tag, "[matl_change]");
                mat.diffuse_texture = line;
                mat.texture_key = texture_key(line);
                mat.mesh_index = mesh_i;
                if (i + 1 < lines.size()) {
                    const auto id_line = trim(lines[i + 1]);
                    if (!id_line.empty() && id_line.front() != '[') {
                        mat.mat_id = parse_i(id_line, -1);
                        ++i;
                    }
                }
                // [matl_change] tex / mat_id / change_var (OSC; swap albedo if var > 0.5).
                if (mat.is_matl_change && i + 1 < lines.size()) {
                    const auto var_line = trim(lines[i + 1]);
                    if (!var_line.empty() && var_line.front() != '[') {
                        mat.change_var = var_line;
                        ++i;
                    }
                }
                obj.materials.push_back(std::move(mat));
                cur = &obj.materials.back();
                obj.material_textures.push_back(cur->diffuse_texture);
                tag.clear();
            } else if (cur && ieq(tag, "[matl_alpha]")) {
                cur->alpha_mode = static_cast<model_cfg::MatlAlphaMode>(
                    (std::min)(2, (std::max)(0, parse_i(line, 0))));
                tag.clear();
            } else if (cur && ieq(tag, "[matl_nozwrite]")) {
                cur->no_zwrite = true;
                tag.clear();
            } else if (cur && (ieq(tag, "[matl_nozcheck]") || ieq(tag, "[matl_nozread]"))) {
                cur->no_zcheck = true;
                tag.clear();
            } else if (cur && ieq(tag, "[matl_freetex]")) {
                if (i + 1 < lines.size()) {
                    const auto friendly = trim(lines[i + 1]);
                    if (!friendly.empty() && friendly.front() != '[') {
                        cur->freetex_label = friendly;
                        ++i;
                    }
                }
                tag.clear();
            } else if (cur && ieq(tag, "[matl_transmap]")) {
                cur->transmap_texture = line;
                tag.clear();
            } else if (cur && ieq(tag, "[matl_envmap_mask]")) {
                cur->envmap_mask_texture = std::string(line);
                tag.clear();
            } else if (cur && ieq(tag, "[matl_nightmap]")) {
                cur->nightmap_texture = std::string(line);
                tag.clear();
            } else if (cur && ieq(tag, "[matl_lightmap]")) {
                cur->lightmap_texture = std::string(line);
                tag.clear();
            } else if (cur && (ieq(tag, "[texcoordtransy]") || ieq(tag, "[texcoordtransY]"))) {
                cur->texcoord_trans_y_var = line;
                tag.clear();
            } else if (cur && (ieq(tag, "[texcoordtransx]") || ieq(tag, "[texcoordtransX]"))) {
                cur->texcoord_trans_x_var = line;
                tag.clear();
            } else if (cur && ieq(tag, "[usetexttexture]")) {
                cur->use_text_texture = parse_i(line, 0);
                tag.clear();
            } else if (cur && (ieq(tag, "[matl_envmap]") || ieq(tag, "[matl_envmaprealtime]"))) {
                cur->envmap_texture = line;
                cur->envmap_realtime = ieq(tag, "[matl_envmaprealtime]");
                if (i + 1 < lines.size()) {
                    const auto strength_line = trim(lines[i + 1]);
                    if (!strength_line.empty() && strength_line.front() != '[') {
                        cur->envmap_strength = parse_f(strength_line, 1.f);
                        ++i;
                    }
                }
                tag.clear();
            }
        }
    }

    // [CTC] var / folder / default and [CTCTexture] name / texture (openOMSI open.rs ctc_schemes)
    {
        const auto& lines = src_lines;
        auto next_val = [&](std::size_t& j) -> std::string {
            for (++j; j < lines.size(); ++j) {
                const auto n = trim(lines[j]);
                if (n.empty()) continue;
                if (n.front() == '[') {
                    --j;
                    return {};
                }
                return std::string(n);
            }
            return {};
        };
        std::vector<std::pair<std::string, std::string>> packs; // var, folder
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto t = trim(lines[i]);
            if (ieq(t, "[CTC]")) {
                const auto var = next_val(i);
                const auto folder = next_val(i);
                if (!var.empty() && !folder.empty()) packs.emplace_back(var, folder);
            } else if (ieq(t, "[CTCTexture]")) {
                const auto name = next_val(i);
                const auto tex = next_val(i);
                if (!name.empty() && !tex.empty()) obj.ctc_textures.emplace_back(name, tex);
            }
        }
        for (const auto& [var, folder] : packs) {
            ScoCtc c{};
            c.var = var;
            const auto dir = base / folder;
            std::error_code ec;
            std::vector<std::filesystem::path> ctis;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (!e.is_regular_file(ec)) continue;
                auto ext = omsi::path_utf8(e.path().extension());
                for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (ext == ".cti") ctis.push_back(e.path());
            }
            std::sort(ctis.begin(), ctis.end());
            for (const auto& f : ctis) {
                const auto cl = cfg::read_cfg_lines(f);
                for (std::size_t i = 0; i < cl.size(); ++i) {
                    if (!ieq(trim(cl[i]), "[item]")) continue;
                    std::string vals[3];
                    std::size_t got = 0;
                    for (std::size_t j = i + 1; j < cl.size() && got < 3; ++j) {
                        const auto n = trim(cl[j]);
                        if (n.empty()) continue;
                        if (n.front() == '[') break;
                        vals[got++] = std::string(n);
                    }
                    if (got < 3) continue;
                    ScoCtcScheme* sc = nullptr;
                    for (auto& x : c.schemes)
                        if (ieq(x.name, vals[0])) sc = &x;
                    if (!sc) {
                        c.schemes.push_back({vals[0], f.parent_path(), {}});
                        sc = &c.schemes.back();
                    }
                    sc->textures.emplace_back(vals[1], vals[2]);
                }
            }
            if (!c.schemes.empty()) obj.ctc.push_back(std::move(c));
        }
    }

    // MergeMapTrees parse_sco [tree]: texture, height, [width|ratio…]
    if (doc->contains("[tree]") && !cfg::cfg_section(*doc, "[tree]").empty()) {
        for (const auto& vals : cfg::cfg_section(*doc, "[tree]")) {
            if (vals.empty() || vals[0].empty() || vals[0][0] == '{') continue;
            obj.tree.valid = true;
            obj.tree.texture = vals[0];
            if (vals.size() > 1) obj.tree.height = parse_f(vals[1], obj.tree.height);
            if (vals.size() >= 4) {
                const float third = parse_f(vals[2], 0.f);
                const float fourth = parse_f(vals[3], 0.f);
                if (fourth > 0.f && fourth <= 2.f) {
                    obj.tree.ratio = fourth;
                    obj.tree.width = 0.f;
                } else if (third > 0.01f) {
                    obj.tree.width = third;
                }
            } else if (vals.size() > 2) {
                obj.tree.width = parse_f(vals[2], obj.tree.width);
            }
        }
        if (obj.tree.valid) obj.special = ScoSpecial::Tree;
    }

    // [new_attachment] points (openOMSI omsi-scenery ATTACH_TOKENS / attachment_matrix): the
    // attach_trans / attach_rot_x|y|z lines are matched as whole lines, case as written, and apply to
    // the last [new_attachment] wherever they stand after it (the stock Timetable_Terminus_Pole_S lifts
    // its point with an attach_trans after [complexity]); each one is a matrix applied after the ones
    // before it, so a rotation after a translation turns the point's place as well.
    {
        const auto& lines = src_lines;
        auto rstrip = [](std::string v) {
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ' || v.back() == '\t')) v.pop_back();
            return v;
        };
        auto mul = [](const float* a, const float* b, float* out) { // out = a * b (row-major 4x4)
            float r[16];
            for (int rr = 0; rr < 4; ++rr)
                for (int c = 0; c < 4; ++c) {
                    float acc = 0.f;
                    for (int k = 0; k < 4; ++k) acc += a[rr * 4 + k] * b[k * 4 + c];
                    r[rr * 4 + c] = acc;
                }
            std::memcpy(out, r, sizeof(r));
        };
        auto next_num = [&](std::size_t& j) -> float {
            for (++j; j < lines.size(); ++j) {
                const auto n = trim(lines[j]);
                if (n.empty()) continue;
                return parse_f(n, 0.f);
            }
            return 0.f;
        };
        bool disabled = false;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string raw = rstrip(lines[i]);
            if (raw == "-<DISABLED>-") {
                disabled = true;
                continue;
            }
            if (raw == "-<ENABLED>-") {
                disabled = false;
                continue;
            }
            if (disabled) continue;
            if (ieq(trim(raw), "[new_attachment]")) {
                obj.attachment_points.push_back(AttachmentPoint{});
                continue;
            }
            if (obj.attachment_points.empty()) continue;
            auto& pt = obj.attachment_points.back();
            float op[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            constexpr float kDeg = 3.14159265358979f / 180.f;
            if (raw == "attach_trans") {
                op[12] = next_num(i);
                op[13] = next_num(i);
                op[14] = next_num(i);
            } else if (raw == "attach_rot_x" || raw == "attach_rot_y" || raw == "attach_rot_z") {
                const float deg = next_num(i);
                const float a = -deg * kDeg, c = std::cos(a), sn = std::sin(a);
                if (raw == "attach_rot_x") {
                    op[5] = c; op[6] = sn; op[9] = -sn; op[10] = c;
                    pt.rot_x += deg;
                } else if (raw == "attach_rot_y") {
                    op[0] = c; op[2] = -sn; op[8] = sn; op[10] = c;
                    pt.rot_y += deg;
                } else {
                    op[0] = c; op[1] = sn; op[4] = -sn; op[5] = c;
                    pt.rot_z += deg;
                }
            } else {
                continue;
            }
            mul(pt.m, op, pt.m);
            pt.local_pos.x = pt.m[12];
            pt.local_pos.y = pt.m[13];
            pt.local_pos.z = pt.m[14];
        }
    }

    // [path] blocks — 12 floats (+ optional 13th: delta_h from f4 bake).
    {
        const auto& lines = src_lines;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (!ieq(trim(lines[i]), "[path]") && !ieq(trim(lines[i]), "[path_2]")) continue;
            std::vector<float> vals;
            for (std::size_t j = i + 1; j < lines.size() && vals.size() < 13; ++j) {
                const auto tok = trim(lines[j]);
                if (tok.empty()) continue;
                if (tok.front() == '[') break;
                vals.push_back(parse_f(tok, 0.f));
                i = j;
            }
            if (vals.size() < 12) continue;
            ScoPath path{};
            path.x = vals[0];
            path.y = vals[1];
            path.z = vals[2];
            path.heading_deg = vals[3];
            path.radius = vals[4];
            path.length = vals[5];
            path.grad_start = vals[6];
            path.grad_end = vals[7];
            path.type = static_cast<int>(vals[8]);
            path.width = vals[9];
            path.direction = static_cast<int>(vals[10]);
            path.blinker = static_cast<int>(vals[11]);
            if (vals.size() >= 13) {
                path.height_delta = vals[12];
                path.height_deformed = true;
            }

            // Optional tags after path floats.
            for (std::size_t j = i + 1; j < lines.size(); ++j) {
                const auto tj = trim(lines[j]);
                if (tj.empty()) continue;
                if (tj.front() != '[') break;
                if (ieq(tj, "[use_traffic_light]") && j + 1 < lines.size()) {
                    path.has_use_traffic_light = true;
                    path.use_traffic_light = static_cast<int>(parse_f(trim(lines[j + 1]), -1.f));
                    i = j + 1;
                    continue;
                }
                if (ieq(tj, "[crossingproblem]")) {
                    path.crossing_problem = true;
                    i = j;
                    continue;
                }
                if (ieq(tj, "[blockpath]")) {
                    path.block_path = true;
                    i = j;
                    continue;
                }
                break;
            }
            obj.paths.push_back(path);
            obj.has_path = true;
        }
    }

    auto parse_numbered_files = [&](const char* tag, std::vector<std::filesystem::path>& out) {
        if (!doc->contains(tag)) return;
        for (const auto& inst : cfg::cfg_section(*doc, tag)) {
            if (inst.empty()) continue;
            int count = 0;
            try {
                count = std::stoi(inst[0]);
            } catch (...) {
                continue;
            }
            for (int k = 1; k <= count && static_cast<std::size_t>(k) < inst.size(); ++k)
                if (!inst[static_cast<std::size_t>(k)].empty())
                    out.emplace_back(inst[static_cast<std::size_t>(k)]);
        }
    };
    // Drop [crossing_heightdeformation] base .o3d from drawable meshes (Unity ModelReferences filter).
    if (!obj.crossing_height_deformation_mesh.empty()) {
        const auto base_stem = omsi::path_utf8(std::filesystem::path(obj.crossing_height_deformation_mesh).stem());
        obj.meshes.erase(
            std::remove_if(obj.meshes.begin(), obj.meshes.end(),
                           [&](const SceneryMeshRef& ref) {
                               return ieq(omsi::path_utf8(ref.mesh_file.stem()), base_stem) ||
                                      ieq(omsi::path_utf8(ref.mesh_file.filename()), obj.crossing_height_deformation_mesh);
                           }),
            obj.meshes.end());
    }

    parse_numbered_files("[script]", obj.script_files);
    // [sound] <cfg> : the object's sound definitions (world sound; see omsi/sound/world_sound_cfg.hpp).
    if (doc->contains("[sound]")) {
        for (const auto& blk : cfg::cfg_section(*doc, "[sound]")) {
            if (blk.empty() || blk[0].empty()) continue;
            std::string rel = blk[0];
            std::replace(rel.begin(), rel.end(), '\\', '/');
            obj.sound_cfgs.push_back(base / rel);
        }
    }
    parse_numbered_files("[varnamelist]", obj.varnamelist_files);
    parse_numbered_files("[stringvarnamelist]", obj.stringvarnamelist_files);

    // [stringvarnamelist] .txt — OMSI Svarstrings labels for map object strings.
    for (const auto& rel : obj.stringvarnamelist_files) {
        const auto list_path = base / rel;
        if (!omsi::vfs::exists(list_path)) continue;
        for (const auto& raw : cfg::read_cfg_lines(list_path)) {
            std::string line = raw;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty() || line[0] == '-' || line[0] == '[') continue;
            obj.string_var_names.push_back(line);
        }
    }

    // Sequential pass: [visible], [light_enh]/[light_enh_2], [traffic_lights_group].
    {
        const auto& lines = src_lines;
        int mesh_i = -1;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto& raw = lines[i];
            if (raw.empty()) continue;
            // OMSI: a command must start the line. Tab-indented [texttexture] in SDK comments
            // (routearrow_L.sco) is documentation, not a real block.
            if (raw.front() != '[') continue;
            const auto tok = trim(raw);
            if (ieq(tok, "[mesh]")) {
                ++mesh_i;
                continue;
            }
            if (ieq(tok, "[texttexture]") || ieq(tok, "[texttexture_enh]")) {
                ScoTextTexture tt{};
                auto read_next = [&]() -> std::string {
                    for (++i; i < lines.size(); ++i) {
                        const auto& r = lines[i];
                        if (!r.empty() && r.front() == '[') {
                            --i;
                            return {};
                        }
                        const auto t = trim(r);
                        if (!t.empty()) return t;
                    }
                    return {};
                };
                const auto src = read_next();
                bool as_index = !src.empty();
                for (std::size_t k = 0; k < src.size(); ++k) {
                    const unsigned char c = static_cast<unsigned char>(src[k]);
                    if (k == 0 && (src[k] == '-' || src[k] == '+')) continue;
                    if (c < '0' || c > '9') {
                        as_index = false;
                        break;
                    }
                }
                if (as_index) tt.source_index = parse_i(src, 0);
                else {
                    tt.string_var = src;
                    tt.source_index = -1;
                }
                tt.font_name = read_next();
                tt.width = parse_i(read_next(), 128);
                tt.height = parse_i(read_next(), 128);
                tt.color_mode = parse_i(read_next(), 0);
                tt.r = parse_i(read_next(), 255);
                tt.g = parse_i(read_next(), 0);
                tt.b = parse_i(read_next(), 0);
                if (ieq(tok, "[texttexture_enh]")) {
                    tt.align = parse_i(read_next(), 1);
                    tt.letter_spacing = parse_i(read_next(), 0);
                }
                obj.text_textures.push_back(std::move(tt));
                continue;
            }
            if (ieq(tok, "[newanim]") && mesh_i >= 0 &&
                static_cast<std::size_t>(mesh_i) < obj.meshes.size()) {
                model_cfg::MeshAnimLayer layer{};
                for (++i; i < lines.size(); ++i) {
                    const auto t = trim(lines[i]);
                    if (t.empty()) continue;
                    if (t.front() == '[') {
                        --i;
                        break;
                    }
                    if (ieq(t, "origin_from_mesh")) {
                        layer.origin_from_mesh = true;
                    } else if (ieq(t, "origin_trans") && i + 3 < lines.size()) {
                        layer.has_origin_trans = true;
                        layer.origin_trans[0] = parse_f(trim(lines[++i]), 0.f);
                        layer.origin_trans[1] = parse_f(trim(lines[++i]), 0.f);
                        layer.origin_trans[2] = parse_f(trim(lines[++i]), 0.f);
                    } else if (ieq(t, "origin_rot_x") && i + 1 < lines.size()) {
                        layer.origin_rot_deg[0] = parse_f(trim(lines[++i]), 0.f);
                    } else if (ieq(t, "origin_rot_y") && i + 1 < lines.size()) {
                        layer.origin_rot_deg[1] = parse_f(trim(lines[++i]), 0.f);
                    } else if (ieq(t, "origin_rot_z") && i + 1 < lines.size()) {
                        layer.origin_rot_deg[2] = parse_f(trim(lines[++i]), 0.f);
                    } else if (ieq(t, "anim_rot") && i + 2 < lines.size()) {
                        model_cfg::MeshAnimBinding b{};
                        b.type = model_cfg::MeshAnimBindingType::rotation;
                        b.var_name = trim(lines[++i]);
                        b.scale = parse_f(trim(lines[++i]), 1.f);
                        layer.bindings.push_back(std::move(b));
                    } else if (ieq(t, "anim_trans") && i + 2 < lines.size()) {
                        model_cfg::MeshAnimBinding b{};
                        b.type = model_cfg::MeshAnimBindingType::translation;
                        b.var_name = trim(lines[++i]);
                        b.scale = parse_f(trim(lines[++i]), 1.f);
                        layer.bindings.push_back(std::move(b));
                    } else if ((ieq(t, "maxspeed") || ieq(t, "delay") || ieq(t, "offset")) &&
                               i + 1 < lines.size() && !layer.bindings.empty()) {
                        const float value = parse_f(trim(lines[++i]), 0.f);
                        auto& b = layer.bindings.back();
                        if (ieq(t, "maxspeed")) b.maxspeed = value;
                        else if (ieq(t, "delay")) b.delay = value;
                        else b.offset = value;
                    }
                }
                obj.meshes[static_cast<std::size_t>(mesh_i)].animations.push_back(std::move(layer));
                continue;
            }
            if (ieq(tok, "[visible]") && i + 2 < lines.size()) {
                ScoVisibleLink v{};
                v.var_name = trim(lines[i + 1]);
                v.value = parse_f(trim(lines[i + 2]), 1.f);
                v.mesh_index = mesh_i;
                obj.visible_links.push_back(std::move(v));
                i += 2;
                continue;
            }
            if (ieq(tok, "[maplight]") && i + 7 < lines.size()) {
                // x y z  r g b  radius  — wiki: RGB 0..255 or 0..1; radius metres
                ScoMapLight ml{};
                ml.position.x = parse_f(trim(lines[i + 1]), 0.f);
                ml.position.y = parse_f(trim(lines[i + 2]), 0.f);
                ml.position.z = parse_f(trim(lines[i + 3]), 0.f);
                float r = parse_f(trim(lines[i + 4]), 255.f);
                float g = parse_f(trim(lines[i + 5]), 255.f);
                float b = parse_f(trim(lines[i + 6]), 255.f);
                if (r > 1.f || g > 1.f || b > 1.f) {
                    r /= 255.f; g /= 255.f; b /= 255.f;
                }
                ml.color.x = r; ml.color.y = g; ml.color.z = b;
                ml.radius = parse_f(trim(lines[i + 7]), 10.f);
                obj.maplights.push_back(ml);
                i += 7;
                continue;
            }
            if (ieq(tok, "[light_enh]") || ieq(tok, "[light_enh_2]")) {
                ScoLightEnh L{};
                L.enh2 = ieq(tok, "[light_enh_2]");
                std::vector<std::string> vals;
                for (std::size_t j = i + 1; j < lines.size() && vals.size() < 24; ++j) {
                    const auto t = trim(lines[j]);
                    if (t.empty()) continue;
                    if (!t.empty() && t.front() == '[') break;
                    vals.push_back(t);
                    i = j;
                }
                auto fv = [&](std::size_t k, float def) { return k < vals.size() ? parse_f(vals[k], def) : def; };
                auto col = [&](float r, float g, float b) {
                    if (r > 1.f || g > 1.f || b > 1.f) {
                        r /= 255.f;
                        g /= 255.f;
                        b /= 255.f;
                    }
                    L.color.x = (std::clamp)(r, 0.f, 1.f);
                    L.color.y = (std::clamp)(g, 0.f, 1.f);
                    L.color.z = (std::clamp)(b, 0.f, 1.f);
                };
                L.position.x = fv(0, 0.f);
                L.position.y = fv(1, 0.f);
                L.position.z = fv(2, 0.f);
                if (L.enh2) {
                    // pos, dir, up, omni, rotating, colour, size, cone inner, cone outer, variable, ...
                    L.direction.x = fv(3, 0.f);
                    L.direction.y = fv(4, 0.f);
                    L.direction.z = fv(5, -1.f);
                    L.omni = fv(9, 1.f) >= 0.5f;
                    col(fv(11, 255.f), fv(12, 255.f), fv(13, 255.f));
                    L.size = fv(14, 0.2f);
                    L.inner_cone = fv(15, 0.f);
                    L.outer_cone = fv(16, 0.f);
                    if (vals.size() > 17) L.variable = vals[17];
                } else {
                    // pos, colour, size, variable, 4 values, [texture]
                    col(fv(3, 1.f), fv(4, 1.f), fv(5, 1.f));
                    L.size = fv(6, 0.2f);
                    if (vals.size() > 7) L.variable = vals[7];
                }
                L.range = (std::max)(0.3f, L.size * 2.f);
                for (const auto& v : vals) {
                    if (v.size() > 4 && (v.find(".bmp") != std::string::npos || v.find(".BMP") != std::string::npos))
                        L.bitmap = v;
                }
                if (!vals.empty()) obj.lights.push_back(std::move(L));
                continue;
            }
            if (ieq(tok, "[traffic_lights_group]")) {
                ScoTrafficLightsGroup grp{};
                if (i + 1 < lines.size()) grp.cycle = parse_f(trim(lines[++i]), 0.f);
                ScoTrafficLight* cur = nullptr;
                for (std::size_t j = i + 1; j < lines.size(); ++j) {
                    const auto t = trim(lines[j]);
                    if (t.empty()) continue;
                    if (t.front() == '[' && !ieq(t, "[traffic_light]") && !ieq(t, "[phase]") &&
                        !ieq(t, "[approachdist]") && !ieq(t, "[stop]")) {
                        i = j - 1;
                        break;
                    }
                    if (ieq(t, "[traffic_light]") && j + 1 < lines.size()) {
                        ScoTrafficLight tl{};
                        tl.name = trim(lines[++j]);
                        grp.lights.push_back(std::move(tl));
                        cur = &grp.lights.back();
                        i = j;
                        continue;
                    }
                    if (ieq(t, "[phase]") && cur && j + 2 < lines.size()) {
                        ScoTrafficPhase ph{};
                        ph.phase = parse_i(trim(lines[j + 1]), 0);
                        ph.duration = parse_f(trim(lines[j + 2]), 1.f);
                        cur->phases.push_back(ph);
                        j += 2;
                        i = j;
                        continue;
                    }
                    if (ieq(t, "[approachdist]") && cur && j + 1 < lines.size()) {
                        cur->approach_dist = parse_f(trim(lines[j + 1]), 0.f);
                        ++j;
                        i = j;
                        continue;
                    }
                    if (ieq(t, "[stop]") && cur && j + 2 < lines.size()) {
                        ScoTrafficStop st{};
                        st.path_index = parse_i(trim(lines[j + 1]), 0);
                        st.distance = parse_f(trim(lines[j + 2]), 0.f);
                        cur->stops.push_back(st);
                        j += 2;
                        i = j;
                        continue;
                    }
                    i = j;
                }
                if (!grp.lights.empty() || grp.cycle > 0.f) obj.traffic_lights_group = std::move(grp);
                continue;
            }
        }
    }

    log::info("sco load " + omsi::path_utf8(sco_path.filename()) + " paths=" + std::to_string(obj.paths.size()) +
              " meshes=" + std::to_string(obj.meshes.size()) +
              " strings=" + std::to_string(obj.string_var_names.size()) +
              " scripts=" + std::to_string(obj.script_files.size()) +
              " lights=" + std::to_string(obj.lights.size()) +
              " texttex=" + std::to_string(obj.text_textures.size()) +
              (obj.has_mesh_anims() ? " newanim" : "") +
              (obj.special == ScoSpecial::HelpArrow ? " helparrow" : "") +
              (obj.traffic_lights_group ? " tl_group" : "") +
              (!obj.crossing_height_deformation_mesh.empty()
                   ? (" crossing_h=" + obj.crossing_height_deformation_mesh)
                   : ""));
    apply_crossing_height_to_sco(obj);
    auto stored = std::make_unique<SceneryObject>(std::move(obj));
    const SceneryObject* ptr = stored.get();
    {
        std::lock_guard lock(g_sco_mu);
        const auto [it, inserted] = g_sco_cache.emplace(key, std::move(stored));
        if (!inserted) return it->second.get();
    }
    return ptr;
}


void preload_scos(const std::vector<std::filesystem::path>& paths, const bool hdd_mode) {
    std::vector<std::filesystem::path> unique;
    unique.reserve(paths.size());
    std::unordered_set<std::string> seen;
    seen.reserve(paths.size() * 2);
    for (const auto& p : paths) {
        if (p.empty()) continue;
        // lexically_normal is pure string work (no disk) — avoid weakly_canonical/resolve.
        const auto k = sco_key(p);
        if (!seen.insert(k).second) continue;
        unique.push_back(p);
    }
    // Alphabetical order ≈ directory locality on HDD (same folder runs together).
    std::sort(unique.begin(), unique.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return sco_key(a) < sco_key(b);
    });
    if (unique.empty()) return;
    if (hdd_mode || unique.size() == 1) {
        for (const auto& p : unique) (void)load_sco(p);
        return;
    }
    // SSD / many cores: still sorted so cache lines of the same folder warm together.
    std::for_each(std::execution::par, unique.begin(), unique.end(), [](const std::filesystem::path& p) {
        try {
            (void)load_sco(p);
        } catch (...) {
        }
    });
}

} // namespace omsi::sco
