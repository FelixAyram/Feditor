#include "beamng/bng_level.hpp"

#include "beamng/bng_vfs.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <random>

namespace bng {

namespace {
std::string folder_of(const std::string& p) {
    const auto k = p.find_last_of('/');
    return k == std::string::npos ? std::string() : p.substr(0, k);
}
std::string leaf_of(const std::string& p) {
    const auto k = p.find_last_of('/');
    return k == std::string::npos ? p : p.substr(k + 1);
}
void read_vec3(const JsonValue& v, float out[3], bool& has) {
    if (v.is_array() && v.size() >= 3) {
        for (int i = 0; i < 3; ++i) out[i] = v[static_cast<std::size_t>(i)].as_float();
        has = true;
    } else if (v.is_string()) { // "x y z" (old Torque style)
        if (std::sscanf(v.as_string().c_str(), "%f %f %f", &out[0], &out[1], &out[2]) == 3) has = true;
    } else if (v.is_number()) { // uniform scale
        out[0] = out[1] = out[2] = v.as_float();
        has = true;
    }
}
// Torque/BeamNG serialize the 9 floats COLUMN-major (array[col*3+row]); Mat3 is row-major, so the transpose
// is stored. Verified against real data: the pieces of a fence row (eca_bld_wood_fence_*, eca_bld_fence_end)
// lie along the local X axis of their placement matrix, and only the column-major reading keeps local X on the
// line between consecutive pieces (row-major gets it right in 698 of 1482 same-heading pairs, column-major in
// 1266; prefab.json children: 32 vs 827). Reading it as row-major mirrored every rotated object (the sign_2way
// in east_coast_usa came out rotated ~46 deg the wrong way).
void read_mat3(const JsonValue& v, Mat3& out, bool& has) {
    float m[9];
    const bool got = [&] {
        if (v.is_array() && v.size() >= 9) {
            for (int i = 0; i < 9; ++i) m[i] = v[static_cast<std::size_t>(i)].as_float();
            return true;
        }
        if (v.is_string())
            return std::sscanf(v.as_string().c_str(), "%f %f %f %f %f %f %f %f %f", &m[0], &m[1], &m[2], &m[3],
                               &m[4], &m[5], &m[6], &m[7], &m[8]) == 9;
        return false;
    }();
    if (!got) return;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[static_cast<std::size_t>(r * 3 + c)] = m[c * 3 + r]; // column-major -> row-major
    has = true;
}
// Kept for callers that already hold a column-major matrix (forest tools / editor round-trips).
Mat3 transpose(const Mat3& m) { return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]}; }

std::string lower_ascii(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}
bool contains_ci(std::string_view hay, std::string_view needle) {
    return lower_ascii(hay).find(lower_ascii(needle)) != std::string::npos;
}
// info.json writes "spawnPoints" as an array; some mods write an object keyed by id. Entry keys vary too
// (objectname / objectName / name), and playerStartPoints is used the same way by older levels.
void read_spawn_list(const JsonValue& v, std::vector<SpawnPointInfo>& out) {
    auto add = [&out](const JsonValue& e, const std::string& key_hint) {
        if (!e.is_object()) return;
        SpawnPointInfo s;
        for (const char* k : {"objectname", "objectName", "name"}) {
            s.object_name = e.get(k).as_string();
            if (!s.object_name.empty()) break;
        }
        s.translation_id = e.get("translationId").as_string();
        if (s.translation_id.empty()) s.translation_id = e.get("translationID").as_string();
        s.preview = e.get("preview").as_string();
        if (s.object_name.empty() &&
            key_hint.find_first_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos)
            s.object_name = key_hint; // dictionary key names the spawn object
        if (s.object_name.empty()) return;
        for (const auto& o : out)
            if (o.object_name == s.object_name) return;
        out.push_back(std::move(s));
    };
    if (v.is_array()) {
        for (const auto& e : v.items()) add(e, std::string());
    } else if (v.is_object()) {
        for (const auto& [k, e] : v.members()) add(e, k);
    }
}
} // namespace

// ── info / discovery ───────────────────────────────────────────────────────────────────────────────────────────
std::vector<LevelInfo> discover_levels(const Vfs& vfs) {
    std::vector<LevelInfo> out;
    for (const auto& p : vfs.list("/levels/", "/info.json")) {
        // only /levels/<name>/info.json
        const std::string rest = p.substr(std::strlen("/levels/"));
        if (std::count(rest.begin(), rest.end(), '/') != 1) continue;
        LevelInfo li;
        li.dir = folder_of(p);
        const VfsEntry* e = vfs.entry(p);
        li.name = leaf_of(folder_of(e ? e->real_name : p));
        if (e && e->mount < vfs.mounts().size()) li.source = vfs.mounts()[e->mount].source.string();
        if (auto t = vfs.read_text(p)) {
            JsonValue j;
            if (json_parse(*t, j).ok) {
                li.title = j.get("title").as_string();
                li.default_spawn = j.get("defaultSpawnPointName").as_string();
                for (const auto& pv : j.get("previews").items()) li.previews.push_back(pv.as_string());
                read_spawn_list(j.get("spawnPoints"), li.spawn_points);
                read_spawn_list(j.get("playerStartPoints"), li.spawn_points);
                const auto& sz = j.get("size");
                if (sz.is_array() && sz.size() >= 2) {
                    li.size[0] = sz[0].as_float();
                    li.size[1] = sz[1].as_float();
                }
            }
        }
        if (li.title.empty()) li.title = li.name;
        // a level folder needs a scene
        if (!vfs.exists(li.dir + "/main/items.level.json") && !vfs.exists(li.dir + "/main.level.json") &&
            !vfs.exists(li.dir + "/items.level.json"))
            continue;
        out.push_back(std::move(li));
    }
    std::sort(out.begin(), out.end(), [](const LevelInfo& a, const LevelInfo& b) { return a.name < b.name; });
    return out;
}

