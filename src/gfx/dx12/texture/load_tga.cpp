#include "gfx/dx12/dx12_texture.hpp"
#include "gfx/dx12/texture/internal.hpp"
#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "omsi/logger.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace omsi::gfx::dx12 {
namespace texture_detail {

std::optional<CpuRgbaImage> load_tga_cpu(const std::filesystem::path& path) {
    if (!path_exists(path)) return std::nullopt;

    std::vector<std::uint8_t> bytes;
    if (!read_file_bytes(path, bytes)) return std::nullopt;
    if (bytes.size() < 18u) return std::nullopt;

    const std::uint8_t image_type = bytes[2];
    const std::uint16_t width = static_cast<std::uint16_t>(bytes[12] | (bytes[13] << 8));
    const std::uint16_t height = static_cast<std::uint16_t>(bytes[14] | (bytes[15] << 8));
    const std::uint8_t bpp = bytes[16];
    const std::uint8_t descriptor = bytes[17];
    if (width == 0 || height == 0) return std::nullopt;
    if (image_type != 2 && image_type != 10) return std::nullopt;
    if (bpp != 24 && bpp != 32) return std::nullopt;

    const std::size_t id_len = bytes[0];
    const std::size_t color_map_type = bytes[1];
    std::size_t offset = 18u + id_len;
    if (color_map_type != 0) {
        const std::uint16_t cm_len = static_cast<std::uint16_t>(bytes[5] | (bytes[6] << 8));
        const std::uint8_t cm_entry = bytes[7];
        offset += static_cast<std::size_t>(cm_len) * ((cm_entry + 7u) / 8u);
    }
    if (offset > bytes.size()) return std::nullopt;

    const std::size_t pixel_bytes = static_cast<std::size_t>(bpp / 8u);
    const bool top_origin = (descriptor & 0x20) != 0;
    CpuRgbaImage out{};
    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<std::size_t>(width) * height * 4u);

    auto write_pixel = [&](std::uint32_t x, std::uint32_t y, const std::uint8_t* src) -> bool {
        if (x >= width || y >= height) return false;
        const std::uint32_t dst_y = top_origin ? y : (height - 1u - y);
        std::uint8_t* dst = out.rgba.data() + (static_cast<std::size_t>(dst_y) * width + x) * 4u;
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = bpp == 32 ? src[3] : 255u;
        return true;
    };

    if (image_type == 2) {
        const std::size_t needed = offset + static_cast<std::size_t>(width) * height * pixel_bytes;
        if (needed > bytes.size()) return std::nullopt;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint8_t* src = bytes.data() + offset;
                offset += pixel_bytes;
                write_pixel(x, y, src);
            }
        }
    } else {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        while (offset < bytes.size() && y < height) {
            if (offset >= bytes.size()) break;
            const std::uint8_t packet = bytes[offset++];
            const bool rle = (packet & 0x80) != 0;
            const std::uint8_t count = static_cast<std::uint8_t>((packet & 0x7Fu) + 1u);
            if (rle) {
                if (offset + pixel_bytes > bytes.size()) break;
                const std::uint8_t* src = bytes.data() + offset;
                offset += pixel_bytes;
                for (int i = 0; i < count; ++i) {
                    if (y >= height) break;
                    write_pixel(x, y, src);
                    ++x;
                    if (x >= width) {
                        x = 0;
                        ++y;
                    }
                }
            } else {
                for (int i = 0; i < count; ++i) {
                    if (offset + pixel_bytes > bytes.size() || y >= height) break;
                    write_pixel(x, y, bytes.data() + offset);
                    offset += pixel_bytes;
                    ++x;
                    if (x >= width) {
                        x = 0;
                        ++y;
                    }
                }
            }
        }
    }
    return out;
}

} // namespace texture_detail

Dx12Texture load_texture_tga(ID3D12Device* device, ID3D12GraphicsCommandList* cmd_list,
                             const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu,
                             const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu, const std::filesystem::path& path) {
    Dx12Texture out{};
    if (!device || !cmd_list) return out;

    if (const auto cached = texture_detail::cpu_mip_cache_take(path)) {
        return texture_detail::upload_rgba_mip_chain(device, cmd_list, srv_cpu, srv_gpu, *cached);
    }

    const auto img = texture_detail::load_tga_cpu(path);
    if (!img) return out;
    return texture_detail::upload_rgba_texture(device, cmd_list, srv_cpu, srv_gpu, img->rgba.data(), img->width,
                                               img->height, img->width * 4u);
}

} // namespace omsi::gfx::dx12
