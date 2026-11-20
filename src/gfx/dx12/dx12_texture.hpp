#pragma once



#include <d3d12.h>
#include <cstdint>
#include <filesystem>

#include <optional>
#include <string>

#include <string_view>

#include <vector>

#include <wrl/client.h>



namespace omsi::gfx::dx12 {



using Microsoft::WRL::ComPtr;



struct Dx12Texture {

    ComPtr<ID3D12Resource> resource;

    // Upload heap must outlive ExecuteCommandLists for the copy that uses it.

    ComPtr<ID3D12Resource> upload_staging;

    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};

    std::uint32_t width = 0;

    std::uint32_t height = 0;

    bool valid = false;

};



// Texture search (only):
//   {sco|sli_dir}\texture\{name.dds|name.orig}
//   for .o3d: {sco_dir}\texture\… (parent of model\), never model\
//   last: {omsi_root}\texture\{name.dds|name.orig}

[[nodiscard]] std::optional<std::filesystem::path> resolve_texture_path(
    const std::filesystem::path& base_file_path, std::string_view texture_name,
    const std::filesystem::path& omsi_root = {});

/// Base texture\ only (no season/night) — catalog thumbs and fixed UI previews.
[[nodiscard]] std::optional<std::filesystem::path> resolve_texture_path_day(
    const std::filesystem::path& base_file_path, std::string_view texture_name,
    const std::filesystem::path& omsi_root = {});

/// Re-pick season folder for an already-resolved albedo path (never texture\night).
[[nodiscard]] std::optional<std::filesystem::path> resolve_season_rebind(
    const std::filesystem::path& current_resolved);



[[nodiscard]] Dx12Texture load_texture_wic(ID3D12Device* device, ID3D12GraphicsCommandList* cmd_list,

                                           D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu,

                                           D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu,

                                           const std::filesystem::path& path);



[[nodiscard]] Dx12Texture load_texture_dds(ID3D12Device* device, ID3D12GraphicsCommandList* cmd_list,
                                             D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu,
                                             D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu,
                                             const std::filesystem::path& path);

// Dispatches to DDS (DXT) or WIC (BMP/PNG/JPG).
[[nodiscard]] Dx12Texture load_texture_file(ID3D12Device* device, ID3D12GraphicsCommandList* cmd_list,
                                            D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu,
                                            D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu,
                                            const std::filesystem::path& path);

// OMSI [texmax256]: cap longest edge (0 = off). Scenery only; call before map GPU upload.
void set_texture_max_edge(std::uint32_t px);
// Skip the N largest mips of BC .dds files that ship a full chain (BeamNG "texture quality"; saves VRAM).
void set_texture_mip_skip(std::uint32_t n);
[[nodiscard]] std::uint32_t texture_mip_skip();
// Virtual files: paths that start with "vfs:" (e.g. "vfs:/assets/materials/x.dds") are served by this provider
// (BeamNG zips) instead of the disk. exists may be null (then read is used).
using VirtualFileExists = bool (*)(const std::string& vpath);
using VirtualFileRead = bool (*)(const std::string& vpath, std::vector<std::uint8_t>& out);
void set_virtual_file_provider(VirtualFileExists exists, VirtualFileRead read);
[[nodiscard]] bool is_virtual_path(const std::filesystem::path& p);
[[nodiscard]] std::uint32_t texture_max_edge();

struct CpuRgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; // width*height*4 RGBA8
};

// CPU decode for UI thumbnails / GDI preview (BMP/PNG/JPG via WIC, DXT via software decode).
[[nodiscard]] std::optional<CpuRgbaImage> load_image_cpu_rgba(const std::filesystem::path& path);

// Decode TGA/WIC on worker threads so mesh upload only does GPU copy.
void prefetch_textures_cpu(const std::vector<std::filesystem::path>& paths);



} // namespace omsi::gfx::dx12

