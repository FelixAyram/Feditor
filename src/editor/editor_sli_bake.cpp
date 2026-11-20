#include "omsi/editor_sli_bake.hpp"
#include "omsi/editor_tile_bake.hpp"
#include "omsi/logger.hpp"
#include "editor/editor_ground.hpp"
#include "omsi/map_attachment.hpp"
#include "omsi/sco_types.hpp"
#include "omsi/model_cfg_types.hpp"
#include "o3d/o3d_loader.hpp"
#include <atomic>
#include <mutex>
#include <sstream>
#include <thread>

#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/o3d_writer.hpp"
#include "omsi/spline_types.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace omsi::editor {
namespace {

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// OMSI .sco / .o3d expect backslash paths like texture\foo.bmp relative to the .sco folder.
std::string sco_texture_ref(const std::filesystem::path& tex_rel) {
    // SCO [matl] uses bare filename (cubo.sco / MergeMapTrees style)
    if (tex_rel.empty()) return "white.bmp";
    return tex_rel.filename().string();
}

std::filesystem::path sco_relative_path(const std::string& ref) {
    std::string s = ref;
    std::replace(s.begin(), s.end(), '\\', '/');
    return std::filesystem::path(s).lexically_normal();
}

// Returns the basename written under texture\ (may differ from tex_rel if resolved to .dds).
// Empty string if nothing was copied / resolved.
std::string copy_bake_texture(const std::filesystem::path& sli_path, const std::filesystem::path& tex_rel,
                              const std::filesystem::path& omsi_root, const std::filesystem::path& sco_dir,
                              std::unordered_set<std::string>& copied) {
    if (tex_rel.empty()) return {};
    const auto src = gfx::dx12::resolve_texture_path(sli_path, tex_rel.string(), omsi_root);
    if (!src || !std::filesystem::exists(*src)) {
        // Fall back: try bare filename under common folders already handled by resolve; keep name for material
        return sco_texture_ref(tex_rel);
    }
    // Critical: dest must use the REAL file extension (.dds when source is dds).
    // Writing shrubbery_03.dds bytes as Shrubbery_03.tga makes the GPU loader fail → white meshes.
    const std::string dest_name = src->filename().string();
    if (!copied.insert(dest_name).second) return dest_name;
    const auto dest = sco_dir / "texture" / dest_name;
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (!std::filesystem::exists(dest, ec)) {
        std::filesystem::copy_file(*src, dest, std::filesystem::copy_options::skip_existing, ec);
    }
    return dest_name;
}

} // namespace

std::filesystem::path unique_sli_bake_dir(const std::filesystem::path& parent, const std::string& base_name) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    auto candidate = parent / base_name;
    if (!std::filesystem::exists(candidate, ec)) return candidate;
    for (int n = 1; n < 100000; ++n) {
        candidate = parent / (base_name + " (" + std::to_string(n) + ")");
        if (!std::filesystem::exists(candidate, ec)) return candidate;
    }
    return parent / (base_name + "_" + std::to_string(static_cast<unsigned long long>(
                                           std::chrono::steady_clock::now().time_since_epoch().count())));
}

std::string sli_bake_folder_name(std::int32_t first_spline_id) {
    std::time_t tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_s(&tm, &tt);
    char ts[32]{};
    std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
    return "bake_sli_" + std::to_string(first_spline_id) + "_" + ts;
}

SliBakeResult bake_splines_to_sco(const std::vector<map::SplineInst>& splines,
                                  const std::filesystem::path& omsi_root,
                                  const std::filesystem::path& out_dir, const std::string& stem) {
    SliBakeResult r{};
    if (splines.empty()) {
        r.error = "sin splines";
        return r;
    }

    std::error_code ec;
    const auto model_dir = out_dir / "model";
    const auto texture_dir = out_dir / "texture";
    std::filesystem::create_directories(model_dir, ec);
    std::filesystem::create_directories(texture_dir, ec);

    o3d::WritableMesh mesh;
    std::unordered_map<std::string, std::uint16_t> mat_of;
    std::unordered_set<std::string> copied_tex;
    float xmin = 1.e30f, ymin = 1.e30f, zmin = 1.e30f;
    float xmax = -1.e30f, ymax = -1.e30f, zmax = -1.e30f;
    bool any_vert = false;

    for (const auto& sp : splines) {
        const auto def = spline::load_sli(sp.sli_path);
        if (!def) continue;
        // [onlyeditor] SLI: no mesh (paths written below)
        if (def->only_editor) continue;
        const auto parts = spline::build_spline_meshes(sp, *def, omsi_root, 0.f, std::nullopt);
        for (const auto& part : parts) {
            copy_bake_texture(sp.sli_path, part.texture_path, omsi_root, out_dir, copied_tex);
            const std::string tex_ref = sco_texture_ref(part.texture_path);
            auto it = mat_of.find(tex_ref);
            if (it == mat_of.end()) {
                o3d::Material m{};
                m.fixed.diffuse[0] = m.fixed.diffuse[1] = m.fixed.diffuse[2] = m.fixed.diffuse[3] = 1.f;
                m.fixed.power = 10.f;
                m.texture = tex_ref;
                it = mat_of.emplace(tex_ref, static_cast<std::uint16_t>(mesh.materials.size())).first;
                mesh.materials.push_back(std::move(m));
            }
            const auto mat = it->second;
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            for (const auto& v : part.vertices) {
                mesh.vertices.push_back(v);
                xmin = (std::min)(xmin, v.position[0]);
                ymin = (std::min)(ymin, v.position[1]);
                zmin = (std::min)(zmin, v.position[2]);
                xmax = (std::max)(xmax, v.position[0]);
                ymax = (std::max)(ymax, v.position[1]);
                zmax = (std::max)(zmax, v.position[2]);
                any_vert = true;
            }
            for (std::size_t i = 0; i + 2 < part.indices.size(); i += 3) {
                o3d::TriangleU32 t{};
                t.i0 = base + part.indices[i];
                t.i1 = base + part.indices[i + 1];
                t.i2 = base + part.indices[i + 2];
                t.material = mat;
                mesh.triangles.push_back(t);
            }
        }
    }
        bool any_path = false;
    for (const auto& sp2 : splines) {
        const auto d2 = spline::load_sli(sp2.sli_path);
        if (d2 && !d2->paths.empty()) { any_path = true; break; }
    }
    if (!any_vert && !any_path) {
        r.error = "malla vacia";
        return r;
    }
    if (!any_vert) {
        xmin = ymin = zmin = 1.e30f;
        xmax = ymax = zmax = -1.e30f;
        for (const auto& sp : splines) {
            xmin = (std::min)(xmin, sp.position.x);
            ymin = (std::min)(ymin, sp.position.y);
            zmin = (std::min)(zmin, sp.position.z);
            xmax = (std::max)(xmax, sp.position.x);
            ymax = (std::max)(ymax, sp.position.y);
            zmax = (std::max)(zmax, sp.position.z);
            if (sp.length > 0.05f) {
                const auto e = spline::evaluate_spline(sp, sp.length);
                xmin = (std::min)(xmin, e.x);
                ymin = (std::min)(ymin, e.y);
                zmin = (std::min)(zmin, e.z);
                xmax = (std::max)(xmax, e.x);
                ymax = (std::max)(ymax, e.y);
                zmax = (std::max)(zmax, e.z);
            }
        }
        r.cx = 0.5f * (xmin + xmax);
        r.cy = 0.5f * (ymin + ymax);
        r.cz = 0.5f * (zmin + zmax);
        o3d::Material m{};
        m.fixed.diffuse[0] = m.fixed.diffuse[1] = m.fixed.diffuse[2] = m.fixed.diffuse[3] = 1.f;
        m.fixed.power = 10.f;
        m.texture = "white.bmp";
        mesh.materials.push_back(m);
        const float h = 0.25f;
        const float p[8][3] = {{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
                               {-h, -h, h},  {h, -h, h},  {h, h, h},  {-h, h, h}};
        for (int i = 0; i < 8; ++i) {
            o3d::Vertex v{};
            v.position[0] = p[i][0];
            v.position[1] = p[i][1];
            v.position[2] = p[i][2];
            v.normal[2] = 1.f;
            mesh.vertices.push_back(v);
        }
        const std::uint32_t faces[][3] = {{0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6}, {0, 4, 5}, {0, 5, 1},
                                          {3, 2, 6}, {3, 6, 7}, {0, 3, 7}, {0, 7, 4}, {1, 5, 6}, {1, 6, 2}};
        for (const auto& f : faces) {
            o3d::TriangleU32 t{};
            t.i0 = f[0];
            t.i1 = f[1];
            t.i2 = f[2];
            t.material = 0;
            mesh.triangles.push_back(t);
        }
        any_vert = true;
    } else {
        r.cx = 0.5f * (xmin + xmax);
        r.cy = 0.5f * (ymin + ymax);
        r.cz = 0.5f * (zmin + zmax);
        for (auto& v : mesh.vertices) {
            v.position[0] -= r.cx;
            v.position[1] -= r.cy;
            v.position[2] -= r.cz;
        }
    }
    std::string use_stem = stem.empty() ? "bake_sli" : stem;
    for (int n = 0; n < 100000; ++n) {
        const std::string cand = (n == 0) ? use_stem : (use_stem + " (" + std::to_string(n) + ")");
        std::error_code fec;
        const auto o3d_c = out_dir / "model" / (cand + ".o3d");
        const auto sco_c = out_dir / (cand + ".sco");
        if (!std::filesystem::exists(o3d_c, fec) && !std::filesystem::exists(sco_c, fec)) {
            use_stem = cand;
            break;
        }
    }
    const std::string mesh_rel = "model\\" + use_stem + ".o3d";
    r.o3d_path = out_dir / sco_relative_path(mesh_rel);
    r.sco_path = out_dir / (use_stem + ".sco");
    if (!o3d::write_o3d_file(r.o3d_path, mesh)) {
        r.error = "no se pudo escribir o3d";
        return r;
    }

    std::ofstream sco(r.sco_path, std::ios::trunc);
    if (!sco) {
        r.error = "no se pudo escribir sco";
        return r;
    }
    sco << "[friendlyname]\nF4 bake " << splines.size() << " sli\n\n";
    sco << "[mesh]\n" << mesh_rel << "\n\n[absheight]\n\n";

    constexpr float kRad2Deg = 180.f / 3.14159265f;
    for (const auto& sp : splines) {
        const auto def = spline::load_sli(sp.sli_path);
        if (!def || def->paths.empty()) continue;
        for (const auto& p : def->paths) {
            // Parallel curve at lateral offset: R' = R - lateral, L' = L * |R'|/|R|.
            // Example: R=20, L=20, lateral=-5 → R'=25, L'=25 (outer circumference).
            float path_r = sp.radius;
            float path_len = (std::max)(sp.length, 0.05f);
            if (std::fabs(sp.radius) > 1e-4f) {
                path_r = sp.radius - p.lateral;
                const float scale = std::fabs(path_r) / std::fabs(sp.radius);
                path_len = (std::max)(0.05f, sp.length * scale);
            }
            // Path anchor at spline start (along=0); length/radius adjusted for offset.
            const auto s0 = spline::evaluate_spline(sp, 0.f, p.lateral, p.height);
            sco << (p.secondary ? "[path_2]\n" : "[path]\n");
            sco << (s0.x - r.cx) << "\n" << (s0.y - r.cy) << "\n" << (s0.z - r.cz) << "\n";
            sco << (s0.heading_rad * kRad2Deg) << "\n";
            sco << path_r << "\n" << path_len << "\n";
            sco << sp.start_grad << "\n" << sp.end_grad << "\n";
            sco << p.typ << "\n" << p.width << "\n" << p.direction << "\n0\n";
            if (sp.use_delta_height || sp.is_spline_h) sco << sp.delta_height << "\n";
            sco << "\n";
            ++r.path_count;
        }
    }
    r.vertex_count = static_cast<int>(mesh.vertices.size());
    return r;
}

} // namespace omsi::editor

