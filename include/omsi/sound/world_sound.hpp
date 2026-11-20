#pragma once

// World (3D) sound for the map editor / viewer: plays OMSI .wav/.ogg files positioned in the
// scene, with a listener tied to the editor camera.
//
// Threading: every public call is cheap and may be made from the main thread or from a script
// thread (guarded by a mutex). Decoding, voice management and mixing parameters live on a worker
// thread and on the miniaudio audio thread; nothing here blocks on file IO.
//
// Coordinates: OMSI editor space, metres: X/Y on the ground, Z = height (same space as
// MapObjectInst::position and input::LunaCamera::eye_position).
//
// Hook API for the .osc script engine and the bus physics ("set_var" / "play" / "stop"):
//   auto& ws = omsi::sound::WorldSound::instance();
//   ws.set_var("engine_n", 1200.f);                       // script/sound variable
//   auto g = ws.play_cfg(cfg_path, {pos, 1.f, 0.f}, "horn"); // all [sound]/[loopsound] of trigger "horn"
//   ws.stop(g);

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace omsi::sound {

// 0 = invalid / nothing started. Ids are unique for the process lifetime.
using SoundId = std::uint32_t;

struct Vec3f {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

struct EmitterDesc {
    Vec3f pos{};                 // OMSI editor space (object origin when used with play_cfg)
    float yaw_deg = 0.f;         // object heading; rotates the [3d] offset around Y (play_cfg only)
    float range_m = 50.f;        // audible range ([3d] radius); beyond it the voice is silent
    float volume = 1.f;          // linear, multiplied by the master volume
    float pitch = 1.f;           // playback speed multiplier
    bool loop = false;           // loop until stop()
};

// OMSI [3d] offset (x, y, z=height) relative to an object origin with heading yaw_deg (around Z).
[[nodiscard]] Vec3f object_offset_to_world(Vec3f offset, float yaw_deg, Vec3f origin);

class WorldSound {
public:
    // Process-wide instance used by the editor and by the script / bus sessions.
    [[nodiscard]] static WorldSound& instance();

    // Starts the audio device and the worker thread. Safe to call again (no-op when running).
    bool init();
    void shutdown();
    [[nodiscard]] bool ready() const;

    // Master switch: when disabled, nothing is audible and new requests are ignored.
    void set_enabled(bool on);
    [[nodiscard]] bool enabled() const;
    void set_master_volume(float v01);     // 0..1
    [[nodiscard]] float master_volume() const;

    // Listener follows the editor camera (forward need not be normalised; up is always +Z).
    void set_listener(Vec3f pos, Vec3f forward);

    // ---- Hook API -----------------------------------------------------------------------------
    // Plays one audio file at a position. Returns an id usable with stop()/set_position().
    SoundId play(const std::filesystem::path& wav, const EmitterDesc& desc);

    // Plays the definitions of an OMSI sound.cfg. If trigger is empty, every definition that has
    // no [trigger] is started; otherwise only definitions whose [trigger] equals it. Returns a group
    // id (stop(group) stops all voices of it); 0 if nothing was started.
    SoundId play_cfg(const std::filesystem::path& sound_cfg, const EmitterDesc& base,
                     std::string_view trigger = {});

    void stop(SoundId id);
    void set_position(SoundId id, Vec3f pos);
    void set_gain(SoundId id, float volume);
    void set_pitch(SoundId id, float pitch);

    // Named script / sound variable. Drives [loopsound] pitch (var / ref) and [conditionSingle].
    void set_var(std::string_view name, float value);
    [[nodiscard]] float var(std::string_view name, float fallback = 0.f) const;

    // Number of voices currently allocated on the worker (diagnostics).
    [[nodiscard]] int active_voices() const;

    WorldSound(const WorldSound&) = delete;
    WorldSound& operator=(const WorldSound&) = delete;

private:
    WorldSound();
    ~WorldSound();

    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace omsi::sound
