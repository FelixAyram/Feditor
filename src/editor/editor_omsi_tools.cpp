#include "omsi/vfs_overlay.hpp"
#include "omsi/fs_cow.hpp"
#include "omsi/editor_omsi_tools.hpp"
#include "omsi/sco_types.hpp"
#include "editor/editor_ground.hpp"
#include "omsi/logger.hpp"
#include "omsi/map_geo_tools.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace omsi::map {
namespace {

constexpr int kLm = 256;

bool write_bmp24_topdown(const std::filesystem::path& path, const int w, const int h,
                         const std::vector<std::uint8_t>& bgr) {
    if (w <= 0 || h <= 0 || static_cast<int>(bgr.size()) < w * h * 3) return false;
    const std::size_t row_bytes = static_cast<std::size_t>((w * 3 + 3) & ~3);
    const std::size_t need = row_bytes * static_cast<std::size_t>(h);
    std::vector<std::uint8_t> padded(need, 0);
    for (int y = 0; y < h; ++y) {
        // BMP is bottom-up
        const int src_y = h - 1 - y;
        std::memcpy(padded.data() + static_cast<std::size_t>(y) * row_bytes,
                    bgr.data() + static_cast<std::size_t>(src_y) * static_cast<std::size_t>(w) * 3,
                    static_cast<std::size_t>(w) * 3);
    }
    omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::uint32_t file_size = static_cast<std::uint32_t>(54 + need);
    const std::uint32_t data_offset = 54;
    const std::uint32_t dib = 40;
    const std::uint16_t planes = 1;
    const std::uint16_t bpp = 24;
    const std::uint32_t zero = 0;
    out.put('B');
    out.put('M');
    out.write(reinterpret_cast<const char*>(&file_size), 4);
    out.write(reinterpret_cast<const char*>(&zero), 4);
    out.write(reinterpret_cast<const char*>(&data_offset), 4);
    out.write(reinterpret_cast<const char*>(&dib), 4);
    out.write(reinterpret_cast<const char*>(&w), 4);
    out.write(reinterpret_cast<const char*>(&h), 4);
    out.write(reinterpret_cast<const char*>(&planes), 2);
    out.write(reinterpret_cast<const char*>(&bpp), 2);
    out.write(reinterpret_cast<const char*>(&zero), 4); // compression
    const std::uint32_t img_sz = static_cast<std::uint32_t>(need);
    out.write(reinterpret_cast<const char*>(&img_sz), 4);
    const std::int32_t ppm = 2835;
    out.write(reinterpret_cast<const char*>(&ppm), 4);
    out.write(reinterpret_cast<const char*>(&ppm), 4);
    out.write(reinterpret_cast<const char*>(&zero), 4);
    out.write(reinterpret_cast<const char*>(&zero), 4);
    out.write(reinterpret_cast<const char*>(padded.data()), static_cast<std::streamsize>(need));
    return static_cast<bool>(out);
}

} // namespace

