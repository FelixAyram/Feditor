#pragma once

#include "omsi/inputs_types.hpp"

#include <filesystem>
#include <string>
#include <vector>

struct GLFWwindow;

namespace omsi::inputs {

// OMSI keyboard.cfg [vehicles] bindings -> trigger commands (bus_doorfront0, kw_m_enginestart, ...).
class VehicleInputBindings {
public:
    void load_from_omsi_root(const std::filesystem::path& omsi_root);
    void update(GLFWwindow* window, float dt = 0.f);

    [[nodiscard]] std::vector<std::string> pressed_triggers() const;
    [[nodiscard]] std::vector<std::string> released_triggers() const;
    [[nodiscard]] bool held(const std::string& command) const;

private:
    struct ResolvedBinding {
        std::string command;
        int glfw_key = -1;
        int modifiers = 0;
        bool continuous = false; // keyboard.cfg mod bit0; held() only — _off always on release
    };

    std::vector<ResolvedBinding> bindings_;
    std::vector<bool> key_down_now_;
    std::vector<bool> key_down_prev_;
    std::vector<bool> latched_; // true after press until we emit _off (survives modifier glitches)
    std::vector<float> hold_s_;
    std::vector<std::string> pressed_;
    std::vector<std::string> released_;
    float stuck_log_accum_ = 0.f;

    void resolve_binding(const KeyboardBinding& binding);
};

} // namespace omsi::inputs
