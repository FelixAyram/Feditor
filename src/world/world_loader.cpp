#include "omsi/vfs_overlay.hpp"
#include "omsi/o3d_format.hpp"
#include "omsi/world.hpp"
#include "omsi/world_load_job.hpp"

#include "omsi/editor_map_workspace.hpp"
#include "omsi/logger.hpp"
#include "omsi/map_attachment.hpp"
#include "omsi/map_tile_layout.hpp"
#include "omsi/spline_types.hpp"
#include "o3d/o3d_loader.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <execution>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace omsi::world {
namespace {

constexpr f4f23ma::f32 kTileSize = 300.f;

bool within_object_dist(const map::EntryPoint& spawn, const f4f23ma::f32 wx, const f4f23ma::f32 wy,
                        const f4f23ma::f32 max_dist) {
    const f4f23ma::f32 dx = wx - spawn.position.x;
    const f4f23ma::f32 dy = wy - spawn.position.y;
    return (dx * dx + dy * dy) <= (max_dist * max_dist);
}

} // namespace

f4f23ma::ma4 tile_offset_matrix(const std::int32_t grid_x, const std::int32_t grid_y) {
    auto m = f4f23ma::ma4::identity();
    m.m[3][0] = static_cast<f4f23ma::f32>(grid_x) * kTileSize;
    m.m[3][1] = static_cast<f4f23ma::f32>(grid_y) * kTileSize;
    return m;
}

f4f23ma::ma4 tile_offset_matrix(const f4f23ma::f32 origin_x, const f4f23ma::f32 origin_y) {
    auto m = f4f23ma::ma4::identity();
    m.m[3][0] = origin_x;
    m.m[3][1] = origin_y;
    return m;
}
f4f23ma::ma4 euler_zyx_deg_to_matrix(const f4f23ma::f4& deg) {
    const f4f23ma::f32 cx = std::cos(deg.x * (f4f23ma::f32(3.14159265) / 180.f));
    const f4f23ma::f32 sx = std::sin(deg.x * (f4f23ma::f32(3.14159265) / 180.f));
    const f4f23ma::f32 cy = std::cos(deg.y * (f4f23ma::f32(3.14159265) / 180.f));
    const f4f23ma::f32 sy = std::sin(deg.y * (f4f23ma::f32(3.14159265) / 180.f));
    const f4f23ma::f32 cz = std::cos(deg.z * (f4f23ma::f32(3.14159265) / 180.f));
    const f4f23ma::f32 sz = std::sin(deg.z * (f4f23ma::f32(3.14159265) / 180.f));

    f4f23ma::ma4 r = f4f23ma::ma4::identity();
    r.m[0][0] = cy * cz;
    r.m[0][1] = cy * sz;
    r.m[0][2] = -sy;
    r.m[1][0] = sx * sy * cz - cx * sz;
    r.m[1][1] = sx * sy * sz + cx * cz;
    r.m[1][2] = sx * cy;
    r.m[2][0] = cx * sy * cz + sx * sz;
    r.m[2][1] = cx * sy * sz - sx * cz;
    r.m[2][2] = cx * cy;
    return r;
}

void World::register_script_asset(const std::filesystem::path& path) {
    script_assets_.push_back(path);
}

void World::begin_load(const map::GlobalMap& global, const map::EntryPoint& entry, const LoadOptions& opts) {
    spawn_ = entry;
    global_ = global;
    // Do not parse every .map in global.tiles — that froze "Cargando tiles" at 0%
    // (coordinator + each worker). Index is rebuilt from tiles actually loaded.
    global_spline_index_.clear();
    max_object_dist_ = opts.max_object_dist;
    tile_layout_ = map::build_map_tile_layout(global);
    absolute_tile_coords_ = map::uses_absolute_tile_file_coords(global);
    classic_grid_tile_origins_ = map::uses_classic_grid_tile_origins(global);
    meshes_.clear();
    tiles_.clear();
    o3d_cache_.clear();
    log::info("begin_load \"" + global.friendly_name + "\" tiles=" + std::to_string(global.tiles.size()) +
              " tile_radius=" + std::to_string(opts.tile_radius) +
              " world_coords=" + (global.world_coordinates ? "on" : "off") +
              " abs_tiles=" + (absolute_tile_coords_ ? "on" : "off") +
              " grid_origins=" + (classic_grid_tile_origins_ ? "on" : "off"));
}
void World::finish_load() {
    global_spline_index_ = map::build_global_spline_index_from_tiles(tiles_);
    log::info("finish_load meshes=" + std::to_string(meshes_.size()) + " tiles=" + std::to_string(tiles_.size()) +
              " spline_index=" + std::to_string(global_spline_index_.size()));
}

