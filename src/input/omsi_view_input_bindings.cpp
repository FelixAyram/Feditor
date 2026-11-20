#include "omsi/inputs.hpp"

#include <GLFW/glfw3.h>

#include <unordered_map>

namespace omsi::inputs {
namespace {

// Same DIK table as vehicle_input_bindings.cpp (DirectInput scancodes from keyboard.cfg).
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

int action_index(ViewAction action) { return static_cast<int>(action); }

} // namespace

void ViewInputBindings::ensure_action_storage() {
    const std::size_t n = static_cast<std::size_t>(ViewAction::toggle_mouse_ctrl) + 1;
    action_pressed_.assign(n, false);
    action_held_.assign(n, false);
}

void ViewInputBindings::add_defaults() {
    const KeyboardBinding defaults[] = {
        {"view_set_driver", 59, 0, "game"},
        {"view_set_passenger", 60, 0, "game"},
        {"view_set_outside", 61, 0, "game"},
        {"view_interiorcam_minus", 203, 0, "game"},
        {"view_interiorcam_plus", 205, 0, "game"},
        {"view_reset_direction", 46, 0, "game"},
        {"view_reset_all_directions", 57, 0, "game"},
        {"view_set_schedule", 210, 1, "game"},
    };
    for (const auto& b : defaults) resolve_binding(b);
}

void ViewInputBindings::resolve_binding(const KeyboardBinding& binding) {
    const auto action = view_action_from_command(binding.command);
    if (action == ViewAction::none) return;
    const int glfw_key = dik_to_glfw_key(binding.dik_code);
    if (glfw_key < 0) return;

    for (auto& existing : bindings_) {
        if (existing.action == action) {
            existing.glfw_key = glfw_key;
            existing.modifiers = binding.modifiers;
            return;
        }
    }
    bindings_.push_back({action, glfw_key, binding.modifiers});
}

void ViewInputBindings::load_from_omsi_root(const std::filesystem::path& omsi_root) {
    bindings_.clear();
    ensure_action_storage();
    add_defaults();

    if (omsi_root.empty()) return;
    const auto path = omsi_root / "Inputs" / "keyboard.cfg";
    if (const auto loaded = load_keyboard_cfg(path)) {
        for (const auto& b : *loaded) {
            if (view_action_from_command(b.command) != ViewAction::none) resolve_binding(b);
        }
    }
}

void ViewInputBindings::update(GLFWwindow* window) {
    if (!window) return;
    ensure_action_storage();

    if (key_down_now_.size() < 512) {
        key_down_now_.assign(512, false);
        key_down_prev_.assign(512, false);
    }

    key_down_prev_ = key_down_now_;
    for (int k = GLFW_KEY_SPACE; k <= GLFW_KEY_LAST; ++k)
        key_down_now_[static_cast<std::size_t>(k)] = glfwGetKey(window, k) == GLFW_PRESS;

    for (std::size_t i = 0; i < action_pressed_.size(); ++i) {
        action_pressed_[i] = false;
        action_held_[i] = false;
    }

    for (const auto& b : bindings_) {
        if (b.glfw_key < 0 || b.glfw_key >= static_cast<int>(key_down_now_.size())) continue;
        if (!modifiers_match(window, b.modifiers)) continue;

        const bool down = key_down_now_[static_cast<std::size_t>(b.glfw_key)];
        const bool was = key_down_prev_[static_cast<std::size_t>(b.glfw_key)];
        const int idx = action_index(b.action);
        action_held_[static_cast<std::size_t>(idx)] = down;
        action_pressed_[static_cast<std::size_t>(idx)] = down && !was;
    }
}

bool ViewInputBindings::pressed(ViewAction action) const {
    const int idx = action_index(action);
    if (idx < 0 || static_cast<std::size_t>(idx) >= action_pressed_.size()) return false;
    return action_pressed_[static_cast<std::size_t>(idx)];
}

bool ViewInputBindings::held(ViewAction action) const {
    const int idx = action_index(action);
    if (idx < 0 || static_cast<std::size_t>(idx) >= action_held_.size()) return false;
    return action_held_[static_cast<std::size_t>(idx)];
}

} // namespace omsi::inputs
