#pragma once

#include <cstddef>
#include <cstdint>

#include <imgui.h>

namespace omsi::ui::theme {

// Build an ImVec4 from 0xRRGGBB + alpha.
inline ImVec4 rgb(std::uint32_t hex, float a = 1.f) {
    return ImVec4(static_cast<float>((hex >> 16) & 0xFF) / 255.f,
                  static_cast<float>((hex >> 8) & 0xFF) / 255.f,
                  static_cast<float>(hex & 0xFF) / 255.f, a);
}

// "Cozy coffee" dark palette: warm neutrals, one amber accent, sage for selection/success.
// 60% surfaces (crust/mantle/base) · 30% supporting (surface/line/muted) · 10% accent.
constexpr std::uint32_t crust = 0x14110F;      // deepest (title bar, scrollbar bg)
constexpr std::uint32_t mantle = 0x1A1714;     // child bg
constexpr std::uint32_t base = 0x211D19;       // window bg
constexpr std::uint32_t surface = 0x2A2520;    // frames, buttons
constexpr std::uint32_t surface_hi = 0x352E28; // frame hover
constexpr std::uint32_t line = 0x3E362E;       // borders, separators
constexpr std::uint32_t text = 0xEDE4D8;       // warm off-white
constexpr std::uint32_t muted = 0xA89A8B;      // secondary text
constexpr std::uint32_t disabled = 0x6F6459;   // disabled text

constexpr std::uint32_t accent = 0xD8A657;     // honey amber (primary)
constexpr std::uint32_t accent_hi = 0xE6B96E;
constexpr std::uint32_t accent_lo = 0xC4943F;
constexpr std::uint32_t selection = 0x8FA37B;  // soft sage (secondary/selected)
constexpr std::uint32_t success = 0x9CC08A;
constexpr std::uint32_t warning = 0xE0B060;
constexpr std::uint32_t danger = 0xD9736B;
constexpr std::uint32_t info = 0x86B5C9;

// Spacing / geometry (4-8-16 rhythm, compact tool density).
constexpr float rounding = 10.f;
constexpr float pad_x = 9.f;
constexpr float pad_y = 5.f;
constexpr float gap_x = 9.f;
constexpr float gap_y = 6.f;
constexpr float inner_x = 6.f;
constexpr float inner_y = 4.f;
constexpr float cell_x = 6.f;
constexpr float cell_y = 3.f;
constexpr float indent = 18.f;
constexpr float scrollbar = 12.f;
constexpr float grab = 10.f;

} // namespace omsi::ui::theme
