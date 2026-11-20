#pragma once

#include <functional>

#include <d3d12.h>

struct GLFWwindow;
struct HWND__;
struct ImFont;

namespace omsi::gfx::dx12 {
class HelloO3dRenderer;
}

namespace omsi::ui {

// Loaded editor fonts (nullptr if the atlas fell back to the default font).
[[nodiscard]] ImFont* ui_font_body();
[[nodiscard]] ImFont* ui_font_strong();
[[nodiscard]] ImFont* ui_font_title();

// Dear ImGui overlay for DX12 + GLFW (GLFW_NO_API) tools.
// Layout: ImGui::SameLine, BeginTable/BeginMenuBar, PushStyleVar(ImGuiStyleVar_*).
// For CSS-like retained UI see RmlUi (future); ImGui is immediate-mode editor standard.
class ImGuiOverlay {
public:
    using BuildFn = std::function<void()>;

    ImGuiOverlay() = default;
    ~ImGuiOverlay();

    ImGuiOverlay(const ImGuiOverlay&) = delete;
    ImGuiOverlay& operator=(const ImGuiOverlay&) = delete;

    bool initialize(GLFWwindow* window, gfx::dx12::HelloO3dRenderer& renderer);
    bool initialize_win32(HWND__* hwnd, gfx::dx12::HelloO3dRenderer& renderer);
    void shutdown();
    void begin_frame();
    void end_frame_and_build();
    void set_build_fn(BuildFn fn);
    void render_dx12(ID3D12GraphicsCommandList* command_list);
    bool alloc_srv(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);

    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] bool wants_capture_mouse() const;
    [[nodiscard]] bool wants_capture_keyboard() const;
    [[nodiscard]] bool wants_text_input() const;

private:
    struct State;

    bool active_ = false;
    BuildFn build_fn_;
    State* state_ = nullptr;
};

} // namespace omsi::ui
