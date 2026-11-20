#include "gfx/dx12/dx12_texture.hpp"
#include "gfx/dx12/texture/internal.hpp"
#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "omsi/logger.hpp"

#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace omsi::gfx::dx12 {

namespace {
std::atomic<std::uint32_t> g_texture_max_edge{0};
}

void set_texture_max_edge(std::uint32_t px) { g_texture_max_edge.store(px); }
namespace {
std::atomic<std::uint32_t> g_texture_mip_skip{0};
}
void set_texture_mip_skip(std::uint32_t n) { g_texture_mip_skip.store(n); }
std::uint32_t texture_mip_skip() { return g_texture_mip_skip.load(); }
std::uint32_t texture_max_edge() { return g_texture_max_edge.load(); }

namespace texture_detail {


// CPU-only: builds the full RGBA mip chain (max-edge scale, leaf dilation, alpha-weighted box filter).
// Thread-safe (no D3D calls) so prefetch workers can run it ahead of the UI-thread GPU upload.
RgbaMipChain build_rgba_mip_chain(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height,
                                  std::uint32_t src_stride, std::uint32_t mip_levels) {
    RgbaMipChain chain{};
    if (!pixels || width == 0 || height == 0) return chain;

    if (src_stride == 0) src_stride = width * 4u;
    std::vector<std::uint8_t> level0(static_cast<std::size_t>(width) * height * 4u);
    const std::uint32_t packed_row = width * 4u;
    for (std::uint32_t y = 0; y < height; ++y)
        std::memcpy(level0.data() + y * packed_row, pixels + y * src_stride, packed_row);

    const std::uint32_t max_edge = g_texture_max_edge.load();
    if (max_edge > 0 && (width > max_edge || height > max_edge)) {
        const float scale = static_cast<float>(max_edge) / static_cast<float>((std::max)(width, height));
        const std::uint32_t nw = (std::max)(1u, static_cast<std::uint32_t>(width * scale + 0.5f));
        const std::uint32_t nh = (std::max)(1u, static_cast<std::uint32_t>(height * scale + 0.5f));
        std::vector<std::uint8_t> scaled(static_cast<std::size_t>(nw) * nh * 4u);
        auto sample = [&](float fx, float fy, int c) -> float {
            fx = (std::max)(0.f, (std::min)(fx, static_cast<float>(width - 1u)));
            fy = (std::max)(0.f, (std::min)(fy, static_cast<float>(height - 1u)));
            const std::uint32_t x0 = static_cast<std::uint32_t>(fx);
            const std::uint32_t y0 = static_cast<std::uint32_t>(fy);
            const std::uint32_t x1 = (std::min)(x0 + 1u, width - 1u);
            const std::uint32_t y1 = (std::min)(y0 + 1u, height - 1u);
            const float tx = fx - static_cast<float>(x0);
            const float ty = fy - static_cast<float>(y0);
            const float a = level0[(y0 * width + x0) * 4u + c];
            const float b = level0[(y0 * width + x1) * 4u + c];
            const float d = level0[(y1 * width + x0) * 4u + c];
            const float e = level0[(y1 * width + x1) * 4u + c];
            return a * (1.f - tx) * (1.f - ty) + b * tx * (1.f - ty) + d * (1.f - tx) * ty + e * tx * ty;
        };
        for (std::uint32_t y = 0; y < nh; ++y) {
            const float fy = (static_cast<float>(y) + 0.5f) * static_cast<float>(height) / static_cast<float>(nh) - 0.5f;
            for (std::uint32_t x = 0; x < nw; ++x) {
                const float fx =
                    (static_cast<float>(x) + 0.5f) * static_cast<float>(width) / static_cast<float>(nw) - 0.5f;
                for (int c = 0; c < 4; ++c)
                    scaled[(y * nw + x) * 4u + c] =
                        static_cast<std::uint8_t>((std::min)(255.f, sample(fx, fy, c) + 0.5f));
            }
        }
        level0 = std::move(scaled);
        width = nw;
        height = nh;
    }

    // Full CPU mip chain for aniso sampling (no GenerateMips in DX12 without RT).
    // mip_levels==0 → auto chain; 1 → text/UI (mips smear glyph alpha into a solid plane).
    UINT levels = mip_levels;
    if (levels == 0) {
        levels = 1;
        UINT w = width, h = height;
        while (w > 1 || h > 1) {
            w = (std::max)(1u, w / 2u);
            h = (std::max)(1u, h / 2u);
            ++levels;
            if (levels >= 16u) break; // full chain through 1×1 for up to 32k edges
        }
    } else {
        levels = (std::max)(1u, (std::min)(levels, 16u));
    }
    chain.width = width;
    chain.height = height;
    chain.levels = levels;
    chain.mips.resize(levels);
    std::vector<std::uint8_t> cur = level0;
    std::uint32_t cw = width, ch = height;
    for (UINT mip = 0; mip < levels; ++mip) {
        chain.mips[mip] = cur; // stored level is the undilated one; next level is filtered from the dilated one
        // Dilate leaf RGB into transparent texels so aniso/mips don't pull black/sky into cutout edges.
        {
            std::vector<std::uint8_t> dil = cur;
            for (std::uint32_t y = 0; y < ch; ++y) {
                for (std::uint32_t x = 0; x < cw; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * cw + x) * 4u;
                    if (cur[i + 3] >= 8) continue;
                    float wr = 0.f, wg = 0.f, wb = 0.f, wsum = 0.f;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 && dy == 0) continue;
                            const int nx = static_cast<int>(x) + dx;
                            const int ny = static_cast<int>(y) + dy;
                            if (nx < 0 || ny < 0 || nx >= static_cast<int>(cw) || ny >= static_cast<int>(ch))
                                continue;
                            const std::size_t ni =
                                (static_cast<std::size_t>(ny) * cw + static_cast<std::size_t>(nx)) * 4u;
                            const float aw = static_cast<float>(cur[ni + 3]);
                            if (aw < 8.f) continue;
                            wr += static_cast<float>(cur[ni + 0]) * aw;
                            wg += static_cast<float>(cur[ni + 1]) * aw;
                            wb += static_cast<float>(cur[ni + 2]) * aw;
                            wsum += aw;
                        }
                    }
                    if (wsum > 0.f) {
                        dil[i + 0] = static_cast<std::uint8_t>(wr / wsum + 0.5f);
                        dil[i + 1] = static_cast<std::uint8_t>(wg / wsum + 0.5f);
                        dil[i + 2] = static_cast<std::uint8_t>(wb / wsum + 0.5f);
                    }
                }
            }
            cur.swap(dil);
        }
        if (mip + 1 >= levels) break;
        const std::uint32_t nw = (std::max)(1u, cw / 2u);
        const std::uint32_t nh = (std::max)(1u, ch / 2u);
        std::vector<std::uint8_t> next(static_cast<std::size_t>(nw) * nh * 4u);
        auto lin = [](std::uint8_t c) {
            const float x = static_cast<float>(c) * (1.f / 255.f);
            return x * x;
        };
        auto srgb = [](float x) -> std::uint8_t {
            x = (std::max)(0.f, x);
            return static_cast<std::uint8_t>((std::min)(255.f, std::sqrt(x) * 255.f + 0.5f));
        };
        // Alpha-weighted box filter (Unity/CE docs): opaque RGB doesn't bleed into cutouts.
        for (std::uint32_t y = 0; y < nh; ++y) {
            for (std::uint32_t x = 0; x < nw; ++x) {
                const std::uint32_t x0 = (std::min)(x * 2u, cw - 1u);
                const std::uint32_t y0 = (std::min)(y * 2u, ch - 1u);
                const std::uint32_t x1 = (std::min)(x0 + 1u, cw - 1u);
                const std::uint32_t y1 = (std::min)(y0 + 1u, ch - 1u);
                const std::size_t i00 = (y0 * cw + x0) * 4u;
                const std::size_t i10 = (y0 * cw + x1) * 4u;
                const std::size_t i01 = (y1 * cw + x0) * 4u;
                const std::size_t i11 = (y1 * cw + x1) * 4u;
                const float a00 = static_cast<float>(cur[i00 + 3]);
                const float a10 = static_cast<float>(cur[i10 + 3]);
                const float a01 = static_cast<float>(cur[i01 + 3]);
                const float a11 = static_cast<float>(cur[i11 + 3]);
                const float asum = a00 + a10 + a01 + a11;
                const std::size_t di = (static_cast<std::size_t>(y) * nw + x) * 4u;
                if (asum > 0.5f) {
                    for (int c = 0; c < 3; ++c) {
                        const float weighted =
                            (lin(cur[i00 + c]) * a00 + lin(cur[i10 + c]) * a10 + lin(cur[i01 + c]) * a01 +
                             lin(cur[i11 + c]) * a11) /
                            asum;
                        next[di + c] = srgb(weighted);
                    }
                } else {
                    for (int c = 0; c < 3; ++c) {
                        next[di + c] =
                            srgb(0.25f * (lin(cur[i00 + c]) + lin(cur[i10 + c]) + lin(cur[i01 + c]) +
                                          lin(cur[i11 + c])));
                    }
                }
                next[di + 3] = static_cast<std::uint8_t>(asum * 0.25f + 0.5f);
            }
        }
        cur = std::move(next);
        cw = nw;
        ch = nh;
    }
    return chain;
}

