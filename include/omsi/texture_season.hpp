#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace omsi::gfx {

/// Global season/night head for texture resolve (openOMSI-style folder chain).
/// Updated from Vista sim time; resolve reads it without freezing the main thread.
struct TextureSeasonState {
    std::string season_dir = "SummerDry"; // Spring/Fall/Winter/WinterSnow/WinterSnowfall/SummerDry
    bool prefer_night = false;
    float night_blend = 0.f;
    std::uint64_t generation = 0;
};

inline TextureSeasonState& texture_season_state() {
    static TextureSeasonState s;
    return s;
}

inline std::mutex& texture_season_mu() {
    static std::mutex m;
    return m;
}

inline void set_texture_season(std::string_view season_dir, bool prefer_night, float night_blend) {
    std::lock_guard<std::mutex> lk(texture_season_mu());
    auto& s = texture_season_state();
    // Generation drives albedo season rebind only — night toggles lighting, not nightmap albedos.
    const bool season_changed = s.season_dir != season_dir;
    s.season_dir = std::string(season_dir);
    s.prefer_night = prefer_night;
    s.night_blend = night_blend;
    if (season_changed) ++s.generation;
}

/// openOMSI season_chain — try folders before base texture\.
inline std::vector<std::string> season_folder_chain(std::string_view head) {
    std::vector<std::string> c;
    if (head.empty()) return c;
    if (head == "WinterSnowfall") {
        c = {"WinterSnowfall", "WinterSnow", "Winter", "Fall"};
    } else if (head == "WinterSnow") {
        c = {"WinterSnow", "Winter", "Fall"};
    } else if (head == "Winter") {
        c = {"Winter", "Fall"};
    } else {
        c = {std::string(head)};
    }
    return c;
}

} // namespace omsi::gfx