namespace {
// Reads one string field out of a single NDJSON line (no JsonValue tree: the scan only streams the lines).
std::string line_string_field(std::string_view line, std::string_view key) {
    const std::string pat = "\"" + std::string(key) + "\"";
    std::size_t p = line.find(pat);
    while (p != std::string_view::npos) {
        std::size_t q = p + pat.size();
        while (q < line.size() && (line[q] == ' ' || line[q] == '\t')) ++q;
        if (q < line.size() && line[q] == ':') {
            ++q;
            while (q < line.size() && (line[q] == ' ' || line[q] == '\t')) ++q;
            if (q >= line.size() || line[q] != '"') return std::string();
            std::string out;
            for (++q; q < line.size(); ++q) {
                if (line[q] == '\\' && q + 1 < line.size()) {
                    out.push_back(line[++q]);
                    continue;
                }
                if (line[q] == '"') break;
                out.push_back(line[q]);
            }
            return out;
        }
        p = line.find(pat, p + 1);
    }
    return std::string();
}
} // namespace

std::vector<std::string> scan_scene_spawns(const Vfs& vfs, const LevelInfo& info) {
    std::vector<std::string> files = vfs.list(info.dir + "/main/", "/items.level.json");
    if (files.empty()) { // level keeps the scene at its root instead of under main/
        for (const auto& p : vfs.list(info.dir + "/", "/items.level.json"))
            if (p.size() > info.dir.size() + 1 && p.compare(0, info.dir.size() + 1, info.dir + "/") == 0 &&
                p.find('/', info.dir.size() + 1) == std::string::npos)
                files.push_back(p);
    }
    std::vector<std::string> out;
    auto want = [](const std::string& cls, const std::string& name) {
        if (cls == "SpawnSphere" || cls == "PlayerSpawn" || cls == "BeamNGWorldSpawn") return true;
        if (cls == "TSStatic" && !name.empty()) return contains_ci(name, "spawn") || contains_ci(name, "playerstart");
        return false;
    };
    for (const auto& f : files) {
        auto txt = vfs.read_text(f);
        if (!txt) continue;
        const std::string_view buf(*txt);
        std::size_t pos = 0;
        while (pos < buf.size()) {
            const std::size_t nl = buf.find('\n', pos);
            const std::string_view line = buf.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
            pos = nl == std::string_view::npos ? buf.size() : nl + 1;
            if (line.find("\"class\"") == std::string_view::npos) continue;
            const std::string cls = line_string_field(line, "class");
            const std::string name = line_string_field(line, "name");
            if (name.empty() || !want(cls, name)) continue;
            if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
        }
    }
    return out;
}

// ── terrain ───────────────────────────────────────────────────────────────────────────────────────────────────
std::optional<TerrainData> read_ter(const std::vector<std::uint8_t>& b, std::string* err) {
    TerrainData t;
    if (b.size() < 5) {
        if (err) *err = "ter too small";
        return std::nullopt;
    }
    t.version = b[0];
    std::memcpy(&t.size, &b[1], 4);
    const std::size_t n = static_cast<std::size_t>(t.size) * t.size;
    std::size_t off = 5;
    if (t.size == 0 || t.size > 16384 || off + n * 2 + n > b.size()) {
        if (err) *err = "ter size mismatch";
        return std::nullopt;
    }
    t.heights.resize(n);
    std::memcpy(t.heights.data(), &b[off], n * 2);
    off += n * 2;
    t.layers.assign(b.begin() + static_cast<std::ptrdiff_t>(off), b.begin() + static_cast<std::ptrdiff_t>(off + n));
    off += n;
    // v7 files carry a layerTextureMap block of the same size before the names (detected by what follows).
    auto try_names = [&](std::size_t o, std::vector<std::string>& names) -> bool {
        if (o + 4 > b.size()) return false;
        std::uint32_t count = 0;
        std::memcpy(&count, &b[o], 4);
        o += 4;
        if (count > 256) return false;
        names.clear();
        for (std::uint32_t k = 0; k < count; ++k) {
            if (o >= b.size()) return false;
            const std::uint8_t len = b[o++];
            if (o + len > b.size()) return false;
            names.emplace_back(reinterpret_cast<const char*>(&b[o]), len);
            o += len;
        }
        return o == b.size();
    };
    if (!try_names(off, t.material_names) && !try_names(off + n, t.material_names)) {
        if (err) *err = "ter material names unreadable";
    }
    return t;
}

