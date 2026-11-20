#pragma once

#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/vfs_overlay.hpp"

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omsi::gfx::dx12 {
namespace texture_detail {

// What a texture file is by its first bytes, as Omsi.exe's D3DX loader decides (D3DXGetImageInfoFromFile):
// add-ons ship 32-bit BMPs renamed .dds (MB T1, the X10 cars) and TGAs named .bmp - by the extension those
// failed to load and the material drew whatever its empty descriptor pointed at (grass, leaves).
enum class TexFormat { unknown, dds, bmp, png, jpg, tga };
inline TexFormat sniff_texture_format_uncached(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    unsigned char h[4]{};
    if (!f || !f.read(reinterpret_cast<char*>(h), 4)) return TexFormat::unknown;
    if (h[0] == 'D' && h[1] == 'D' && h[2] == 'S' && h[3] == ' ') return TexFormat::dds;
    if (h[0] == 'B' && h[1] == 'M') return TexFormat::bmp;
    if (h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G') return TexFormat::png;
    if (h[0] == 0xFF && h[1] == 0xD8) return TexFormat::jpg;
    // TGA has no magic: colour-map type 0/1 and image type 1-3 / 9-11
    if (h[1] <= 1 && (h[2] == 1 || h[2] == 2 || h[2] == 3 || h[2] == 9 || h[2] == 10 || h[2] == 11)) return TexFormat::tga;
    return TexFormat::unknown;
}
// (cached per path: every [matl_change] lamp tick asked again and opened the file each time - 30-40 ms
//  stalls with many AI cars; a file's first bytes do not change while the editor runs)
inline TexFormat sniff_texture_format(const std::filesystem::path& p) {
    static std::mutex mu;
    static std::unordered_map<std::wstring, TexFormat> cache;
    const std::wstring key = p.native();
    {
        std::lock_guard<std::mutex> lk(mu);
        if (const auto it = cache.find(key); it != cache.end()) return it->second;
    }
    const TexFormat f = sniff_texture_format_uncached(p);
    std::lock_guard<std::mutex> lk(mu);
    if (cache.size() > 200000) cache.clear();
    cache.emplace(key, f);
    return f;
}

inline std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline std::filesystem::path texture_path_from_name(std::string_view texture_name) {
    std::string normalized = lower_ascii(texture_name);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return std::filesystem::path(normalized).lexically_normal();
}

inline bool iequals_ext(const std::filesystem::path& path, const wchar_t* ext) {
    const auto p = path.extension().wstring();
    if (p.size() != std::wcslen(ext)) return false;
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (::towlower(p[i]) != ::towlower(ext[i])) return false;
    }
    return true;
}

bool virtual_path_exists(const std::filesystem::path& path);
inline bool path_exists(const std::filesystem::path& path) {
    if (is_virtual_path(path)) return virtual_path_exists(path);
    if (omsi::vfs::active()) return !path.empty() && omsi::vfs::exists(path); // co-op RAM files
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec) && !ec;
}

// Fully built RGBA8 mip chain (CPU work done off the UI thread). mips[i] is tightly packed,
// (width>>i)x(height>>i) with min 1, undilated as stored; the next level is filtered from the dilated one.
struct RgbaMipChain {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 0;
    std::vector<std::vector<std::uint8_t>> mips;
};

[[nodiscard]] RgbaMipChain build_rgba_mip_chain(const std::uint8_t* pixels, std::uint32_t width,
                                                std::uint32_t height, std::uint32_t src_stride,
                                                std::uint32_t mip_levels);
Dx12Texture upload_rgba_mip_chain(ID3D12Device* device, ID3D12GraphicsCommandList* cmd_list,
                                  D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu, D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu,
                                  const RgbaMipChain& chain);
// Prebuilt mip chains keyed by source path (filled by prefetch workers, consumed by the UI-thread upload).
void cpu_mip_cache_put(const std::filesystem::path& path, RgbaMipChain chain);
[[nodiscard]] std::shared_ptr<const RgbaMipChain> cpu_mip_cache_take(const std::filesystem::path& path);

Dx12Texture upload_rgba_texture(ID3D12Device* device, ID3D12GraphicsCommandList* cmd_list,
                                D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu, D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu,
                                const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height,
                                std::uint32_t src_stride, std::uint32_t mip_levels = 0);

Dx12Texture upload_bc_texture(ID3D12Device* device, ID3D12GraphicsCommandList* cmd_list,
                              D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu, D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu,
                              DXGI_FORMAT format, const std::uint8_t* blocks, std::uint32_t width,
                              std::uint32_t height, std::uint32_t mip_levels = 1);

[[nodiscard]] std::uint32_t dds_bc_mip_bytes(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format);
[[nodiscard]] std::uint32_t dds_usable_mip_count(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format,
                                                 std::uint32_t available_bytes, std::uint32_t header_mips);
[[nodiscard]] bool dds_file_needs_cpu_mips(const std::filesystem::path& path);

[[nodiscard]] std::optional<CpuRgbaImage> load_tga_cpu(const std::filesystem::path& path);
[[nodiscard]] std::shared_ptr<const CpuRgbaImage> cpu_texture_cache_take(const std::filesystem::path& path);
void cpu_texture_cache_put(const std::filesystem::path& path, CpuRgbaImage image);
void cpu_texture_cache_invalidate(const std::filesystem::path& path);
// Raw file bytes read on a worker (BC .dds with mips: no decode needed, only the disk read).
[[nodiscard]] std::shared_ptr<const std::vector<std::uint8_t>> raw_file_cache_take(const std::filesystem::path& path);
void raw_file_cache_put(const std::filesystem::path& path, std::vector<std::uint8_t> bytes);
[[nodiscard]] bool read_file_bytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out);

DXGI_FORMAT dds_dxgi_format(std::uint32_t fourcc, std::uint32_t rgb_bitcount);
// Full header parse (legacy + DX10 extension). data_offset = 128 or 148.
struct DdsInfo {
    std::uint32_t width = 0, height = 0, mips = 1, pitch = 0, data_offset = 128;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
bool dds_parse_header(const std::uint8_t* bytes, std::size_t size, DdsInfo& out);
// any uncompressed DDS (RGB / luminance / alpha masks, 8-32 bit) -> RGBA8 top level; false if compressed
bool dds_uncompressed_to_rgba(const std::uint8_t* bytes, std::size_t size, std::vector<std::uint8_t>& out,
                              std::uint32_t& width, std::uint32_t& height);
std::uint32_t bc_block_bytes(DXGI_FORMAT format);
bool is_block_compressed(DXGI_FORMAT format);
std::uint32_t dds_top_mip_size(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format,
                               std::uint32_t pitch);
std::vector<std::uint8_t> decompress_dds_blocks(const std::uint8_t* blocks, std::uint32_t width,
                                                std::uint32_t height, DXGI_FORMAT format);

} // namespace texture_detail

Dx12Texture load_texture_tga(ID3D12Device* device, ID3D12GraphicsCommandList* cmd_list,
                             D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu, D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu,
                             const std::filesystem::path& path);

} // namespace omsi::gfx::dx12