bool World::has_tile(const std::int32_t grid_x, const std::int32_t grid_y) const {
    for (const auto& t : tiles_)
        if (t.grid_x == grid_x && t.grid_y == grid_y) return true;
    return false;
}

void World::unload_tiles(const std::vector<std::pair<std::int32_t, std::int32_t>>& grids) {
    if (grids.empty()) return;
    std::unordered_set<std::int64_t> doomed;
    doomed.reserve(grids.size() * 2);
    for (const auto& g : grids) {
        doomed.insert((static_cast<std::int64_t>(g.first) << 32) ^
                      static_cast<std::uint32_t>(g.second));
    }
    auto key = [](std::int32_t gx, std::int32_t gy) {
        return (static_cast<std::int64_t>(gx) << 32) ^ static_cast<std::uint32_t>(gy);
    };
    tiles_.erase(std::remove_if(tiles_.begin(), tiles_.end(),
                                [&](const map::MapTileData& t) { return doomed.count(key(t.grid_x, t.grid_y)) != 0; }),
                 tiles_.end());
    meshes_.erase(std::remove_if(meshes_.begin(), meshes_.end(),
                                 [&](const PlacedMesh& m) {
                                     return doomed.count(key(m.tile_grid_x, m.tile_grid_y)) != 0;
                                 }),
                  meshes_.end());
    log::info("unload_tiles n=" + std::to_string(grids.size()) + " left_tiles=" + std::to_string(tiles_.size()) +
              " left_meshes=" + std::to_string(meshes_.size()));
}

void World::merge_from(World&& other) {
    if (other.tiles_.empty() && other.meshes_.empty()) return;
    tiles_.insert(tiles_.end(), std::make_move_iterator(other.tiles_.begin()),
                  std::make_move_iterator(other.tiles_.end()));
    meshes_.insert(meshes_.end(), std::make_move_iterator(other.meshes_.begin()),
                   std::make_move_iterator(other.meshes_.end()));
    for (auto& [path, mapped] : other.o3d_cache_) {
        if (!o3d_cache_.count(path)) o3d_cache_[path] = std::move(mapped);
    }
    other.tiles_.clear();
    other.meshes_.clear();
    other.o3d_cache_.clear();
    log::info("merge_from tiles=" + std::to_string(tiles_.size()) + " meshes=" + std::to_string(meshes_.size()));
}

bool World::load_map(const map::GlobalMap& global, const map::EntryPoint& entry, const LoadOptions& opts) {
    begin_load(global, entry, opts);

    std::vector<map::MapTileRef> tiles;
    if (!global.tiles.empty()) {
        const f4f23ma::f32 cx = entry.position.x;
        const f4f23ma::f32 cy = entry.position.y;
        const auto local_gx = static_cast<std::int32_t>(std::floor(cx / kTileSize));
        const auto local_gy = static_cast<std::int32_t>(std::floor(cy / kTileSize));

        const map::MapTileRef* origin = nullptr;
        for (const auto& tile_ref : global.tiles) {
            if (tile_ref.grid_x == local_gx && tile_ref.grid_y == local_gy) {
                origin = &tile_ref;
                break;
            }
        }
        if (!origin) {
            const auto want = "tile_" + std::to_string(local_gx) + "_" + std::to_string(local_gy) + ".map";
            for (const auto& tile_ref : global.tiles) {
                if (tile_ref.tile_path.filename().string() == want) {
                    origin = &tile_ref;
                    break;
                }
            }
        }
        if (!origin && !global.tiles.empty()) origin = &global.tiles.front();
        if (origin) {
            if (opts.load_radius_m > 0.f) {
                tiles = tiles_near_entry(global, entry, opts);
            } else {
                const f4f23ma::f32 radius = static_cast<f4f23ma::f32>(opts.tile_radius);
                for (const auto& tile_ref : global.tiles) {
                    const f4f23ma::f32 dx = static_cast<f4f23ma::f32>(tile_ref.grid_x - origin->grid_x);
                    const f4f23ma::f32 dy = static_cast<f4f23ma::f32>(tile_ref.grid_y - origin->grid_y);
                    if (std::sqrt(dx * dx + dy * dy) <= radius + 0.5f) tiles.push_back(tile_ref);
                }
            }
        }
    }

    for (const auto& tile_ref : tiles) load_tile_incremental(tile_ref, opts);

    finish_load();
    return !meshes_.empty() || !tiles_.empty();
}

