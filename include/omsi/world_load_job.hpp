#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "omsi/map_types.hpp"
#include "omsi/world.hpp"

namespace omsi::world {

struct LoadProgress {
    float fraction = 0.f;
    std::string status;
    bool done = false;
    bool failed = false;
    bool cancelled = false; // superseded by a newer stream focus (mouse moved away)
};

// Tile selection: load_radius_m > 0 → AABB metres from focus; else Chebyshev tile_radius (tests).
[[nodiscard]] std::optional<map::MapTileRef> find_origin_tile(const map::GlobalMap& global,
                                                             const map::EntryPoint& entry);
[[nodiscard]] std::optional<map::MapTileRef> find_tile_at_world(const map::GlobalMap& global, float world_x,
                                                               float world_y);
// Strict: only a tile whose .map exists at the camera cell — no fallback to nearest/origin.
[[nodiscard]] std::optional<map::MapTileRef> find_existing_tile_at_world(const map::GlobalMap& global,
                                                                         float world_x, float world_y);
[[nodiscard]] float tile_aabb_dist_m(const map::GlobalMap& global, const map::MapTileLayout& layout,
                                     std::int32_t grid_x, std::int32_t grid_y, float wx, float wy);
[[nodiscard]] std::vector<map::MapTileRef> tiles_near_entry(const map::GlobalMap& global,
                                                           const map::EntryPoint& entry,
                                                           const LoadOptions& opts);

// Incremental map load on a worker thread (OMSI pumps UI between tiles; we do the same).
class AsyncWorldLoader {
public:
    AsyncWorldLoader() = default;
    ~AsyncWorldLoader();

    AsyncWorldLoader(const AsyncWorldLoader&) = delete;
    AsyncWorldLoader& operator=(const AsyncWorldLoader&) = delete;

    void start(map::GlobalMap global, map::EntryPoint entry, LoadOptions opts);
    // Only load these tiles (streaming diff). Empty → tiles_near_entry.
    void start(map::GlobalMap global, map::EntryPoint entry, LoadOptions opts,
               std::vector<map::MapTileRef> only_tiles);
    // Abort in-flight load (checked between tiles). Caller should poll until finished then discard.
    void request_cancel();
    [[nodiscard]] LoadProgress poll();
    [[nodiscard]] bool finished() const { return done_.load(std::memory_order_acquire); }
    [[nodiscard]] World take_world();

private:
    void worker();

    map::GlobalMap global_{};
    map::EntryPoint entry_{};
    LoadOptions opts_{};
    std::vector<map::MapTileRef> only_tiles_{};
    World world_{};

    std::thread thread_{};
    std::mutex mu_{};
    LoadProgress progress_{};
    std::atomic<bool> done_{false};
    std::atomic<bool> cancel_{false};
    bool has_world_ = false;
};

} // namespace omsi::world
