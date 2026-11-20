#include "omsi/vfs_overlay.hpp"
#include "omsi/world_load_job.hpp"

#include "o3d/o3d_loader.hpp"
#include "omsi/editor_map_workspace.hpp"
#include "omsi/logger.hpp"
#include "omsi/map_tile_layout.hpp"
#include "omsi/sco_types.hpp"
#include "omsi/spline_types.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <execution>
#include <limits>
#include <optional>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace omsi::world {

namespace {
// build_map_tile_layout stats every tile file of the map (GetFileAttributesEx per tile). These lookups run per spline
// / per frame during streaming, so rebuild the layout only when the map (root + tile count) changes.
std::shared_ptr<const map::MapTileLayout> cached_tile_layout(const map::GlobalMap& global) {
    static std::mutex mu;
    static std::shared_ptr<const map::MapTileLayout> layout;
    static std::filesystem::path root;
    static std::size_t tiles = static_cast<std::size_t>(-1);
    static const map::GlobalMap* owner = nullptr;
    std::lock_guard<std::mutex> lk(mu);
    if (!layout || owner != &global || tiles != global.tiles.size() || root != global.root) {
        layout = std::make_shared<const map::MapTileLayout>(map::build_map_tile_layout(global));
        owner = &global;
        tiles = global.tiles.size();
        root = global.root;
    }
    return layout;
}
} // namespace

namespace {

constexpr f4f23ma::f32 kTileSize = 300.f;

std::optional<map::MapTileRef> find_tile(const map::GlobalMap& global, const std::int32_t gx,
                                        const std::int32_t gy) {
    for (const auto& tile_ref : global.tiles) {
        if (tile_ref.grid_x == gx && tile_ref.grid_y == gy) return tile_ref;
    }
    const auto path = global.root / ("tile_" + std::to_string(gx) + "_" + std::to_string(gy) + ".map");
    if (omsi::vfs::exists(path)) return map::MapTileRef{gx, gy, path};
    return std::nullopt;
}

std::optional<map::MapTileRef> find_origin_tile_impl(const map::GlobalMap& global, const map::EntryPoint& entry) {
    // Entrypoint category = [map] index (juañ Adrogué → tile_-10_-26), classic and worldcoords.
    if (entry.category >= 0 && static_cast<std::size_t>(entry.category) < global.tiles.size()) {
        return global.tiles[static_cast<std::size_t>(entry.category)];
    }

    const auto layout_sp = cached_tile_layout(global);
    const auto& layout = *layout_sp;
    if (const auto* metric = map::find_tile_at_world_position(layout, entry.position.x, entry.position.y)) {
        for (const auto& tile_ref : global.tiles) {
            if (tile_ref.grid_x == metric->grid_x && tile_ref.grid_y == metric->grid_y) return tile_ref;
        }
        return find_tile(global, metric->grid_x, metric->grid_y);
    }

    const auto local_gx = static_cast<std::int32_t>(std::floor(entry.position.x / kTileSize));
    const auto local_gy = static_cast<std::int32_t>(std::floor(entry.position.y / kTileSize));
    if (auto exact = find_tile(global, local_gx, local_gy)) return exact;

    if (!global.tiles.empty()) return global.tiles.front();
    return std::nullopt;
}

std::vector<map::MapTileRef> tiles_near_entry_impl(const map::GlobalMap& global, const map::EntryPoint& entry,
                                                   const LoadOptions& opts) {
    const auto origin = find_origin_tile_impl(global, entry);
    if (!origin) return {};

    const auto layout_sp = cached_tile_layout(global);
    const auto& layout = *layout_sp;
    if (opts.load_radius_m > 0.f) {
        struct Ranked {
            float dist = 0.f;
            int cheb = 0;
            map::MapTileRef ref{};
        };
        const float reach = opts.load_radius_m + (std::max)(0.f, opts.spline_reach_m);
        std::vector<Ranked> ranked;
        const int scan =
            (std::max)(2, static_cast<int>(std::ceil(reach / 150.f)) + 2);
        for (int dy = -scan; dy <= scan; ++dy) {
            for (int dx = -scan; dx <= scan; ++dx) {
                auto tile = find_tile(global, origin->grid_x + dx, origin->grid_y + dy);
                if (!tile) continue;
                const float d = tile_aabb_dist_m(global, layout, tile->grid_x, tile->grid_y, entry.position.x,
                                                 entry.position.y);
                const bool center = (dx == 0 && dy == 0);
                if (!center && d > reach) continue;
                Ranked r{};
                r.dist = d;
                r.cheb = (std::max)(std::abs(dx), std::abs(dy));
                r.ref = std::move(*tile);
                ranked.push_back(std::move(r));
            }
        }
        std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
            if (a.dist != b.dist) return a.dist < b.dist;
            return a.cheb < b.cheb;
        });
        std::vector<map::MapTileRef> out;
        out.reserve(ranked.size());
        for (auto& r : ranked) out.push_back(std::move(r.ref));
        return out;
    }

    // OMSI CalcTileLoadOrderIndex / Span: ring order by Chebyshev distance to centerkachel.
    struct Ranked {
        int cheb = 0;
        int dx = 0;
        int dy = 0;
        map::MapTileRef ref{};
    };
    std::vector<Ranked> ranked;
    const int radius = opts.tile_radius;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (auto tile = find_tile(global, origin->grid_x + dx, origin->grid_y + dy)) {
                Ranked r{};
                r.cheb = (std::max)(std::abs(dx), std::abs(dy));
                r.dx = dx;
                r.dy = dy;
                r.ref = *tile;
                ranked.push_back(std::move(r));
            }
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.cheb != b.cheb) return a.cheb < b.cheb;
        if (a.dy != b.dy) return a.dy < b.dy;
        return a.dx < b.dx;
    });
    std::vector<map::MapTileRef> out;
    out.reserve(ranked.size());
    for (auto& r : ranked) out.push_back(std::move(r.ref));
    return out;
}

} // namespace

