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
namespace texture_detail {


#ifndef MAKEFOURCC
#define MAKEFOURCC(ch0, ch1, ch2, ch3)                                                                                 \
    (static_cast<std::uint32_t>(static_cast<std::uint8_t>(ch0)) |                                                      \
     (static_cast<std::uint32_t>(static_cast<std::uint8_t>(ch1)) << 8) |                                                \
     (static_cast<std::uint32_t>(static_cast<std::uint8_t>(ch2)) << 16) |                                               \
     (static_cast<std::uint32_t>(static_cast<std::uint8_t>(ch3)) << 24))
#endif

DXGI_FORMAT dds_dxgi_format(std::uint32_t fourcc, std::uint32_t rgb_bitcount) {
    if (fourcc == MAKEFOURCC('D', 'X', 'T', '1')) return DXGI_FORMAT_BC1_UNORM;
    if (fourcc == MAKEFOURCC('D', 'X', 'T', '2') || fourcc == MAKEFOURCC('D', 'X', 'T', '3')) return DXGI_FORMAT_BC2_UNORM;
    if (fourcc == MAKEFOURCC('D', 'X', 'T', '4') || fourcc == MAKEFOURCC('D', 'X', 'T', '5')) return DXGI_FORMAT_BC3_UNORM;
    if (fourcc == MAKEFOURCC('A', 'T', 'I', '1') || fourcc == MAKEFOURCC('B', 'C', '4', 'U')) return DXGI_FORMAT_BC4_UNORM;
    if (fourcc == MAKEFOURCC('A', 'T', 'I', '2') || fourcc == MAKEFOURCC('B', 'C', '5', 'U')) return DXGI_FORMAT_BC5_UNORM;
    if (fourcc == 0 && rgb_bitcount == 32) return DXGI_FORMAT_R8G8B8A8_UNORM;
    return DXGI_FORMAT_UNKNOWN;
}

bool is_block_compressed(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC7_UNORM: return true;
    default: return false;
    }
}

std::uint32_t bc_block_bytes(DXGI_FORMAT format) {
    return (format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC4_UNORM) ? 8u : 16u;
}