std::vector<std::uint8_t> write_ter(const TerrainData& t) {
    const std::size_t n = static_cast<std::size_t>(t.size) * t.size;
    std::vector<std::uint8_t> b;
    b.reserve(5 + n * 3 + 256);
    b.push_back(9); // always write the current format
    const auto* sz = reinterpret_cast<const std::uint8_t*>(&t.size);
    b.insert(b.end(), sz, sz + 4);
    const auto* h = reinterpret_cast<const std::uint8_t*>(t.heights.data());
    b.insert(b.end(), h, h + n * 2);
    b.insert(b.end(), t.layers.begin(), t.layers.end());
    const std::uint32_t count = static_cast<std::uint32_t>(t.material_names.size());
    const auto* c = reinterpret_cast<const std::uint8_t*>(&count);
    b.insert(b.end(), c, c + 4);
    for (const auto& nm : t.material_names) {
        b.push_back(static_cast<std::uint8_t>((std::min)(nm.size(), std::size_t(255))));
        b.insert(b.end(), nm.begin(), nm.begin() + static_cast<std::ptrdiff_t>((std::min)(nm.size(), std::size_t(255))));
    }
    return b;
}

bool TerrainData::contains(float wx, float wy) const {
    const float ext = static_cast<float>(size) * square_size;
    return wx >= origin[0] && wy >= origin[1] && wx <= origin[0] + ext && wy <= origin[1] + ext;
}

float TerrainData::sample_world(float wx, float wy) const {
    if (size == 0 || heights.empty()) return origin[2];
    const float fx = (std::clamp)((wx - origin[0]) / square_size, 0.f, static_cast<float>(size - 1));
    const float fy = (std::clamp)((wy - origin[1]) / square_size, 0.f, static_cast<float>(size - 1));
    const auto x0 = static_cast<std::uint32_t>(fx), y0 = static_cast<std::uint32_t>(fy);
    const std::uint32_t x1 = (std::min)(x0 + 1, size - 1), y1 = (std::min)(y0 + 1, size - 1);
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    const float h00 = height_at_sample(x0, y0), h10 = height_at_sample(x1, y0);
    const float h01 = height_at_sample(x0, y1), h11 = height_at_sample(x1, y1);
    return (h00 * (1 - tx) + h10 * tx) * (1 - ty) + (h01 * (1 - tx) + h11 * tx) * ty;
}

// ── materials ─────────────────────────────────────────────────────────────────────────────────────────────────
namespace {
std::string first_str(const JsonValue& o, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        const auto& v = o.get(k);
        if (v.is_string() && !v.as_string().empty()) return v.as_string();
    }
    return {};
}
void read_distances(const JsonValue& v, float out[4]) {
    if (v.is_array())
        for (std::size_t i = 0; i < 4 && i < v.size(); ++i) out[i] = v[i].as_float();
}
} // namespace

// Public form of the scene-object reader (the parser test locks the column-major convention down with it).
Mat3 mat3_from_rotation_matrix(const JsonValue& v, bool& has) {
    Mat3 out = mat3_identity();
    has = false;
    read_mat3(v, out, has);
    return out;
}

std::string material_base_name(const std::string& n) {
    std::string s;
    s.reserve(n.size());
    for (const char c : n) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    bool changed = true;
    while (changed && !s.empty()) {
        changed = false;
        for (const char* suf : {"_lod", "-lod", ".lod", "_far", "-far", "_near", "-near", "_low", "_high",
                                "_impostor", "_dist", "_lo", "_hi", "lod"}) {
            const std::size_t l = std::strlen(suf);
            if (s.size() > l && s.compare(s.size() - l, l, suf) == 0) {
                s.resize(s.size() - l);
                changed = true;
            }
        }
        if (changed) continue;
        std::size_t e = s.size();
        while (e > 0 && (std::isdigit(static_cast<unsigned char>(s[e - 1])) || s[e - 1] == '_' || s[e - 1] == '-' ||
                         s[e - 1] == '.' || s[e - 1] == ' '))
            --e;
        if (e != s.size()) {
            s.resize(e);
            changed = true;
        }
    }
    return s;
}

