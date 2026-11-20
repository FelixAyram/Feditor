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

namespace {

bool write_bmp_bgra8(const std::filesystem::path& path, const std::uint8_t* bgra, std::uint32_t width,
                     std::uint32_t height, std::uint32_t row_pitch) {
    if (!bgra || width == 0 || height == 0) return false;
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

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(out);
}

} // namespace

bool HelloO3dRenderer::save_screenshot_bmp(const std::filesystem::path& path) {
    if (!impl_ || width_ == 0 || height_ == 0) return false;
    auto& d = *impl_;
    ID3D12Resource* const back = d.render_targets[last_drawn_frame_index_].Get();

    D3D12_RESOURCE_DESC back_desc = back->GetDesc();
    UINT64 dst_row_size = 0;
    UINT64 dst_total = 0;
    UINT row_count = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    d.device->GetCopyableFootprints(&back_desc, 0, 1, 0, &footprint, &row_count, &dst_row_size, &dst_total);

    if (!d.screenshot_readback || d.screenshot_readback_width != width_ || d.screenshot_readback_height != height_) {
        d.screenshot_readback.Reset();
        CD3DX12_HEAP_PROPERTIES readback_heap(D3D12_HEAP_TYPE_READBACK);
        const CD3DX12_RESOURCE_DESC readback_desc = CD3DX12_RESOURCE_DESC::Buffer(dst_total);
        throw_hr(d.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(d.screenshot_readback.GetAddressOf())),
                 "screenshot readback");
        d.screenshot_readback_width = width_;
        d.screenshot_readback_height = height_;
    }

    wait_gpu_idle_for_upload("screenshot_capture"); // current ring slot may still be in flight
    throw_hr(d.upload_allocator->Reset(), "screenshot alloc reset");
    throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "screenshot cmd reset");

    D3D12_RESOURCE_BARRIER to_copy = dx12_transition_barrier(back, D3D12_RESOURCE_STATE_PRESENT,
                                                             D3D12_RESOURCE_STATE_COPY_SOURCE);
    d.upload_command_list->ResourceBarrier(1, &to_copy);

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = d.screenshot_readback.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = back;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;
    d.upload_command_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    D3D12_RESOURCE_BARRIER to_present = dx12_transition_barrier(back, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                                D3D12_RESOURCE_STATE_PRESENT);
    d.upload_command_list->ResourceBarrier(1, &to_present);
    throw_hr(d.upload_command_list->Close(), "screenshot cmd close");

    ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    const UINT64 copy_fence = d.fence_value++;
    throw_hr(d.queue->Signal(d.fence.Get(), copy_fence), "screenshot Signal");
    if (d.fence->GetCompletedValue() < copy_fence) {
        throw_hr(d.fence->SetEventOnCompletion(copy_fence, d.fence_event), "screenshot SetEventOnCompletion");
        WaitForSingleObject(d.fence_event, INFINITE);
    }

    void* mapped = nullptr;
    D3D12_RANGE read_range{};
    throw_hr(d.screenshot_readback->Map(0, &read_range, &mapped), "screenshot map");
    // Footprint.RowPitch is GPU-padded; dst_row_size is unpadded — using the latter shreds the BMP.
    const bool ok = write_bmp_bgra8(path, static_cast<const std::uint8_t*>(mapped), width_, height_,
                                    footprint.Footprint.RowPitch);
    d.screenshot_readback->Unmap(0, nullptr);
    if (ok) omsi::log::info("screenshot: " + path.string());
    return ok;
}

} // namespace omsi::gfx::dx12
