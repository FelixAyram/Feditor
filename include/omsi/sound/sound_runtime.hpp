#pragma once

#include "omsi/script/var_registry.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmsystem.h>
#endif

namespace omsi::sound {

// OMSI sound.cfg loader + async trigger / loop playback (T.L.xxx / conditionSingle / loopsound).
class SoundRuntime {
public:
    SoundRuntime();
    ~SoundRuntime();

    SoundRuntime(const SoundRuntime&) = delete;
    SoundRuntime& operator=(const SoundRuntime&) = delete;

    bool load(const std::filesystem::path& bus_dir, const std::filesystem::path& sound_cfg_rel);
    void bind_vars(const std::vector<float>* public_floats, const omsi::script::VarRegistry* vars);

    // via: who requested playback, e.g. "osc:{trigger:horn}" / "osc:{frame}".
    void fire_trigger(const std::string& trigger_id, std::string_view via = {});
    void update(float dt_sec);

    [[nodiscard]] int definition_count() const { return static_cast<int>(triggers_.size() + loops_.size()); }

private:
    struct VolCurve {
        std::string var_name;
        std::vector<std::pair<float, float>> points; // x,y piecewise linear
        // [volcurve] -1: loudness vs seconds since the voice started (OMSI fade-in).
        bool is_age = false;
    };

    struct TriggerSound {
        std::string trigger_id;
        std::filesystem::path wav_path;
        float base_volume = 1.f;
        std::vector<VolCurve> vol_curves; // sampled once at play start (OMSI)
    };

    // OMSI conditionSingle: var, reference value, then compare op
    // (0:<>, 1:=, 2:<, 3:>, 4:<=, 5:>=). Multiple blocks are AND-ed.
    struct CondClause {
        std::string var_name;
        int compare_op = 3;
        float compare_value = 0.f;
    };

    struct ConditionalLoop {
        std::vector<CondClause> conditions;
        bool has_condition = false;
        std::vector<VolCurve> vol_curves;
        std::filesystem::path wav_path;
        float base_volume = 1.f;
        std::string pitch_var;
        float pitch_ref = 0.f;
        std::string kind;           // "loopsound" | "sound"
        std::string linked_trigger; // [trigger] on same cfg block, if any
        bool no_loop = false;       // [noloop]: rising-edge oneshot, not waveOut loop
        bool was_active = false;
        int loop_id = -1;
        float last_gain = -1.f;
        float last_pitch = -1.f;
        // Edge hold: OMSI does not restart a voice every frame the condition flickers.
        float cond_true_age = 0.f;
        float cond_false_age = 0.f;
        // After start, keep the waveOut open while muted; free after prolonged silence.
        float silent_age = 0.f;
        // Seconds since last LOOP+ (for [volcurve] -1).
        float voice_age = 0.f;
        // Smoothed params sent to waveOut (anti-pump / anti-click).
        float smooth_gain = 0.f;
        float smooth_pitch = 1.f;
    };

    struct PlayCommand {
        enum class Kind { one_shot, loop_start, loop_stop, loop_params } kind = Kind::one_shot;
        std::filesystem::path path;
        float volume = 1.f;
        float pitch = 1.f;
        int loop_id = -1;
    };

    // Software mixer (single miniaudio device, resampled voices); defined in sound_runtime.cpp.
    struct Mixer;
    std::unique_ptr<Mixer> mixer_;

    std::filesystem::path sound_dir_;
    std::vector<TriggerSound> triggers_;
    std::vector<ConditionalLoop> loops_;
    const std::vector<float>* floats_ = nullptr;
    const omsi::script::VarRegistry* vars_ = nullptr;
    int next_loop_id_ = 1;
    float dbg_summary_age_ = 0.f;

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> oneshot_last_fire_;
    std::unordered_map<std::string, int> oneshot_dbg_count_;

    std::thread worker_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<PlayCommand> queue_;
    std::atomic<bool> stop_{false};

    void start_worker();
    void stop_worker();
    void enqueue(PlayCommand cmd);
    void worker_main();

    [[nodiscard]] float var_value(const std::string& name) const;
    [[nodiscard]] float eval_vol_curves(const std::vector<VolCurve>& curves, float voice_age = 0.f) const;
    [[nodiscard]] float eval_vol_curves(const ConditionalLoop& c) const;
    [[nodiscard]] bool eval_condition(const ConditionalLoop& c) const;
    // Debounced condition + mute-vs-stop policy (updates ages on the loop).
    [[nodiscard]] bool loop_want_device(ConditionalLoop& c, float dt_sec, float gain) const;
    [[nodiscard]] float loop_gain(const ConditionalLoop& c) const;
    [[nodiscard]] float loop_pitch(const ConditionalLoop& c) const;
    [[nodiscard]] std::string describe_loop_reason(const ConditionalLoop& c, float gain, float pitch) const;
};

} // namespace omsi::sound