void load_materials_under(const Vfs& vfs, std::string_view dir, Level& lvl) {
    auto files = vfs.list(dir, "materials.json");
    for (const auto& f : files) {
        auto t = vfs.read_text(f);
        if (!t) continue;
        JsonValue j;
        if (!json_parse(*t, j).ok || !j.is_object()) {
            lvl.warnings.push_back("materials no legible: " + f);
            continue;
        }
        const std::string base = folder_of(f);
        for (const auto& [key, m] : j.members()) {
            std::string cls = m.get("class").as_string();
            for (auto& ch : cls) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (cls == "material" || cls == "custommaterial") {
                Material mat;
                mat.name = m.get("name").as_string().empty() ? key : m.get("name").as_string();
                mat.map_to = m.get("mapTo").as_string();
                mat.file = f;
                const auto& st = m.get("Stages");
                const JsonValue& s0 = st.is_array() && st.size() ? st[0] : m;
                auto tex = [&](std::initializer_list<const char*> keys) {
                    std::string p = first_str(s0, keys);
                    if (p.empty()) return p;
                    std::string r = vfs.resolve_texture(p, base);
                    return r.empty() ? Vfs::normalize(p) : r;
                };
                mat.stage0.color_map = tex({"baseColorMap", "colorMap", "diffuseMap"});
                mat.stage0.normal_map = tex({"normalMap"});
                mat.stage0.roughness_map = tex({"roughnessMap", "specularMap"});
                mat.stage0.metallic_map = tex({"metallicMap"});
                mat.stage0.ao_map = tex({"ambientOcclusionMap", "aoMap"});
                mat.stage0.opacity_map = tex({"opacityMap"});
                mat.stage0.emissive_map = tex({"emissiveMap"});
                mat.stage0.detail_map = tex({"detailMap", "detailBaseColorMap"});
                mat.stage0.detail_normal_map = tex({"detailNormalMap"});
                if (const auto& dsv = s0.get("detailScale"); dsv.is_array() && dsv.size() >= 2) {
                    mat.stage0.detail_scale[0] = dsv[0].as_float(1.f);
                    mat.stage0.detail_scale[1] = dsv[1].as_float(1.f);
                }
                if (const auto& dv = s0.get("detailDistances"); dv.is_array() && dv.size() >= 2) {
                    mat.stage0.detail_far = dv[dv.size() - 1].as_float(0.f); // far fade edge (m)
                }
                mat.stage0.vert_color = s0.get("vertColor").as_bool();
                const auto& bc = s0.get("baseColorFactor").is_array() ? s0.get("baseColorFactor") : s0.get("diffuseColor");
                if (bc.is_array())
                    for (std::size_t i = 0; i < 4 && i < bc.size(); ++i) mat.stage0.base_color[i] = bc[i].as_float(1.f);
                mat.stage0.roughness = static_cast<float>(s0.get("roughnessFactor").as_number_lenient(1.0));
                mat.stage0.metallic = static_cast<float>(s0.get("metallicFactor").as_number_lenient(0.0));
                mat.alpha_test = m.get("alphaTest").as_bool();
                mat.alpha_ref = static_cast<int>(m.get("alphaRef").as_number_lenient(127));
                mat.stage0.opacity_factor = static_cast<float>(
                    s0.get("opacityFactor").as_number_lenient(m.get("opacityFactor").as_number_lenient(1.0)));
                mat.double_sided = m.get("doubleSided").as_bool();
                mat.translucent = m.get("translucent").as_bool();
                // Mods (and Torque-exported materials) also keep the stage state inside Stages[0]: read the
                // flags there when the root does not carry them, otherwise leaves / chainlink come out opaque.
                auto flag_root_then_stage = [&](const char* k, bool def) {
                    if (!m.get(k).is_null()) return m.get(k).as_bool();
                    if (!s0.get(k).is_null()) return s0.get(k).as_bool();
                    return def;
                };
                mat.alpha_test = flag_root_then_stage("alphaTest", mat.alpha_test);
                mat.double_sided = flag_root_then_stage("doubleSided", mat.double_sided);
                mat.translucent = flag_root_then_stage("translucent", mat.translucent);
                if (m.get("alphaRef").is_null() && !s0.get("alphaRef").is_null())
                    mat.alpha_ref = static_cast<int>(s0.get("alphaRef").as_number_lenient(mat.alpha_ref));
                mat.translucent_blend_op = first_str(m, {"translucentBlendOp"});
                if (mat.translucent_blend_op.empty()) mat.translucent_blend_op = first_str(s0, {"translucentBlendOp"});
                for (auto& ch : mat.translucent_blend_op)
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                // A declared blend op (Add/SrcAlpha/…) is transparency even when `translucent` is absent.
                if (!mat.translucent_blend_op.empty()) mat.translucent = true;
                mat.raw = m;
                if (!mat.map_to.empty()) lvl.material_by_map_to[mat.map_to] = mat.name;
                if (const std::string mb = material_base_name(mat.name); !mb.empty())
                    lvl.material_by_base.emplace(mb, mat.name); // first definition wins (later files override by name)
                lvl.materials[mat.name] = std::move(mat);
            } else if (cls == "terrainmaterial") {
                TerrainMaterialDef tm;
                tm.key = key;
                tm.internal_name = m.get("internalName").as_string();
                auto tex = [&](const char* k) {
                    std::string p = m.get(k).as_string();
                    if (p.empty()) return p;
                    std::string r = vfs.resolve_texture(p, base);
                    return r.empty() ? Vfs::normalize(p) : r;
                };
                tm.base_color = tex("baseColorBaseTex");
                tm.base_normal = tex("normalBaseTex");
                tm.macro_color = tex("baseColorMacroTex");
                tm.macro_normal = tex("normalMacroTex");
                tm.detail_color = tex("baseColorDetailTex");
                tm.detail_normal = tex("normalDetailTex");
                tm.roughness_base = tex("roughnessBaseTex");
                tm.ao_base = tex("aoBaseTex");
                tm.height_base = tex("heightBaseTex");
                tm.detail_size = static_cast<float>(m.get("baseColorDetailSize").as_number_lenient(m.get("detailSize").as_number_lenient(2.0)));
                tm.macro_size = static_cast<float>(m.get("baseColorMacroSize").as_number_lenient(60.0));
                tm.base_size = static_cast<float>(m.get("baseColorBaseTexSize").as_number_lenient(512.0));
                tm.detail_strength = static_cast<float>(m.get("baseColorDetailStrength").as_number_lenient(1.0));
                tm.macro_strength = static_cast<float>(m.get("baseColorMacroStrength").as_number_lenient(0.5));
                read_distances(m.get("detailDistances"), tm.detail_distances);
                read_distances(m.get("macroDistances"), tm.macro_distances);
                tm.raw = m;
                // replace an earlier definition with the same internal name
                auto it = std::find_if(lvl.terrain_materials.begin(), lvl.terrain_materials.end(),
                                       [&](const TerrainMaterialDef& d) { return d.internal_name == tm.internal_name; });
                if (it != lvl.terrain_materials.end()) *it = std::move(tm);
                else lvl.terrain_materials.push_back(std::move(tm));
            }
        }
    }
}

