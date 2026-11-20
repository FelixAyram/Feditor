#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace omsi::inputs {

// OMSI Inputs/keyboard.cfg [entry]: command, DirectInput scancode, modifier bitmask.
// Modifiers: 1=continuous/hold, 2=Shift, 4=Ctrl (summed).
struct KeyboardBinding {
    std::string command;
    int dik_code = 0;
    int modifiers = 0;
    std::string section; // "game" or "vehicles"
};

enum class ViewAction {
    none,
    set_driver,
    set_passenger,
    set_outside,
    set_map,
    set_schedule,
    reset_direction,
    reset_all_directions,
    interiorcam_minus,
    interiorcam_plus,
    toggle_mouse_ctrl, // O — toggel_mouse_ctrl (Maussteuerung)
};

[[nodiscard]] ViewAction view_action_from_command(const std::string& command);

} // namespace omsi::inputs
