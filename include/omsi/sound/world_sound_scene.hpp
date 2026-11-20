#pragma once

// Editor glue: turns placed scenery objects (MapObjectInst -> .sco [sound] -> sound.cfg) into
// positional world sounds around the editor camera. Main thread only calls tick(); audio work
// happens inside WorldSound (worker thread + miniaudio).
//
// What starts by itself: unconditional, trigger-less [loopsound] definitions of objects within
// audible range. Everything else (triggers, conditionSingle/Bool/Int, one-shots) is driven by the
// hook API in omsi/sound/world_sound.hpp (play_cfg with a trigger name, set_var, ...).

#include "omsi/sound/world_sound.hpp"
#include "omsi/sound/world_sound_cfg.hpp"

#include <cstddef>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace omsi::map {
struct MapObjectInst;
}

namespace omsi::sound {

class WorldSoundScene {
public:
    // Call once per frame (after the camera moved). Cheap: candidates are rebuilt only when the
    // object vector changes; range checks run at ~5 Hz and start at most a few objects per tick.
    void tick(const std::vector<map::MapObjectInst>& objects, Vec3f listener_pos, float dt_sec);

    // Stops every voice started by this scene and forgets the candidate list.
    void reset();

    [[nodiscard]] int candidate_count() const { return static_cast<int>(cands_.size()); }
    [[nodiscard]] int live_objects() const { return static_cast<int>(live_.size()); }
    [[nodiscard]] int live_voices() const;

private:
    struct Candidate {
        std::size_t index = 0;        // index into the objects vector
        std::filesystem::path sco;    // identity check against the vector
        std::vector<SoundDef> loops;  // auto-start loops
        float reach_m = 0.f;          // largest audible range of its loops
    };
    struct Live {
        std::vector<SoundId> ids; // one voice per loop, same order as Candidate::loops
    };

    // what one .sco brings: its auto-start loops (empty: none), read once per .sco
    struct ScoLoops {
        std::vector<SoundDef> loops;
        float reach_m = 0.f;
    };
    struct PathHash {
        std::size_t operator()(const std::filesystem::path& p) const noexcept { return std::filesystem::hash_value(p); }
    };

    // Looks at objects [scan_pos_, size) for a while (a streaming map grows the vector every frame: going
    // through all 47k objects and their .sco again each time held Thuringer Wald at 5 fps).
    void scan_some(const std::vector<map::MapObjectInst>& objects);
    const ScoLoops* sco_loops(const std::filesystem::path& sco);
    void stop_live(std::size_t obj_index);

    std::vector<Candidate> cands_;
    std::unordered_map<std::size_t, Live> live_; // object index -> voices
    std::unordered_map<std::filesystem::path, ScoLoops, PathHash> by_sco_;
    std::size_t scan_pos_ = 0;
    bool scan_done_ = false;
    std::size_t size_ = 0;
    float sync_timer_ = 0.f;
};

} // namespace omsi::sound