Dx12Texture upload_rgba_texture(ID3D12Device* device, ID3D12GraphicsCommandList* cmd_list,
                                const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu,
                                const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu, const std::uint8_t* pixels,
                                std::uint32_t width, std::uint32_t height, std::uint32_t src_stride,
                                std::uint32_t mip_levels) {
    return upload_rgba_mip_chain(device, cmd_list, srv_cpu, srv_gpu,
                                 build_rgba_mip_chain(pixels, width, height, src_stride, mip_levels));
}

// UI-thread part: committed resource + staging copy of a prebuilt chain + SRV. No per-texel work here.
Dx12Texture upload_rgba_mip_chain(ID3D12Device* device, ID3D12GraphicsCommandList* cmd_list,
                                  const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu,
                                  const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu, const RgbaMipChain& chain) {
    Dx12Texture out{};
    if (!device || !cmd_list || chain.levels == 0 || chain.width == 0 || chain.height == 0 ||
        chain.mips.size() < chain.levels)
        return out;
    const std::uint32_t width = chain.width;
    const std::uint32_t height = chain.height;
    const UINT levels = chain.levels;

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = static_cast<UINT16>(levels);
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    throw_hr(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(out.resource.GetAddressOf())),
             "rgba texture resource");

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(levels);
    std::vector<UINT> num_rows(levels);
    std::vector<UINT64> row_sizes(levels);
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&tex_desc, 0, levels, 0, footprints.data(), num_rows.data(),
                                  row_sizes.data(), &total_bytes);

    CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC upload_desc = CD3DX12_RESOURCE_DESC::Buffer(total_bytes);
    out.upload_staging.Reset();
    throw_hr(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(out.upload_staging.GetAddressOf())),
             "rgba texture upload");

    void* mapped = nullptr;
    out.upload_staging->Map(0, nullptr, &mapped);
    auto* dst_base = static_cast<std::uint8_t*>(mapped);
    for (UINT mip = 0; mip < levels; ++mip) {
        const std::uint32_t mw = (std::max)(1u, width >> mip);
        const std::uint32_t mh = (std::max)(1u, height >> mip);
        const auto& src = chain.mips[mip];
        auto* dst = dst_base + footprints[mip].Offset;
        const UINT row_pitch = footprints[mip].Footprint.RowPitch;
        for (std::uint32_t y = 0; y < mh; ++y)
            std::memcpy(dst + y * row_pitch, src.data() + static_cast<std::size_t>(y) * mw * 4u, mw * 4u);
    }
    out.upload_staging->Unmap(0, nullptr);

    for (UINT mip = 0; mip < levels; ++mip) {
        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = out.resource.Get();
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = mip;

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = out.upload_staging.Get();
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint = footprints[mip];
        cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier = dx12_transition_barrier(out.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd_list->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = levels;
    device->CreateShaderResourceView(out.resource.Get(), &srv_desc, srv_cpu);

    out.srv_gpu = srv_gpu;
    out.width = width;
    out.height = height;
    out.valid = true;
    return out;
}

Dx12Texture upload_bc_texture(ID3D12Device* device, ID3D12GraphicsCommandList* cmd_list,
                              const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu,
                              const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu, DXGI_FORMAT format,
                              const std::uint8_t* blocks, std::uint32_t width, std::uint32_t height,
                              std::uint32_t mip_levels) {
    Dx12Texture out{};
    if (!device || !cmd_list || !blocks || width == 0 || height == 0) return out;
    if (!is_block_compressed(format)) return out;
    const UINT levels = (std::max)(1u, (std::min)(mip_levels, 16u));

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = static_cast<UINT16>(levels);
    tex_desc.Format = format;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    throw_hr(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(out.resource.GetAddressOf())),
             "bc texture resource");

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(levels);
    std::vector<UINT> num_rows(levels);
    std::vector<UINT64> row_sizes(levels);
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&tex_desc, 0, levels, 0, footprints.data(), num_rows.data(),
                                  row_sizes.data(), &total_bytes);

    CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC upload_desc = CD3DX12_RESOURCE_DESC::Buffer(total_bytes);
    throw_hr(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(out.upload_staging.GetAddressOf())),
             "bc texture upload");

    const std::uint32_t block_bytes = bc_block_bytes(format);
    void* mapped = nullptr;
    out.upload_staging->Map(0, nullptr, &mapped);
    auto* dst_base = static_cast<std::uint8_t*>(mapped);
    const std::uint8_t* src = blocks;
    std::uint32_t cw = width, ch = height;
    for (UINT mip = 0; mip < levels; ++mip) {
        const std::uint32_t src_row = ((cw + 3u) / 4u) * block_bytes;
        auto* dst = dst_base + footprints[mip].Offset;
        for (UINT y = 0; y < num_rows[mip]; ++y)
            std::memcpy(dst + y * footprints[mip].Footprint.RowPitch,
                        src + static_cast<std::size_t>(y) * src_row, src_row);
        src += static_cast<std::size_t>(num_rows[mip]) * src_row;
        cw = (std::max)(1u, cw / 2u);
        ch = (std::max)(1u, ch / 2u);
    }
    out.upload_staging->Unmap(0, nullptr);

    for (UINT mip = 0; mip < levels; ++mip) {
        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource = out.resource.Get();
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.SubresourceIndex = mip;
        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = out.upload_staging.Get();
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_loc.PlacedFootprint = footprints[mip];
        cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier = dx12_transition_barrier(out.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd_list->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    // BC4 is single channel: replicate R into RGB and force alpha 1 so detail maps,
    // masks and AO read as greyscale instead of solid red.
    srv_desc.Shader4ComponentMapping =
        format == DXGI_FORMAT_BC4_UNORM
            ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                                                     D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                                                     D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                                                     D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1)
            : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = levels;
    device->CreateShaderResourceView(out.resource.Get(), &srv_desc, srv_cpu);

    out.srv_gpu = srv_gpu;
    out.width = width;
    out.height = height;
    out.valid = true;
    return out;
}
} // namespace texture_detail

} // namespace omsi::gfx::dx12
