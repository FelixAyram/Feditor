#pragma once
// Async BeamNG asset pipeline: worker threads read shapes from the VFS, parse COLLADA, resolve materials and
// produce GPU-ready meshes (editor vertex format + one sub-range per material). The editor thread only drains
// finished results and uploads them under its frame budget.
#include "beamng/bng_level.hpp"
#include "omsi/o3d_format.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bng {

class Vfs;

struct ReadyMesh {
    struct Sub {
        std::uint32_t index_start = 0;
        std::uint32_t index_count = 0;
        std::string texture;   // "vfs:/..." or empty (untextured)
        std::string normal, detail, detail_normal; // "vfs:/..." or empty
        float detail_scale = 0.f;
        float detail_scale_y = 0.f;
        bool has_solid = false; // no albedo map: draw this colour (vertex colour average x base colour)
        float solid[4]{1.f, 1.f, 1.f, 1.f};
        std::uint8_t alpha_mode = 0; // 0 opaque, 1 alpha test, 2 blend
        bool double_sided = false;
        // BeamNG opacityMap ("vfs:/..." or empty): the mask lives in its alpha / luminance and is
        // multiplied by the colour alpha. opacity_factor = stage0.opacityFactor.
        std::string opacity;
        float opacity_factor = 1.f;
        // stage0.baseColorFactor / diffuseColor (rgb) — the BeamNG 1x1 material params tint.
        float base_color[4]{1.f, 1.f, 1.f, 1.f};
        // alphaRef from materials.json (0..255) → PS discard threshold.
        int alpha_ref = 127;
        // BeamNG detailDistances far edge in metres (0 = renderer default 80 m).
        float detail_distance = 0.f;
        std::string material;
    };
    struct Lod {
        int detail_size = 0;
        std::vector<omsi::o3d::Vertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<Sub> subs;
    };
    std::vector<Lod> lods; // [0] = most detailed
    // lods[0] mirrored here for older callers
    std::vector<omsi::o3d::Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Sub> subs;
    float bmin[3]{0, 0, 0}, bmax[3]{0, 0, 0};
    float radius = 0.f;
    int lod_count = 0;
    int detail_size = 0;
    std::size_t triangles = 0;
    bool ok = false;
    std::string error;
};

// Global material table for shared assets (/assets, /art, /core) built once in the background.
class MaterialIndex {
public:
    void build(const Vfs& vfs);              // blocking (call on a worker)
    bool ready() const { return ready_.load(); }
    const Material* find(const std::string& name_or_map_to) const;
    std::size_t size() const { return mats_.size(); }

private:
    std::unordered_map<std::string, Material> mats_;
    std::unordered_map<std::string, std::string> by_map_to_;
    // base material name (per-LOD suffix stripped) -> name, for "<mat>_lod" lookups that have no exact entry.
    std::unordered_map<std::string, std::string> by_base_;
    std::atomic<bool> ready_{false};
};

class AssetPool {
public:
    AssetPool(const Vfs& vfs, const Level& level, int threads = 0);
    ~AssetPool();
    AssetPool(const AssetPool&) = delete;
    AssetPool& operator=(const AssetPool&) = delete;

    // Lower priority value = loaded sooner (use distance to camera). Re-requesting only bumps the priority.
    void request(const std::string& shape, float priority);
    // Main thread: next finished mesh (false when none).
    bool take_ready(std::string& shape, std::shared_ptr<ReadyMesh>& out);
    std::size_t pending() const;
    std::size_t done_count() const { return done_.load(); }
    // BeamNG $pref::TS::detailAdjust: picks a coarser LOD below 1.
    void set_detail_adjust(float v) { detail_adjust_.store(v); }
    const MaterialIndex& materials() const { return *global_mats_; }

private:
    void worker_main();
    std::shared_ptr<ReadyMesh> build(const std::string& shape);

    const Vfs& vfs_;
    const Level& level_;
    std::shared_ptr<MaterialIndex> global_mats_;
    std::thread mat_thread_;
    std::vector<std::thread> threads_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<std::string, float> queued_;       // shape -> priority (not started yet)
    std::unordered_set<std::string> known_;               // ever requested
    std::deque<std::pair<std::string, std::shared_ptr<ReadyMesh>>> ready_;
    std::atomic<bool> stop_{false};
    std::atomic<std::size_t> done_{0};
    std::atomic<float> detail_adjust_{1.f};
};

// Converts a ReadyMesh vertex buffer for an already-built DaeShape (used by thumbnails too).
std::shared_ptr<ReadyMesh> make_ready_mesh(const Vfs& vfs, const Level& level, const MaterialIndex* global,
                                           const std::string& shape_path, float detail_adjust);

// Alpha mode a subrange of this material is drawn with: 0 = opaque, 1 = cutout (alphaTest), 2 = blend.
// alphaTest wins over opacityMap/translucent, so foliage is clipped instead of blended.
std::uint8_t alpha_mode_for(const Material& m);

// rotationMatrix (row-major, column vectors) <-> editor yaw/pitch/bank degrees (OMSI convention used by the
// editor tools: R = Rz(-yaw) * Rx(-pitch) * Ry(-bank)).
void mat3_to_editor_euler(const Mat3& r, float& yaw_deg, float& pitch_deg, float& bank_deg);
Mat3 editor_euler_to_mat3(float yaw_deg, float pitch_deg, float bank_deg);

} // namespace bng