std::vector<std::uint8_t> bake_lightmap_bgr_from_heights(const std::vector<float>& heights,
                                                          float size_meters,
                                                          const std::vector<LightmapPoint>& lights) {
    constexpr int kLm = 256;
    constexpr int dim = 61;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(kLm * kLm * 3), 0);
    if (heights.size() < static_cast<std::size_t>(dim * dim)) return out;
    if (size_meters < 1.f) size_meters = 300.f;
    auto at = [&](int x, int y) -> float {
        x = (std::max)(0, (std::min)(dim - 1, x));
        y = (std::max)(0, (std::min)(dim - 1, y));
        return heights[static_cast<std::size_t>(y) * static_cast<std::size_t>(dim) +
                       static_cast<std::size_t>(x)];
    };
    // 1) Ambient from height slopes (soft sky/sun — same baseline as before).
    std::vector<float> ambient(static_cast<std::size_t>(kLm * kLm), 0.55f);
    for (int y = 0; y < kLm; ++y) {
        for (int x = 0; x < kLm; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kLm);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kLm);
            const float fx = u * static_cast<float>(dim - 1);
            const float fy = v * static_cast<float>(dim - 1);
            const int x0 = static_cast<int>(fx);
            const int y0 = static_cast<int>(fy);
            const float dx = at(x0 + 1, y0) - at(x0 - 1, y0);
            const float dy = at(x0, y0 + 1) - at(x0, y0 - 1);
            float n = 0.55f - 0.22f * dx + 0.18f * dy;
            n = (std::max)(0.15f, (std::min)(1.f, n));
            ambient[static_cast<std::size_t>(y * kLm + x)] = n;
        }
    }
    // 2) [maplight] contribution (wiki): full until radius, then 1/d^2.
    // RGB lightmap stores final illumination (OMSI samples this for terrain + objects).
    for (int y = 0; y < kLm; ++y) {
        for (int x = 0; x < kLm; ++x) {
            const float wx = ((static_cast<float>(x) + 0.5f) / static_cast<float>(kLm)) * size_meters;
            const float wy = ((static_cast<float>(y) + 0.5f) / static_cast<float>(kLm)) * size_meters;
            // height at this texel for vertical distance
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(kLm);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(kLm);
            const float fx = u * static_cast<float>(dim - 1);
            const float fy = v * static_cast<float>(dim - 1);
            const float wz = at(static_cast<int>(fx), static_cast<int>(fy));
            float R = ambient[static_cast<std::size_t>(y * kLm + x)];
            float G = R, B = R;
            for (const auto& L : lights) {
                const float dx = wx - L.x;
                const float dy = wy - L.y;
                const float dz = wz - L.z;
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float rad = (std::max)(L.radius, 0.5f);
                float att = 0.f;
                if (dist <= rad)
                    att = 1.f;
                else {
                    // square falloff beyond radius (wiki)
                    const float t = rad / dist;
                    att = t * t;
                }
                if (att < 0.002f) continue;
                R += L.r * att;
                G += L.g * att;
                B += L.b * att;
            }
            R = (std::max)(0.f, (std::min)(1.f, R));
            G = (std::max)(0.f, (std::min)(1.f, G));
            B = (std::max)(0.f, (std::min)(1.f, B));
            const std::size_t i =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(kLm) + static_cast<std::size_t>(x)) * 3;
            out[i] = static_cast<std::uint8_t>(B * 255.f);     // BGR
            out[i + 1] = static_cast<std::uint8_t>(G * 255.f);
            out[i + 2] = static_cast<std::uint8_t>(R * 255.f);
        }
    }
    return out;
}

std::vector<LightmapPoint> collect_tile_maplights(const std::filesystem::path& tile_map_path,
                                                  const std::filesystem::path& omsi_root) {
    std::vector<LightmapPoint> out;
    const auto tile = load_map_tile(tile_map_path, omsi_root);
    if (!tile) return out;
    auto add_from_sco = [&](const std::filesystem::path& sco_path, float ox, float oy, float oz, float yaw_deg) {
        const auto sco = sco::load_sco(sco_path);
        if (!sco) return;
        const float yaw = yaw_deg * 0.01745329251f;
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        auto xform = [&](float lx, float ly, float lz, LightmapPoint& p) {
            p.x = ox + cy * lx - sy * ly;
            p.y = oy + sy * lx + cy * ly;
            p.z = oz + lz;
        };
        for (const auto& ml : sco->maplights) {
            LightmapPoint p{};
            xform(ml.position.x, ml.position.y, ml.position.z, p);
            p.r = ml.color.x;
            p.g = ml.color.y;
            p.b = ml.color.z;
            p.radius = ml.radius;
            out.push_back(p);
        }
        // light_enh also contributes a soft fill (OMSI uses for visible point; maplight is primary)
        for (const auto& Le : sco->lights) {
            LightmapPoint p{};
            xform(Le.position.x, Le.position.y, Le.position.z, p);
            p.r = Le.color.x;
            p.g = Le.color.y;
            p.b = Le.color.z;
            p.radius = Le.range > 0.5f ? Le.range : 8.f;
            if (p.radius < 4.f) p.radius = 8.f;
            // weaker than maplight so billboards don't over-brighten LM
            p.r *= 0.35f;
            p.g *= 0.35f;
            p.b *= 0.35f;
            out.push_back(p);
        }
    };
    for (const auto& o : tile->objects) {
        const float yaw = o.rotation_deg.z; // degrees in MapObjectInst
        add_from_sco(o.sco_path, o.position.x, o.position.y, o.position.z, yaw);
    }
    return out;
}

