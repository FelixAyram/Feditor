#pragma once

// Terrain AlignTerrain re-stamp pipeline (pure C++, no renderer / UI dependencies).
//
// WHY THIS EXISTS
//   The editor used to restamp each terrain tile inside a giant lambda on the main thread. Per tile
//   (x ~315 tiles on the large map) it rebuilt two hash maps over every object/spline, re-resolved
//   every spline-attachment pose, re-loaded and re-copied every .sli hole profile, and then ran the
//   stamp loops - all while the frame was blocked (main_hang.log: terrain_align_gpu_reload, up to 23 s).
//
//   Everything here follows one rule: the main thread only takes a cheap immutable SNAPSHOT
//   (TerrainRestampContext) and later APPLIES finished results. All heavy work (stamping, polycut
//   ring tessellation, hole masks, attachment poses, .sco hole probes) runs on JobSystem workers.
//   The per-batch work that does not depend on the tile is done once per context, not once per tile.

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "editor/terrain_polycut.hpp"
#include "omsi/map_types.hpp"
#include "omsi/spline_types.hpp"

namespace omsi::editor {

// One object (or attachment child) flagged align_terrain. Plain data copied on the main thread.
struct TerrainRestampObjectIn {
    f4f23ma::f32 x = 0.f;
    f4f23ma::f32 y = 0.f;
    f4f23ma::f32 z = 0.f;
    f4f23ma::f32 rot_z = 0.f;
    std::filesystem::path sco_path;
    // OMSI heading (MapObjectInst::rotation_deg.x). Used to place the [terrainhole] outline.
    f4f23ma::f32 yaw_deg = 0.f;
};

// True when the .sco has an [absheight] [terrainhole] whose outline the OMSI editor cached as
// "<hole mesh>.hole". With AlignTerrain such an object cuts the terrain along that outline, so the
// authored .map Z is final (no ground-layer lift). Thread-safe, cached per .sco.
[[nodiscard]] bool sco_has_terrain_hole_outline(const std::filesystem::path& sco_path);

// One [splineAttachement] with align_terrain. parent_index indexes the splines vector passed to build().
struct TerrainRestampSaIn {
    map::MapSplineAttachmentInst att;
    std::int32_t parent_index = -1;
};

struct TerrainRestampResult {
    std::vector<float> heights;
    std::vector<std::uint8_t> hole_mask;
    std::vector<TerrainHoleRing> polycut_rings;
    // false when heights.size() < dim*dim: the old code returned before touching rings/mask, so the
    // caller must keep the field's previous hole_mask / polycut_rings in that case.
    bool has_cut_data = false;
};

// Immutable once built; safe to share between worker threads.
class TerrainRestampContext {
public:
    // Main thread. Cheap: copies the splines once and the (few) align_terrain objects / SA records.
    [[nodiscard]] static std::shared_ptr<TerrainRestampContext>
    build(const std::vector<map::SplineInst>& splines, std::vector<TerrainRestampObjectIn> objects,
          std::vector<TerrainRestampSaIn> spline_attachments);

    // Thread-safe, idempotent. Resolves .sco hole metadata and SA poses ONCE for all tiles.
    void ensure_prepared() const;

    // Thread-safe and const. Equivalent to the old restamp_terrain_field body.
    [[nodiscard]] TerrainRestampResult stamp_tile(const std::vector<float>& heights_base,
                                                  f4f23ma::f32 rel_x, f4f23ma::f32 rel_y,
                                                  f4f23ma::f32 size_m) const;

    [[nodiscard]] const spline::TerrainAlignPlan& plan() const { return *plan_; }

private:
    struct PreparedObject {
        f4f23ma::f32 x, y, z, rot_z;
        f4f23ma::f32 overlap_radius;
        f4f23ma::f32 half;
        bool has_hole;
        // [terrainhole] outline rings in world XY + absolute Z (empty: legacy circle / square path).
        std::vector<std::vector<std::array<f4f23ma::f32, 3>>> outline;
        f4f23ma::f32 out_minx = 0.f, out_miny = 0.f, out_maxx = 0.f, out_maxy = 0.f;
    };
    TerrainRestampContext() = default;
    void prepare_impl() const;

