#include "omsi/map_attachment.hpp"
#include "omsi/map_types.hpp"
#include "omsi/sco_types.hpp"
#include "omsi/spline_types.hpp"
#include "o3d/o3d_loader.hpp"
#include "omsi/o3d_format.hpp"
#include <fstream>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool approx_eq(float a, float b, float eps = 0.05f) { return std::fabs(a - b) <= eps; }

int fail(const char* msg) {
    std::cerr << "FAIL: " << msg << '\n';
    return 1;
}

const omsi::map::SplineInst* find_local_spline(const omsi::map::MapTileData& tile, const std::int32_t idx) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= tile.splines.size()) return nullptr;
    return &tile.splines[static_cast<std::size_t>(idx)];
}

int run_scan(const std::filesystem::path& map, const std::filesystem::path& omsi_root) {
    const auto tile = omsi::map::load_map_tile(map, omsi_root);
    if (!tile) return fail("load_map_tile returned null");

    std::cout << "SCAN " << map.filename().string() << " attachments=" << tile->spline_attachments.size()
              << " splines=" << tile->splines.size() << '\n';

    for (const auto& att : tile->spline_attachments) {
        const auto* spline = find_local_spline(*tile, att.spline_index);
        const float local_len = spline ? omsi::map::spline_local_length(*spline) : 0.f;
        const float spl_len = spline ? spline->length : 0.f;
        const float start_along = att.offset_along - local_len;
        std::size_t repeats = 0;
        if (spline) repeats = omsi::map::enumerate_spline_attachment_along(att, *spline).size();

        std::cout << "  id=" << att.instance_id << " sco=" << att.sco_path.filename().string()
                  << " splIdx=" << att.spline_index << " step=" << att.repeat_step << " range=" << att.repeat_range
                  << " along=" << att.offset_along << " startAlong=" << start_along << " repeats=" << repeats
                  << " splLen=" << spl_len << " localLength=" << local_len;
        if (spline) std::cout << " splId=" << spline->id;
        std::cout << '\n';
    }
    return 0;
}

int raster_alpha_pixels(const omsi::sco::ScoTextTexture& tt, const std::string& text,
                        const std::filesystem::path& omsi_root) {
    const auto img = omsi::sco::rasterize_text_texture(tt, text, omsi_root);
    if (!img || img->rgba.size() < 4) return -1;
    int n = 0;
    for (std::size_t i = 3; i < img->rgba.size(); i += 4)
        if (img->rgba[i] > 8) ++n;
    return n;
}

int run_busstop_text_dump(const std::filesystem::path& map, const std::filesystem::path& omsi_root) {
    const auto tile = omsi::map::load_map_tile(map, omsi_root);
    if (!tile) return fail("load_map_tile returned null");

    std::cout << "TILE objects=" << tile->objects.size() << " attachObj=" << tile->attachments.size() << '\n';

    const omsi::map::MapObjectInst* obj2041 = nullptr;
    for (const auto& o : tile->objects) {
        if (o.id != 2041 && o.id != 577) continue;
        std::cout << "OBJECT id=" << o.id << " sco=" << o.sco_path.filename().string()
                  << " string_line_count=" << o.string_line_count << " strings=" << o.strings.size();
        for (const auto& s : o.strings) std::cout << " [" << s << "]";
        std::cout << '\n';
        if (o.id == 2041) obj2041 = &o;
    }
    if (!obj2041) return fail("object 2041 missing");
    if (obj2041->strings.empty() || obj2041->strings.front() != "Siegener Str.")
        return fail("object 2041 extra text not 'Siegener Str.'");

    struct Want {
        std::int32_t child = 0;
        const char* text = nullptr;
    };
    const Want wants[] = {{2042, "5"}, {2043, "92"}, {540774, "5N"}};
    for (const auto& w : wants) {
        const omsi::map::MapAttachObjectInst* found = nullptr;
        for (const auto& a : tile->attachments)
            if (a.child_id == w.child) found = &a;
        if (!found) return fail(("attachObj " + std::to_string(w.child) + " missing").c_str());
        std::cout << "ATTACH child=" << found->child_id << " parent=" << found->parent_id
                  << " sco=" << found->sco_path.filename().string() << " flag=" << found->options_flag
                  << " label=[" << found->label << "] count=" << found->object_text_line_count
                  << " lines=" << found->object_text_lines.size();
        for (const auto& s : found->object_text_lines) std::cout << " [" << s << "]";
        std::cout << '\n';
        if (found->object_text_lines.empty() || found->object_text_lines.front() != w.text)
            return fail(("attachObj " + std::to_string(w.child) + " extra text != '" + w.text + "'").c_str());
        if (found->parent_id != 2041) return fail("attach parent is not 2041");
    }

    struct RasterCase {
        std::int32_t id = 0;
        std::filesystem::path sco;
        std::string text;
    };
    std::vector<RasterCase> rasters = {
        {2041, obj2041->sco_path, obj2041->strings.front()},
    };
    for (const auto& w : wants) {
        for (const auto& a : tile->attachments) {
            if (a.child_id != w.child) continue;
            rasters.push_back({w.child, a.sco_path, a.object_text_lines.front()});
        }
    }
    for (const auto& rc : rasters) {
        const auto sco = omsi::sco::load_sco(rc.sco);
        if (!sco) return fail(("load_sco failed " + rc.sco.string()).c_str());
        if (sco->text_textures.empty()) return fail(("no [texttexture] " + rc.sco.filename().string()).c_str());
        const auto& tt = sco->text_textures.front();
        const int px = raster_alpha_pixels(tt, rc.text, omsi_root);
        std::cout << "RASTER id=" << rc.id << " sco=" << rc.sco.filename().string() << " text=[" << rc.text
                  << "] font=[" << tt.font_name << "] alpha_px=" << px
                  << " useTT=" << (sco->materials.empty() || !sco->materials.front().use_text_texture
                                       ? -1
                                       : *sco->materials.front().use_text_texture)
                  << '\n';
        if (px <= 0) return fail(("raster empty for id " + std::to_string(rc.id)).c_str());
    }

    std::cout << "OK busstop map text loaded + rasterized\n";
    return 0;
}