// ── decals ────────────────────────────────────────────────────────────────────────────────────────────────────
void load_decals(const Vfs& vfs, const LevelInfo& info, Level& lvl) {
    // Types first: art/decals/managedDecalData.json holds material, atlas grid and the UV rects.
    if (auto t = vfs.read_text(info.dir + "/art/decals/managedDecalData.json")) {
        JsonValue j;
        if (json_parse(*t, j).ok) {
            for (const auto& [key, d] : j.members()) {
                if (!d.is_object()) continue;
                const std::string cls = d.get("class").as_string();
                if (!cls.empty() && cls != "DecalData") continue; // "header" and friends
                DecalType dt;
                dt.name = d.get("name").as_string().empty() ? key : d.get("name").as_string();
                dt.material = d.get("material").as_string();
                dt.tex_rows = (std::max)(1, d.get("texRows").as_int(1));
                dt.tex_cols = (std::max)(1, d.get("texCols").as_int(1));
                dt.size = static_cast<float>(d.get("size").as_number_lenient(1.0));
                dt.render_priority = static_cast<float>(d.get("renderPriority").as_number_lenient(1.0));
                for (const auto& c : d.get("textureCoords").items()) {
                    std::array<float, 4> r{0.f, 0.f, 1.f, 1.f};
                    if (c.is_array())
                        for (std::size_t k = 0; k < 4 && k < c.size(); ++k) r[k] = c[k].as_float();
                    dt.texture_coords.push_back(r);
                }
                if (dt.texture_coords.empty()) dt.texture_coords.push_back({0.f, 0.f, 1.f, 1.f});
                dt.raw = d;
                lvl.decal_type_by_name[key] = static_cast<int>(lvl.decal_types.size());
                lvl.decal_types.push_back(std::move(dt));
            }
        }
    }
    auto txt = vfs.read_text(info.dir + "/main.decals.json");
    if (!txt) return;
    JsonValue j;
    if (!json_parse(*txt, j).ok) {
        lvl.warnings.push_back("decals no legible: " + info.dir + "/main.decals.json");
        return;
    }
    // instances[type] = [rectIdx, size, renderPriority, px, py, pz, nx, ny, nz, tx, ty, tz, uid]
    for (const auto& [type_name, list] : j.get("instances").members()) {
        auto it = lvl.decal_type_by_name.find(type_name);
        if (it == lvl.decal_type_by_name.end()) { // referenced but not declared: 1x1 atlas, no material
            DecalType dt;
            dt.name = type_name;
            dt.texture_coords.push_back({0.f, 0.f, 1.f, 1.f});
            lvl.decal_type_by_name[type_name] = static_cast<int>(lvl.decal_types.size());
            lvl.decal_types.push_back(std::move(dt));
            it = lvl.decal_type_by_name.find(type_name);
        }
        const DecalType& dty = lvl.decal_types[static_cast<std::size_t>(it->second)];
        for (const auto& a : list.items()) {
            if (!a.is_array() || a.size() < 13) continue;
            DecalInstance di;
            di.id = static_cast<std::uint32_t>(lvl.decals.size());
            di.type = it->second;
            const int rect = a[0].as_int();
            di.rect = rect >= 0 && static_cast<std::size_t>(rect) < dty.texture_coords.size() ? rect : 0;
            di.size = a[1].as_float(dty.size);
            if (!(di.size > 0.f)) di.size = dty.size > 0.f ? dty.size : 1.f;
            di.render_priority = a[2].as_float();
            for (int k = 0; k < 3; ++k) {
                di.pos[k] = a[3 + static_cast<std::size_t>(k)].as_float();
                di.normal[k] = a[6 + static_cast<std::size_t>(k)].as_float();
                di.tangent[k] = a[9 + static_cast<std::size_t>(k)].as_float();
            }
            di.uid = static_cast<std::uint32_t>(a[12].as_number_lenient(0.0));
            lvl.decals.push_back(di);
        }
    }
}

const Material* Level::material_for(const std::string& n) const {
    if (auto it = materials.find(n); it != materials.end()) return &it->second;
    if (auto it = material_by_map_to.find(n); it != material_by_map_to.end())
        if (auto jt = materials.find(it->second); jt != materials.end()) return &jt->second;
    // Per-LOD material names: shape LODs ship their own variant ("<mat>_lod", "lod_<mat>", "<mat>_far", case
    // differences). When that name is not in the level, fall back to the base name so the LOD still gets the
    // colour map, alphaTest and doubleSided of the real material instead of drawing flat/white up close.
    const std::string base = material_base_name(n);
    if (!base.empty() && base != n) {
        if (auto it = material_by_base.find(base); it != material_by_base.end())
            if (auto jt = materials.find(it->second); jt != materials.end()) return &jt->second;
        if (auto it = materials.find(base); it != materials.end()) return &it->second;
    }
    return nullptr;
}

