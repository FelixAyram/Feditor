#pragma once

// OMSI sky / clouds / stars / weather / water passes. Shared between sky.cpp (sky, stars, texture
// binding, environment) and water.cpp (water tiles, precipitation). Internal to the o3d renderer.

#include "gfx/dx12/hello_o3d_renderer.hpp"

#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <vector>

namespace omsi::gfx::dx12 {

using Microsoft::WRL::ComPtr;

// One constant block per pass per frame-in-flight. Layout is mirrored by kSwCbHlsl below (all
// float4 after the matrix, so no packing surprises). 13 float4 + float4x4 = 272 bytes.
struct SwCbCpu {
    DirectX::XMFLOAT4X4 view_proj; // same convention as the existing sky (row-vector mul in HLSL)
    float cam[4];                  // xyz camera, w time seconds
    float zenith[4];               // rgb zenith (fallback gradient), w night weight
    float horizon[4];              // rgb horizon, w dusk weight
    float sun_dir[4];              // xyz toward sun (ENU, Z-up), w sun intensity
    float sun_color[4];            // rgb
    float day_w[4];                // x day, y dusk, z night, w sun altitude (deg)
    float cloud0[4];               // x cover, y base altitude (m), z texture size (m), w texture slot index
    float cloud_wind[4];           // xy direction clouds move to, z speed (m/s), w cloud brightness
    float star[4];                 // x LST (rad), y latitude (rad), z viewport width px, w viewport height px
    float star_cfg[4];             // x star radius px, y star brightness, z moon intensity, w horizon haze
    float fog[4];                  // rgb fog colour, w density (1/m)
    float moon_dir[4];             // xyz toward moon
    float extra[4];                // x cloud texture uses alpha, y precip intensity, z precip type, w unused
};
static_assert(sizeof(SwCbCpu) == 272, "SwCbCpu layout must match kSwCbHlsl");

inline constexpr std::uint32_t kSwCbStride = 8192;  // CBV alignment 256; 8 KB: the corona pass's list (96 x 48 B) follows the common part
inline constexpr std::uint32_t kSwPassSky = 0;
inline constexpr std::uint32_t kSwPassWater = 1;
inline constexpr std::uint32_t kSwPassPrecip = 2;
inline constexpr std::uint32_t kSwPassCorona = 3;
inline constexpr std::uint32_t kSwPassCount = 4;
inline constexpr std::uint32_t kSwCoronaMax = 96; // lit [light_enh] / [light_enh_2] glows a frame (3 float4 each)
// Shader-visible SRV table (contiguous, see sky.cpp): day, dusk, night, cumulus1, cumulus3, ovc1, ovc2,
// water base, water bump, water envmap.
inline constexpr std::uint32_t kSwTexCount = 10;
inline constexpr std::uint32_t kSwTexDay = 0, kSwTexDusk = 1, kSwTexNight = 2;
inline constexpr std::uint32_t kSwTexCloudFirst = 3; // cloud_type 1..4 -> 3..6
inline constexpr std::uint32_t kSwTexWaterBase = 7, kSwTexWaterBump = 8, kSwTexWaterEnv = 9;
inline constexpr std::uint32_t kSwPrecipMax = 3000;  // particles (quads) per frame

inline constexpr char kSwCbHlsl[] = R"(
cbuffer SwCB : register(b0) {
    float4x4 gViewProj;
    float4 gCam;        // xyz cam, w time
    float4 gZenith;     // rgb, w night
    float4 gHorizon;    // rgb, w dusk
    float4 gSunDir;     // xyz, w sun intensity
    float4 gSunColor;   // rgb
    float4 gDayW;       // x day, y dusk, z night, w sun alt deg
    float4 gCloud0;     // x cover, y base m, z tex m, w tex index
    float4 gCloudWind;  // xy dir to, z speed, w brightness
    float4 gStar;       // x lst, y lat, z vp w, w vp h
    float4 gStarCfg;    // x star radius px, y star brightness, z moon intensity, w haze
    float4 gFog;        // rgb, w density
    float4 gMoonDir;    // xyz
    float4 gExtra;      // x cloud alpha, y precip intensity, z precip type, w corona count
    float4 gCorona[288]; // corona pass: per glow pos+size, dir (w: 1 directional), rgb+intensity
};
Texture2D gTex[10] : register(t0);
SamplerState gSamp : register(s0);
float swHash(uint n) {
    n = (n ^ 61u) ^ (n >> 16u);
    n *= 9u;
    n = n ^ (n >> 4u);
    n *= 0x27d4eb2du;
    n = n ^ (n >> 15u);
    return float(n & 0x00ffffffu) / 16777216.0;
}
)";

// Per-renderer GPU state for the sky/weather/water passes.
struct SkyWeatherGpu {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> sky_pso;
    ComPtr<ID3D12PipelineState> stars_pso;
    ComPtr<ID3D12PipelineState> water_pso;
    ComPtr<ID3D12PipelineState> precip_pso;
    ComPtr<ID3D12PipelineState> corona_pso;
    bool pipelines_ready = false;

    ComPtr<ID3D12Resource> cb_ring; // kFrameCount * kSwPassCount * kSwCbStride
    std::uint8_t* cb_cpu = nullptr;

    // Sky dome (unit sphere, scaled in the shader).
    ComPtr<ID3D12Resource> dome_vb;
    ComPtr<ID3D12Resource> dome_ib;
    D3D12_VERTEX_BUFFER_VIEW dome_vbv{};
    D3D12_INDEX_BUFFER_VIEW dome_ibv{};
    std::uint32_t dome_index_count = 0;

    // Star catalogue (instanced quads: per-instance RA, Dec, brightness).
    ComPtr<ID3D12Resource> star_vb;
    D3D12_VERTEX_BUFFER_VIEW star_vbv{};
    std::uint32_t star_count = 0;

    // Water: rebuilt on set_water_quads; old buffers kept alive for in-flight frames.
    ComPtr<ID3D12Resource> water_vb;
    ComPtr<ID3D12Resource> water_ib;
    D3D12_VERTEX_BUFFER_VIEW water_vbv{};
    D3D12_INDEX_BUFFER_VIEW water_ibv{};
    std::uint32_t water_index_count = 0;
    std::deque<ComPtr<ID3D12Resource>> retired_water;

    // Reserved contiguous SRV block (kSwTexCount) in the main shader-visible heap.
    std::uint32_t tex_base_slot = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE tex_table{};
    bool textures_ready = false;
    std::uint32_t tex_attempts = 0;

    HelloO3dRenderer::SkyEnvironment env{};
    bool env_set = false;
    bool stars_ready = false;
};

} // namespace omsi::gfx::dx12