bool write_tile_lightmap_bmp(const std::filesystem::path& tile_map_path, const std::vector<float>& heights,
                             const std::vector<LightmapPoint>& lights) {
    if (omsi::vfs::write_blocked(tile_map_path)) return false;
    const auto bgr = bake_lightmap_bgr_from_heights(heights, 300.f, lights);
    const auto path = std::filesystem::path(tile_map_path.string() + ".LM.bmp");
    if (!write_bmp24_topdown(path, kLm, kLm, bgr)) return false;
    log::info("wrote lightmap " + path.filename().string() + " lights=" + std::to_string(lights.size()));
    return true;
}

bool refresh_tile_lightmap(const std::filesystem::path& tile_map_path, const std::filesystem::path& omsi_root) {
    auto heights = editor::load_terrain_heights(tile_map_path);
    if (!heights) heights = editor::make_flat_terrain_heights(0.f);
    auto lights = collect_tile_maplights(tile_map_path, omsi_root);
    return write_tile_lightmap_bmp(tile_map_path, *heights, lights);
}


WholeMapJobResult run_whole_map_jobs(const GlobalMap& global, const WholeMapJobOptions& opt,
                                     AerialDownloadFn aerial_fn) {
    WholeMapJobResult r{};
    std::ostringstream msg;
    if (opt.create_lightmaps) {
        for (const auto& ref : global.tiles) {
            auto heights = editor::load_terrain_heights(ref.tile_path);
            if (!heights) heights = editor::make_flat_terrain_heights(0.f);
            auto lights = collect_tile_maplights(ref.tile_path, global.root);
            if (write_tile_lightmap_bmp(ref.tile_path, *heights, lights)) ++r.lightmaps;
        }
        msg << "lightmaps=" << r.lightmaps << " ";
    }
    if (opt.find_zero_splines) {
        for (const auto& ref : global.tiles) {
            const auto tile = load_map_tile(ref.tile_path, global.root.parent_path().parent_path());
            // omsi_root unknown here — pass empty and rely on absolute paths in tile if any
            if (!tile) continue;
            for (const auto& s : tile->splines) {
                if (s.length < 0.05f) ++r.zero_splines;
            }
        }
        // Retry with global.root as omsi-ish: load_map_tile needs omsi_root for relative sco paths only
        msg << "zero_splines=" << r.zero_splines << " ";
    }
    if (opt.validate_attach) {
        for (const auto& ref : global.tiles) {
            auto tile = load_map_tile(ref.tile_path, global.root);
            if (!tile) tile = load_map_tile(ref.tile_path, {});
            if (!tile) continue;
            for (const auto& a : tile->attachments) {
                bool parent_ok = false;
                for (const auto& o : tile->objects)
                    if (o.id == a.parent_id) {
                        parent_ok = true;
                        break;
                    }
                if (!parent_ok) {
                    for (const auto& s : tile->splines)
                        if (s.id == a.parent_id) {
                            parent_ok = true;
                            break;
                        }
                }
                if (!parent_ok) ++r.attach_issues;
            }
        }
        msg << "attach_issues=" << r.attach_issues << " ";
    }
    if (opt.list_objects) {
        int n = 0;
        for (const auto& ref : global.tiles) {
            auto tile = load_map_tile(ref.tile_path, global.root);
            if (!tile) continue;
            n += static_cast<int>(tile->objects.size());
        }
        msg << "objects=" << n << " ";
    }
    if ((opt.aerial_all || opt.aerial_missing) && aerial_fn) {
        for (const auto& ref : global.tiles) {
            std::string err;
            if (aerial_fn(global, ref.grid_x, ref.grid_y, opt.aerial_missing && !opt.aerial_all, err))
                ++r.aerials;
        }
        msg << "aerials=" << r.aerials << " ";
    }
    r.ok = true;
    r.message = msg.str().empty() ? "nothing selected" : msg.str();
    return r;
}