const SceneObject* Level::find_named(std::string_view n) const {
    if (n.empty()) return nullptr;
    for (const auto& o : objects)
        if (!o.deleted && o.name == n) return &o;
    const std::string want = lower_ascii(n); // mods disagree on case with info.json
    for (const auto& o : objects)
        if (!o.deleted && o.name.size() == want.size() && lower_ascii(o.name) == want) return &o;
    return nullptr;
}

// ── scene ─────────────────────────────────────────────────────────────────────────────────────────────────────
std::optional<Level> load_level(const Vfs& vfs, const LevelInfo& info, LoadProgress* progress) {
    Level lvl;
    lvl.info = info;
    auto stage = [&](const char* s, float f) {
        if (progress) {
            progress->stage = s;
            progress->fraction = f;
        }
    };
    stage("escena", 0.05f);
    const std::string main_dir = info.dir + "/main/";
    auto files = vfs.list(main_dir, "/items.level.json");
    if (files.empty()) {
        // no items under main/: some levels keep items.level.json at the level root
        for (const auto& p : vfs.list(info.dir + "/", "/items.level.json"))
            if (p.size() > info.dir.size() + 1 && p.compare(0, info.dir.size() + 1, info.dir + "/") == 0 &&
                p.find('/', info.dir.size() + 1) == std::string::npos)
                files.push_back(p);
    }
    if (files.empty()) {
        lvl.warnings.push_back("el nivel no tiene items.level.json (ni en main/ ni en la raiz)");
        return lvl;
    }
    // Root file first, then by depth so parents exist before children (deterministic order).
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
        const auto da = std::count(a.begin(), a.end(), '/'), db = std::count(b.begin(), b.end(), '/');
        return da != db ? da < db : a < b;
    });
    // Parse every items.level.json in parallel.
    std::vector<std::future<std::vector<JsonValue>>> parsed;
    parsed.reserve(files.size());
    for (const auto& f : files) {
        parsed.push_back(std::async(std::launch::async, [&vfs, f]() {
            std::vector<JsonValue> rows;
            if (auto t = vfs.read_text(f)) rows = json_parse_lines(*t);
            return rows;
        }));
    }
    for (std::size_t fi = 0; fi < files.size(); ++fi) {
        const std::string& f = files[fi];
        const VfsEntry* fe = vfs.entry(f);
        const std::string real_file = fe ? fe->real_name : f;
        auto rows = parsed[fi].get();
        for (auto& row : rows) {
            SceneObject o;
            o.id = static_cast<std::uint32_t>(lvl.objects.size());
            o.cls = row.get("class").as_string();
            o.name = row.get("name").as_string();
            o.persistent_id = row.get("persistentId").as_string();
            o.parent = row.get("__parent").as_string();
            o.file = real_file;
            read_vec3(row.get("position"), o.pos, o.has_pos);
            read_mat3(row.get("rotationMatrix"), o.rot, o.has_rot);
            read_vec3(row.get("scale"), o.scale, o.has_scale);
            if (o.cls == "TSStatic") o.shape = vfs.resolve_shape(row.get("shapeName").as_string(), info.dir);
            o.raw = std::move(row);
            if (o.cls == "SimGroup") {
                SceneGroup g;
                g.name = o.name;
                g.parent = o.parent;
                g.file = folder_of(real_file) + "/" + o.name + "/items.level.json";
                g.self_object = static_cast<int>(o.id);
                const int gi = static_cast<int>(lvl.groups.size());
                lvl.group_by_name[o.name] = gi;
                lvl.groups.push_back(std::move(g));
            }
            auto git = lvl.group_by_name.find(o.parent);
            if (git != lvl.group_by_name.end()) {
                o.group_index = git->second;
                lvl.groups[static_cast<std::size_t>(git->second)].objects.push_back(o.id);
            }
            lvl.objects.push_back(std::move(o));
        }
    }
    for (std::size_t gi = 0; gi < lvl.groups.size(); ++gi) {
        auto it = lvl.group_by_name.find(lvl.groups[gi].parent);
        if (it != lvl.group_by_name.end()) lvl.groups[static_cast<std::size_t>(it->second)].children.push_back(static_cast<int>(gi));
    }

    // Terrain
    stage("terreno", 0.35f);
    for (auto& o : lvl.objects) {
        if (o.cls != "TerrainBlock") continue;
        const std::string tf = o.raw.get("terrainFile").as_string();
        std::string path = Vfs::normalize(tf);
        if (!vfs.exists(path)) path = Vfs::normalize(info.dir + "/" + leaf_of(path));
        if (auto bytes = vfs.read(path)) {
            std::string err;
            if (auto t = read_ter(*bytes, &err)) {
                t->file = vfs.entry(path) ? vfs.entry(path)->real_name : path;
                std::memcpy(t->origin, o.pos, sizeof(t->origin));
                t->max_height = static_cast<float>(o.raw.get("maxHeight").as_number_lenient(0.0));
                t->square_size = static_cast<float>(o.raw.get("squareSize").as_number_lenient(1.0));
                t->object_id = o.id;
                lvl.terrain = std::move(*t);
            }
            if (!err.empty()) lvl.warnings.push_back("terreno: " + err);
        } else {
            lvl.warnings.push_back("no se encontro el terreno " + tf);
        }
        break;
    }

    // Materials (level folder). Shared /assets materials are looked up on demand by the renderer.
    stage("materiales", 0.55f);
    load_materials_under(vfs, info.dir, lvl);

    // Forest
    stage("forest", 0.75f);
    if (auto t = vfs.read_text(info.dir + "/art/forest/managedItemData.json")) {
        JsonValue j;
        if (json_parse(*t, j).ok) {
            for (const auto& [key, it] : j.members()) {
                ForestItemType ft;
                ft.name = key;
                ft.shape_file = vfs.resolve_shape(it.get("shapeFile").as_string(), info.dir);
                ft.radius = static_cast<float>(it.get("radius").as_number_lenient(1.0));
                ft.raw = it;
                lvl.forest_type_by_name[key] = static_cast<int>(lvl.forest_types.size());
                lvl.forest_types.push_back(std::move(ft));
            }
        }
    }
    {
        const auto ffiles = vfs.list(info.dir + "/forest/", ".forest4.json");
        std::vector<std::future<std::vector<JsonValue>>> fp;
        for (const auto& f : ffiles)
            fp.push_back(std::async(std::launch::async, [&vfs, f]() {
                std::vector<JsonValue> rows;
                if (auto t = vfs.read_text(f)) rows = json_parse_lines(*t);
                return rows;
            }));
        for (std::size_t k = 0; k < fp.size(); ++k) {
            for (const auto& row : fp[k].get()) {
                ForestInstance fi;
                fi.id = static_cast<std::uint32_t>(lvl.forest.size());
                const std::string type = row.get("type").as_string();
                auto it = lvl.forest_type_by_name.find(type);
                if (it == lvl.forest_type_by_name.end()) {
                    ForestItemType ft;
                    ft.name = type;
                    lvl.forest_type_by_name[type] = static_cast<int>(lvl.forest_types.size());
                    lvl.forest_types.push_back(std::move(ft));
                    it = lvl.forest_type_by_name.find(type);
                }
                fi.type = it->second;
                bool h = false;
                read_vec3(row.get("pos"), fi.pos, h);
                Mat3 cols = mat3_identity();
                read_mat3(row.get("rotationMatrix"), cols, h); // already transposed: forest4 stores columns too
                fi.rot = cols;
                fi.scale = static_cast<float>(row.get("scale").as_number_lenient(1.0));
                fi.ctxid = row.get("ctxid").as_int();
                lvl.forest.push_back(fi);
            }
        }
    }
    stage("decals", 0.9f);
    load_decals(vfs, info, lvl);

    stage("listo", 1.f);
    return lvl;
}