void World::load_tile_incremental(const map::MapTileRef& ref, const LoadOptions& opts) {
    load_tile(ref, opts);
}

namespace {
void warm_tile_cpu_assets(const map::MapTileData& tile, const LoadOptions& opts) {
    std::vector<std::filesystem::path> scos;
    std::unordered_set<std::string> seen;
    auto add_sco = [&](const std::filesystem::path& p) {
        if (p.empty()) return;
        if (seen.insert(p.lexically_normal().string()).second) scos.push_back(p);
    };
    for (const auto& obj : tile.objects) add_sco(obj.sco_path);
    for (const auto& a : tile.attachments) add_sco(a.sco_path);
    for (const auto& sa : tile.spline_attachments) add_sco(sa.sco_path);
    const char* hdd_env = std::getenv("OMSI_HDD_LOAD");
    if (!hdd_env || !hdd_env[0]) hdd_env = std::getenv("OMSI_LOAD_HDD");
    const bool hdd_mode = hdd_env && hdd_env[0] && hdd_env[0] != '0';
    sco::preload_scos(scos, hdd_mode);
    if (opts.load_splines && !tile.splines.empty()) {
        std::for_each(std::execution::par, tile.splines.begin(), tile.splines.end(),
                      [](const map::SplineInst& sp) {
                          try {
                              (void)spline::load_sli_shared(sp.sli_path);
                          } catch (...) {
                          }
                      });
    }
    std::vector<std::filesystem::path> o3ds;
    seen.clear();
    for (const auto& p : scos) {
        const auto* sco = sco::load_sco(p);
        if (!sco) continue;
        for (const auto& mesh_ref : sco->meshes) {
            if (!mesh_ref.is_o3d) continue;
            const auto abs =
                mesh_ref.mesh_file.is_absolute() ? mesh_ref.mesh_file : opts.omsi_root / mesh_ref.mesh_file;
            if (seen.insert(abs.lexically_normal().string()).second) o3ds.push_back(abs);
        }
    }
    if (o3ds.size() >= 2) {
        std::for_each(std::execution::par, o3ds.begin(), o3ds.end(), [](const std::filesystem::path& p) {
            try {
                (void)o3d::load_mapped_shared(p);
            } catch (...) {
            }
        });
    } else if (o3ds.size() == 1) {
        (void)o3d::load_mapped_shared(o3ds.front());
    }
}
} // namespace

void World::load_tile(const map::MapTileRef& ref, const LoadOptions& opts) {
    const auto disk_map = map::workspace_tile_map_for_read(opts.workspace, ref.tile_path);
    if (!omsi::vfs::exists(disk_map)) {
        log::warn("tile missing: " + disk_map.string());
        return;
    }
    auto tile = map::load_map_tile(disk_map, opts.omsi_root);
    if (!tile) {
        log::warn("tile parse failed: " + disk_map.string());
        return;
    }
    // Keep logical path on the live map so .terrain / companions resolve next to original.
    tile->path = ref.tile_path;
    if (tile->grid_x == 0 && tile->grid_y == 0) {
        tile->grid_x = ref.grid_x;
        tile->grid_y = ref.grid_y;
    }
    load_parsed_tile(std::move(*tile), opts);
}

void World::load_parsed_tile(map::MapTileData tile, const LoadOptions& opts, bool warm_cpu) {
    if (warm_cpu) warm_tile_cpu_assets(tile, opts);

    // tt-viewer applyWorldLayout / getTileOriginFromMetric: layout (x,z) from min grid, no -x/-y.
    f4f23ma::f32 tile_ox = 0.f;
    f4f23ma::f32 tile_oy = 0.f;
    if (!absolute_tile_coords_) {
        const auto origin = map::tile_origin_xy(tile_layout_, tile.grid_x, tile.grid_y);
        tile_ox = origin.first;
        tile_oy = origin.second;
    }
    const auto offset = tile_offset_matrix(tile_ox, tile_oy);
    std::size_t placed = 0;
    std::size_t skipped_dist = 0;
    for (const auto& obj : tile.objects) {
        const f4f23ma::f32 wx = obj.position.x + offset.m[3][0];
        const f4f23ma::f32 wy = obj.position.y + offset.m[3][1];
        if (!within_object_dist(spawn_, wx, wy, max_object_dist_)) {
            ++skipped_dist;
            continue;
        }
        const std::size_t before = meshes_.size();
        place_object(obj, opts, offset.m[3][0], offset.m[3][1], tile.grid_x, tile.grid_y);
        if (meshes_.size() > before) ++placed;
    }

    log::info("tile " + tile.path.filename().string() + " objects=" + std::to_string(tile.objects.size()) +
              " meshes_placed=" + std::to_string(placed) + " skipped_dist=" + std::to_string(skipped_dist));

    tiles_.push_back(std::move(tile));
    place_tile_attachments(tiles_.back(), opts, offset.m[3][0], offset.m[3][1]);
}