int run_unit_test(const std::filesystem::path& map) {
    const auto tile = omsi::map::load_map_tile(map, {});
    if (!tile) return fail("load_map_tile returned null");

    const auto attachments = omsi::map::parse_map_spline_attachment_entries(map);
    if (attachments.size() != 1) {
        return fail(("expected 1 attachment, got " + std::to_string(attachments.size())).c_str());
    }

    const auto& att = attachments.front();
    if (att.instance_id != 91) return fail("wrong instance_id");
    if (!approx_eq(att.repeat_step, 10.f)) return fail(("repeat_step=" + std::to_string(att.repeat_step)).c_str());
    if (!approx_eq(att.repeat_range, 20.f)) return fail(("repeat_range=" + std::to_string(att.repeat_range)).c_str());
    if (!approx_eq(att.offset_along, 4.502f, 0.01f)) {
        return fail(("offset_along=" + std::to_string(att.offset_along)).c_str());
    }

    if (tile->splines.empty()) return fail("no splines loaded");
    const auto& spline = tile->splines[0];
    const auto along_list = omsi::map::enumerate_spline_attachment_along(att, spline);
    if (along_list.size() < 2) {
        return fail(("expected >=2 repeats, got " + std::to_string(along_list.size())).c_str());
    }

    std::cout << "OK id=" << att.instance_id << " step=" << att.repeat_step << " range=" << att.repeat_range
              << " along=" << att.offset_along << " repeats=" << along_list.size() << " localLength="
              << omsi::map::spline_local_length(spline) << '\n';
    return 0;
}

} // namespace

// --texdiag <sco> <text> <out.ppm>: the text raster of texttexture 0, and every o3d slot's bounds / UVs
static int run_texdiag(const std::filesystem::path& sco_path, const std::string& text, const std::filesystem::path& out,
                       const std::filesystem::path& omsi_root) {
    const auto* sco = omsi::sco::load_sco(sco_path);
    if (!sco) return fail("sco");
    const char nl = '\n';
    std::cout << "text_textures=" << sco->text_textures.size() << " materials=" << sco->materials.size() << nl;
    for (const auto& m : sco->materials)
        std::cout << " matl " << m.diffuse_texture << " id=" << m.mat_id << " tt=" << (m.use_text_texture ? *m.use_text_texture : -1)
                  << " alpha=" << static_cast<int>(m.alpha_mode) << " mesh=" << m.mesh_index << nl;
    if (!sco->text_textures.empty()) {
        const auto& tt = sco->text_textures[0];
        std::cout << "tt font=" << tt.font_name << " " << tt.width << "x" << tt.height << " color_mode=" << tt.color_mode << nl;
        omsi::sco::prefetch_oft_images(tt.font_name, omsi_root);
        const auto img = omsi::sco::rasterize_text_texture(tt, text, omsi_root);
        if (!img) {
            std::cout << "raster: none" << nl;
        } else {
            int lit = 0;
            for (std::size_t i = 3; i < img->rgba.size(); i += 4) lit += img->rgba[i] > 8;
            std::cout << "raster " << img->width << "x" << img->height << " alpha_px=" << lit << nl;
            std::ofstream f(out, std::ios::binary);
            f << "P6" << nl << img->width << " " << img->height << nl << "255" << nl;
            for (std::size_t i = 0; i + 3 < img->rgba.size(); i += 4) {
                const unsigned a = img->rgba[i + 3];
                for (int c = 0; c < 3; ++c) f.put(static_cast<char>(img->rgba[i + c] * a / 255 + (255 - a) * 40 / 255));
            }
        }
    }
    for (const auto& mr : sco->meshes) {
        if (!mr.is_o3d) continue;
        auto mapped = omsi::o3d::load_mapped_shared(mr.mesh_file);
        if (!mapped || !mapped->valid()) continue;
        const auto& mesh = mapped->mesh();
        auto vs = omsi::o3d::decrypt_o3d_vertices(mesh);
        for (auto& v : vs) v = omsi::o3d::transform_vertex_to_omsi_world(v, mesh);
        const auto mats = omsi::o3d::parse_materials_from_section(mesh.materials_raw, mesh.material_count);
        std::cout << "mesh " << mr.mesh_file.filename().string() << " verts=" << vs.size() << nl;
        for (std::size_t mi = 0; mi < mats.size(); ++mi) {
            float lo[5] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f}, hi[5] = {-1e9f, -1e9f, -1e9f, -1e9f, -1e9f};
            int tris = 0;
            auto take = [&](std::uint32_t i0, std::uint32_t i1, std::uint32_t i2, std::uint16_t mat) {
                if (mat != mi) return;
                ++tris;
                for (auto i : {i0, i1, i2}) {
                    if (i >= vs.size()) continue;
                    const float val[5] = {vs[i].position[0], vs[i].position[1], vs[i].position[2], vs[i].uv[0], vs[i].uv[1]};
                    for (int k = 0; k < 5; ++k) {
                        lo[k] = (std::min)(lo[k], val[k]);
                        hi[k] = (std::max)(hi[k], val[k]);
                    }
                }
            };
            if (mesh.long_triangle_indices) {
                for (const auto& t : mesh.triangle_u32_span()) take(t.i0, t.i1, t.i2, t.material);
            } else {
                for (const auto& t : mesh.triangle_u16_span()) take(t.i0, t.i1, t.i2, t.material);
            }
            std::cout << " slot " << mi << " tex=" << mats[mi].texture << " tris=" << tris << " x[" << lo[0] << "," << hi[0]
                      << "] y[" << lo[1] << "," << hi[1] << "] z[" << lo[2] << "," << hi[2] << "] u[" << lo[3] << "," << hi[3]
                      << "] v[" << lo[4] << "," << hi[4] << "]" << nl;
        }
    }
    return 0;
}