bool find_spawn(const Level& lvl, std::string_view spawn_name, float out_pos[3], float* out_heading_rad) {
    const SceneObject* s = nullptr;
    if (!spawn_name.empty()) s = lvl.find_named(spawn_name);
    if (!s && !lvl.info.default_spawn.empty()) s = lvl.find_named(lvl.info.default_spawn);
    if (!s)
        for (const auto& o : lvl.objects)
            if (!o.deleted && (o.cls == "SpawnSphere" || o.cls == "PlayerSpawn" || o.cls == "BeamNGWorldSpawn")) {
                s = &o;
                break;
            }
    if (s) {
        std::memcpy(out_pos, s->pos, sizeof(float) * 3);
        if (out_heading_rad) *out_heading_rad = mat3_heading(s->rot);
        return true;
    }
    if (lvl.terrain) {
        const auto& t = *lvl.terrain;
        const float half = static_cast<float>(t.size) * t.square_size * 0.5f;
        out_pos[0] = t.origin[0] + half;
        out_pos[1] = t.origin[1] + half;
        out_pos[2] = t.sample_world(out_pos[0], out_pos[1]) + 2.f;
        if (out_heading_rad) *out_heading_rad = 0.f;
        return true;
    }
    out_pos[0] = out_pos[1] = out_pos[2] = 0.f;
    if (out_heading_rad) *out_heading_rad = 0.f;
    return false;
}

// ── pose math ─────────────────────────────────────────────────────────────────────────────────────────────────
void mat3_to_world4x4(const Mat3& r, const float s[3], const float p[3], float o[16]) {
    // Row-major 4x4, column vectors: world = R * diag(s) * local + p. `r` must already be the row-major pose
    // (mat3_heading / mat3_to_editor_euler conventions); the level parser produces it with the transpose taken,
    // because BeamNG serializes rotationMatrix column-major. Reading that array as rows here is what mirrored
    // every rotated object (sign_2way / eca_bld_wood_fence_c / eca_bld_bridge_* in east_coast_usa).
    o[0] = r[0] * s[0]; o[1] = r[1] * s[1]; o[2] = r[2] * s[2]; o[3] = p[0];
    o[4] = r[3] * s[0]; o[5] = r[4] * s[1]; o[6] = r[5] * s[2]; o[7] = p[1];
    o[8] = r[6] * s[0]; o[9] = r[7] * s[1]; o[10] = r[8] * s[2]; o[11] = p[2];
    o[12] = 0.f; o[13] = 0.f; o[14] = 0.f; o[15] = 1.f;
}

float mat3_heading(const Mat3& r) {
    // forward = R * (0,1,0) = column 1
    return std::atan2(-r[1], r[4]);
}

Mat3 mat3_from_euler_zyx(float rz, float ry, float rx) {
    const float cz = std::cos(rz), sz = std::sin(rz), cy = std::cos(ry), sy = std::sin(ry), cx = std::cos(rx), sx = std::sin(rx);
    // R = Rz * Ry * Rx
    return {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,
            sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,
            -sy,     cy * sx,                cy * cx};
}