float tile_aabb_dist_m(const map::GlobalMap& global, const map::MapTileLayout& layout, std::int32_t grid_x,
                       std::int32_t grid_y, float wx, float wy) {
    const auto rect = map::grid_tile_rect_at(global, layout, grid_x, grid_y);
    float minx = 0.f, miny = 0.f, maxx = map::kStandardTileMeters, maxy = map::kStandardTileMeters;
    if (rect.resolved) {
        minx = rect.origin_x;
        miny = rect.origin_y;
        maxx = rect.origin_x + rect.width_m;
        maxy = rect.origin_y + rect.height_m;
    } else {
        const auto o = map::tile_origin_xy(layout, grid_x, grid_y);
        minx = o.first;
        miny = o.second;
        maxx = minx + map::kStandardTileMeters;
        maxy = miny + map::kStandardTileMeters;
    }
    const float qx = (std::clamp)(wx, minx, maxx);
    const float qy = (std::clamp)(wy, miny, maxy);
    const float dx = wx - qx;
    const float dy = wy - qy;
    return std::sqrt(dx * dx + dy * dy);
}

std::optional<map::MapTileRef> find_origin_tile(const map::GlobalMap& global, const map::EntryPoint& entry) {
    return find_origin_tile_impl(global, entry);
}

std::optional<map::MapTileRef> find_tile_at_world(const map::GlobalMap& global, float world_x, float world_y) {
    const auto layout_sp = cached_tile_layout(global);
    const auto& layout = *layout_sp;
    if (const auto* metric = map::find_tile_at_world_position(layout, world_x, world_y)) {
        for (const auto& tile_ref : global.tiles) {
            if (tile_ref.grid_x == metric->grid_x && tile_ref.grid_y == metric->grid_y) return tile_ref;
        }
        const auto path =
            global.root / ("tile_" + std::to_string(metric->grid_x) + "_" + std::to_string(metric->grid_y) + ".map");
        if (omsi::vfs::exists(path))
            return map::MapTileRef{metric->grid_x, metric->grid_y, path};
    }
    map::EntryPoint fake{};
    fake.position.x = world_x;
    fake.position.y = world_y;
    fake.position.z = 0.f;
    return find_origin_tile_impl(global, fake);
}

std::optional<map::MapTileRef> find_existing_tile_at_world(const map::GlobalMap& global, const float world_x,
                                                           const float world_y) {
    const auto layout_sp = cached_tile_layout(global);
    const auto& layout = *layout_sp;
    if (const auto* metric = map::find_tile_at_world_position(layout, world_x, world_y)) {
        for (const auto& tile_ref : global.tiles) {
            if (tile_ref.grid_x == metric->grid_x && tile_ref.grid_y == metric->grid_y) return tile_ref;
        }
        const auto path =
            global.root / ("tile_" + std::to_string(metric->grid_x) + "_" + std::to_string(metric->grid_y) + ".map");
        if (omsi::vfs::exists(path))
            return map::MapTileRef{metric->grid_x, metric->grid_y, path};
    }
    return std::nullopt;
}

