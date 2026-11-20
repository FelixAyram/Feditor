#pragma once

#include "omsi/inputs_types.hpp"

#include <filesystem>
#include <vector>

struct GLFWwindow;

namespace omsi::inputs {

[[nodiscard]] std::optional<std::vector<KeyboardBinding>> load_keyboard_cfg(const std::filesystem::path& path);

// Poll OMSI view bindings from keyboard.cfg against GLFW state.
class ViewInputBindings {
public:
    void load_from_omsi_root(const std::filesystem::path& omsi_root);
    void update(GLFWwindow* window);
    [[nodiscard]] bool pressed(ViewAction action) const;
    [[nodiscard]] bool held(ViewAction action) const;

private:
    struct ResolvedBinding {
        ViewAction action = ViewAction::none;
        int glfw_key = -1;
        int modifiers = 0;
    };

    std::vector<ResolvedBinding> bindings_;
    std::vector<bool> key_down_now_;
    std::vector<bool> key_down_prev_;
    std::vector<bool> action_pressed_;
    std::vector<bool> action_held_;

    void add_defaults();
    void resolve_binding(const KeyboardBinding& binding);
    void ensure_action_storage();
};

} // namespace omsi::inputs
