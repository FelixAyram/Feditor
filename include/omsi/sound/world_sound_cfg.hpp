#pragma once

// Parser for OMSI sound definitions used by scenery objects (.sco "[sound]" -> *.cfg) and vehicles.
// Only the parts needed for positional world sound are read; [volcurve]/[pnt] are skipped.

#include "omsi/sound/world_sound.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace omsi::sound {

struct SoundCondition {
    std::string var;
    float ref = 0.f;
    int op = 3; // OMSI conditionSingle: 0:<>, 1:=, 2:<, 3:>, 4:<=, 5:>=
};

struct SoundDef {
    enum class Kind { OneShot, Loop };
    Kind kind = Kind::OneShot;
    std::filesystem::path wav;         // resolved against the cfg folder; may not exist
    float volume = 1.f;                // [sound] / [loopsound] volume (last field)
    std::string trigger;               // [trigger] name; empty = not trigger-driven
    bool no_loop = false;              // [noloop]
    // [3d]: offset from the object origin in OMSI axes (x, y, z=height) and audible radius.
    Vec3f offset{};
    float range_m = 50.f;
    bool has_3d = false;
    // [loopsound] pitch = var / ref (engine rpm style).
    std::string pitch_var;
    float pitch_ref = 0.f;
    std::vector<SoundCondition> conditions; // all must hold (AND)
};

struct SoundCfg {
    std::filesystem::path path;
    std::vector<SoundDef> defs;
};

// Cached, thread-safe. Returns nullptr when the file cannot be read.
[[nodiscard]] std::shared_ptr<const SoundCfg> load_sound_cfg(const std::filesystem::path& cfg_path);

// Evaluates one conditionSingle clause against a current variable value.
[[nodiscard]] bool sound_condition_holds(const SoundCondition& c, float value);

} // namespace omsi::sound