void World::place_object(const map::MapObjectInst& obj, const LoadOptions& opts, const f4f23ma::f32 tile_ox,
                         const f4f23ma::f32 tile_oy, const std::int32_t tile_grid_x,
                         const std::int32_t tile_grid_y) {
    const auto ext = obj.sco_path.extension().string();
    if (ext == ".osc" || ext == ".ovh") {
        register_script_asset(obj.sco_path);
        return;
    }

    const auto sco = sco::load_sco(obj.sco_path);
    if (!sco) {
        log::trace("sco missing: " + obj.sco_path.string());
        return;
    }

    // [tree] billboards (Trees_MC etc.): mesh is often treehelper.x + [onlyeditor];
    // MergeMapTrees / OMSI editor draw the [tree] texture, not the .x. Editor places trees separately.
    if (sco->tree.valid) return;

    for (const auto& mesh_ref : sco->meshes) {
        if (!mesh_ref.is_o3d) continue;
        const auto stem = mesh_ref.mesh_file.stem().string();
        if (stem == "empty_lod" || stem == "empty") continue; // LOD stub — not drawable scenery
        const auto abs = mesh_ref.mesh_file.is_absolute() ? mesh_ref.mesh_file : opts.omsi_root / mesh_ref.mesh_file;
        if (!omsi::vfs::exists(abs)) continue;

        std::shared_ptr<o3d::MappedO3D> mapped;
        const auto it = o3d_cache_.find(abs);
        if (it != o3d_cache_.end()) mapped = it->second;
        else {
            mapped = o3d::load_mapped_shared(abs);
            if (!mapped || !mapped->valid()) {
                log::warn("o3d load failed: " + abs.string());
                continue;
            }
            o3d_cache_[abs] = mapped;
        }

        PlacedMesh placed{};
        placed.o3d = mapped;
        placed.source_path = abs;
        placed.sco_path = obj.sco_path;
        placed.object_id = obj.id;
        placed.tile_grid_x = tile_grid_x;
        placed.tile_grid_y = tile_grid_y;

        // .map [object]: rotation_deg.x=yaw, .y=pitch, .z=bank (OMSI lines 6–8).
        f4f23ma::ma4 t = f4f23ma::ma4::identity();
        t.m[3][0] = obj.position.x + tile_ox;
        t.m[3][1] = obj.position.z;
        t.m[3][2] = obj.position.y + tile_oy;
        placed.transform =
            euler_zyx_deg_to_matrix(obj.rotation_deg) * t *
            o3d::o3d_unity_root_transform(mapped->mesh().object_rows()) *
            o3d::o3d_mesh_draw_matrix(mapped->mesh());
        meshes_.push_back(std::move(placed));
    }

    if (obj.sco_path.extension() == ".cfg") {
        register_script_asset(obj.sco_path);
    }
}

void World::place_object_transform(const map::MapObjectInst& obj, const LoadOptions& opts,
                                   const f4f23ma::ma4 world_xform, const f4f23ma::f32 tile_ox,
                                   const f4f23ma::f32 tile_oy, const std::int32_t tile_grid_x,
                                   const std::int32_t tile_grid_y) {
    const auto ext = obj.sco_path.extension().string();
    if (ext == ".osc" || ext == ".ovh") {
        register_script_asset(obj.sco_path);
        return;
    }

    const auto sco = sco::load_sco(obj.sco_path);
    if (!sco || sco->tree.valid) return;

    const f4f23ma::ma4 tile_off = tile_offset_matrix(tile_ox, tile_oy);

    for (const auto& mesh_ref : sco->meshes) {
        if (!mesh_ref.is_o3d) continue;
        const auto stem = mesh_ref.mesh_file.stem().string();
        if (stem == "empty_lod" || stem == "empty") continue;
        const auto abs = mesh_ref.mesh_file.is_absolute() ? mesh_ref.mesh_file : opts.omsi_root / mesh_ref.mesh_file;
        if (!omsi::vfs::exists(abs)) continue;

        std::shared_ptr<o3d::MappedO3D> mapped;
        const auto it = o3d_cache_.find(abs);
        if (it != o3d_cache_.end()) mapped = it->second;
        else {
            mapped = o3d::load_mapped_shared(abs);
            if (!mapped || !mapped->valid()) continue;
            o3d_cache_[abs] = mapped;
        }

        PlacedMesh placed{};
        placed.o3d = mapped;
        placed.source_path = abs;
        placed.sco_path = obj.sco_path;
        placed.object_id = obj.id;
        placed.tile_grid_x = tile_grid_x;
        placed.tile_grid_y = tile_grid_y;
        placed.transform = tile_off * world_xform * o3d::o3d_unity_root_transform(mapped->mesh().object_rows()) *
                           o3d::o3d_mesh_draw_matrix(mapped->mesh());
        meshes_.push_back(std::move(placed));
    }
}

