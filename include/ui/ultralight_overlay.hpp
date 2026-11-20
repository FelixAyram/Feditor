#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct GLFWwindow;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace omsi::gfx::dx12 {
class HelloO3dRenderer;
}

namespace omsi::ui {

// Ultralight HTML UI rendered to CPU bitmap, composited via Dear ImGui (texture blit only).
// HTML/JS live under assets/ui/map_editor — edit + F5 reload, no C++ rebuild for UI tweaks.
class UltralightOverlay {
public:
    using EventCallback = std::function<void(const std::string& json)>;

    UltralightOverlay() = default;
    ~UltralightOverlay();

    UltralightOverlay(const UltralightOverlay&) = delete;
    UltralightOverlay& operator=(const UltralightOverlay&) = delete;

    bool initialize(GLFWwindow* window, gfx::dx12::HelloO3dRenderer& renderer, class ImGuiOverlay* imgui,
                    int panel_width_px);
    void shutdown();
    void resize(int panel_width_px, int height_px);
    void set_event_callback(EventCallback cb);
    void reload(); // reload index.html / app.js from disk (hot UI)

    void begin_frame();
    void end_frame_and_draw();
    void upload_texture(ID3D12GraphicsCommandList* command_list);

    void evaluate_script(const std::string& script);

    void on_mouse_move(double view_x, double view_y);
    void on_mouse_button(int button, int action, double view_x, double view_y);
    void on_scroll(double dx, double dy);
    void on_char(unsigned int codepoint);
    void on_key(int key, int scancode, int action, int mods);

    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] bool js_ready() const { return js_ready_; }
    [[nodiscard]] int panel_width() const { return panel_w_; }
    // view_x/y = GLFW window (client) coords; panel is left-docked at (0,0)..(panel_w,panel_h).
    [[nodiscard]] bool pointer_over_panel(double view_x, double view_y) const;
    [[nodiscard]] bool wants_capture_mouse() const { return mouse_over_; }
    [[nodiscard]] bool wants_capture_keyboard() const { return keyboard_focus_; }

private:
    struct State;
    void flush_pending_scroll();
    bool active_ = false;
    bool js_ready_ = false;
    int panel_w_ = 360;
    int panel_h_ = 900;
    bool mouse_over_ = false;
    bool keyboard_focus_ = false;
    double last_x_ = 0.0;
    double last_y_ = 0.0;
    double pending_scroll_y_ = 0.0;
    EventCallback event_cb_;
    State* state_ = nullptr;
};

} // namespace omsi::ui