// DX10 header: sRGB / typeless / signed variants fold onto the UNORM formats the shaders expect.
static DXGI_FORMAT dx10_fold(std::uint32_t f) {
    switch (f) {
    case 70: case 71: case 72: return DXGI_FORMAT_BC1_UNORM;
    case 73: case 74: case 75: return DXGI_FORMAT_BC2_UNORM;
    case 76: case 77: case 78: return DXGI_FORMAT_BC3_UNORM;
    case 79: case 80: case 81: return DXGI_FORMAT_BC4_UNORM;
    case 82: case 83: case 84: return DXGI_FORMAT_BC5_UNORM;
    case 97: case 98: case 99: return DXGI_FORMAT_BC7_UNORM;
    case 27: case 28: case 29: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool dds_parse_header(const std::uint8_t* b, std::size_t size, DdsInfo& o) {
    if (size < 128u || std::memcmp(b, "DDS ", 4) != 0) return false;
    auto u32 = [&](std::size_t off) { std::uint32_t v; std::memcpy(&v, b + off, 4); return v; };
    const std::uint32_t flags = u32(8);
    o.height = u32(12);
    o.width = u32(16);
    o.pitch = u32(20);
    const std::uint32_t mip_field = u32(28);
    const std::uint32_t fourcc = u32(84);
    const std::uint32_t bits = u32(88);
    o.mips = (flags & 0x20000u) ? (std::max)(1u, mip_field) : 1u;
    o.data_offset = 128;
    if (fourcc == MAKEFOURCC('D', 'X', '1', '0')) {
        if (size < 148u) return false;
        o.format = dx10_fold(u32(128));
        o.data_offset = 148;
    } else {
        o.format = dds_dxgi_format(fourcc, bits);
    }
    return o.format != DXGI_FORMAT_UNKNOWN && o.width && o.height;
}

// Uncompressed DDS by its pixel-format masks, to RGBA8 (top level). D3DX (Omsi.exe's loader) takes any
// DDPF_RGB / DDPF_LUMINANCE / DDPF_ALPHA layout; the loader took only 32-bit as RGBA in memory order -
// A8R8G8B8 (BGRA bytes) came out red/blue swapped, X8R8G8B8 see-through, and A8 / L8 / A8L8 / 565 / 1555 /
// 4444 / 24-bit files failed (the W123's transmap MB_W123_230E_T.dds is A8: the taxi lost its transparency).
bool dds_uncompressed_to_rgba(const std::uint8_t* b, std::size_t size, std::vector<std::uint8_t>& out,
                              std::uint32_t& w, std::uint32_t& h) {
    if (size < 128u || std::memcmp(b, "DDS ", 4) != 0) return false;
    auto u32 = [&](std::size_t off) { std::uint32_t v; std::memcpy(&v, b + off, 4); return v; };
    const std::uint32_t pf_flags = u32(80), fourcc = u32(84), bits = u32(88);
    const std::uint32_t rm = u32(92), gm = u32(96), bm = u32(100), am = u32(104);
    if (fourcc != 0 || (pf_flags & 0x4u) != 0) return false; // DDPF_FOURCC: compressed or DX10
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) return false;
    h = u32(12);
    w = u32(16);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return false;
    const std::size_t bpp = bits / 8u;
    const std::uint32_t pitch_field = u32(20);
    const std::size_t stride = ((u32(8) & 0x8u) != 0 && pitch_field >= w * bpp) ? pitch_field : w * bpp;
    if (128u + stride * (h - 1u) + w * bpp > size) return false;
    const bool lum = (pf_flags & 0x20000u) != 0;
    const bool rgb = (pf_flags & 0x40u) != 0;
    const bool alpha_only = (pf_flags & 0x2u) != 0 && !rgb && !lum;
    const bool has_alpha = am != 0 && ((pf_flags & 0x1u) != 0 || alpha_only);
    if (!alpha_only && !lum && !(rm | gm | bm)) return false;
    auto chan = [](std::uint32_t px, std::uint32_t mask) -> std::uint8_t {
        if (mask == 0) return 0;
        unsigned sh = 0;
        while (((mask >> sh) & 1u) == 0u) ++sh;
        const std::uint32_t mx = mask >> sh;
        return static_cast<std::uint8_t>(((px & mask) >> sh) * 255u / mx);
    };
    out.resize(static_cast<std::size_t>(w) * h * 4u);
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint8_t* row = b + 128u + stride * y;
        std::uint8_t* dst = out.data() + static_cast<std::size_t>(y) * w * 4u;
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint32_t px = 0;
            std::memcpy(&px, row + x * bpp, bpp);
            std::uint8_t r = 0, g = 0, bl = 0;
            if (lum) r = g = bl = chan(px, rm);
            else if (!alpha_only) {
                r = chan(px, rm);
                g = chan(px, gm);
                bl = chan(px, bm);
            }
            dst[x * 4u + 0] = r;
            dst[x * 4u + 1] = g;
            dst[x * 4u + 2] = bl;
            dst[x * 4u + 3] = has_alpha ? chan(px, am) : 255u;
        }
    }
    return true;
}

// Blender O3D-IO dds_loader: ignore dwPitchOrLinearSize for block formats and compute manually.
std::uint32_t dds_top_mip_size(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format,
                               std::uint32_t pitch) {
    if (texture_detail::is_block_compressed(format)) {
        return ((width + 3u) / 4u) * ((height + 3u) / 4u) * bc_block_bytes(format);
    }
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        if (pitch > 0) return pitch * height;
        return width * height * 4u;
    }
    if (pitch > 0) return pitch;
    return 0;
}

void unpack_rgb565(std::uint16_t c, std::uint8_t rgb[3]) {
    rgb[0] = static_cast<std::uint8_t>(((c >> 11) & 0x1Fu) * 255u / 31u);
    rgb[1] = static_cast<std::uint8_t>(((c >> 5) & 0x3Fu) * 255u / 63u);
    rgb[2] = static_cast<std::uint8_t>((c & 0x1Fu) * 255u / 31u);
}

void decode_bc1_color_block(const std::uint8_t* block, std::uint8_t* out_rgba16) {
    const std::uint16_t c0 = static_cast<std::uint16_t>(block[0] | (block[1] << 8));
    const std::uint16_t c1 = static_cast<std::uint16_t>(block[2] | (block[3] << 8));
    std::uint8_t colors[4][4]{};
    unpack_rgb565(c0, colors[0]);
    colors[0][3] = 255;
    unpack_rgb565(c1, colors[1]);
    colors[1][3] = 255;

    if (c0 > c1) {
        for (int i = 0; i < 3; ++i) {
            colors[2][i] = static_cast<std::uint8_t>((2 * colors[0][i] + colors[1][i]) / 3);
            colors[3][i] = static_cast<std::uint8_t>((colors[0][i] + 2 * colors[1][i]) / 3);
        }
        colors[2][3] = colors[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) {
            colors[2][i] = static_cast<std::uint8_t>((colors[0][i] + colors[1][i]) / 2);
            colors[3][i] = 0;
        }
        colors[2][3] = 255;
        colors[3][3] = 0;
    }

    const std::uint32_t bits =
        static_cast<std::uint32_t>(block[4]) | (static_cast<std::uint32_t>(block[5]) << 8) |
        (static_cast<std::uint32_t>(block[6]) << 16) | (static_cast<std::uint32_t>(block[7]) << 24);
    for (int i = 0; i < 16; ++i) {
        const int idx = static_cast<int>((bits >> (2 * i)) & 3u);
        std::memcpy(out_rgba16 + i * 4, colors[idx], 4);
    }
}

