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

void Dx12Renderer::set_pending_screenshot(const std::filesystem::path& path) {
    pending_screenshot_path_ = path;
}

void Dx12Renderer::enqueue_screenshot_copy(ID3D12GraphicsCommandList* cmd, ID3D12Resource* back) {
    if (!cmd || !back) return;
    D3D12_RESOURCE_DESC back_desc = back->GetDesc();
    UINT64 dst_row_size = 0;
    UINT64 dst_total = 0;
    UINT row_count = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    device_->GetCopyableFootprints(&back_desc, 0, 1, 0, &footprint, &row_count, &dst_row_size, &dst_total);
    screenshot_row_pitch_ = static_cast<std::uint32_t>(footprint.Footprint.RowPitch);

    if (!screenshot_readback_ || screenshot_w_ != config_.width || screenshot_h_ != config_.height) {
        screenshot_readback_.Reset();
        CD3DX12_HEAP_PROPERTIES readback_heap(D3D12_HEAP_TYPE_READBACK);
        const CD3DX12_RESOURCE_DESC readback_desc = CD3DX12_RESOURCE_DESC::Buffer(dst_total);
        throw_hr(device_->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(screenshot_readback_.GetAddressOf())),
                 "screenshot readback");
        screenshot_w_ = config_.width;
        screenshot_h_ = config_.height;
    }

    D3D12_RESOURCE_BARRIER to_copy =
        dx12_transition_barrier(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &to_copy);

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = screenshot_readback_.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = back;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;
    cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    D3D12_RESOURCE_BARRIER back_to_rt =
        dx12_transition_barrier(back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &back_to_rt);
}

bool Dx12Renderer::finalize_screenshot_bmp(const std::filesystem::path& path) {
    if (!screenshot_readback_ || config_.width == 0 || config_.height == 0) return false;
    void* mapped = nullptr;
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(screenshot_row_pitch_) * config_.height};
    throw_hr(screenshot_readback_->Map(0, &read_range, &mapped), "screenshot map");

    const std::uint32_t width = config_.width;
    const std::uint32_t height = config_.height;
    const std::uint32_t row_pitch = screenshot_row_pitch_;
    const std::uint8_t* bgra = static_cast<const std::uint8_t*>(mapped);
    const std::uint32_t row_bytes = width * 4u;
    const std::uint32_t image_size = row_bytes * height;
    const std::uint32_t file_size = 14u + 40u + image_size;
    std::vector<std::uint8_t> file(file_size);
    auto* hdr = file.data();
    hdr[0] = 'B';
    hdr[1] = 'M';
    std::memcpy(hdr + 2, &file_size, 4);
    const std::uint32_t pixel_offset = 14u + 40u;
    std::memcpy(hdr + 10, &pixel_offset, 4);
    std::uint8_t* info = hdr + 14;
    const std::uint32_t info_size = 40u;
    const std::int32_t h = static_cast<std::int32_t>(height);
    const std::uint16_t planes = 1;
    const std::uint16_t bpp = 32;
    const std::uint32_t compression = 0;
    std::memcpy(info + 0, &info_size, 4);
    std::memcpy(info + 4, &width, 4);
    std::memcpy(info + 8, &h, 4);
    std::memcpy(info + 12, &planes, 2);
    std::memcpy(info + 14, &bpp, 2);
    std::memcpy(info + 16, &compression, 4);
    std::memcpy(info + 20, &image_size, 4);
    std::uint8_t* dst = file.data() + pixel_offset;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* src_row = bgra + static_cast<std::size_t>(height - 1u - y) * row_pitch;
        std::memcpy(dst + static_cast<std::size_t>(y) * row_bytes, src_row, row_bytes);
    }
    screenshot_readback_->Unmap(0, nullptr);

    FILE* out = nullptr;
    if (_wfopen_s(&out, path.wstring().c_str(), L"wb") != 0 || !out) return false;
    fwrite(file.data(), 1, file.size(), out);
    fclose(out);
    log::info("screenshot: " + path.string());
    return true;
}

} // namespace omsi::gfx::dx12