TimetableCatalog scan_timetable_catalog(const std::filesystem::path& map_root) {
    TimetableCatalog c{};
    const auto td = map_root / "TTData";
    if (std::filesystem::exists(td)) {
        for (const auto& e : std::filesystem::directory_iterator(td)) {
            if (!e.is_regular_file()) continue;
            const auto ext = e.path().extension().string();
            auto low = ext;
            for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (low == ".ttr") c.tracks.push_back(e.path());
            else if (low == ".ttp") c.trips.push_back(e.path());
            else if (e.path().filename() == "Busstops.cfg") c.busstops.push_back(e.path());
            else if (e.path().filename() == "StnLinks.cfg") c.stn_links = e.path();
        }
    }
    const auto sig = map_root / "signalroutes.cfg";
    if (std::filesystem::exists(sig)) c.signal_routes = sig;
    std::sort(c.tracks.begin(), c.tracks.end());
    std::sort(c.trips.begin(), c.trips.end());
    return c;
}

bool create_empty_track_file(const std::filesystem::path& map_root, const std::string& name_stem) {
    if (map_root.empty() || name_stem.empty()) return false;
    const auto dir = map_root / "TTData";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / (name_stem + ".ttr");
    if (std::filesystem::exists(path)) return false;
    omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "-----------------------\r\nTime Table Track File\r\n-----------------------\r\n\r\n";
    out << "Created with omsi-f4-engine\r\n\r\n";
    return true;
}

bool create_empty_trip_file(const std::filesystem::path& map_root, const std::string& name_stem, const int type,
                            const std::filesystem::path& from_track) {
    // OMSI [trip] is ALWAYS 3 lines: track_name (empty for type 2), terminus, line.
    // Type is NOT written as a number — type1 has non-empty track, type2 has empty track line.
    if (map_root.empty() || name_stem.empty()) return false;
    const auto dir = map_root / "TTData";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / (name_stem + ".ttp");
    if (std::filesystem::exists(path)) return false;
    omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "-----------------------\r\nTime Table Trip File\r\n-----------------------\r\n\r\n";
    out << "Created with omsi-f4-engine\r\n\r\n";
    out << "[trip]\r\n";
    if (type == 1 && !from_track.empty())
        out << from_track.stem().string() << "\r\n";
    else
        out << "\r\n"; // type 2: empty track name
    out << "\r\n"; // terminus
    out << "\r\n"; // line
    out << "\r\n";
    out << ".........................\r\n        Stations\r\n.........................\r\n\r\n";
    out << ".........................\r\n        Profiles\r\n.........................\r\n\r\n";
    out << "[profile]\r\nstandard\r\n1.000\r\n";
    return true;
}

bool copy_tt_file(const std::filesystem::path& src, const std::filesystem::path& dst_stem_path) {
    if (src.empty() || dst_stem_path.empty() || !std::filesystem::exists(src)) return false;
    std::error_code ec;
    omsi::fsutil::break_link(dst_stem_path); // copy-on-write workspace: never write through a link
    std::filesystem::copy_file(src, dst_stem_path, std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}

} // namespace omsi::map