void decode_bc3_alpha_block(const std::uint8_t* block, std::uint8_t alpha[16]) {
    const std::uint8_t a0 = block[0];
    const std::uint8_t a1 = block[1];
    std::uint8_t alphas[8]{a0, a1};
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i)
            alphas[i + 1] = static_cast<std::uint8_t>(((7 - i) * a0 + i * a1) / 7);
    } else {
        for (int i = 1; i < 5; ++i)
            alphas[i + 1] = static_cast<std::uint8_t>(((5 - i) * a0 + i * a1) / 5);
        alphas[6] = 0;
        alphas[7] = 255;
    }

    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(block[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) {
        const int idx = static_cast<int>((bits >> (3 * i)) & 7u);
        alpha[i] = alphas[idx];
    }
}

void decode_bc2_block(const std::uint8_t* block, std::uint8_t* out_rgba16) {
    std::uint8_t alpha[16]{};
    for (int i = 0; i < 16; ++i) {
        const int word = block[(i / 4) * 2] | (block[(i / 4) * 2 + 1] << 8);
        const int a4 = (word >> ((i % 4) * 4)) & 0xF;
        alpha[i] = static_cast<std::uint8_t>((a4 * 255 + 7) / 15);
    }

    std::uint8_t color[64]{};
    decode_bc1_color_block(block + 8, color);
    for (int i = 0; i < 16; ++i) {
        out_rgba16[i * 4 + 0] = color[i * 4 + 0];
        out_rgba16[i * 4 + 1] = color[i * 4 + 1];
        out_rgba16[i * 4 + 2] = color[i * 4 + 2];
        out_rgba16[i * 4 + 3] = alpha[i];
    }
}

void decode_bc3_block(const std::uint8_t* block, std::uint8_t* out_rgba16) {
    std::uint8_t alpha[16]{};
    decode_bc3_alpha_block(block, alpha);

    std::uint8_t color[64]{};
    decode_bc1_color_block(block + 8, color);
    for (int i = 0; i < 16; ++i) {
        out_rgba16[i * 4 + 0] = color[i * 4 + 0];
        out_rgba16[i * 4 + 1] = color[i * 4 + 1];
        out_rgba16[i * 4 + 2] = color[i * 4 + 2];
        out_rgba16[i * 4 + 3] = alpha[i];
    }
}

void decode_bc1_block(const std::uint8_t* block, std::uint8_t* out_rgba16) {
    decode_bc1_color_block(block, out_rgba16);
}

// BC4 = single channel (height / opacity masks). Replicate to rgb and keep alpha opaque so the
// BeamNG shader's min(alpha, luminance) mask and grey-scale reads both work.
void decode_bc4_block(const std::uint8_t* block, std::uint8_t* out_rgba16) {
    std::uint8_t r[16]{};
    decode_bc3_alpha_block(block, r);
    for (int i = 0; i < 16; ++i) {
        out_rgba16[i * 4 + 0] = r[i];
        out_rgba16[i * 4 + 1] = r[i];
        out_rgba16[i * 4 + 2] = r[i];
        out_rgba16[i * 4 + 3] = 255;
    }
}

// BC5 = two BC4 blocks (normal XY). r/g from the blocks, b = 0, a = 255.
void decode_bc5_block(const std::uint8_t* block, std::uint8_t* out_rgba16) {
    std::uint8_t r[16]{}, g[16]{};
    decode_bc3_alpha_block(block, r);
    decode_bc3_alpha_block(block + 8, g);
    for (int i = 0; i < 16; ++i) {
        out_rgba16[i * 4 + 0] = r[i];
        out_rgba16[i * 4 + 1] = g[i];
        out_rgba16[i * 4 + 2] = 0;
        out_rgba16[i * 4 + 3] = 255;
    }
}