void World::place_tile_attachments(const map::MapTileData& tile, const LoadOptions& opts,
                                   const f4f23ma::f32 tile_ox, const f4f23ma::f32 tile_oy) {
    std::unordered_map<std::int32_t, f4f23ma::f32> spline_z;
    for (const auto& t : tiles_)
        for (const auto& sp : t.splines) spline_z[sp.id] = sp.position.z;

    std::unordered_set<std::int32_t> attach_done;
    const int attach_passes = static_cast<int>(tile.attachments.size());
    for (int pass = 0; pass < attach_passes; ++pass) {
        for (const auto& attach : tile.attachments) {
            if (attach_done.count(attach.child_id)) continue;
            const map::MapObjectInst* parent = nullptr;
            for (const auto& o : tile.objects)
                if (o.id == attach.parent_id) {
                    parent = &o;
                    break;
                }
            if (!parent) continue;

            const f4f23ma::f32 wx = parent->position.x + tile_ox;
            const f4f23ma::f32 wy = parent->position.y + tile_oy;
            if (!within_object_dist(spawn_, wx, wy, max_object_dist_)) {
                attach_done.insert(attach.child_id);
                continue;
            }

            map::MapObjectInst child{};
            child.id = attach.child_id;
            child.sco_path = attach.sco_path;
            child.varparent_id = attach.varparent_id;
            const f4f23ma::ma4 xform = map::attach_obj_tile_transform(attach, *parent);
            place_object_transform(child, opts, xform, tile_ox, tile_oy, tile.grid_x, tile.grid_y);
            attach_done.insert(attach.child_id);
        }
    }

    for (const auto& sa : tile.spline_attachments) {
        const auto ref_opt = map::resolve_spline_attachment_index(
            sa.spline_index, tile.grid_x, tile.grid_y,
            static_cast<std::int32_t>(tile.splines.size()), global_spline_index_);
        if (!ref_opt) continue;
        const map::SplineInst* sp = map::find_spline_in_tiles(tiles_, *ref_opt);
        if (!sp || sp->length <= 1.e-4f) continue;

        f4f23ma::f32 sp_ox = tile_ox;
        f4f23ma::f32 sp_oy = tile_oy;
        if (!absolute_tile_coords_) {
            const auto origin = map::tile_origin_xy(tile_layout_, ref_opt->tile_gx, ref_opt->tile_gy);
            sp_ox = origin.first;
            sp_oy = origin.second;
        }

        const auto chain_z = spline::resolve_spline_mesh_chain_end_z(*sp, spline_z);
        const auto along_list = map::enumerate_spline_attachment_along(sa, *sp);
        for (const float along : along_list) {
            const auto pose = map::compute_spline_attachment_pose(*sp, sa, along, chain_z);
            const f4f23ma::f32 wx = pose.x + sp_ox;
            const f4f23ma::f32 wy = pose.y + sp_oy;
            if (!within_object_dist(spawn_, wx, wy, max_object_dist_)) continue;

            map::MapObjectInst child{};
            child.id = sa.instance_id;
            child.sco_path = sa.sco_path;
            child.strings = sa.object_text_lines;
            f4f23ma::ma4 xform =
                euler_zyx_deg_to_matrix({pose.pitch_deg, pose.yaw_deg, pose.bank_deg, 0.f});
            xform.m[3][0] = pose.x;
            xform.m[3][1] = pose.y;
            xform.m[3][2] = pose.z;
            place_object_transform(child, opts, xform, sp_ox, sp_oy, ref_opt->tile_gx, ref_opt->tile_gy);
        }
    }
}

void World::refresh_tile_layout(const map::GlobalMap& global) {
    if (tile_layout_.origin_anchor_set) {
        tile_layout_ = map::build_map_tile_layout(global, tile_layout_.origin_anchor_gx,
                                                  tile_layout_.origin_anchor_gy);
    } else {
        tile_layout_ = map::build_map_tile_layout(global);
    }
}

} // namespace omsi::world