std::vector<map::MapTileRef> tiles_near_entry(const map::GlobalMap& global, const map::EntryPoint& entry,
                                            const LoadOptions& opts) {
    return tiles_near_entry_impl(global, entry, opts);
}

AsyncWorldLoader::~AsyncWorldLoader() {
    if (thread_.joinable()) thread_.join();
}

void AsyncWorldLoader::start(map::GlobalMap global, map::EntryPoint entry, LoadOptions opts) {
    start(std::move(global), entry, std::move(opts), {});
}

void AsyncWorldLoader::start(map::GlobalMap global, map::EntryPoint entry, LoadOptions opts,
                             std::vector<map::MapTileRef> only_tiles) {
    if (thread_.joinable()) thread_.join();

    global_ = std::move(global);
    entry_ = entry;
    opts_ = opts;
    only_tiles_ = std::move(only_tiles);
    world_ = World{};
    has_world_ = false;
    cancel_.store(false, std::memory_order_release);

    {
        std::lock_guard lock(mu_);
        progress_ = {};
        progress_.status = only_tiles_.empty() ? "Preparando tiles..." : "Cargando tiles nuevos...";
    }
    done_.store(false, std::memory_order_release);

    thread_ = std::thread([this] { worker(); });
}

void AsyncWorldLoader::request_cancel() { cancel_.store(true, std::memory_order_release); }

LoadProgress AsyncWorldLoader::poll() {
    std::lock_guard lock(mu_);
    return progress_;
}

World AsyncWorldLoader::take_world() {
    if (thread_.joinable()) thread_.join();
    return std::move(world_);
}