    std::shared_ptr<const spline::TerrainAlignPlan> plan_;
    std::vector<TerrainRestampObjectIn> objects_in_;
    std::vector<TerrainRestampSaIn> sa_in_;
    mutable std::once_flag prepared_once_;
    mutable std::vector<PreparedObject> objects_;
    mutable std::vector<std::array<f4f23ma::f32, 3>> sa_poses_;
};

struct TerrainRestampTileIn {
    std::int64_t key = 0;
    std::uint64_t token = 0;
    std::vector<float> heights_base;
    f4f23ma::f32 rel_x = 0.f;
    f4f23ma::f32 rel_y = 0.f;
    f4f23ma::f32 size_m = 300.f;
    // When set, stamping is skipped and this (already stamped) data is used as-is. Lets the same
    // queue build GPU meshes for tiles whose RAM heightfield is already up to date.
    std::shared_ptr<const TerrainRestampResult> prestamped;
    // Opaque per-tile data for the post hook (paths, texture info, ...), copied on the main thread.
    std::shared_ptr<const void> user;
};

struct TerrainRestampDone {
    std::int64_t key = 0;
    std::uint64_t token = 0;
    bool stamped = false; // false when the input was prestamped (result then mirrors it)
    TerrainRestampResult result;
    // Whatever the post hook returned (e.g. a finished GPU mesh). Null when there is no hook.
    std::shared_ptr<void> payload;
};

// Runs on the worker right after stamping. Must be thread-safe and must not touch UI/renderer state.
using TerrainRestampPostHook =
    std::function<std::shared_ptr<void>(const TerrainRestampTileIn& in, const TerrainRestampResult& result)>;

// Fire-and-forget batch runner. submit()/drain() are meant for the main thread; the stamping runs
// on JobSystem workers at Low priority and never blocks the caller.
class TerrainRestampQueue {
public:
    TerrainRestampQueue() : state_(std::make_shared<State>()) {}

    // Takes ownership of the tiles. Each tile becomes one worker job.
    void submit(const std::shared_ptr<const TerrainRestampContext>& ctx, std::vector<TerrainRestampTileIn> tiles,
                TerrainRestampPostHook post = {});

    // Finished results (at most max_items). Results of cancelled batches are dropped.
    [[nodiscard]] std::vector<TerrainRestampDone> drain(std::size_t max_items = static_cast<std::size_t>(-1));

    // Tiles submitted whose result has not been drained yet (running + finished-but-undrained).
    [[nodiscard]] std::size_t outstanding() const { return state_->outstanding.load(std::memory_order_acquire); }

    // Discard everything in flight (e.g. the map was closed). Workers finish but results are dropped.
    void cancel_all();

private:
    struct State {
        std::mutex mu;
        std::vector<TerrainRestampDone> done;
        std::atomic<std::size_t> outstanding{0};
        std::atomic<std::uint64_t> generation{0};
    };
    std::shared_ptr<State> state_;
};

// Small thread-safe LRU of immutable values keyed by tile. Used to keep each tile's pristine terrain
// mesh (vertices/indices/texture path) so re-uploading a restamped tile never re-reads the .terrain,
// the ground masks or re-bakes the ground paint on the main thread.
template <class T>
class KeyedLruCache {
public:
    explicit KeyedLruCache(std::size_t capacity = 192) : capacity_(capacity ? capacity : 1) {}

    [[nodiscard]] std::shared_ptr<const T> get(std::int64_t key) {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        order_.splice(order_.begin(), order_, it->second.second);
        return it->second.first;
    }

    void put(std::int64_t key, std::shared_ptr<const T> value) {
        std::lock_guard<std::mutex> lk(mu_);
        if (const auto it = map_.find(key); it != map_.end()) {
            it->second.first = std::move(value);
            order_.splice(order_.begin(), order_, it->second.second);
            return;
        }
        order_.push_front(key);
        map_.emplace(key, std::make_pair(std::move(value), order_.begin()));
        while (map_.size() > capacity_) {
            map_.erase(order_.back());
            order_.pop_back();
        }
    }

    void erase(std::int64_t key) {
        std::lock_guard<std::mutex> lk(mu_);
        if (const auto it = map_.find(key); it != map_.end()) {
            order_.erase(it->second.second);
            map_.erase(it);
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        map_.clear();
        order_.clear();
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return map_.size();
    }

private:
    mutable std::mutex mu_;
    std::size_t capacity_;
    std::list<std::int64_t> order_;
    std::unordered_map<std::int64_t, std::pair<std::shared_ptr<const T>, std::list<std::int64_t>::iterator>> map_;
};

} // namespace omsi::editor
