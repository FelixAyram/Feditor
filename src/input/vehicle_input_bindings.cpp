#include "omsi/vehicle_input_bindings.hpp"

#include "omsi/inputs.hpp"
#include "omsi/logger.hpp"

#include <GLFW/glfw3.h>

#include <sstream>
#include <unordered_map>

namespace omsi::inputs {
namespace {

// OMSI Inputs/keyboard.cfg stores DirectInput DIK scancodes (dinput.h).
// Numpad: DIK_NUMPAD7..0 = 71..82, DIK_DECIMAL=83, DIK_MULTIPLY=55, DIK_SUBTRACT=74,
// DIK_ADD=78, DIK_DIVIDE=181, DIK_NUMPADENTER=156. Extended nav cluster = 199..211.
int dik_to_glfw_key(int dik) {
    static const std::unordered_map<int, int> map = {
        {1, GLFW_KEY_ESCAPE},     {2, GLFW_KEY_1},          {3, GLFW_KEY_2},          {4, GLFW_KEY_3},
        {5, GLFW_KEY_4},          {6, GLFW_KEY_5},          {7, GLFW_KEY_6},          {8, GLFW_KEY_7},
        {9, GLFW_KEY_8},          {10, GLFW_KEY_9},         {11, GLFW_KEY_0},         {12, GLFW_KEY_MINUS},
        {13, GLFW_KEY_EQUAL},     {14, GLFW_KEY_BACKSPACE}, {15, GLFW_KEY_TAB},       {16, GLFW_KEY_Q},
        {17, GLFW_KEY_W},         {18, GLFW_KEY_E},         {19, GLFW_KEY_R},         {20, GLFW_KEY_T},
        {21, GLFW_KEY_Y},         {22, GLFW_KEY_U},         {23, GLFW_KEY_I},         {24, GLFW_KEY_O},
        {25, GLFW_KEY_P},         {28, GLFW_KEY_ENTER},     {29, GLFW_KEY_LEFT_CONTROL}, {30, GLFW_KEY_A},
        {31, GLFW_KEY_S},         {32, GLFW_KEY_D},         {33, GLFW_KEY_F},         {34, GLFW_KEY_G},
        {35, GLFW_KEY_H},         {36, GLFW_KEY_J},         {37, GLFW_KEY_K},         {38, GLFW_KEY_L},
        {39, GLFW_KEY_SEMICOLON}, {40, GLFW_KEY_APOSTROPHE}, {41, GLFW_KEY_GRAVE_ACCENT}, {42, GLFW_KEY_LEFT_SHIFT},
        {43, GLFW_KEY_BACKSLASH}, {44, GLFW_KEY_Z},         {45, GLFW_KEY_X},         {46, GLFW_KEY_C},
        {47, GLFW_KEY_V},         {48, GLFW_KEY_B},         {49, GLFW_KEY_N},         {50, GLFW_KEY_M},
        {51, GLFW_KEY_COMMA},     {52, GLFW_KEY_PERIOD},    {53, GLFW_KEY_SLASH},     {54, GLFW_KEY_RIGHT_SHIFT},
        {55, GLFW_KEY_KP_MULTIPLY}, {56, GLFW_KEY_LEFT_ALT}, {57, GLFW_KEY_SPACE},    {58, GLFW_KEY_CAPS_LOCK},
        {59, GLFW_KEY_F1},        {60, GLFW_KEY_F2},        {61, GLFW_KEY_F3},        {62, GLFW_KEY_F4},
        {63, GLFW_KEY_F5},        {64, GLFW_KEY_F6},        {65, GLFW_KEY_F7},        {66, GLFW_KEY_F8},
        {67, GLFW_KEY_F9},        {68, GLFW_KEY_F10},       {69, GLFW_KEY_NUM_LOCK},  {70, GLFW_KEY_SCROLL_LOCK},
        {71, GLFW_KEY_KP_7},      {72, GLFW_KEY_KP_8},      {73, GLFW_KEY_KP_9},      {74, GLFW_KEY_KP_SUBTRACT},
        {75, GLFW_KEY_KP_4},      {76, GLFW_KEY_KP_5},      {77, GLFW_KEY_KP_6},      {78, GLFW_KEY_KP_ADD},
        {79, GLFW_KEY_KP_1},      {80, GLFW_KEY_KP_2},      {81, GLFW_KEY_KP_3},      {82, GLFW_KEY_KP_0},
        {83, GLFW_KEY_KP_DECIMAL},{87, GLFW_KEY_F11},       {88, GLFW_KEY_F12},
        {156, GLFW_KEY_KP_ENTER}, {157, GLFW_KEY_RIGHT_CONTROL}, {181, GLFW_KEY_KP_DIVIDE}, {184, GLFW_KEY_RIGHT_ALT},
        {199, GLFW_KEY_HOME},     {200, GLFW_KEY_UP},       {201, GLFW_KEY_PAGE_UP},  {203, GLFW_KEY_LEFT},
        {205, GLFW_KEY_RIGHT},    {207, GLFW_KEY_END},      {208, GLFW_KEY_DOWN},     {209, GLFW_KEY_PAGE_DOWN},
        {210, GLFW_KEY_INSERT},   {211, GLFW_KEY_DELETE},
    };
    const auto it = map.find(dik);
    return it != map.end() ? it->second : -1;
}

bool modifiers_match(GLFWwindow* window, int modifiers) {
    const bool need_shift = (modifiers & 2) != 0;
    const bool need_ctrl = (modifiers & 4) != 0;
    const bool shift =
        glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
    const bool ctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                      glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    return need_shift == shift && need_ctrl == ctrl;
}

const char* glfw_key_label(int key) {
    if (const char* n = glfwGetKeyName(key, 0)) return n;
    switch (key) {
    case GLFW_KEY_SPACE: return "Space";
    case GLFW_KEY_LEFT_SHIFT:
    case GLFW_KEY_RIGHT_SHIFT: return "Shift";
    case GLFW_KEY_LEFT_CONTROL:
    case GLFW_KEY_RIGHT_CONTROL: return "Ctrl";
    case GLFW_KEY_ENTER: return "Enter";
    case GLFW_KEY_ESCAPE: return "Esc";
    default: return "?";
    }
}

bool is_watch_command(const std::string& cmd) {
    return cmd == "kw_m_enginestart" || cmd == "cp_batterietrennschalter_toggle" || cmd == "automatic_N" ||
           cmd == "automatic_D" || cmd == "horn" || cmd == "parking_brake_toggle" || cmd == "steering_left" ||
           cmd == "steering_right" || cmd == "steering_neutral";
}

} // namespace

void VehicleInputBindings::resolve_binding(const KeyboardBinding& binding) {
    if (binding.section != "vehicles") return;
    const int glfw_key = dik_to_glfw_key(binding.dik_code);
    if (glfw_key < 0) return;
    bindings_.push_back({binding.command, glfw_key, binding.modifiers, (binding.modifiers & 1) != 0});
}

void VehicleInputBindings::load_from_omsi_root(const std::filesystem::path& omsi_root) {
    bindings_.clear();
    latched_.clear();
    hold_s_.clear();
    stuck_log_accum_ = 0.f;
    if (omsi_root.empty()) return;
    const auto path = omsi_root / "Inputs" / "keyboard.cfg";
    if (const auto loaded = load_keyboard_cfg(path)) {
        int count = 0;
        for (const auto& b : *loaded) {
            if (b.section == "vehicles") {
                resolve_binding(b);
                ++count;
            }
        }
        omsi::log::info("OMSI vehicle inputs: " + std::to_string(count) + " bindings from keyboard.cfg");
    }
    latched_.assign(bindings_.size(), false);
    hold_s_.assign(bindings_.size(), 0.f);
}

void VehicleInputBindings::update(GLFWwindow* window, float dt) {
    pressed_.clear();
    released_.clear();
    if (!window) return;

    if (key_down_now_.size() < 512) {
        key_down_now_.assign(512, false);
        key_down_prev_.assign(512, false);
    }
    if (latched_.size() != bindings_.size()) {
        latched_.assign(bindings_.size(), false);
        hold_s_.assign(bindings_.size(), 0.f);
    }

    key_down_prev_ = key_down_now_;
    for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; ++k)
        key_down_now_[static_cast<std::size_t>(k)] = glfwGetKey(window, k) == GLFW_PRESS;

