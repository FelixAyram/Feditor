#include "gfx/dx12/dx12_renderer.hpp"
#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/o3d_format.hpp"
#include "omsi/logger.hpp"

#include <algorithm>
#include <cstring>
#include <d3dcompiler.h>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")

namespace omsi::gfx::dx12 {

Dx12Renderer::Dx12Renderer(const GfxConfig& config) : config_(config) {}

Dx12Renderer::~Dx12Renderer() { shutdown(); }

bool Dx12Renderer::initialize(void* native_window) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE && co != S_FALSE) return false;
    hwnd_ = static_cast<HWND>(native_window);
    try {
        if (!create_device()) return false;
        if (!create_swapchain(hwnd_)) return false;
        if (!create_render_targets()) return false;
        if (!create_depth_buffer()) return false;
        if (!create_command_objects()) return false;
        if (!create_srv_heap()) return false;
        if (!create_root_signature_and_pso()) return false;
        if (!create_overlay_geometry()) return false;
    } catch (const std::exception& ex) {
        omsi::log::error(std::string("DX12 init failed: ") + ex.what());
        return false;
    }
    return true;
}

} // namespace omsi::gfx::dx12
