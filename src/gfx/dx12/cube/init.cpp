#include "gfx/dx12/hello_cube_renderer.hpp"

#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/cube/internal.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "omsi/logger.hpp"

#include <DirectXMath.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace omsi::gfx::dx12 {

HelloCubeRenderer::~HelloCubeRenderer() { shutdown(); }

bool HelloCubeRenderer::initialize(HWND__* hwnd, std::uint32_t width, std::uint32_t height) {
    shutdown();
    impl_ = new Impl{};
    hwnd_ = hwnd;
    width_ = width;
    height_ = height;
    try {
        if (!create_device_and_swapchain(hwnd)) return false;
        if (!create_pipeline()) return false;
        if (!create_geometry()) return false;
        wait_for_previous_frame();
        omsi::log::info("hello cube renderer ready");
        return true;
    } catch (const std::exception& ex) {
        omsi::log::error(std::string("hello cube init failed: ") + ex.what());
        shutdown();
        return false;
    }
}
void HelloCubeRenderer::shutdown() {
    if (!impl_) return;
    if (impl_->fence && impl_->queue) wait_for_previous_frame();
    if (impl_->constant_buffer && impl_->cb_cpu) {
        impl_->constant_buffer->Unmap(0, nullptr);
        impl_->cb_cpu = nullptr;
    }
    if (impl_->fence_event) CloseHandle(impl_->fence_event);
    delete impl_;
    impl_ = nullptr;
}

} // namespace omsi::gfx::dx12