    for (std::size_t bi = 0; bi < bindings_.size(); ++bi) {
        const auto& b = bindings_[bi];
        if (b.glfw_key < 0 || b.glfw_key >= static_cast<int>(key_down_now_.size())) continue;

        const bool down = key_down_now_[static_cast<std::size_t>(b.glfw_key)];
        const bool was = key_down_prev_[static_cast<std::size_t>(b.glfw_key)];
        const bool mods_ok = modifiers_match(window, b.modifiers);

        // Press only with matching modifiers (OMSI keyboard.cfg).
        if (down && !was && mods_ok) {
            pressed_.push_back(b.command);
            latched_[bi] = true;
            hold_s_[bi] = 0.f;
            if (is_watch_command(b.command)) {
                omsi::log::info("input press: " + b.command + " key=" + std::string(glfw_key_label(b.glfw_key)) +
                                " mods=" + std::to_string(b.modifiers));
            }
        }

        // Release whenever latched key goes up — even if Shift/Ctrl changed mid-hold.
        // Old code skipped the whole binding when mods mismatched → stuck antirepeat.
        if (!down && latched_[bi]) {
            released_.push_back(b.command + "_off");
            latched_[bi] = false;
            if (is_watch_command(b.command) || hold_s_[bi] >= 2.f) {
                omsi::log::info("input release: " + b.command + "_off key=" +
                                std::string(glfw_key_label(b.glfw_key)) + " held=" + std::to_string(hold_s_[bi]) +
                                "s" + (mods_ok ? "" : " (mods mismatch — still cleared)"));
            }
            hold_s_[bi] = 0.f;
        }

        if (latched_[bi] && down) {
            hold_s_[bi] += dt;
            if (!b.continuous && hold_s_[bi] >= 2.f && hold_s_[bi] - dt < 2.f) {
                omsi::log::warn("input hold>2s: " + b.command + " key=" + std::string(glfw_key_label(b.glfw_key)) +
                                " (waiting for key-up → " + b.command + "_off)");
            }
        }
    }

