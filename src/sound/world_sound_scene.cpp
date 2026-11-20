#include "omsi/sound/world_sound_scene.hpp"

#include "omsi/map_types.hpp"
#include "omsi/sco_types.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>

namespace omsi::sound {
namespace {

constexpr float kSyncSec = 0.2f;        // range checks ~5 Hz
constexpr int kMaxStartPerSync = 6;     // spreads decode/start work over frames
constexpr int kSceneVoiceCap = 40;      // leaves headroom in the WorldSound pool (48)
constexpr float kReachMarginM = 5.f;    // hysteresis so objects at the edge do not flicker
constexpr float kDefaultRangeM = 50.f;  // when a cfg has no [3d]

float loop_pitch(const SoundDef& d, const WorldSound& ws) {
    if (d.pitch_var.empty() || d.pitch_ref <= 0.f) return 1.f;
    const float v = ws.var(d.pitch_var, d.pitch_ref);
    return std::clamp(v / d.pitch_ref, 0.05f, 4.f);
}

Vec3f loop_position(const SoundDef& d, float yaw_deg, const Vec3f& origin) {
    return d.has_3d ? object_offset_to_world(d.offset, yaw_deg, origin) : origin;
}

} // namespace

int WorldSoundScene::live_voices() const {
    int n = 0;
    for (const auto& [ci, L] : live_) n += static_cast<int>(L.ids.size());
    return n;
}

void WorldSoundScene::stop_live(std::size_t obj_index) {
    const auto it = live_.find(obj_index);
    if (it == live_.end()) return;
    auto& ws = WorldSound::instance();
    for (SoundId id : it->second.ids) ws.stop(id);
    live_.erase(it);
}

void WorldSoundScene::reset() {
    while (!live_.empty()) stop_live(live_.begin()->first);
    cands_.clear();
    scan_pos_ = 0;
    scan_done_ = false;
    size_ = 0;
    sync_timer_ = 0.f;
}

const WorldSoundScene::ScoLoops* WorldSoundScene::sco_loops(const std::filesystem::path& sco_path) {
    if (const auto it = by_sco_.find(sco_path); it != by_sco_.end()) return &it->second;
    ScoLoops out;
    if (const sco::SceneryObject* so = sco::load_sco(sco_path)) {
        for (const auto& cfg_path : so->sound_cfgs) {
            const auto cfg = load_sound_cfg(cfg_path);
            if (!cfg) continue;
            for (const auto& d : cfg->defs) {
                if (d.kind != SoundDef::Kind::Loop || !d.trigger.empty() || !d.conditions.empty() || d.no_loop)
                    continue;
                if (!d.wav.empty()) out.loops.push_back(d);
                out.reach_m = std::max(out.reach_m, d.has_3d ? d.range_m : kDefaultRangeM);
            }
        }
    }
    return &by_sco_.emplace(sco_path, std::move(out)).first->second;
}

void WorldSoundScene::scan_some(const std::vector<map::MapObjectInst>& objects) {
    const auto t0 = std::chrono::steady_clock::now();
    int n = 0;
    while (scan_pos_ < objects.size()) {
        const std::size_t i = scan_pos_++;
        const auto& o = objects[i];
        if (!o.sco_path.empty()) {
            const ScoLoops* sl = sco_loops(o.sco_path);
            if (sl && !sl->loops.empty()) {
                Candidate c;
                c.index = i;
                c.sco = o.sco_path;
                c.loops = sl->loops;
                c.reach_m = sl->reach_m;
                cands_.push_back(std::move(c));
            }
        }
        // (about a millisecond a frame; the rest next frame)
        if ((++n & 255) == 0 &&
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > 1.0)
            return;
    }
    scan_done_ = true;
    // voices of objects that have no candidate any more (the vector shrank and was looked at again)
    std::unordered_set<std::size_t> keep;
    for (const auto& c : cands_) keep.insert(c.index);
    for (auto it = live_.begin(); it != live_.end();) {
        if (keep.count(it->first)) {
            ++it;
            continue;
        }
        auto& ws = WorldSound::instance();
        for (SoundId id : it->second.ids) ws.stop(id);
        it = live_.erase(it);
    }
}

void WorldSoundScene::tick(const std::vector<map::MapObjectInst>& objects, Vec3f listener, float dt_sec) {
    auto& ws = WorldSound::instance();
    if (!ws.ready()) return;

    if (!ws.enabled()) {
        while (!live_.empty()) stop_live(live_.begin()->first);
        return;
    }

    // Objects only added (streaming): the new ones are looked at, a little each frame. Objects gone:
    // indices may have moved, so the list is looked at again from the start (voices kept meanwhile).
    if (objects.size() < size_) {
        cands_.clear();
        scan_pos_ = 0;
        scan_done_ = false;
    } else if (objects.size() > size_) {
        scan_done_ = false;
    }
    size_ = objects.size();
    if (!scan_done_) scan_some(objects);

    sync_timer_ += dt_sec;
    if (sync_timer_ < kSyncSec) return;
    sync_timer_ = 0.f;

    int started = 0;
    for (std::size_t ci = 0; ci < cands_.size(); ++ci) {
        const Candidate& c = cands_[ci];
        const bool valid = c.index < objects.size() && objects[c.index].sco_path == c.sco;
        if (!valid) {
            stop_live(c.index);
            continue;
        }
        const auto& o = objects[c.index];
        const Vec3f origin{o.position.x, o.position.y, o.position.z};
        const float dx = origin.x - listener.x;
        const float dy = origin.y - listener.y;
        const float dz = origin.z - listener.z;
        const float reach = c.reach_m + kReachMarginM;
        if (dx * dx + dy * dy + dz * dz > reach * reach) {
            stop_live(c.index);
            continue;
        }

        const auto it = live_.find(c.index);
        if (it == live_.end()) {
            if (started >= kMaxStartPerSync) continue;
            if (live_voices() + static_cast<int>(c.loops.size()) > kSceneVoiceCap) continue;
            Live L;
            for (const auto& d : c.loops) {
                EmitterDesc e;
                e.pos = loop_position(d, o.rotation_deg.y, origin);
                e.range_m = d.has_3d ? d.range_m : kDefaultRangeM;
                e.volume = d.volume;
                e.pitch = loop_pitch(d, ws);
                e.loop = true;
                L.ids.push_back(ws.play(d.wav, e));
            }
            live_.emplace(c.index, std::move(L));
            ++started;
        } else {
            // Keep voices in sync with the object (moved by the user) and with engine variables.
            for (std::size_t j = 0; j < c.loops.size() && j < it->second.ids.size(); ++j) {
                const SoundId id = it->second.ids[j];
                ws.set_position(id, loop_position(c.loops[j], o.rotation_deg.y, origin));
                ws.set_pitch(id, loop_pitch(c.loops[j], ws));
            }
        }
    }
}

} // namespace omsi::sound
