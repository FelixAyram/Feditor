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

void Dx12Renderer::shutdown() {
    wait_gpu();
    if (draw_cb_upload_) {
        draw_cb_upload_->Unmap(0, nullptr);
        draw_cb_upload_.Reset();
    }
    draw_cb_cpu_ = nullptr;
    if (fence_event_) CloseHandle(fence_event_);
    fence_event_ = nullptr;
}

} // namespace omsi::gfx::dx12