    stuck_log_accum_ += dt;
    if (stuck_log_accum_ >= 5.f) {
        stuck_log_accum_ = 0.f;
        std::ostringstream stuck;
        int n = 0;
        for (std::size_t bi = 0; bi < bindings_.size(); ++bi) {
            if (!latched_[bi]) continue;
            const auto& b = bindings_[bi];
            const bool phys = b.glfw_key >= 0 && key_down_now_[static_cast<std::size_t>(b.glfw_key)];
            if (!phys || hold_s_[bi] >= 3.f) {
                if (n++) stuck << " | ";
                stuck << b.command << " key=" << glfw_key_label(b.glfw_key) << " held=" << hold_s_[bi]
                      << "s phys=" << (phys ? 1 : 0);
            }
            if (!phys) {
                released_.push_back(b.command + "_off");
                latched_[bi] = false;
                hold_s_[bi] = 0.f;
                omsi::log::warn("input desync clear: " + b.command + "_off (latched but key up)");
            }
        }
        if (n > 0) omsi::log::warn("input stuck/held: " + stuck.str());
    }
}

std::vector<std::string> VehicleInputBindings::pressed_triggers() const { return pressed_; }

std::vector<std::string> VehicleInputBindings::released_triggers() const { return released_; }

bool VehicleInputBindings::held(const std::string& command) const {
    for (const auto& b : bindings_) {
        if (b.command != command || !b.continuous) continue;
        if (b.glfw_key >= 0 && b.glfw_key < static_cast<int>(key_down_now_.size()))
            return key_down_now_[static_cast<std::size_t>(b.glfw_key)];
    }
    return false;
}

} // namespace omsi::inputs
