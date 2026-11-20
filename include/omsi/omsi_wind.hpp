#pragma once

// Shared wind state for vegetation sway (trees / grass vertex shader).
// Written from the app (weather → strength) and read once per frame by the renderer.
// The sky/water pipeline may overwrite it via set(); readers only ever see a consistent snapshot.

#include <cstdint>
#include <mutex>

namespace omsi::wind {

struct WindState {
    float dir_x = 0.8f;       // unit direction in the world XY plane (OMSI Z-up)
    float dir_y = 0.6f;
    float strength_ms = 0.f;  // metres per second
    std::uint64_t gen = 0;    // bumps on every set()
};

inline WindState& state() {
    static WindState s;
    return s;
}

inline std::mutex& mu() {
    static std::mutex m;
    return m;
}

inline void set(float dir_x, float dir_y, float strength_ms) {
    std::lock_guard<std::mutex> lk(mu());
    auto& s = state();
    s.dir_x = dir_x;
    s.dir_y = dir_y;
    s.strength_ms = strength_ms;
    ++s.gen;
}

inline WindState snapshot() {
    std::lock_guard<std::mutex> lk(mu());
    return state();
}

} // namespace omsi::wind