// ---- tile bake (same TU so CMake does not need an extra source) ----
namespace omsi::editor {
namespace {

void prog_log(TileBakeProgress* p, std::string line) {
    if (!p) return;
    std::lock_guard<std::mutex> lock(p->log_mutex);
    p->log_lines.push_back(std::move(line));
    if (p->log_lines.size() > 400)
        p->log_lines.erase(p->log_lines.begin(), p->log_lines.begin() + 50);
}

std::string lower_ascii_str(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool name_has_carpark_tag(const sco::SceneryObject& sco) {
    return lower_ascii_str(sco.sco_path.filename().string()).find("carpark") != std::string::npos;
}

bool sco_has_texttexture(const sco::SceneryObject& sco) {
    return !sco.text_textures.empty();
}

bool sco_has_script(const sco::SceneryObject& sco) {
    return !sco.script_files.empty();
}

bool sco_excluded(const sco::SceneryObject* sco, std::string& reason) {
    if (!sco) {
        reason = "missing_sco";
        return true;
    }
    using S = sco::ScoSpecial;
    // Keep as free map objects (not merged into tile bake mesh) — MergeMapTrees style + busstop
    if (sco->special == S::TrafficLight) {
        reason = "trafficlight";
        return true;
    }
    if (sco->special == S::HelpArrow) {
        reason = "helparrow";
        return true;
    }
    if (name_has_carpark_tag(*sco)) {
        reason = "carpark_p";
        return true;
    }
    const auto low = lower_ascii_str(sco->sco_path.filename().string());
    if (low.find("entrypoint") != std::string::npos) {
        reason = "entrypoint";
        return true;
    }
    if (low.find("carpark") != std::string::npos) {
        reason = "carpark";
        return true;
    }
    if (low.find("busstop") != std::string::npos || low.find("bus_stop") != std::string::npos) {
        reason = "busstop";
        return true;
    }
    // Dynamic / scripted objects with text textures must stay as their own .sco
    if (sco_has_texttexture(*sco) && sco_has_script(*sco)) {
        reason = "texttexture_script";
        return true;
    }
    if (sco_has_texttexture(*sco)) {
        reason = "texttexture";
        return true;
    }
    if (sco_has_script(*sco) && !sco->script_files.empty()) {
        // keep pure-script helpers (nothing.osc on static mesh is OK to bake — only skip if texttex already handled)
    }
    return false;
}

// True when replace should leave this object on the map (same rules as sco_excluded keep set).
bool sco_should_keep_on_map(const sco::SceneryObject* sco, const std::filesystem::path& sco_path) {
    std::string reason;
    if (sco && sco_excluded(sco, reason)) {
        if (reason == "missing_sco") return false;
        return true;
    }
    const auto low = lower_ascii_str(sco_path.filename().string());
    if (low.find("entrypoint") != std::string::npos) return true;
    if (low.find("carpark") != std::string::npos) return true;
    if (low.find("busstop") != std::string::npos || low.find("bus_stop") != std::string::npos) return true;
    return false;
}

void mul_mat4_vec3(const float m[16], float x, float y, float z, float& ox, float& oy, float& oz) {
    ox = m[0] * x + m[4] * y + m[8] * z + m[12];
    oy = m[1] * x + m[5] * y + m[9] * z + m[13];
    oz = m[2] * x + m[6] * y + m[10] * z + m[14];
}

// MergeMapTrees _roty/_rotx/_rotz then order: rot(Y), pitch(X), bank(Z) in OMSI Y-up.
// Editor stores Z-up (x,y_horiz,z_height). Convert: Y-up (x,h,y) <-> Z-up (x,y,h).
void rotate_omsi_euler_yup(float x, float y, float z, float rot_deg, float pitch_deg, float bank_deg,
                           float& ox, float& oy, float& oz) {
    const float d = 3.14159265f / 180.f;
    // roty (heading)
    {
        const float r = rot_deg * d;
        const float c = std::cos(r), s = std::sin(r);
        const float nx = x * c + z * s;
        const float nz = -x * s + z * c;
        x = nx;
        z = nz;
    }
    // rotx (pitch)
    {
        const float r = pitch_deg * d;
        const float c = std::cos(r), s = std::sin(r);
        const float ny = y * c - z * s;
        const float nz = y * s + z * c;
        y = ny;
        z = nz;
    }
    // rotz (bank)
    {
        const float r = bank_deg * d;
        const float c = std::cos(r), s = std::sin(r);
        const float nx = x * c - y * s;
        const float ny = x * s + y * c;
        x = nx;
        y = ny;
    }
    ox = x;
    oy = y;
    oz = z;
}

// Instance pose: editor Z-up position + rotation_deg (x=rot/yaw, y=pitch, z=bank).
// Mesh verts: prepared to OMSI Z-up (same as GPU). Output stays Z-up.
void transform_instance_vertex_zup(float lx, float ly, float lz, const map::MapObjectInst& o,
                                   float origin_x, float origin_y, float& ox, float& oy, float& oz) {
    // Z-up local -> temporary Y-up for MergeMapTrees euler: (x,z,y)
    float yx = lx, yy = lz, yz = ly;
    float rx, ry, rz;
    rotate_omsi_euler_yup(yx, yy, yz, o.rotation_deg.x, o.rotation_deg.y, o.rotation_deg.z, rx, ry, rz);
    // MergeMapTrees stores (rx+tx, ry+tz, rz+ty) in Y-up file space; convert back to Z-up:
    // Y-up (x,h,y) -> Z-up (x,y,h)
    const float tx = o.position.x - origin_x;
    const float ty = o.position.y - origin_y;
    const float tz = o.position.z;
    ox = rx + tx;
    oy = rz + ty;
    oz = ry + tz;
}


struct TrafficLightSig {
    std::string name;
    float approach_dist = 0.f;
    std::vector<std::pair<int, float>> phases; // phase, duration
    std::string fingerprint() const {
        std::ostringstream os;
        for (const auto& ph : phases) os << ph.first << ':' << ph.second << ';';
        os << 'A' << approach_dist;
        return os.str();
    }
};

struct AccumMesh {
    o3d::WritableMesh mesh;
    std::unordered_map<std::string, int> mat_by_tex;
    std::unordered_set<std::string> alpha_keys;
    std::vector<std::string> matl_alpha_lines;
    std::vector<std::string> traffic_blocks; // legacy text blocks (unused if lights used)
    std::vector<TrafficLightSig> traffic_lights; // merged unique lights (order = use_traffic_light index)
    float traffic_cycle = 0.f;
    std::vector<sco::ScoLightEnh> lights; // [light_enh] / [light_enh_2] transformed to tile space
    std::vector<sco::AttachmentPoint> attachment_points; // [new_attachment] in tile space
    std::unordered_set<std::string> copied_tex;
    std::filesystem::path omsi_root;
    std::filesystem::path out_dir;
};

// Merge lights from one SCO group into acc; returns local->global index remap.
std::vector<int> merge_traffic_lights(AccumMesh& acc, const sco::ScoTrafficLightsGroup& g) {
    if (acc.traffic_cycle <= 0.f) acc.traffic_cycle = g.cycle;
    std::vector<int> remap;
    remap.reserve(g.lights.size());
    for (const auto& L : g.lights) {
        TrafficLightSig sig;
        sig.name = L.name.empty() ? "Main" : L.name;
        sig.approach_dist = L.approach_dist;
        for (const auto& ph : L.phases) sig.phases.emplace_back(ph.phase, ph.duration);
        const std::string fp = sig.fingerprint();
        int found = -1;
        for (int i = 0; i < static_cast<int>(acc.traffic_lights.size()); ++i) {
            if (acc.traffic_lights[static_cast<std::size_t>(i)].fingerprint() == fp) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            remap.push_back(found);
            continue;
        }
        // Unique name if same base name already used with different timings
        std::string base = sig.name;
        int n = 2;
        auto name_used = [&](const std::string& nm) {
            for (const auto& e : acc.traffic_lights)
                if (e.name == nm) return true;
            return false;
        };
        while (name_used(sig.name)) {
            sig.name = base + "_" + std::to_string(n++);
        }
        remap.push_back(static_cast<int>(acc.traffic_lights.size()));
        acc.traffic_lights.push_back(std::move(sig));
    }
    return remap;
}

int ensure_material(AccumMesh& acc, const std::string& tex_ref, const void*,
                    const std::filesystem::path& resolve_from) {
    std::string bare = std::filesystem::path(tex_ref).filename().string();
    if (!bare.empty()) {
        const std::string dest =
            copy_bake_texture(resolve_from, std::filesystem::path(bare), acc.omsi_root, acc.out_dir, acc.copied_tex);
        if (!dest.empty()) bare = dest;
    }
    if (bare.empty()) bare = "white.bmp";
    auto it = acc.mat_by_tex.find(bare);
    if (it != acc.mat_by_tex.end()) return it->second;
    o3d::Material om{};
    om.fixed.diffuse[0] = om.fixed.diffuse[1] = om.fixed.diffuse[2] = om.fixed.diffuse[3] = 1.f;
    om.fixed.power = 10.f;
    om.texture = bare;
    const int idx = static_cast<int>(acc.mesh.materials.size());
    acc.mesh.materials.push_back(std::move(om));
    acc.mat_by_tex[bare] = idx;
    return idx;
}

bool append_transformed_mesh(AccumMesh& acc, const o3d::NativeMesh& src, const map::MapObjectInst& pose,
                             const std::filesystem::path& resolve_from, std::string* err_out,
                             float origin_x = 0.f, float origin_y = 0.f) {
    // Same verts the GPU sees (decrypt + Y<->Z if 0x79)
    std::vector<o3d::Vertex> prepared;
    o3d::prepare_o3d_mesh_vertices_for_gpu(src, prepared);
    if (prepared.empty()) {
        if (err_out) *err_out = "empty_mesh";
        return false;
    }
    if (acc.mesh.vertices.size() + prepared.size() > 65535) {
        if (err_out) *err_out = "vertex_limit";
        return false;
    }

    // Materials
    const auto mats = src.parse_materials();
    std::vector<std::uint16_t> mat_remap;
    mat_remap.reserve(mats.size());
    for (const auto& m : mats) {
        std::string tex = m.texture;
        if (tex.empty()) tex = "white.bmp";
        // copy texture file (basename may become .dds)
        const std::string bare =
            copy_bake_texture(resolve_from, std::filesystem::path(tex), acc.omsi_root, acc.out_dir, acc.copied_tex);
        const std::string mat_tex = bare.empty() ? std::filesystem::path(tex).filename().string() : bare;
        auto it = acc.mat_by_tex.find(mat_tex);
        if (it == acc.mat_by_tex.end()) {
            o3d::Material om{};
            om.fixed.diffuse[0] = om.fixed.diffuse[1] = om.fixed.diffuse[2] = om.fixed.diffuse[3] = 1.f;
            om.fixed.power = 10.f;
            om.texture = mat_tex;
            const int idx = static_cast<int>(acc.mesh.materials.size());
            acc.mesh.materials.push_back(std::move(om));
            it = acc.mat_by_tex.emplace(mat_tex, idx).first;
        }
        mat_remap.push_back(static_cast<std::uint16_t>(it->second));
    }
    if (mat_remap.empty()) {
        if (acc.mesh.materials.empty()) {
            o3d::Material om{};
            om.fixed.diffuse[0] = om.fixed.diffuse[1] = om.fixed.diffuse[2] = om.fixed.diffuse[3] = 1.f;
            om.texture = "white.bmp";
            acc.mesh.materials.push_back(std::move(om));
        }
        mat_remap.push_back(0);
    }

    const auto base = static_cast<std::uint32_t>(acc.mesh.vertices.size());
    for (const auto& v : prepared) {
        o3d::Vertex nv = v;
        float ox, oy, oz;
        transform_instance_vertex_zup(v.position[0], v.position[1], v.position[2], pose, origin_x, origin_y, ox,
                                      oy, oz);
        nv.position[0] = ox;
        nv.position[1] = oy;
        nv.position[2] = oz;
        float nx, ny, nz;
        transform_instance_vertex_zup(v.normal[0], v.normal[1], v.normal[2], pose, 0.f, 0.f, nx, ny, nz);
        // normals: subtract translation (pose at 0) — use rotation only
        {
            float yx = v.normal[0], yy = v.normal[2], yz = v.normal[1];
            float rx, ry, rz;
            rotate_omsi_euler_yup(yx, yy, yz, pose.rotation_deg.x, pose.rotation_deg.y, pose.rotation_deg.z, rx, ry,
                                  rz);
            nx = rx;
            ny = rz;
            nz = ry;
        }
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-6f) {
            nx /= len;
            ny /= len;
            nz /= len;
        }
        nv.normal[0] = nx;
        nv.normal[1] = ny;
        nv.normal[2] = nz;
        acc.mesh.vertices.push_back(nv);
    }
    // Y-up <-> Z-up axis permutation in transform_instance_vertex_zup flips handedness;
    // reverse winding so front faces stay correct (same as SLI/tree strips which already look OK).
    auto emit_tri = [&](std::uint32_t i0, std::uint32_t i1, std::uint32_t i2, std::uint16_t mat) {
        o3d::TriangleU32 nt{};
        nt.i0 = base + i0;
        nt.i1 = base + i2;
        nt.i2 = base + i1;
        const int sm = static_cast<int>(mat);
        nt.material = static_cast<std::uint16_t>(
            (sm >= 0 && sm < static_cast<int>(mat_remap.size())) ? mat_remap[static_cast<std::size_t>(sm)]
                                                                : mat_remap.front());
        acc.mesh.triangles.push_back(nt);
    };
    if (src.long_triangle_indices) {
        for (const auto& tr : src.triangle_u32_span()) emit_tri(tr.i0, tr.i1, tr.i2, tr.material);
    } else {
        for (const auto& tr : src.triangle_u16_span()) emit_tri(tr.i0, tr.i1, tr.i2, tr.material);
    }
    return true;
}

void collect_matl_alpha(AccumMesh& acc, const sco::SceneryObject& sco) {
    for (const auto& mat : sco.materials) {
        if (mat.alpha_mode == model_cfg::MatlAlphaMode::opaque) continue;
        std::string key = mat.diffuse_texture.empty() ? mat.texture_key : mat.diffuse_texture;
        if (key.empty()) continue;
        {
            const std::string dest = copy_bake_texture(sco.sco_path, std::filesystem::path(key), acc.omsi_root,
                                                       acc.out_dir, acc.copied_tex);
            key = dest.empty() ? std::filesystem::path(key).filename().string() : dest;
        }
        const std::string uniq = key + "#" + std::to_string(static_cast<int>(mat.alpha_mode));
        if (!acc.alpha_keys.insert(uniq).second) continue;
        std::ostringstream os;
        os << "[matl]\n" << key << "\n0\n";
        os << "[matl_alpha]\n" << static_cast<int>(mat.alpha_mode) << "\n";
        acc.matl_alpha_lines.push_back(os.str());
    }
}

bool write_part_sco(const std::filesystem::path& sco_path, const std::filesystem::path& o3d_rel,
                    const AccumMesh& acc, const std::vector<sco::ScoPath>& paths) {
    std::ofstream sco(sco_path);
    if (!sco) return false;
    sco << "[friendlyname]\nTile bake part\n\n";
    // bare o3d filename (same folder layout as MergeMapTrees / user request)
    std::string mesh_ref = o3d_rel.filename().string();
    sco << "[mesh]\n" << mesh_ref << "\n\n";

    // Only alpha materials (bare filename)
    for (const auto& block : acc.matl_alpha_lines) sco << block << "\n";

    // Merged traffic_lights_group (lights ordered = use_traffic_light indices)
    if (!acc.traffic_lights.empty()) {
        sco << "[traffic_lights_group]\n" << acc.traffic_cycle << "\n";
        for (const auto& L : acc.traffic_lights) {
            sco << "[traffic_light]\n" << L.name << "\n";
            for (const auto& ph : L.phases)
                sco << "[phase]\n" << ph.first << "\n" << ph.second << "\n";
            sco << "[approachdist]\n" << L.approach_dist << "\n";
        }
        sco << "\n";
    } else {
        for (const auto& tb : acc.traffic_blocks) sco << tb << "\n";
    }

    // Paths (already in local space relative to mesh center)
    for (const auto& p : paths) {
        sco << "[path]\n";
        sco << p.x << "\n" << p.y << "\n" << p.z << "\n";
        sco << p.heading_deg << "\n" << p.radius << "\n" << p.length << "\n";
        sco << p.grad_start << "\n" << p.grad_end << "\n";
        sco << p.type << "\n" << p.width << "\n" << p.direction << "\n" << p.blinker << "\n";
        if (p.use_traffic_light >= 0)
            sco << "[use_traffic_light]\n" << p.use_traffic_light << "\n";
        sco << "\n";
    }

    // [light_enh] / [light_enh_2] (positions already in tile-local space)
    for (const auto& L : acc.lights) {
        sco << (L.enh2 ? "[light_enh_2]\n" : "[light_enh]\n");
        sco << L.position.x << "\n" << L.position.y << "\n" << L.position.z << "\n";
        sco << L.direction.x << "\n" << L.direction.y << "\n" << L.direction.z << "\n";
        if (L.enh2) {
            // OMSI light_enh_2: color often 0..255 after dir; write normalized*255 style ints
            sco << static_cast<int>(L.color.x * 255.f) << "\n"
                << static_cast<int>(L.color.y * 255.f) << "\n"
                << static_cast<int>(L.color.z * 255.f) << "\n";
            sco << L.range << "\n";
            if (!L.bitmap.empty()) sco << L.bitmap << "\n";
        } else {
            sco << L.color.x << "\n" << L.color.y << "\n" << L.color.z << "\n";
            sco << L.range << "\n";
        }
        sco << "\n";
    }

    // [new_attachment] — OMSI key form (attach_trans / attach_rot_*), not bare floats
    for (const auto& ap : acc.attachment_points) {
        sco << "[new_attachment]\n";
        if (std::fabs(ap.rot_x) > 1e-5f) sco << "attach_rot_x\n" << ap.rot_x << "\n";
        if (std::fabs(ap.rot_y) > 1e-5f) sco << "attach_rot_y\n" << ap.rot_y << "\n";
        if (std::fabs(ap.rot_z) > 1e-5f) sco << "attach_rot_z\n" << ap.rot_z << "\n";
        sco << "attach_trans\n";
        sco << ap.local_pos.x << "\n" << ap.local_pos.y << "\n" << ap.local_pos.z << "\n\n";
    }

    sco << "[complexity]\n0\n";
    return true;
}


struct CellWork {
    TileGridCell cell;
    std::vector<const map::MapObjectInst*> objects;
    std::vector<const map::SplineInst*> splines;
};

} // namespace

bool is_tile_bake_product_name(const std::string& name) {
    const auto low = lower_ascii_str(name);
    return low.find("tile_bake_") != std::string::npos || low.find("tilebake_") != std::string::npos ||
           low.find("_bake_part") != std::string::npos ||
           low.find("bake_tiles") != std::string::npos;
}

std::vector<TileGridCell> tile_bake_grid(int parts, float origin_x, float origin_y, float tile_size) {
    parts = (std::max)(kTileBakePartsMin, (std::min)(parts, kTileBakePartsMax));
    int nx = 1, ny = 1;
    switch (parts) {
    case 1: nx = 1; ny = 1; break;
    case 2: nx = 2; ny = 1; break;
    case 3: nx = 3; ny = 1; break;
    case 4: nx = 2; ny = 2; break;
    case 5: nx = 5; ny = 1; break;
    default: break;
    }
    std::vector<TileGridCell> cells;
    cells.reserve(static_cast<std::size_t>(nx * ny));
    const float dx = tile_size / static_cast<float>(nx);
    const float dy = tile_size / static_cast<float>(ny);
    for (int iy = 0; iy < ny; ++iy) {
        for (int ix = 0; ix < nx; ++ix) {
            TileGridCell c;
            c.ix = ix;
            c.iy = iy;
            c.min_x = origin_x + static_cast<float>(ix) * dx;
            c.min_y = origin_y + static_cast<float>(iy) * dy;
            c.max_x = c.min_x + dx;
            c.max_y = c.min_y + dy;
            cells.push_back(c);
        }
    }
    return cells;
}

TileBakeResult bake_tile(const TileBakeRequest& req, TileBakeProgress* progress) {
    TileBakeResult result;
    if (req.parts < kTileBakePartsMin || req.parts > kTileBakePartsMax) {
        result.error = "parts must be 1..5";
        return result;
    }
    int bake_hits = 0;
    for (const auto& o : req.objects) {
        if (is_tile_bake_product_name(o.sco_path.filename().string()) ||
            is_tile_bake_product_name(o.sco_path.parent_path().filename().string()))
            ++bake_hits;
    }
    if (bake_hits > 0 && bake_hits * 2 >= static_cast<int>(req.objects.size()) && !req.objects.empty()) {
        result.error = "tile already appears baked (tile_bake_* objects present)";
        prog_log(progress, result.error);
        return result;
    }

    auto cells = tile_bake_grid(req.parts, req.tile_origin_x, req.tile_origin_y, req.tile_size);
    if (progress) {
        progress->parts_total = static_cast<int>(cells.size());
        progress->parts_done = 0;
        progress->percent = 0;
        progress->finished = false;
        progress->success = false;
    }

    std::error_code ec;
    // Fixed layout: Sceneryobjects/bake_tiles/<map>/  + model/ + texture/ + fbx/
    const auto out_dir = req.out_parent; // already bake_tiles/<map>
    std::filesystem::create_directories(out_dir, ec);
    std::filesystem::create_directories(out_dir / "model", ec);
    std::filesystem::create_directories(out_dir / "texture", ec);
    std::filesystem::create_directories(out_dir / "fbx", ec);
    result.out_dir = out_dir;
    prog_log(progress, "out_dir=" + out_dir.string());

    std::vector<CellWork> work(cells.size());
    for (std::size_t i = 0; i < cells.size(); ++i) work[i].cell = cells[i];

    auto cell_index = [&](float x, float y) -> int {
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const auto& c = cells[i];
            if (x >= c.min_x && x < c.max_x && y >= c.min_y && y < c.max_y) return static_cast<int>(i);
        }
        int best = 0;
        float bd = 1e30f;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const float cx = 0.5f * (cells[i].min_x + cells[i].max_x);
            const float cy = 0.5f * (cells[i].min_y + cells[i].max_y);
            const float d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d < bd) {
                bd = d;
                best = static_cast<int>(i);
            }
        }
        return best;
    };

    for (const auto& o : req.objects) {
        const int ci = cell_index(o.position.x, o.position.y);
        work[static_cast<std::size_t>(ci)].objects.push_back(&o);
    }
    for (const auto& s : req.splines) {
        const int ci = cell_index(s.position.x, s.position.y);
        work[static_cast<std::size_t>(ci)].splines.push_back(&s);
    }

    const unsigned hw = (std::max)(1u, std::thread::hardware_concurrency());
    const unsigned workers = (std::min)(hw, static_cast<unsigned>(work.size()));
    prog_log(progress, "cells=" + std::to_string(work.size()) + " threads~" + std::to_string(workers));

    std::mutex result_mutex;
    std::atomic<int> done{0};

    auto process_cell = [&](std::size_t wi) {
        CellWork& cw = work[wi];
        TileBakePartResult part;
        part.part_index = static_cast<int>(wi);

        AccumMesh acc;
        acc.omsi_root = req.omsi_root;
        acc.out_dir = out_dir;
        std::error_code ec_tex;
        std::filesystem::create_directories(out_dir / "texture", ec_tex);
        std::vector<sco::ScoPath> collected_paths;
        int obj_ok = 0, obj_skip = 0;

        // Terrain sample (MergeMapTrees: inst.z + terrain(xy) unless [absheight]/paths; mesh relative to pivot)
        const bool have_terrain =
            req.terrain_heights.size() >= static_cast<std::size_t>(editor::kTerrainDim * editor::kTerrainDim);
        auto terrain_at = [&](float x, float y) -> float {
            if (!have_terrain) return 0.f;
            return editor::sample_terrain_height(req.terrain_heights, req.tile_size, x, y);
        };
        const float pivot_terrain_h = terrain_at(0.f, 0.f);
        if (have_terrain)
            prog_log(progress, "terrain samples ok pivot_h=" + std::to_string(pivot_terrain_h));

        auto pose_with_terrain = [&](const map::MapObjectInst& o, const sco::SceneryObject* s) {
            map::MapObjectInst pose = o;
            if (s && !sco::skip_terrain_add(*s)) {
                pose.position.z += terrain_at(o.position.x, o.position.y) - pivot_terrain_h;
            } else {
                // absheight / path SCO: keep authored Z relative to pivot (no terrain add)
                pose.position.z -= 0.f;
            }
            return pose;
        };

        for (const map::MapObjectInst* op : cw.objects) {
            if (!op) continue;
            if (is_tile_bake_product_name(op->sco_path.filename().string())) {
                ++obj_skip;
                continue;
            }
            const sco::SceneryObject* sco = sco::load_sco(op->sco_path);
            std::string reason;
            if (sco_excluded(sco, reason)) {
                ++obj_skip;
                prog_log(progress, "skip " + op->sco_path.filename().string() + " (" + reason + ")");
                continue;
            }
            if (sco) {
                if (sco->special == sco::ScoSpecial::Tree || sco->tree.valid) {
                    std::string tex = sco->tree.texture;
                    if (!op->tree_texture.empty()) tex = op->tree_texture;
                    float h = sco->tree.height > 0.f ? sco->tree.height : 12.f;
                    if (op->tree_height > 0.f) h = op->tree_height;
                    float ratio = sco->tree.ratio > 0.f ? sco->tree.ratio : 0.85f;
                    if (op->tree_aspect > 0.f) ratio = op->tree_aspect;
                    if (!tex.empty()) {
                        const std::string dest_name = copy_bake_texture(
                            op->sco_path, std::filesystem::path(tex), req.omsi_root, out_dir, acc.copied_tex);
                        // matl_alpha for tree texture (no [tree] block — geometry in o3d)
                        {
                            const std::string bare =
                                dest_name.empty() ? std::filesystem::path(tex).filename().string() : dest_name;
                            const std::string uniq = bare + "#1";
                            if (acc.alpha_keys.insert(uniq).second) {
                                std::ostringstream os;
                                os << "[matl]\n" << bare << "\n0\n[matl_alpha]\n1\n";
                                acc.matl_alpha_lines.push_back(os.str());
                            }
                        }
                        // Billboard cross double-sided (MergeMapTrees _billboard_cross_mesh) in Z-up
                        {
                            const std::string bare =
                                dest_name.empty() ? std::filesystem::path(tex).filename().string() : dest_name;
                            auto it = acc.mat_by_tex.find(bare);
                            if (it == acc.mat_by_tex.end()) {
                                o3d::Material om{};
                                om.fixed.diffuse[0] = om.fixed.diffuse[1] = om.fixed.diffuse[2] = om.fixed.diffuse[3] =
                                    1.f;
                                om.texture = bare;
                                const int idx = static_cast<int>(acc.mesh.materials.size());
                                acc.mesh.materials.push_back(std::move(om));
                                it = acc.mat_by_tex.emplace(bare, idx).first;
                            }
                            const auto mat = static_cast<std::uint16_t>(it->second);
                            const float hw = h * ratio * 0.5f;
                            // local Y-up cross like MergeMapTrees, then instance transform
                            struct Corner {
                                float x, y, z;
                            };
                            auto emit_plane = [&](Corner c0, Corner c1, Corner c2, Corner c3, float nx, float ny,
                                                  float nz) {
                                auto push_face = [&](bool back) {
                                    const auto base = static_cast<std::uint32_t>(acc.mesh.vertices.size());
                                    Corner cs[4] = {c0, c1, c2, c3};
                                    float uvs[4][2] = {{1, 1}, {0, 1}, {0, 0}, {1, 0}};
                                    for (int i = 0; i < 4; ++i) {
                                        // local Y-up (x, height, z) -> prepare as Z-up input to transform:
                                        // Y-up (x,h,z) as if already prepared? Use transform from Y-up local:
                                        map::MapObjectInst local = pose_with_terrain(*op, sco);
                                        // treat local mesh as Z-up height along Z: (x, lateral, h)
                                        float ox, oy, oz;
                                        // Build Z-up local from Y-up billboard: (x,z,y_height)->(x,y_height as z?)
                                        // MergeMapTrees local: (x,y=h,z). Convert to Z-up local (x,z,y)=(x,z,h)
                                        float lx = cs[i].x, ly = cs[i].z, lz = cs[i].y;
                                        transform_instance_vertex_zup(lx, ly, lz, local, req.tile_origin_x,
                                                                      req.tile_origin_y, ox, oy, oz);
                                        o3d::Vertex v{};
                                        v.position[0] = ox;
                                        v.position[1] = oy;
                                        v.position[2] = oz;
                                        float nnx, nny, nnz;
                                        float ngx = nx, ngy = nz, ngz = ny;
                                        rotate_omsi_euler_yup(ngx, ngy, ngz, op->rotation_deg.x, op->rotation_deg.y,
                                                              op->rotation_deg.z, nnx, nny, nnz);
                                        // Y-up normal -> Z-up
                                        v.normal[0] = nnx;
                                        v.normal[1] = nnz;
                                        v.normal[2] = nny;
                                        if (back) {
                                            v.normal[0] = -v.normal[0];
                                            v.normal[1] = -v.normal[1];
                                            v.normal[2] = -v.normal[2];
                                        }
                                        v.uv[0] = uvs[i][0];
                                        v.uv[1] = uvs[i][1];
                                        acc.mesh.vertices.push_back(v);
                                    }
                                    auto tri = [&](int a, int b, int c) {
                                        o3d::TriangleU32 t{};
                                        t.i0 = base + static_cast<std::uint32_t>(a);
                                        t.i1 = base + static_cast<std::uint32_t>(b);
                                        t.i2 = base + static_cast<std::uint32_t>(c);
                                        t.material = mat;
                                        acc.mesh.triangles.push_back(t);
                                    };
                                    if (!back) {
                                        tri(0, 1, 2);
                                        tri(0, 2, 3);
                                    } else {
                                        tri(0, 2, 1);
                                        tri(0, 3, 2);
                                    }
                                };
                                push_face(false);
                                push_face(true);
                            };
                            // plane X/Y (z=0) and Y/Z (x=0) — MergeMapTrees
                            emit_plane({hw, 0, 0}, {-hw, 0, 0}, {-hw, h, 0}, {hw, h, 0}, 0, 0, -1);
                            emit_plane({0, 0, hw}, {0, 0, -hw}, {0, h, -hw}, {0, h, hw}, 1, 0, 0);
                            ++obj_ok;
                        }
                    }
                }
                std::vector<int> tl_remap;
                if (sco->traffic_lights_group) {
                    tl_remap = merge_traffic_lights(acc, *sco->traffic_lights_group);
                    prog_log(progress, "tl merge " + op->sco_path.filename().string() +
                                           " lights=" + std::to_string(tl_remap.size()) +
                                           " total=" + std::to_string(acc.traffic_lights.size()));
                }
                collect_matl_alpha(acc, *sco);
                for (const auto& p : sco->paths) {
                    sco::ScoPath wp = p;
                    transform_instance_vertex_zup(p.x, p.y, p.z, *op, req.tile_origin_x, req.tile_origin_y, wp.x, wp.y,
                                                  wp.z);
                    wp.heading_deg = p.heading_deg + op->rotation_deg.x;
                    // Remap [use_traffic_light] local index -> merged global index
                    if (p.use_traffic_light >= 0 && !tl_remap.empty()) {
                        if (p.use_traffic_light < static_cast<int>(tl_remap.size()))
                            wp.use_traffic_light = tl_remap[static_cast<std::size_t>(p.use_traffic_light)];
                        else
                            wp.use_traffic_light = tl_remap.front();
                    }
                    collected_paths.push_back(wp);
                }
                // [light_enh] / [light_enh_2] — transform local light pos into tile space
                for (const auto& Lsrc : sco->lights) {
                    sco::ScoLightEnh L = Lsrc;
                    transform_instance_vertex_zup(Lsrc.position.x, Lsrc.position.y, Lsrc.position.z, *op,
                                                  req.tile_origin_x, req.tile_origin_y, L.position.x, L.position.y,
                                                  L.position.z);
                    // direction: rotation only
                    float dx, dy, dz;
                    transform_instance_vertex_zup(Lsrc.direction.x, Lsrc.direction.y, Lsrc.direction.z, *op, 0.f, 0.f,
                                                  dx, dy, dz);
                    {
                        float yx = Lsrc.direction.x, yy = Lsrc.direction.z, yz = Lsrc.direction.y;
                        float rx, ry, rz;
                        rotate_omsi_euler_yup(yx, yy, yz, op->rotation_deg.x, op->rotation_deg.y, op->rotation_deg.z,
                                              rx, ry, rz);
                        L.direction.x = rx;
                        L.direction.y = rz;
                        L.direction.z = ry;
                    }
                    acc.lights.push_back(std::move(L));
                }
                // [new_attachment] points
                for (const auto& ap0 : sco->attachment_points) {
                    sco::AttachmentPoint ap = ap0;
                    transform_instance_vertex_zup(ap0.local_pos.x, ap0.local_pos.y, ap0.local_pos.z, *op,
                                                  req.tile_origin_x, req.tile_origin_y, ap.local_pos.x, ap.local_pos.y,
                                                  ap.local_pos.z);
                    ap.rot_x = ap0.rot_x + op->rotation_deg.x;
                    ap.rot_y = ap0.rot_y + op->rotation_deg.y;
                    ap.rot_z = ap0.rot_z + op->rotation_deg.z;
                    acc.attachment_points.push_back(std::move(ap));
                }
            }

            bool mesh_added = false;
            if (sco) {
                // [onlyeditor] helpers: do not bake mesh, paths already collected above
                if (sco->only_editor_mesh) {
                    prog_log(progress, "onlyeditor paths-only " + op->sco_path.filename().string());
                    ++obj_skip;
                    continue;
                }
                for (const auto& mr : sco->meshes) {
                    if (!mr.is_o3d) continue;
                    if (sco->special == sco::ScoSpecial::Tree && sco->only_editor_mesh) continue;
                    std::filesystem::path mesh_path = mr.mesh_file;
                    if (!mesh_path.is_absolute()) mesh_path = op->sco_path.parent_path() / mesh_path;
                    auto mapped = o3d::load_mapped_shared(mesh_path);
                    if (!mapped || !mapped->valid()) continue;
                    std::string err;
                    if (!append_transformed_mesh(acc, mapped->mesh(), pose_with_terrain(*op, sco), op->sco_path, &err, req.tile_origin_x, req.tile_origin_y)) {
                        if (err == "vertex_limit" && !acc.mesh.vertices.empty()) {
                            const std::string stem = req.stem + "_p" + std::to_string(wi) + "_s" +
                                                     std::to_string(part.sub_part);
                            const auto o3d_path = out_dir / "model" / (stem + ".o3d");
                            if (o3d::write_o3d_file(o3d_path, acc.mesh)) {
                                const auto sco_path = out_dir / (stem + ".sco");
                                write_part_sco(sco_path, std::filesystem::path(stem + ".o3d"), acc, {});
                                TileBakePartResult sub = part;
                                sub.o3d_path = o3d_path;
                                sub.sco_path = sco_path;
                                sub.vertex_count = static_cast<int>(acc.mesh.vertices.size());
                                sub.object_count = obj_ok;
                                {
                                    std::lock_guard<std::mutex> lock(result_mutex);
                                    result.parts.push_back(sub);
                                }
                                prog_log(progress, "auto-split vertex limit -> " + stem);
                            }
                            acc = AccumMesh{};
                            part.sub_part++;
                            if (!append_transformed_mesh(acc, mapped->mesh(), pose_with_terrain(*op, sco), op->sco_path, &err, req.tile_origin_x, req.tile_origin_y)) {
                                prog_log(progress, "drop mesh still over limit " + mesh_path.filename().string());
                                continue;
                            }
                        } else {
                            continue;
                        }
                    }
                    mesh_added = true;
                }
            }
            if (mesh_added) ++obj_ok;
        }


        // Bake [splineAttachement] meshes at curve pose (then remove them from .map on Replace)
        for (const auto& att : req.spline_attaches) {
            const map::SplineInst* host = nullptr;
            // spline_index is local index within tile spline list
            if (att.spline_index >= 0 &&
                static_cast<std::size_t>(att.spline_index) < req.splines.size()) {
                host = &req.splines[static_cast<std::size_t>(att.spline_index)];
            }
            if (!host) {
                for (const auto* sp : cw.splines) {
                    if (sp && sp->id == att.spline_index) { host = sp; break; }
                }
            }
            if (!host) continue;
            const float along = att.offset_along - map::spline_local_length(*host);
            const auto pose = map::compute_spline_attachment_pose(*host, att, along);
            map::MapObjectInst fake{};
            fake.position.x = pose.x;
            fake.position.y = pose.y;
            // evaluate_spline returns absolute Z; map objects store offset-above-terrain.
            // Convert to the same convention as free objects so pose_with_terrain does not double.
            {
                const float terr = terrain_at(pose.x, pose.y);
                fake.position.z = pose.z - terr; // offset above terrain at this XY
            }
            fake.rotation_deg.x = pose.yaw_deg;
            fake.rotation_deg.y = pose.pitch_deg;
            fake.rotation_deg.z = pose.bank_deg;
            fake.sco_path = att.sco_path;
            const sco::SceneryObject* sco = sco::load_sco(att.sco_path);
            if (!sco) continue;
            std::string reason;
            if (sco_excluded(sco, reason)) continue;
            for (const auto& mr : sco->meshes) {
                if (!mr.is_o3d) continue;
                std::filesystem::path mesh_path = mr.mesh_file;
                if (!mesh_path.is_absolute()) mesh_path = att.sco_path.parent_path() / mesh_path;
                auto mapped = o3d::load_mapped_shared(mesh_path);
                if (!mapped || !mapped->valid()) continue;
                std::string err;
                // terrain-aware like free objects (fake.z already offset-above-terrain)
                append_transformed_mesh(acc, mapped->mesh(), pose_with_terrain(fake, sco), att.sco_path, &err,
                                        req.tile_origin_x, req.tile_origin_y);
            }
            collect_matl_alpha(acc, *sco);
        }

        // Bake [attachObj] at parent [new_attachment] pose (same idea as splineAttachement).
        // Skip keep-list SCOs (entrypoint / carpark / busstop / trafficlight / texttexture…).
        {
            // Parent lookup among tile objects
            std::unordered_map<std::int32_t, const map::MapObjectInst*> by_id;
            for (const auto& o : req.objects) by_id[o.id] = &o;
            int att_ok = 0, att_skip = 0;
            for (const auto& att : req.attaches) {
                auto pit = by_id.find(att.parent_id);
                if (pit == by_id.end() || !pit->second) {
                    ++att_skip;
                    prog_log(progress, "attachObj SKIP no_parent child=" + std::to_string(att.child_id) +
                                           " parent=" + std::to_string(att.parent_id));
                    continue;
                }
                const sco::SceneryObject* sco = sco::load_sco(att.sco_path);
                std::string reason;
                if (sco_excluded(sco, reason)) {
                    ++att_skip;
                    prog_log(progress, "attachObj KEEP " + att.sco_path.filename().string() + " (" + reason + ")");
                    continue;
                }
                if (!sco) {
                    ++att_skip;
                    continue;
                }
                const auto pose_opt = map::compute_attach_obj_pose(att, *pit->second);
                if (!pose_opt) {
                    ++att_skip;
                    prog_log(progress, "attachObj SKIP pose_fail " + att.sco_path.filename().string());
                    continue;
                }
                map::MapObjectInst fake{};
                fake.position.x = pose_opt->x;
                fake.position.y = pose_opt->y;
                fake.position.z = pose_opt->z;
                fake.rotation_deg.x = pose_opt->yaw_deg;
                fake.rotation_deg.y = pose_opt->pitch_deg;
                fake.rotation_deg.z = pose_opt->bank_deg;
                fake.sco_path = att.sco_path;
                // Terrain relative to pivot (parent already baked with terrain; attach world z is absolute-ish)
                fake = pose_with_terrain(fake, sco);
                for (const auto& mr : sco->meshes) {
                    if (!mr.is_o3d) continue;
                    std::filesystem::path mesh_path = mr.mesh_file;
                    if (!mesh_path.is_absolute()) mesh_path = att.sco_path.parent_path() / mesh_path;
                    auto mapped = o3d::load_mapped_shared(mesh_path);
                    if (!mapped || !mapped->valid()) continue;
                    std::string err;
                    if (append_transformed_mesh(acc, mapped->mesh(), fake, att.sco_path, &err, req.tile_origin_x,
                                                req.tile_origin_y))
                        ++att_ok;
                }
                collect_matl_alpha(acc, *sco);
            }
            prog_log(progress, "attachObj baked=" + std::to_string(att_ok) + " skip=" + std::to_string(att_skip) +
                                   " of=" + std::to_string(req.attaches.size()));
        }

        // Merge spline meshes into the same AccumMesh (textures copied to texture\)
        if (!cw.splines.empty()) {
            int sli_ok = 0;
            int sli_skip_load = 0, sli_skip_onlyed = 0, sli_skip_empty = 0, sli_skip_vlimit = 0;
            for (const auto* sp : cw.splines) {
                if (!sp) continue;
                const auto def = spline::load_sli(sp->sli_path);
                if (!def) {
                    ++sli_skip_load;
                    prog_log(progress, "sli SKIP load_fail id=" + std::to_string(sp->id) + " " +
                                           sp->sli_path.filename().string());
                    continue;
                }
                if (def->only_editor) {
                    ++sli_skip_onlyed;
                    prog_log(progress, "sli SKIP onlyeditor id=" + std::to_string(sp->id) + " " +
                                           sp->sli_path.filename().string());
                    continue; // paths collected later, no mesh
                }
                const auto parts = spline::build_spline_meshes(*sp, *def, req.omsi_root, 0.f, std::nullopt);
                if (parts.empty()) {
                    ++sli_skip_empty;
                    prog_log(progress, "sli SKIP empty_mesh id=" + std::to_string(sp->id) + " " +
                                           sp->sli_path.filename().string() +
                                           " len=" + std::to_string(sp->length));
                    continue;
                }
                for (const auto& partm : parts) {
                    const std::string dest_name =
                        copy_bake_texture(sp->sli_path, partm.texture_path, req.omsi_root, out_dir, acc.copied_tex);
                    const std::string tex_ref =
                        dest_name.empty() ? sco_texture_ref(partm.texture_path) : dest_name;
                    int mat_i = ensure_material(acc, tex_ref, nullptr, sp->sli_path);
                    if (mat_i >= 0 && mat_i < static_cast<int>(acc.mesh.materials.size()))
                        acc.mesh.materials[static_cast<std::size_t>(mat_i)].texture = tex_ref;
                    const auto base = static_cast<std::uint32_t>(acc.mesh.vertices.size());
                    if (base + static_cast<std::uint32_t>(partm.vertices.size()) > kO3dMaxVertices) {
                        ++sli_skip_vlimit;
                        prog_log(progress, "sli SKIP vertex_limit id=" + std::to_string(sp->id) + " " +
                                               sp->sli_path.filename().string() +
                                               " base=" + std::to_string(base) +
                                               " add=" + std::to_string(partm.vertices.size()));
                        continue;
                    }
                    // Mesh is tile-local: subtract focus tile editor origin (same as object bake)
                    for (const auto& v : partm.vertices) {
                        o3d::Vertex nv = v;
                        nv.position[0] -= req.tile_origin_x;
                        nv.position[1] -= req.tile_origin_y;
                        acc.mesh.vertices.push_back(nv);
                    }
                    for (std::size_t i = 0; i + 2 < partm.indices.size(); i += 3) {
                        o3d::TriangleU32 nt{};
                        nt.i0 = base + partm.indices[i];
                        nt.i1 = base + partm.indices[i + 1];
                        nt.i2 = base + partm.indices[i + 2];
                        nt.material = static_cast<std::uint16_t>(mat_i);
                        acc.mesh.triangles.push_back(nt);
                    }
                    ++sli_ok;
                    prog_log(progress, "sli OK id=" + std::to_string(sp->id) + " " +
                                           sp->sli_path.filename().string() +
                                           " verts=" + std::to_string(partm.vertices.size()) +
                                           " tris=" + std::to_string(partm.indices.size() / 3));
                }
            }
            part.spline_count = static_cast<int>(cw.splines.size());
            prog_log(progress, "sli summary ok=" + std::to_string(sli_ok) +
                                   " load_fail=" + std::to_string(sli_skip_load) +
                                   " onlyed=" + std::to_string(sli_skip_onlyed) +
                                   " empty=" + std::to_string(sli_skip_empty) +
                                   " vlimit=" + std::to_string(sli_skip_vlimit) +
                                   " of=" + std::to_string(cw.splines.size()));
            log::info("tile bake sli summary ok=" + std::to_string(sli_ok) +
                      " load_fail=" + std::to_string(sli_skip_load) +
                      " onlyed=" + std::to_string(sli_skip_onlyed) +
                      " empty=" + std::to_string(sli_skip_empty) +
                      " vlimit=" + std::to_string(sli_skip_vlimit) +
                      " of=" + std::to_string(cw.splines.size()));

            // Paths from SLI definitions + instance geometry for [onlyeditor]/invis_*.
            // onlyeditor: NO mesh (skipped above); YES path with curve/radius/length from SplineInst.
            {
                constexpr float kRad2Deg = 180.f / 3.14159265f;
                int path_from_onlyed = 0, path_from_vis = 0, path_from_inst = 0;
                for (const auto* sp : cw.splines) {
                    if (!sp) continue;
                    const auto def = spline::load_sli(sp->sli_path);
                    const bool onlyed = def && def->only_editor;
                    const auto sli_name = sp->sli_path.filename().string();

                    auto push_path = [&](const sco::ScoPath& wp, const char* src) {
                        collected_paths.push_back(wp);
                        prog_log(progress,
                                 std::string("path ") + src + " id=" + std::to_string(sp->id) + " " + sli_name +
                                     " len=" + std::to_string(wp.length) + " r=" + std::to_string(wp.radius) +
                                     " at=(" + std::to_string(wp.x) + "," + std::to_string(wp.y) + "," +
                                     std::to_string(wp.z) + ")");
                        log::info(std::string("tile bake path ") + src + " id=" + std::to_string(sp->id) + " " +
                                  sli_name + " len=" + std::to_string(wp.length) +
                                  " r=" + std::to_string(wp.radius));
                    };

                    // --- Paths authored in the .sli (including onlyeditor) ---
                    if (def && !def->paths.empty()) {
                        for (const auto& p : def->paths) {
                            float path_r = sp->radius;
                            // Keep centerline length; only adjust radius for lateral offset on arcs.
                            // Do NOT shrink length for onlyeditor (user expects full spline length).
                            float path_len = (std::max)(sp->length, 0.05f);
                            if (std::fabs(sp->radius) > 1e-4f && std::fabs(p.lateral) > 1e-4f && !onlyed) {
                                path_r = sp->radius - p.lateral;
                                const float scale = std::fabs(path_r) / std::fabs(sp->radius);
                                path_len = (std::max)(0.05f, sp->length * scale);
                            } else if (std::fabs(sp->radius) > 1e-4f) {
                                path_r = sp->radius - p.lateral;
                            }
                            const auto s0 = spline::evaluate_spline(*sp, 0.f, p.lateral, p.height);
                            sco::ScoPath wp{};
                            wp.x = s0.x - req.tile_origin_x;
                            wp.y = s0.y - req.tile_origin_y;
                            wp.z = s0.z;
                            wp.heading_deg = s0.heading_rad * kRad2Deg;
                            wp.radius = path_r;
                            wp.length = path_len;
                            wp.grad_start = sp->start_grad;
                            wp.grad_end = sp->end_grad;
                            wp.type = p.typ;
                            wp.width = p.width > 0.f ? p.width : 1.f;
                            wp.direction = p.direction;
                            wp.blinker = 0;
                            if (onlyed) ++path_from_onlyed;
                            else ++path_from_vis;
                            push_path(wp, onlyed ? "sli_onlyed" : "sli_vis");
                        }
                    }

                    // --- onlyeditor / path-only with empty SLI paths: synthesize from instance ---
                    if (onlyed && (!def || def->paths.empty())) {
                        const auto s0 = spline::evaluate_spline(*sp, 0.f, 0.f, 0.f);
                        sco::ScoPath wp{};
                        wp.x = s0.x - req.tile_origin_x;
                        wp.y = s0.y - req.tile_origin_y;
                        wp.z = s0.z;
                        wp.heading_deg = s0.heading_rad * kRad2Deg;
                        wp.radius = sp->radius;
                        wp.length = (std::max)(sp->length, 0.05f);
                        wp.grad_start = sp->start_grad;
                        wp.grad_end = sp->end_grad;
                        wp.type = 0;
                        wp.width = 1.f;
                        wp.direction = 0;
                        wp.blinker = 0;
                        ++path_from_inst;
                        ++path_from_onlyed;
                        push_path(wp, "inst_onlyed");
                    }
                }
                prog_log(progress, "sli paths collected=" + std::to_string(collected_paths.size()) +
                                       " onlyed=" + std::to_string(path_from_onlyed) +
                                       " vis=" + std::to_string(path_from_vis) +
                                       " inst=" + std::to_string(path_from_inst));
                log::info("tile bake sli paths=" + std::to_string(collected_paths.size()) +
                          " onlyed=" + std::to_string(path_from_onlyed) +
                          " vis=" + std::to_string(path_from_vis) +
                          " inst=" + std::to_string(path_from_inst));
            }

        }

        // Place SCO at focus-tile editor origin (not always 0,0 — multi-tile session).
        const float cx = req.tile_origin_x;
        const float cy = req.tile_origin_y;
        const float cz = 0.f;
        const std::string stem = req.stem + "_p" + std::to_string(wi);
        const bool have_mesh = !acc.mesh.vertices.empty();
        const bool have_paths = !collected_paths.empty();
        if (have_mesh || have_paths) {
            const auto o3d_path = out_dir / "model" / (stem + ".o3d");
            bool o3d_ok = true;
            if (have_mesh) {
                o3d_ok = o3d::write_o3d_file(o3d_path, acc.mesh);
                if (!o3d_ok) part.error = "write_o3d failed";
            } else {
                // Paths-only (e.g. onlyeditor / invis_*): write a tiny placeholder o3d so SCO [mesh] resolves,
                // or omit mesh file and write SCO with paths only (write_part_sco still refs o3d name).
                o3d::WritableMesh empty_m;
                o3d::Material om{};
                om.fixed.diffuse[0] = om.fixed.diffuse[1] = om.fixed.diffuse[2] = om.fixed.diffuse[3] = 1.f;
                om.texture = "white.bmp";
                empty_m.materials.push_back(om);
                // single degenerate tri far away so bounds aren't zero
                o3d::Vertex v{};
                v.position[0] = v.position[1] = v.position[2] = 0.f;
                empty_m.vertices.push_back(v);
                empty_m.vertices.push_back(v);
                empty_m.vertices.push_back(v);
                o3d::TriangleU32 t{};
                t.i0 = 0; t.i1 = 1; t.i2 = 2; t.material = 0;
                empty_m.triangles.push_back(t);
                o3d_ok = o3d::write_o3d_file(o3d_path, empty_m);
            }
            if (o3d_ok) {
                const auto sco_path = out_dir / (stem + ".sco");
                write_part_sco(sco_path, std::filesystem::path(stem + ".o3d"), acc, collected_paths);
                part.o3d_path = o3d_path;
                part.sco_path = sco_path;
                part.cx = cx;
                part.cy = cy;
                part.cz = cz;
                part.vertex_count = static_cast<int>(acc.mesh.vertices.size());
                part.object_count = obj_ok;
                part.spline_count = static_cast<int>(cw.splines.size());
                prog_log(progress, "wrote " + stem + " paths=" + std::to_string(collected_paths.size()) +
                                       " verts=" + std::to_string(part.vertex_count));
                log::info("tile bake part " + stem + " place=(" + std::to_string(cx) + "," +
                          std::to_string(cy) + ") paths=" + std::to_string(collected_paths.size()) +
                          " verts=" + std::to_string(part.vertex_count));
            }
        } else if (part.sco_path.empty()) {
            part.error = "empty cell";
        }

        {
            std::lock_guard<std::mutex> lock(result_mutex);
            result.parts.push_back(part);
        }
        const int d = ++done;
        if (progress) {
            progress->parts_done = d;
            progress->percent = static_cast<int>(
                100.f * static_cast<float>(d) / static_cast<float>((std::max)(1, progress->parts_total.load())));
        }
        prog_log(progress, "cell " + std::to_string(wi) + " done verts=" + std::to_string(part.vertex_count) +
                               " objs=" + std::to_string(obj_ok) + " skip=" + std::to_string(obj_skip));
    };

    std::vector<std::thread> pool;
    pool.reserve(work.size());
    for (std::size_t i = 0; i < work.size(); ++i) pool.emplace_back(process_cell, i);
    for (auto& t : pool) t.join();

    result.ok = false;
    for (const auto& p : result.parts) {
        if (!p.sco_path.empty() && p.error.empty()) {
            result.ok = true;
            break;
        }
    }
    if (!result.ok && result.error.empty()) result.error = "no parts produced";
    if (progress) {
        progress->finished = true;
        progress->success = result.ok;
        progress->percent = 100;
    }
    return result;
}

} // namespace omsi::editor