std::vector<std::uint8_t> decompress_dds_blocks(const std::uint8_t* blocks, std::uint32_t width,
                                                std::uint32_t height, DXGI_FORMAT format) {
    const std::uint32_t blocks_wide = (width + 3u) / 4u;
    const std::uint32_t blocks_high = (height + 3u) / 4u;
    // BC1 and BC4 are 8 bytes per 4x4 block; BC2/BC3/BC5 are 16 (BC4 was decoded with a 16-byte
    // stride before, which walked off the mip and produced garbage in BC4 opacity/height maps).
    const std::uint32_t block_bytes = bc_block_bytes(format);
    std::vector<std::uint8_t> rgba(width * height * 4u, 0);

    for (std::uint32_t by = 0; by < blocks_high; ++by) {
        for (std::uint32_t bx = 0; bx < blocks_wide; ++bx) {
            const std::uint8_t* block = blocks + (by * blocks_wide + bx) * block_bytes;
            std::uint8_t pixels[64]{};
            if (format == DXGI_FORMAT_BC1_UNORM) decode_bc1_block(block, pixels);
            else if (format == DXGI_FORMAT_BC2_UNORM) decode_bc2_block(block, pixels);
            else if (format == DXGI_FORMAT_BC4_UNORM) decode_bc4_block(block, pixels);
            else if (format == DXGI_FORMAT_BC5_UNORM) decode_bc5_block(block, pixels);
            else decode_bc3_block(block, pixels);

            for (std::uint32_t py = 0; py < 4; ++py) {
                for (std::uint32_t px = 0; px < 4; ++px) {
                    const std::uint32_t x = bx * 4u + px;
                    const std::uint32_t y = by * 4u + py;
                    if (x >= width || y >= height) continue;
                    std::memcpy(rgba.data() + (y * width + x) * 4u, pixels + (py * 4u + px) * 4u, 4);
                }
            }
        }
    }
    return rgba;
}

std::uint32_t dds_bc_mip_bytes(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format) {
    if (!is_block_compressed(format)) return 0;
    return ((width + 3u) / 4u) * ((height + 3u) / 4u) * bc_block_bytes(format);
}

std::uint32_t dds_usable_mip_count(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format,
                                   std::uint32_t available_bytes, std::uint32_t header_mips) {
    const std::uint32_t want = (std::max)(1u, (std::min)(header_mips, 16u));
    if (!is_block_compressed(format)) return 1;
    std::uint32_t w = width, h = height, used = 0, n = 0;
    for (std::uint32_t i = 0; i < want; ++i) {
        const std::uint32_t sz = dds_bc_mip_bytes(w, h, format);
        if (sz == 0 || used + sz > available_bytes) break;
        used += sz;
        ++n;
        w = (std::max)(1u, w / 2u);
        h = (std::max)(1u, h / 2u);
    }
    return (std::max)(1u, n);
}

bool dds_file_needs_cpu_mips(const std::filesystem::path& path) {
    if (is_virtual_path(path)) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::uint8_t hdr[128]{};
    file.read(reinterpret_cast<char*>(hdr), 128);
    if (file.gcount() < 128 || std::memcmp(hdr, "DDS ", 4) != 0) return false;
    const std::uint32_t flags = *reinterpret_cast<const std::uint32_t*>(hdr + 8);
    const std::uint32_t height = *reinterpret_cast<const std::uint32_t*>(hdr + 12);
    const std::uint32_t width = *reinterpret_cast<const std::uint32_t*>(hdr + 16);
    const std::uint32_t mip_field = *reinterpret_cast<const std::uint32_t*>(hdr + 28);
    const std::uint32_t fourcc = *reinterpret_cast<const std::uint32_t*>(hdr + 84);
    const std::uint32_t rgb_bitcount = *reinterpret_cast<const std::uint32_t*>(hdr + 88);
    const DXGI_FORMAT format = dds_dxgi_format(fourcc, rgb_bitcount);
    if (!is_block_compressed(format) || width == 0 || height == 0) return true;
    const std::uint32_t header_mips = (flags & 0x20000u) ? (std::max)(1u, mip_field) : 1u;
    if (header_mips <= 1u) return true;
    file.seekg(0, std::ios::end);
    const auto total = static_cast<std::uint32_t>(file.tellg());
    if (total <= 128u) return true;
    return dds_usable_mip_count(width, height, format, total - 128u, header_mips) <= 1u;
}
} // namespace texture_detail

} // namespace omsi::gfx::dx12
