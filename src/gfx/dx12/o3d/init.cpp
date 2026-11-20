#include "gfx/dx12/hello_o3d_renderer.hpp"

#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/o3d/internal.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/logger.hpp"
#include "omsi/model_cfg.hpp"
#include "omsi/visible_runtime.hpp"
#include "omsi/o3d_format.hpp"
#include "o3d/o3d_loader.hpp"

#include <DirectXMath.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <objbase.h>
#include <wrl/client.h>

#include <fstream>
#include <cstring>
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")

namespace omsi::gfx::dx12 {

HelloO3dRenderer::~HelloO3dRenderer() { shutdown(); }

bool HelloO3dRenderer::initialize(HWND__* hwnd, std::uint32_t width, std::uint32_t height, bool clip_x_mirrored) {
    shutdown();
    impl_ = new Impl{};
    impl_->clip_x_mirrored = clip_x_mirrored;
    hwnd_ = hwnd;
    width_ = width;
    height_ = height;

    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE && co != S_FALSE) return false;

    try {
        if (!create_device_and_swapchain(hwnd)) return false;
        if (!create_pipeline()) return false;
        if (!create_fxaa_pipeline()) return false;
        if (!create_axis_pipeline()) return false;
        if (!create_constant_buffer()) return false;
        if (!upload_axis_gpu()) return false;
        if (!ensure_sky_resources(*impl_)) omsi::log::warn("Sky dome unavailable");
        wait_for_previous_frame();
        omsi::log::info("hello o3d renderer ready");
        return true;
    } catch (const std::exception& ex) {
        omsi::log::error(std::string("hello o3d init failed: ") + ex.what());
        shutdown();
        return false;
    }
}

} // namespace omsi::gfx::dx12