void AsyncWorldLoader::worker() {
    try {
        std::vector<map::MapTileRef> tiles =
            only_tiles_.empty() ? tiles_near_entry(global_, entry_, opts_) : only_tiles_;
        // Diff path: still order by distance to focus (OMSI ring) so nearest GPU first.
        if (!only_tiles_.empty()) {
            const auto origin = find_origin_tile_impl(global_, entry_);
            if (origin) {
                std::sort(tiles.begin(), tiles.end(), [&](const map::MapTileRef& a, const map::MapTileRef& b) {
                    const int da = (std::max)(std::abs(a.grid_x - origin->grid_x), std::abs(a.grid_y - origin->grid_y));
                    const int db = (std::max)(std::abs(b.grid_x - origin->grid_x), std::abs(b.grid_y - origin->grid_y));
                    if (da != db) return da < db;
                    if (a.grid_y != b.grid_y) return a.grid_y < b.grid_y;
                    return a.grid_x < b.grid_x;
                });
            }
        }
        const std::size_t total = tiles.empty() ? 1 : tiles.size();

        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        const unsigned n_workers =
            tiles.empty() ? 1u
                          : (std::min)(hw, static_cast<unsigned>((std::max)(std::size_t{1}, tiles.size())));

        log::info("async load \"" + global_.friendly_name + "\" tiles=" + std::to_string(tiles.size()) +
                  (only_tiles_.empty() ? " (near)" : " (diff)") + "/" + std::to_string(global_.tiles.size()) +
                  " workers=" + std::to_string(n_workers) + "/" + std::to_string(hw));

        if (tiles.empty()) {
            // Diff stream with nothing to add: empty success world for merge-only/remove path.
            world_.begin_load(global_, entry_, opts_);
            world_.finish_load();
            std::lock_guard lock(mu_);
            progress_.fraction = 1.f;
            progress_.status = only_tiles_.empty() ? "Mapa sin tiles cerca del entrypoint" : "Sin tiles nuevos";
            progress_.done = true;
            progress_.failed = only_tiles_.empty();
            has_world_ = true;
            done_.store(true, std::memory_order_release);
            return;
        }

        {
            std::lock_guard lock(mu_);
            progress_.fraction = 0.02f;
            progress_.status = "Cargando " + std::to_string(tiles.size()) + " tiles (0/" +
                               std::to_string(tiles.size()) + ")";
        }
        world_.begin_load(global_, entry_, opts_);

        std::vector<std::optional<map::MapTileData>> parsed(tiles.size());
        std::atomic<std::size_t> parsed_n{0};
        std::vector<std::size_t> parse_idx(tiles.size());
        for (std::size_t i = 0; i < parse_idx.size(); ++i) parse_idx[i] = i;
        // Path order → sequential .map reads on the same volume.
        std::sort(parse_idx.begin(), parse_idx.end(), [&](std::size_t a, std::size_t b) {
            return tiles[a].tile_path.lexically_normal().wstring() <
                   tiles[b].tile_path.lexically_normal().wstring();
        });
        const char* hdd_env0 = std::getenv("OMSI_HDD_LOAD");
        if (!hdd_env0 || !hdd_env0[0]) hdd_env0 = std::getenv("OMSI_LOAD_HDD");
        const bool hdd_parse = hdd_env0 && hdd_env0[0] && hdd_env0[0] != '0';
        auto parse_one = [&](std::size_t i) {
            try {
                if (cancel_.load(std::memory_order_acquire)) return;
                const auto disk = map::workspace_tile_map_for_read(opts_.workspace, tiles[i].tile_path);
                parsed[i] = map::load_map_tile(disk, opts_.omsi_root);
                if (parsed[i]) {
                    parsed[i]->path = tiles[i].tile_path;
                    if (parsed[i]->grid_x == 0 && parsed[i]->grid_y == 0) {
                        parsed[i]->grid_x = tiles[i].grid_x;
                        parsed[i]->grid_y = tiles[i].grid_y;
                    }
                }
            } catch (const std::exception& e) {
                log::error(std::string("world_load_job parse tile: ") + e.what());
            } catch (...) {
                log::error("world_load_job parse tile: unknown exception");
            }
            const std::size_t n = parsed_n.fetch_add(1) + 1;
            std::lock_guard lock(mu_);
            progress_.fraction = 0.02f + 0.38f * (static_cast<float>(n) / static_cast<float>(total));
            progress_.status = "Cargando " + std::to_string(total) + " tiles (" + std::to_string(n) + "/" +
                               std::to_string(total) + ")";
        };
        if (hdd_parse) {
            for (const std::size_t i : parse_idx) parse_one(i);
        } else {
            std::for_each(std::execution::par, parse_idx.begin(), parse_idx.end(), parse_one);
        }

        if (!cancel_.load(std::memory_order_acquire)) {
            std::vector<std::filesystem::path> scos;
            std::vector<std::filesystem::path> slis;
            std::unordered_set<std::string> seen_sco;
            std::unordered_set<std::string> seen_sli;
            for (const auto& tile : parsed) {
                if (!tile) continue;
                auto add = [](std::vector<std::filesystem::path>& out, std::unordered_set<std::string>& seen,
                              const std::filesystem::path& p) {
                    if (p.empty()) return;
                    if (seen.insert(p.lexically_normal().string()).second) out.push_back(p);
                };
                for (const auto& obj : tile->objects) add(scos, seen_sco, obj.sco_path);
                for (const auto& a : tile->attachments) add(scos, seen_sco, a.sco_path);
                for (const auto& sa : tile->spline_attachments) add(scos, seen_sco, sa.sco_path);
                if (opts_.load_splines) {
                    for (const auto& sp : tile->splines) add(slis, seen_sli, sp.sli_path);
                }
            }
            {
                std::lock_guard lock(mu_);
                progress_.status = "CPU prefetch sco=" + std::to_string(scos.size()) +
                                   " sli=" + std::to_string(slis.size()) + "…";
                progress_.fraction = 0.42f;
            }
            // Two-pass I/O (HDD rule): unique + sorted paths → sequential seeks, never re-parse.
            // OMSI_HDD_LOAD=1 or OMSI_LOAD_HDD=1 → single-thread sequential (no parallel seeks).
            const char* hdd_env = std::getenv("OMSI_HDD_LOAD");
            if (!hdd_env || !hdd_env[0]) hdd_env = std::getenv("OMSI_LOAD_HDD");
            const bool hdd_mode = hdd_env && hdd_env[0] && hdd_env[0] != '0';
            sco::preload_scos(scos, hdd_mode);
            if (slis.size() >= 1) {
                std::sort(slis.begin(), slis.end());
                if (hdd_mode || slis.size() == 1) {
                    for (const auto& p : slis) (void)spline::load_sli_shared(p);
                } else {
                    std::for_each(std::execution::par, slis.begin(), slis.end(), [](const std::filesystem::path& p) {
                        try {
                            (void)spline::load_sli_shared(p);
                        } catch (...) {
                        }
                    });
                }
            }
            std::vector<std::filesystem::path> o3ds;
            std::unordered_set<std::string> seen_o3d;
            for (const auto& p : scos) {
                const auto* sco = sco::load_sco(p);
                if (!sco) continue;
                for (const auto& mesh_ref : sco->meshes) {
                    if (!mesh_ref.is_o3d) continue;
                    const auto abs = mesh_ref.mesh_file.is_absolute() ? mesh_ref.mesh_file
                                                                     : opts_.omsi_root / mesh_ref.mesh_file;
                    if (seen_o3d.insert(abs.lexically_normal().string()).second) o3ds.push_back(abs);
                }
            }
            std::sort(o3ds.begin(), o3ds.end());
            if (o3ds.size() >= 1) {
                if (hdd_mode || o3ds.size() == 1) {
                    for (const auto& p : o3ds) (void)o3d::load_mapped_shared(p);
                } else {
                    std::for_each(std::execution::par, o3ds.begin(), o3ds.end(), [](const std::filesystem::path& p) {
                        try {
                            (void)o3d::load_mapped_shared(p);
                        } catch (...) {
                        }
                    });
                }
            }
            log::info(std::string("async load prefetch sco=") + std::to_string(scos.size()) +
                      " sli=" + std::to_string(slis.size()) + " o3d=" + std::to_string(o3ds.size()) +
                      (hdd_mode ? " mode=HDD(seq)" : " mode=SSD(par)"));
        }

        std::atomic<std::size_t> done_count{0};
        std::vector<World> parts(n_workers);
        std::vector<std::thread> workers;
        workers.reserve(n_workers);

        for (unsigned w = 0; w < n_workers; ++w) {
            workers.emplace_back([&, w] {
                parts[w].begin_load(global_, entry_, opts_);
                for (std::size_t i = w; i < tiles.size(); i += n_workers) {
                    if (cancel_.load(std::memory_order_acquire)) break;
                    if (!parsed[i]) continue;
                    parts[w].load_parsed_tile(std::move(*parsed[i]), opts_, false);
                    const std::size_t finished = done_count.fetch_add(1) + 1;
                    std::lock_guard lock(mu_);
                    progress_.fraction = 0.45f + 0.55f * (static_cast<float>(finished) / static_cast<float>(total));
                    progress_.status = "Tile " + std::to_string(finished) + "/" + std::to_string(total) +
                                       " (" + std::to_string(n_workers) + " hilos): " +
                                       tiles[i].tile_path.filename().string();
                }
                parts[w].finish_load();
            });
        }
        for (auto& th : workers) th.join();

        if (cancel_.load(std::memory_order_acquire)) {
            world_ = World{};
            {
                std::lock_guard lock(mu_);
                progress_.fraction = 0.f;
                progress_.status = "Cancelado — nuevo foco";
                progress_.done = true;
                progress_.cancelled = true;
                progress_.failed = false;
                has_world_ = true;
            }
            done_.store(true, std::memory_order_release);
            log::info("async load cancelled (mouse/camera focus moved)");
            return;
        }

        for (auto& part : parts) world_.merge_from(std::move(part));
        world_.finish_load();

        {
            std::lock_guard lock(mu_);
            progress_.fraction = 1.f;
            progress_.status = "Listo — meshes=" + std::to_string(world_.meshes().size()) + " workers=" +
                               std::to_string(n_workers);
            progress_.done = true;
            has_world_ = true;
        }
        done_.store(true, std::memory_order_release);

        log::info("async load done meshes=" + std::to_string(world_.meshes().size()) +
                  " workers=" + std::to_string(n_workers));
    } catch (const std::exception& ex) {
        log::error(std::string("async load exception: ") + ex.what());
        std::lock_guard lock(mu_);
        progress_.done = true;
        progress_.failed = true;
        progress_.status = ex.what();
        has_world_ = true;
        done_.store(true, std::memory_order_release);
    } catch (...) {
        log::error("async load unknown exception");
        std::lock_guard lock(mu_);
        progress_.done = true;
        progress_.failed = true;
        progress_.status = "unknown error";
        has_world_ = true;
        done_.store(true, std::memory_order_release);
    }
}

} // namespace omsi::world
