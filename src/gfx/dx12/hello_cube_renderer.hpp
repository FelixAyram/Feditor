#pragma once

#include <cstdint>

struct HWND__;

namespace omsi::gfx::dx12 {

// Minimal Microsoft HelloTriangle + HelloConstBuffers style renderer.
class HelloCubeRenderer {
public:
    HelloCubeRenderer() = default;
    ~HelloCubeRenderer();

    HelloCubeRenderer(const HelloCubeRenderer&) = delete;
    HelloCubeRenderer& operator=(const HelloCubeRenderer&) = delete;

    bool initialize(HWND__* hwnd, std::uint32_t width, std::uint32_t height);
    void resize(std::uint32_t width, std::uint32_t height);
    void set_mvp(const float mvp_column_major[16]);
    void render();
    void shutdown();

private:
    static constexpr std::uint32_t kFrameCount = 2;

    void wait_for_previous_frame();
    void populate_command_list();
    bool create_device_and_swapchain(HWND__* hwnd);
    bool create_pipeline();
    bool create_geometry();
    void create_depth_buffer(std::uint32_t width, std::uint32_t height);

    HWND__* hwnd_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t frame_index_ = 0;
    std::uint32_t rtv_descriptor_size_ = 0;

    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace omsi::gfx::dx12
