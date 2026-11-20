#include "omsi/editor_tile_bake.hpp"

#include "omsi/editor_sli_bake.hpp"
#include "omsi/o3d_writer.hpp"
#include "omsi/sco_types.hpp"
#include "omsi/model_cfg_types.hpp"
#include "o3d/o3d_loader.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace omsi::editor {
namespace {

void prog_log(TileBakeProgress* p, std::string line) {
    if (!p) return;
    std::lock_guard<std::mutex> lock(p->log_mutex);
    p->log_lines.push_back(std::move(line));
    if (p->log_lines.size() > 400) p->log_lines.erase(p->log_lines.begin(), p->log_lines.begin() + 50);
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool name_has_carpark_tag(const sco::SceneryObject& sco) {
    // Keep as loose SCO: content that declares carpark_p in path strings / scripts is rare;
    // primary exclusion is special kinds + filename heuristics.
    const auto low = lower_ascii(sco.sco_path.filename().string());
    if (low.find("carpark") != std::string::npos) return true;
    return false;
}

bool sco_excluded(const sco::SceneryObject* sco, std::string& reason) {
    if (!sco) {
        reason = "missing_sco";
        return true;
    }
    using S = sco::ScoSpecial;
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
    return false;
}

void mul_mat4_vec3(const float m[16], float x, float y, float z, float& ox, float& oy, float& oz) {
    // row-major or column? OMSI object: apply ZYX euler then translate.
    ox = m[0] * x + m[4] * y + m[8] * z + m[12];
    oy = m[1] * x + m[5] * y + m[9] * z + m[13];
    oz = m[2] * x + m[6] * y + m[10] * z + m[14];
}

// Build OMSI Z-up transform from map object (rotation_deg ZYX).
void object_world_matrix(const map::MapObjectInst& o, float out[16]) {
    const float deg = 3.14159265f / 180.f;
    const float rx = o.rotation_deg.x * deg;
    const float ry = o.rotation_deg.y * deg;
    const float rz = o.rotation_deg.z * deg;
    const float cx = std::cos(rx), sx = std::sin(rx);
    const float cy = std::cos(ry), sy = std::sin(ry);
    const float cz = std::cos(rz), sz = std::sin(rz);
    // R = Rz * Ry * Rx (common OMSI-ish)
    const float r00 = cz * cy;
    const float r01 = cz * sy * sx - sz * cx;
    const float r02 = cz * sy * cx + sz * sx;
    const float r10 = sz * cy;
    const float r11 = sz * sy * sx + cz * cx;
    const float r12 = sz * sy * cx - cz * sx;
    const float r20 = -sy;
    const float r21 = cy * sx;
    const float r22 = cy * cx;
    out[0] = r00; out[1] = r10; out[2] = r20; out[3] = 0;
    out[4] = r01; out[5] = r11; out[6] = r21; out[7] = 0;
    out[8] = r02; out[9] = r12; out[10] = r22; out[11] = 0;
    out[12] = o.position.x; out[13] = o.position.y; out[14] = o.position.z; out[15] = 1;
}

struct AccumMesh {
    o3d::WritableMesh mesh;
    std::unordered_map<std::string, int> mat_by_tex; // texture key -> material index
    std::vector<std::string> matl_alpha_lines;       // unique matl_alpha blocks for .sco
    std::unordered_set<std::string> alpha_keys;
};

int ensure_material(AccumMesh& acc, const std::string& tex_key, const o3d::Material* src) {
    auto it = acc.mat_by_tex.find(tex_key);
    if (it != acc.mat_by_tex.end()) return it->second;
    o3d::Material m{};
    if (src) m = *src;
    else {
        m.diffuse[0] = m.diffuse[1] = m.diffuse[2] = m.diffuse[3] = 1.f;
        m.texture = tex_key;
    }
    if (m.texture.empty()) m.texture = tex_key;
    const int idx = static_cast<int>(acc.mesh.materials.size());
    acc.mesh.materials.push_back(m);
    acc.mat_by_tex[tex_key] = idx;
    return idx;
}

bool append_transformed_mesh(AccumMesh& acc, const o3d::NativeMesh& src, const float world[16],
                             std::string* err) {
    const auto vspan = src.vertex_span();
    if (vspan.empty()) return true;
    const std::uint32_t base = static_cast<std::uint32_t>(acc.mesh.vertices.size());
    if (base + static_cast<std::uint32_t>(vspan.size()) > kO3dMaxVertices) {
        if (err) *err = "vertex_limit";
        return false;
    }
    // Materials from raw section
    std::vector<o3d::Material> src_mats =
        o3d::parse_materials_from_section(src.materials_raw, src.material_count);
    std::vector<int> mat_remap(src_mats.empty() ? 1 : src_mats.size(), 0);
    if (src_mats.empty()) {
        mat_remap[0] = ensure_material(acc, "white", nullptr);
    } else {
        for (std::size_t mi = 0; mi < src_mats.size(); ++mi) {
            std::string key = src_mats[mi].texture;
            if (key.empty()) key = "white";
            mat_remap[mi] = ensure_material(acc, key, &src_mats[mi]);
        }
    }

    acc.mesh.vertices.reserve(acc.mesh.vertices.size() + vspan.size());
    for (const auto& v : vspan) {
        o3d::Vertex nv = v;
        float ox, oy, oz;
        mul_mat4_vec3(world, v.position[0], v.position[1], v.position[2], ox, oy, oz);
        nv.position[0] = ox;
        nv.position[1] = oy;
        nv.position[2] = oz;
        float nx, ny, nz;
        const float m3[16] = {world[0], world[1], world[2], 0, world[4], world[5], world[6], 0,
                              world[8], world[9], world[10], 0, 0, 0, 0, 1};
        mul_mat4_vec3(m3, v.normal[0], v.normal[1], v.normal[2], nx, ny, nz);
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

    auto emit_tri = [&](std::uint32_t i0, std::uint32_t i1, std::uint32_t i2, std::uint16_t mat) {
        o3d::TriangleU32 nt{};
        nt.i0 = base + i0;
        nt.i1 = base + i1;
        nt.i2 = base + i2;
        const int sm = static_cast<int>(mat);
        nt.material = static_cast<std::uint16_t>(
            (sm >= 0 && sm < static_cast<int>(mat_remap.size())) ? mat_remap[static_cast<std::size_t>(sm)]
                                                                : mat_remap.front());
        acc.mesh.triangles.push_back(nt);
    };
    if (src.long_triangle_indices) {
        for (const auto& t : src.triangle_u32_span()) emit_tri(t.i0, t.i1, t.i2, t.material);
    } else {
        for (const auto& t : src.triangle_u16_span()) emit_tri(t.i0, t.i1, t.i2, t.material);
    }
    return true;
}

void collect_matl_alpha(AccumMesh& acc, const sco::SceneryObject& sco) {
    for (const auto& mat : sco.materials) {
        std::string key = mat.diffuse_texture.empty() ? mat.texture_key : mat.diffuse_texture;
        if (key.empty()) continue;
        const std::string uniq = key + "#" + std::to_string(static_cast<int>(mat.alpha_mode));
        if (!acc.alpha_keys.insert(uniq).second) continue;
        std::ostringstream os;
        os << "[matl]
" << key << "
0
";
        if (mat.alpha_mode != model_cfg::MatlAlphaMode::opaque)
            os << "[matl_alpha]
" << static_cast<int>(mat.alpha_mode) << "
";
        acc.matl_alpha_lines.push_back(os.str());
    }
}

bool write_part_sco(const std::filesystem::path& sco_path, const std::filesystem::path& o3d_rel,
                    const AccumMesh& acc, const std::vector<sco::ScoPath>& paths) {
    std::ofstream sco(sco_path);
    if (!sco) return false;
    sco << "[friendlyname]\nTile bake part\n\n";
    sco << "[mesh]\n" << o3d_rel.string() << "\n\n";
    for (const auto& block : acc.matl_alpha_lines) sco << block << "\n";
    for (const auto& p : paths) {
        sco << "[path]\n";
        sco << p.x << "\n" << p.y << "\n" << p.z << "\n";
        sco << p.heading_deg << "\n" << p.radius << "\n" << p.length << "\n";
        sco << p.grad_start << "\n" << p.grad_end << "\n";
        sco << p.type << "\n" << p.width << "\n" << p.direction << "\n" << p.blinker << "\n\n";
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
    const auto low = lower_ascii(name);
    return low.find("tile_bake_") != std::string::npos || low.find("tilebake_") != std::string::npos ||
           low.find("_bake_part") != std::string::npos;
}

std::vector<TileGridCell> tile_bake_grid(int parts, float origin_x, float origin_y, float tile_size) {
    parts = std::clamp(parts, kTileBakePartsMin, kTileBakePartsMax);
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
    // Refuse if already baked products dominate the object list
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
    const auto out_dir = unique_sli_bake_dir(req.out_parent, req.stem.empty() ? "tile_bake" : req.stem);
    std::filesystem::create_directories(out_dir, ec);
    result.out_dir = out_dir;
    prog_log(progress, "out_dir=" + out_dir.string());

    // Assign instances to cells by XY
    std::vector<CellWork> work(cells.size());
    for (std::size_t i = 0; i < cells.size(); ++i) work[i].cell = cells[i];

    auto cell_index = [&](float x, float y) -> int {
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const auto& c = cells[i];
            if (x >= c.min_x && x < c.max_x && y >= c.min_y && y < c.max_y) return static_cast<int>(i);
        }
        // clamp to nearest
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

    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned workers = std::min(hw, static_cast<unsigned>(work.size()));
    prog_log(progress, "cells=" + std::to_string(work.size()) + " threads~" + std::to_string(workers));

    std::mutex result_mutex;
    std::atomic<int> done{0};

    auto process_cell = [&](std::size_t wi) {
        CellWork& cw = work[wi];
        TileBakePartResult part;
        part.part_index = static_cast<int>(wi);

        AccumMesh acc;
        std::vector<sco::ScoPath> collected_paths;
        int obj_ok = 0, obj_skip = 0;

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
            float world[16];
            object_world_matrix(*op, world);

            // Paths → object local to world (simple: path origin + object heading)
            if (sco) {
                collect_matl_alpha(acc, *sco);
                for (const auto& p : sco->paths) {
                    sco::ScoPath wp = p;
                    float px, py, pz;
                    mul_mat4_vec3(world, p.x, p.y, p.z, px, py, pz);
                    wp.x = px;
                    wp.y = py;
                    wp.z = pz;
                    wp.heading_deg = p.heading_deg + op->rotation_deg.z;
                    collected_paths.push_back(wp);
                }
            }

            bool mesh_added = false;
            if (sco) {
                for (const auto& mr : sco->meshes) {
                    if (!mr.is_o3d) continue; // .x trees/helpers: skip geometry path for now (billboards stay excluded or tree special)
                    if (sco->special == sco::ScoSpecial::Tree && sco->only_editor_mesh) continue;
                    std::filesystem::path mesh_path = mr.mesh_file;
                    if (!mesh_path.is_absolute())
                        mesh_path = op->sco_path.parent_path() / mesh_path;
                    auto mapped = o3d::load_mapped_shared(mesh_path);
                    if (!mapped || !mapped->valid()) continue;
                    std::string err;
                    if (!append_transformed_mesh(acc, mapped->mesh(), world, &err)) {
                        // Vertex limit: flush current part and continue in sub-part
                        if (err == "vertex_limit" && !acc.mesh.vertices.empty()) {
                            // Write current accumulation as sub-part then reset
                            const std::string stem = req.stem + "_p" + std::to_string(wi) + "_s" +
                                                     std::to_string(part.sub_part);
                            const auto o3d_path = out_dir / (stem + ".o3d");
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
                            // retry append
                            if (!append_transformed_mesh(acc, mapped->mesh(), world, &err)) {
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

        // Splines in cell → reuse SLI bake into a temp folder then note path (optional mesh)
        if (!cw.splines.empty()) {
            std::vector<map::SplineInst> pack;
            pack.reserve(cw.splines.size());
            for (const auto* s : cw.splines)
                if (s) pack.push_back(*s);
            const std::string stem = req.stem + "_p" + std::to_string(wi) + "_sli";
            const auto sli_dir = out_dir / stem;
            auto sli_res = bake_splines_to_sco(pack, req.omsi_root, sli_dir, stem);
            if (sli_res.error.empty() && !sli_res.o3d_path.empty()) {
                prog_log(progress, "sli bake " + sli_res.o3d_path.filename().string() +
                                       " verts=" + std::to_string(sli_res.vertex_count));
                // If object mesh empty, use spline bake as the part
                if (acc.mesh.vertices.empty()) {
                    part.o3d_path = sli_res.o3d_path;
                    part.sco_path = sli_res.sco_path;
                    part.cx = sli_res.cx;
                    part.cy = sli_res.cy;
                    part.cz = sli_res.cz;
                    part.vertex_count = sli_res.vertex_count;
                    part.spline_count = static_cast<int>(pack.size());
                }
            } else if (!sli_res.error.empty()) {
                prog_log(progress, "sli bake err: " + sli_res.error);
            }
        }

        if (!acc.mesh.vertices.empty()) {
            // Center origin for SCO placement
            float minx = 1e30f, miny = 1e30f, minz = 1e30f, maxx = -1e30f, maxy = -1e30f, maxz = -1e30f;
            for (const auto& v : acc.mesh.vertices) {
                minx = std::min(minx, v.position[0]);
                miny = std::min(miny, v.position[1]);
                minz = std::min(minz, v.position[2]);
                maxx = std::max(maxx, v.position[0]);
                maxy = std::max(maxy, v.position[1]);
                maxz = std::max(maxz, v.position[2]);
            }
            const float cx = 0.5f * (minx + maxx);
            const float cy = 0.5f * (miny + maxy);
            const float cz = 0.5f * (minz + maxz);
            for (auto& v : acc.mesh.vertices) {
                v.position[0] -= cx;
                v.position[1] -= cy;
                v.position[2] -= cz;
            }
            for (auto& p : collected_paths) {
                p.x -= cx;
                p.y -= cy;
                p.z -= cz;
            }
            const std::string stem = req.stem + "_p" + std::to_string(wi);
            const auto o3d_path = out_dir / (stem + ".o3d");
            if (!o3d::write_o3d_file(o3d_path, acc.mesh)) {
                part.error = "write_o3d failed";
            } else {
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
            progress->percent = static_cast<int>(100.f * static_cast<float>(d) /
                                                 static_cast<float>(std::max(1, progress->parts_total.load())));
        }
        prog_log(progress, "cell " + std::to_string(wi) + " done verts=" + std::to_string(part.vertex_count) +
                               " objs=" + std::to_string(obj_ok) + " skip=" + std::to_string(obj_skip));
    };

    // Parallel over cells
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