// --splinez <tile.map> <lx> <ly> <r> <out.txt>: every triangle of the spline meshes near (lx, ly)
static int run_splinez(const std::filesystem::path& map, float lx, float ly, float r, const std::filesystem::path& outp,
                       const std::filesystem::path& omsi_root) {
    const auto tile = omsi::map::load_map_tile(map, omsi_root);
    if (!tile) return fail("load_map_tile failed");
    std::ofstream o(outp);
    int n = 0;
    for (const auto& sp : tile->splines) {
        const float dx = sp.position.x - lx, dy = sp.position.y - ly;
        if (std::sqrt(dx * dx + dy * dy) > r + sp.length + 10.f) continue;
        const auto def = omsi::spline::load_sli_shared(sp.sli_path);
        if (!def) continue;
        const auto meshes = omsi::spline::build_spline_meshes(sp, *def, omsi_root, 0.f, std::nullopt);
        for (std::size_t mi = 0; mi < meshes.size(); ++mi) {
            const auto& m = meshes[mi];
            o << "S " << sp.id << " " << mi << " " << int(m.alpha_mode) << " " << int(sp.is_spline_h) << " "
              << sp.cant_start << " " << sp.cant_end << " " << sp.start_grad << " " << sp.end_grad << " "
              << sp.delta_height << " " << int(sp.use_delta_height) << " " << int(sp.mirror) << " | "
              << sp.sli_path.filename().string() << " | " << m.texture_path.filename().string() << "\n";
            for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
                o << "T";
                for (int k = 0; k < 3; ++k) {
                    const auto& v = m.vertices[m.indices[i + k]];
                    o << " " << v.position[0] << " " << v.position[1] << " " << v.position[2];
                }
                o << "\n";
            }
            ++n;
        }
    }
    std::cout << "meshes " << n << "\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 7 && std::string(argv[1]) == "--splinez")
        return run_splinez(argv[2], std::stof(argv[3]), std::stof(argv[4]), std::stof(argv[5]), argv[6],
                           argc >= 8 ? std::filesystem::path(argv[7]) : std::filesystem::path("C:/Program Files (x86)/Steam/steamapps/common/OMSI 2"));
    if (argc >= 5 && std::string(argv[1]) == "--texdiag")
        return run_texdiag(argv[2], argv[3], argv[4], argc >= 6 ? std::filesystem::path(argv[5]) : std::filesystem::path("C:/Program Files (x86)/Steam/steamapps/common/OMSI 2"));
    bool scan = false;
    bool text_dump = false;
    std::filesystem::path map{"_test_data/spline_attach_tile.map"};
    std::filesystem::path omsi_root{"C:/Program Files (x86)/Steam/steamapps/common/OMSI 2"};

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--scan")
            scan = true;
        else if (arg == "--text-dump")
            text_dump = true;
        else if (arg == "--omsi-root" && i + 1 < argc)
            omsi_root = argv[++i];
        else
            map = arg;
    }

    if (!std::filesystem::exists(map)) return fail(("missing map: " + map.string()).c_str());
    if (text_dump) return run_busstop_text_dump(map, omsi_root);
    return scan ? run_scan(map, omsi_root) : run_unit_test(map);
}
