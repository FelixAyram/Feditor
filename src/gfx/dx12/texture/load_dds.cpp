#include "gfx/dx12/dx12_texture.hpp"
#include "gfx/dx12/texture/internal.hpp"
#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "omsi/logger.hpp"

#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace omsi::gfx::dx12 {

Dx12Texture load_texture_dds(ID3D12Device* device, ID3D12GraphicsCommandList* cmd_list,
                             const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu,
                             const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu, const std::filesystem::path& path) {
    Dx12Texture out{};
    if (!device || !cmd_list) return out;

    std::vector<std::uint8_t> bytes;
    if (const auto raw = texture_detail::raw_file_cache_take(path)) {
        bytes = *raw;
    } else if (!texture_detail::path_exists(path) || !texture_detail::read_file_bytes(path, bytes)) {
        return out;
    }
    {
        std::vector<std::uint8_t> rgba;
        std::uint32_t uw = 0, uh = 0;
        if (texture_detail::dds_uncompressed_to_rgba(bytes.data(), bytes.size(), rgba, uw, uh))
            return texture_detail::upload_rgba_texture(device, cmd_list, srv_cpu, srv_gpu, rgba.data(), uw, uh, uw * 4u);
    }
    texture_detail::DdsInfo info;
    if (!texture_detail::dds_parse_header(bytes.data(), bytes.size(), info)) return out;
    std::uint32_t width = info.width, height = info.height;
    const std::uint32_t pitch = info.pitch;
    const DXGI_FORMAT format = info.format;

    const std::uint32_t computed_size = texture_detail::dds_top_mip_size(width, height, format, pitch);
    const std::uint32_t available = static_cast<std::uint32_t>(bytes.size() - info.data_offset);
    if (computed_size == 0 || computed_size > available) return out;
    const std::uint8_t* pixel_data = bytes.data() + info.data_offset;
    const std::uint32_t header_mips = info.mips;

    if (texture_detail::is_block_compressed(format)) {
        const std::uint32_t max_edge = texture_max_edge();
        std::uint32_t file_mips =
            texture_detail::dds_usable_mip_count(width, height, format, available, header_mips);
        const bool cpu_decodable =
            format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC2_UNORM || format == DXGI_FORMAT_BC3_UNORM;
        // Quality mip skip: drop the largest levels while keeping blocks >= 4x4.
        for (std::uint32_t skip = texture_mip_skip(); skip > 0 && file_mips > 1u && width > 4u && height > 4u; --skip) {
            pixel_data += texture_detail::dds_bc_mip_bytes(width, height, format);
            width = (std::max)(1u, width / 2u);
            height = (std::max)(1u, height / 2u);
            --file_mips;
        }
        const bool need_cpu_mips = cpu_decodable && (max_edge > 0 || file_mips <= 1u);
        if (need_cpu_mips) {
            if (const auto cached = texture_detail::cpu_mip_cache_take(path)) {
                return texture_detail::upload_rgba_mip_chain(device, cmd_list, srv_cpu, srv_gpu, *cached);
            }
            const auto rgba = texture_detail::decompress_dds_blocks(pixel_data, width, height, format);
            return texture_detail::upload_rgba_texture(device, cmd_list, srv_cpu, srv_gpu, rgba.data(), width, height,
                                                       width * 4u);
        }
        return texture_detail::upload_bc_texture(device, cmd_list, srv_cpu, srv_gpu, format, pixel_data, width,
                                                 height, file_mips);
    }

    if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        const std::uint32_t src_stride = pitch > 0 ? pitch : width * 4u;
        return texture_detail::upload_rgba_texture(device, cmd_list, srv_cpu, srv_gpu, pixel_data, width, height, src_stride);
    }

    return out;
}
} // namespace omsi::gfx::dx12