// ── scene save ────────────────────────────────────────────────────────────────────────────────────────────────
JsonValue json_vec3(const float v[3]) {
    JsonValue a = JsonValue::make_array();
    for (int i = 0; i < 3; ++i) a.push_back(JsonValue::make_number(v[i]));
    return a;
}

JsonValue json_rotation_matrix(const Mat3& r) {
    JsonValue a = JsonValue::make_array();
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row) a.push_back(JsonValue::make_number(r[static_cast<std::size_t>(row * 3 + c)]));
    return a;
}

std::string make_persistent_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> d;
    const std::uint64_t a = d(rng), b = d(rng);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-4%03x-%04x-%012llx", static_cast<unsigned>(a >> 32),
                  static_cast<unsigned>((a >> 16) & 0xffff), static_cast<unsigned>(a & 0x0fff),
                  static_cast<unsigned>(0x8000 | ((b >> 48) & 0x3fff)),
                  static_cast<unsigned long long>(b & 0xffffffffffffull));
    return buf;
}

std::uint32_t add_scene_object(Level& lvl, std::string cls, std::string parent, JsonValue raw) {
    SceneObject o;
    o.id = static_cast<std::uint32_t>(lvl.objects.size());
    o.cls = std::move(cls);
    o.parent = std::move(parent);
    if (!raw.is_object()) raw = JsonValue::make_object();
    o.name = raw.get("name").as_string();
    o.persistent_id = raw.get("persistentId").as_string();
    if (o.persistent_id.empty()) {
        o.persistent_id = make_persistent_id();
        raw.set("persistentId", JsonValue::make_string(o.persistent_id));
    }
    raw.set("class", JsonValue::make_string(o.cls));
    raw.set("__parent", JsonValue::make_string(o.parent));
    auto git = lvl.group_by_name.find(o.parent);
    if (git != lvl.group_by_name.end()) {
        o.group_index = git->second;
        auto& g = lvl.groups[static_cast<std::size_t>(git->second)];
        o.file = g.file;
        g.objects.push_back(o.id);
    } else {
        for (const auto& so : lvl.objects)
            if (!so.file.empty()) {
                o.file = so.file;
                break;
            }
    }
    o.raw = std::move(raw);
    o.dirty = true;
    lvl.objects.push_back(std::move(o));
    return lvl.objects.back().id;
}

namespace {
std::string path_utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}
// Part of a scene file path after "/levels/<level>/" (zip entry, VFS path or loose file on disk).
std::filesystem::path rel_in_level(const std::string& file, const std::string& level) {
    std::string p = file;
    std::replace(p.begin(), p.end(), '\\', '/');
    const std::string lp = lower_ascii(p);
    const std::string key = "levels/" + lower_ascii(level) + "/";
    const std::size_t at = lp.find(key);
    if (at != std::string::npos) return std::filesystem::u8path(p.substr(at + key.size()));
    return std::filesystem::u8path(leaf_of(p));
}
} // namespace

SceneSaveResult save_scene(Level& lvl, const std::filesystem::path& root) {
    SceneSaveResult res;
    // files with something to write, and their rows in load order (new rows were appended, so id order is file order)
    std::map<std::string, std::vector<std::uint32_t>> rows_of;
    std::map<std::string, bool> touched;
    for (const auto& o : lvl.objects) {
        if (o.file.empty()) continue;
        rows_of[o.file].push_back(o.id);
        if (o.dirty || o.deleted) touched[o.file] = true;
    }
    for (auto& [file, ids] : rows_of) {
        if (!touched.count(file)) continue;
        std::string text;
        for (const std::uint32_t id : ids) {
            SceneObject& o = lvl.objects[id];
            if (o.deleted) {
                ++res.removed;
                continue;
            }
            if (o.dirty) {
                if (o.has_pos || o.pos[0] != 0.f || o.pos[1] != 0.f || o.pos[2] != 0.f)
                    o.raw.set("position", json_vec3(o.pos));
                const bool ident_rot = o.rot == mat3_identity();
                if (o.has_rot || !ident_rot) o.raw.set("rotationMatrix", json_rotation_matrix(o.rot));
                const bool unit = o.scale[0] == 1.f && o.scale[1] == 1.f && o.scale[2] == 1.f;
                if (o.has_scale || !unit) o.raw.set("scale", json_vec3(o.scale));
                if (!o.name.empty()) o.raw.set("name", JsonValue::make_string(o.name));
                ++res.changed;
            }
            text += json_write_compact(o.raw);
            text += '\n';
            ++res.rows;
        }
        const std::filesystem::path dest = root / rel_in_level(file, lvl.info.name);
        std::error_code ec;
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec) {
            res.error = "no se pudo crear " + path_utf8(dest.parent_path()) + ": " + ec.message();
            return res;
        }
        if (std::filesystem::is_regular_file(dest, ec)) {
            std::filesystem::path bak = dest;
            bak += ".bak";
            std::filesystem::copy_file(dest, bak, std::filesystem::copy_options::overwrite_existing, ec);
        }
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            res.error = "no se pudo escribir " + path_utf8(dest);
            return res;
        }
        res.written.push_back(path_utf8(dest));
    }
    for (auto& o : lvl.objects) o.dirty = false;
    res.ok = true;
    return res;
}

} // namespace bng
