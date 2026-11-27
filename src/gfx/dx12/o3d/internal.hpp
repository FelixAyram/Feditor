#pragma once

#include "gfx/dx12/hello_o3d_renderer.hpp"
#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/o3d/csm_cache.hpp"
#include "gfx/dx12/o3d/sky_weather.hpp"
#include "gfx/dx12/o3d/terrain_lod.hpp"

#include "omsi/model_cfg_types.hpp"
#include "omsi/model_cfg.hpp"
#include "omsi/o3d_format.hpp"

#include <DirectXMath.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omsi::gfx::dx12 {

struct MaterialDrawBatch {
    std::uint32_t index_start = 0;
    std::uint32_t index_count = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{};
    bool has_texture = false;
    omsi::model_cfg::MatlAlphaMode alpha_mode = omsi::model_cfg::MatlAlphaMode::opaque;
    bool no_zwrite = false;
    bool no_zcheck = false;
    bool is_shadow = false;
    bool has_envmap = false;
    float envmap_strength = 0.f;
    float uv_off_x = 0.f;
    float uv_off_y = 0.f;
    std::string texcoord_trans_x_var;
    std::string texcoord_trans_y_var;
    // [alphascale] var: alpha multiplier driven by a script variable (rain on the windscreen, fades).
    std::string alpha_scale_var;
    float alpha_scale = 1.f;
    bool is_text_texture = false;
    int text_texture_index = -1;
    // its text SRV slot is its own (a loaded mesh's unique slot); a clone's copy shares the prototype's until its
    // first text is drawn, then takes one of its own - pooled: back to text_slot_free when the clone goes
    bool text_slot_own = true;
    bool text_slot_pooled = false;
    bool is_freetex = false;
    std::string freetex_key;
    std::string original_albedo; // O3D embedded filename — restore after [matl_change]
    D3D12_GPU_DESCRIPTOR_HANDLE base_srv_gpu{}; // own table while a [matl_change] variant shows
    bool variant_on = false;
    bool is_on_surface = false;
    bool is_presurface = false;
    std::uint32_t srv_slot = ~0u;
    // BeamNG material: 6 contiguous SRVs (albedo, env/1x1 params, normal, detail, detail normal, opacityMap)
    // + shader params
    bool pbr = false;
    float pbr_flags = 0.f;
    float pbr_detail_scale = 0.f;
    bool pbr_opacity_map = false; // opacityMap bound at t7: mask alpha (min of alpha/luminance) x colour alpha
    bool pbr_tint_rgb = false;    // t0 is a colorMap: multiply rgb by baseColorFactor (t1)
    float alpha_ref = 0.5f;       // BeamNG alphaRef / 255 → PS discard threshold (both pixel shaders)
    bool double_sided = false;
    bool has_nightmap = false;    // matl_nightmap loaded at srv_slot+2 (= t4 in OMSI shaders)    // draw with the CULL_NONE PSO (doubleSided materials)
    bool has_transmap = false;    // [matl_transmap] loaded at srv_slot+3 (= t5): its alpha is the transparency
    bool night_always = false;    // night slot holds a [matl_item] night map: added whatever the daylight
    bool variant_night = false;   // a [matl_item] night / light map variant is set (flags below are the own ones)
    bool base_has_nightmap = false;
    bool base_lightmap_mode = false;
    float base_lightmap_scale = -1.f;
    float pbr_detail_distance = 0.f; // BeamNG detailDistances far edge in metres (0 = 80 m legacy)
    // o3d material emissive (D3D fixed function: tex * saturate(emissive + lighting) - traffic light
    // lenses, displays), 8 bits per channel packed r | g<<8 | b<<16; rides in gDetailDistance on OMSI draws
    float emissive_packed = 0.f;
    bool env_alpha_mask = false; // [matl_transmap] / [matl_envmap_mask]: the reflection by the texture alpha
    bool lightmap_mode = false; // night slot holds a [matl_lightmap] (ADDSMOOTH x albedo), not a night map
    std::string lightmap_var;   // its variable (vehicles): lit as much as the variable says
    float lightmap_scale = -1.f; // the variable's value now; < 0 = no variable (lit in the dark)
};

// [PERF] Vertex + index data of the meshes sub-allocated from big DEFAULT-heap buffers (64 MB arenas) instead
// of one committed resource per mesh: with 4 tiles streamed there were ~84k committed buffers, and the driver
// (AMD) stalled inside the draw calls tracking them (MAIN_STACK render_cpu_dx12 in amdxc64, ~38 ms). AMD's
// RDNA guide and NVIDIA's CPU guide: sub-allocate from large heaps. A range is reused only kFrameCount + 2
// frames after its last user let go (the GPU may still read it); clones share the allocation (shared_ptr).
struct VbArenaPool {
    struct Arena {
        Microsoft::WRL::ComPtr<ID3D12Resource> buf;
        UINT64 cap = 0;
        std::map<UINT64, UINT64> free_ranges; // offset -> size (coalesced)
        UINT64 max_free = 0;                  // an upper bound of the largest free range
    };
    struct Pending {
        std::uint32_t arena = 0;
        UINT64 off = 0, size = 0;
        std::uint64_t frame = 0;
    };
    std::mutex mu;
    std::vector<Arena> arenas;
    std::vector<Pending> pending;
    std::uint64_t frame_now = 0; // set each frame by the renderer
    std::uint64_t bytes_used = 0;

    static void insert_free(Arena& a, UINT64 off, UINT64 size) {
        auto next = a.free_ranges.lower_bound(off);
        if (next != a.free_ranges.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == off) {
                off = prev->first;
                size += prev->second;
                a.free_ranges.erase(prev);
            }
        }
        if (next != a.free_ranges.end() && off + size == next->first) {
            size += next->second;
            a.free_ranges.erase(next);
        }
        a.free_ranges[off] = size;
        if (size > a.max_free) a.max_free = size;
    }
    void reclaim_locked(std::uint64_t frames_in_flight) {
        std::size_t w = 0;
        for (std::size_t i = 0; i < pending.size(); ++i) {
            const auto& pd = pending[i];
            if (pd.frame + frames_in_flight + 2 <= frame_now && pd.arena < arenas.size()) {
                insert_free(arenas[pd.arena], pd.off, pd.size);
                bytes_used -= pd.size;
            } else {
                pending[w++] = pd;
            }
        }
        pending.resize(w);
    }
};

struct VbArenaAlloc {
    std::shared_ptr<VbArenaPool> pool;
    std::uint32_t arena = 0;
    UINT64 off = 0, size = 0;
    ~VbArenaAlloc() {
        if (!pool) return;
        std::lock_guard<std::mutex> lk(pool->mu);
        pool->pending.push_back({arena, off, size, pool->frame_now});
    }
};

struct GpuMeshChunk {
    mutable std::int8_t content_mesh = -1; // cached: mesh_file is a .o3d / .x (back faces culled)
    std::shared_ptr<VbArenaAlloc> vb_alloc; // (set when the VB/IB live in a VbArenaPool arena)
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> index_buffer;
    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view{};
    D3D12_INDEX_BUFFER_VIEW index_buffer_view{};
    std::vector<MaterialDrawBatch> shadow_batches;
    std::vector<MaterialDrawBatch> opaque_batches;
    std::vector<MaterialDrawBatch> alpha_batches;       // alpha with depth test, may write depth (mode 1)
    std::vector<MaterialDrawBatch> alpha_nozwrite_batches; // matl_alpha=2 glass (OMSI: ZWRITE off)
    LoadedMeshChunkInfo meta{};
    std::size_t cfg_mesh_index = 0;
    DirectX::XMFLOAT4X4 local_world_transposed{};
    // World-space AABB cache: recomputing 8 transformed corners for every chunk every frame was
    // the dominant CPU cost of culling on large maps. Cache keyed by the world matrix itself so
    // it is always correct (no invalidation bugs) and only recomputed when the matrix changes.
    mutable DirectX::XMFLOAT4X4 aabb_cache_matrix{};
    mutable float aabb_cache_min[3]{0.f, 0.f, 0.f};
    mutable float aabb_cache_max[3]{0.f, 0.f, 0.f};
    mutable bool aabb_cache_valid = false;
    // CSM caster bounding sphere, keyed by world matrix + local bbox (recomputed only when either changes).
    mutable DirectX::XMFLOAT4X4 csm_sphere_matrix{};
    mutable float csm_sphere_bbox[6]{0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    mutable float csm_sphere_c[3]{0.f, 0.f, 0.f};
    mutable float csm_sphere_r = 0.f;
    mutable bool csm_sphere_valid = false;
    float highlight = 0.f; // 0..1 tint strength
    float highlight_r = 1.f;
    float highlight_g = 0.95f;
    float highlight_b = 0.55f;
    float opacity = 1.f; // 0..1 mesh opacity (editor ghost slider)
    bool screen_space = false; // verts already in NDC; skip world*VP
    bool invert_color = false; // editor spline arrows: REV_SUBTRACT pass at frame end
    bool wireframe = false;    // editor spline preview: full mesh, WIREFRAME fill, no texture
    bool wind_sway = false;    // vegetation: VS sway + tilted lighting (tree billboards)
    bool interior = false;     // [illumination_interior] names a lamp: lit by its vehicle's [interiorlight]s
    bool vehicle_self = false; // a mesh of the player's bus: its own headlight pools leave it dark
    // [smoothskin]: the vertices as uploaded (their uvs) - update_chunk_skin writes the bones' pose over them
    std::shared_ptr<std::vector<omsi::o3d::Vertex>> skin_base;
    struct SkinRing {
        ComPtr<ID3D12Resource> buf[4];
        std::uint8_t* map[4]{};
        std::uint32_t next = 0;
        int mode = -1; // -1 unknown, 0 same frame as the file's vertices, 1 y/z swapped
        std::vector<float> last;
    };
    std::shared_ptr<SkinRing> skin;
    std::uint8_t editor_helper = 0; // helper kinds (1 [onlyeditor] / entrypoint, 2 carpark box): drawn only while
                                    // every kind it has is shown (editor_helper_mask)
    bool want_visible = true;   // what set_chunk_visible asked for (the helper switch masks it)
    bool split_blend = false;  // vehicle: [matl_alpha] 2 drawn as a solid core with depth + its glass blended
    bool path_rail = false;    // editor path rail: drawn after the scene, pulled over the ground (never sinks)
    bool fixed_lit = false;    // path rails / UI: ignore sun/season lighting (constant day shade)
    bool is_presurface = false; // [rendertype] presurface — TMap pass 0, before terrain+splines
    WorldRenderPhase render_phase = WorldRenderPhase::Normal;
    bool surface_bias = false;
    bool decal = false;
    bool has_uv_vars = false; // cached: any batch uses script-driven UV offset variables
    // CPU copy for editor/mouseevent mesh picks (AABB alone is too loose for long splines).
    std::vector<DirectX::XMFLOAT3> pick_positions;
    std::vector<std::uint32_t> pick_indices;
    // SIZE_MAX = this chunk owns pick verts. Else index of proto whose tris we share.
    std::size_t pick_src = static_cast<std::size_t>(-1);
    std::filesystem::path bound_tex;
    std::uintmax_t bound_tex_size = 0;
    std::filesystem::file_time_type bound_tex_mtime{};

    // --- Terrain tile LOD (see terrain_lod.hpp): index-only LODs over the SAME vertex buffer ---------
    static constexpr int kLodLevels = 4;
    struct LodIndexBuffer {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        D3D12_INDEX_BUFFER_VIEW view{};
        std::uint32_t index_count = 0;
    };
    // BeamNG TSStatic / forest / ground-cover chunks: never hidden by the projected-size cull in draw.cpp.
    // Their app-side streaming radius is the only limit (a rock 200 m away must not blink out).
    bool no_size_cull = false;
    // x the projected-size threshold for this chunk (AI / parked cars: their small parts - lamps, interior,
    // badges - stop being drawn far sooner than scenery; set_chunk_size_cull_scale, kept by clones)
    float size_cull_scale = 1.f;
    // Terrain tile (set_chunk_terrain): drawn in the terrain pass, BEFORE surfaces, with
    // pipeline_state_terrain_bias (openOMSI: no push, no bias - formerly pushed away by 2 cm + 0.15 % of the
    // distance). Without this the tile went to the surface pass with the plain PSO, so its
    // stamped triangles (flushed to the road/object height) covered splines and SCOs up close.
    // Kept as its own flag because meta.mesh_file is rewritten by every mesh upload.
    bool is_terrain = false;
    bool lod_capable = false;       // heavy single-batch tile mesh (decided at upload)
    bool lod_listed = false;        // already in Impl::lod_chunks
    // Terrain tile detail (cut) mesh. The chunk's own VB/IB is the cheap uncut 61x61 grid; `detail_*` holds the
    // polycut mesh built only for tiles near the camera. While the detail is drawn, the base mesh becomes an
    // "underlay" pushed back in depth so a gap in the cut shows terrain instead of the clear colour.
    Microsoft::WRL::ComPtr<ID3D12Resource> detail_vb;
    Microsoft::WRL::ComPtr<ID3D12Resource> detail_ib;
    D3D12_VERTEX_BUFFER_VIEW detail_vbv{};
    D3D12_INDEX_BUFFER_VIEW detail_ibv{};
    std::vector<MaterialDrawBatch> detail_batches;
    bool detail_ready = false;   // detail_* valid
    bool detail_active = false;  // camera ring within detail_ring_max this frame
    int detail_ring_max = -1;    // < 0: tile has no detail mesh
    bool lod_job_submitted = false; // worker building the levels for lod_gen
    std::uint32_t lod_gen = 0;      // bumped when the geometry is replaced (drops stale jobs / buffers)
    std::uint8_t lod_level = 0;     // level used this frame (0 = full quality)
    LodIndexBuffer lod_ib[kLodLevels]; // [k] = level k+1
};

namespace {

using Microsoft::WRL::ComPtr;
using DirectX::XMFLOAT4X4;
using omsi::o3d::MappedO3D;
using omsi::o3d::Vertex;

using omsi::model_cfg::MatlAlphaMode;
using omsi::model_cfg::ModelCfgDocument;
using omsi::model_cfg::ModelCfgMeshEntry;
using omsi::model_cfg::resolve_cfg_material_by_o3d_texture;
using omsi::model_cfg::apply_omsi_material_state;

struct SceneConstantBuffer {
    XMFLOAT4X4 world_view_proj;
};

struct AxisConstantBuffer {
    XMFLOAT4X4 view_proj;
};

constexpr std::uint32_t kConstantBufferSize = (sizeof(SceneConstantBuffer) + 255u) & ~255u;
// Per-draw WVP slots: one shared mapped CB was overwritten before GPU execute (all draws saw last matrix).
// Per-frame scene CB budget. 131072 total / kFrameCount(3) = 43690 slots/frame.
// Was 65536 (~21845/frame) which overflowed on very dense maps (>21k chunk draws).
constexpr std::uint32_t kSceneCbSlotCount = 131072;
constexpr std::uint32_t kSceneCbRingBytes = kConstantBufferSize * kSceneCbSlotCount;
constexpr std::uint32_t kAxisConstantBufferSize = (sizeof(AxisConstantBuffer) + 255u) & ~255u;
// Shared shader-visible SRV heap. Was 2048 → white after ~1k mat instances (2 slots each).
// Reuse by path below; 64k pairs ≈ room for large maps (~8 GB texture RAM is the real cap).
constexpr std::uint32_t kSrvDescriptorCount = 65536;
constexpr std::uint32_t kCbvDescriptorCount = 2;
constexpr float kAxisGizmoScreenFraction = 0.20f;
constexpr float kAxisGizmoMarginFraction = 0.015f;
constexpr std::uint32_t kHeapDescriptorCount = kCbvDescriptorCount + kSrvDescriptorCount;

// O3D native vertex (32 B): position + normal + uv — UV at byte 24.
static_assert(sizeof(omsi::o3d::Vertex) == 32);

struct AxisVertex {
    float pos[3];
    float color[4];
};

// Expanded triangle list uses GpuVertex (position + uv).

float axis_gizmo_viewport_size(std::uint32_t width, std::uint32_t height) {
    return (std::min)(static_cast<float>(width), static_cast<float>(height)) * kAxisGizmoScreenFraction;
}

float axis_gizmo_margin(std::uint32_t width, std::uint32_t height) {
    return (std::min)(static_cast<float>(width), static_cast<float>(height)) * kAxisGizmoMarginFraction;
}

void scene_viewport_rect(std::uint32_t width, std::uint32_t height, D3D12_VIEWPORT& vp, D3D12_RECT& scissor) {
    vp.TopLeftX = 0.f;
    vp.TopLeftY = 0.f;
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MinDepth = 0.f;
    vp.MaxDepth = 1.f;
    scissor.left = 0;
    scissor.top = 0;
    scissor.right = static_cast<LONG>(width);
    scissor.bottom = static_cast<LONG>(height);
}

struct AxisStroke {
    float u0, v0, u1, v1;
};

enum class AxisLetterPlane { YZ, XZ, XY };

void axis_map_letter_point(AxisLetterPlane plane, float ox, float oy, float oz, float u, float v, float scale,
                           float& x, float& y, float& z) {
    u *= scale;
    v *= scale;
    switch (plane) {
    case AxisLetterPlane::YZ:
        x = ox;
        y = oy + u;
        z = oz + v;
        break;
    case AxisLetterPlane::XZ:
        x = ox + u;
        y = oy;
        z = oz + v;
        break;
    case AxisLetterPlane::XY:
        x = ox + u;
        y = oy + v;
        z = oz;
        break;
    }
}

void axis_add_segment(std::vector<AxisVertex>& out, float ax, float ay, float az, float bx, float by, float bz,
                      float r, float g, float b) {
    out.push_back(AxisVertex{{ax, ay, az}, {r, g, b, 1.f}});
    out.push_back(AxisVertex{{bx, by, bz}, {r, g, b, 1.f}});
}

void axis_add_letter(std::vector<AxisVertex>& out, std::initializer_list<AxisStroke> strokes, float ox, float oy,
                     float oz, AxisLetterPlane plane, float scale, float r, float g, float b) {
    for (const AxisStroke& stroke : strokes) {
        float ax = 0.f, ay = 0.f, az = 0.f, bx = 0.f, by = 0.f, bz = 0.f;
        axis_map_letter_point(plane, ox, oy, oz, stroke.u0, stroke.v0, scale, ax, ay, az);
        axis_map_letter_point(plane, ox, oy, oz, stroke.u1, stroke.v1, scale, bx, by, bz);
        axis_add_segment(out, ax, ay, az, bx, by, bz, r, g, b);
    }
}

// Compiled shaders are kept in <exe dir>/shader_cache, keyed by a hash of source + entry + profile + flags:
// D3DCompile of the scene shaders took 3-4 s on the main thread at every start.
inline std::filesystem::path shader_cache_dir() {
    static const std::filesystem::path dir = [] {
        wchar_t buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::filesystem::path p = std::filesystem::path(buf).parent_path() / L"shader_cache";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p;
    }();
    return dir;
}

ComPtr<ID3DBlob> compile_shader(const char* source, const char* entry, const char* profile) {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const std::size_t src_len = std::strlen(source);
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    mix(source, src_len);
    mix(entry, std::strlen(entry) + 1);
    mix(profile, std::strlen(profile) + 1);
    mix(&flags, sizeof(flags));
    char name[48];
    std::snprintf(name, sizeof(name), "%016llx.cso", static_cast<unsigned long long>(h));
    const std::filesystem::path path = shader_cache_dir() / name;
    {
        std::FILE* f = _wfopen(path.c_str(), L"rb");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n >= 64 && SUCCEEDED(D3DCreateBlob(static_cast<SIZE_T>(n), blob.GetAddressOf())) &&
                std::fread(blob->GetBufferPointer(), 1, static_cast<std::size_t>(n), f) == static_cast<std::size_t>(n) &&
                std::memcmp(blob->GetBufferPointer(), "DXBC", 4) == 0) {
                std::fclose(f);
                return blob;
            }
            std::fclose(f);
            blob.Reset();
        }
    }
    const HRESULT hr = D3DCompile(source, src_len, nullptr, nullptr, nullptr, entry, profile, flags, 0,
                                  blob.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        if (errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        throw_hr(hr, "D3DCompile failed");
    }
    {
        std::filesystem::path tmp = path;
        tmp += L".tmp";
        if (std::FILE* f = _wfopen(tmp.c_str(), L"wb")) {
            const bool ok = std::fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), f) == blob->GetBufferSize();
            std::fclose(f);
            std::error_code ec;
            if (ok) std::filesystem::rename(tmp, path, ec);
            else std::filesystem::remove(tmp, ec);
        }
    }
    return blob;
}

const char* kShaderSource = R"(
cbuffer SceneCB : register(b0) {
    float4x4 gWorldViewProj;
};
cbuffer EnvCB : register(b2) {
    float gEnvStrength;
    float gMaterialMode; // 0=normal, 1=shadow decal, 2=invert-color overlay (white PS)
    float gHighlightAmount;
    float gHighlightR;
    float gHighlightG;
    float gHighlightB;
    float gUvOffX;
    float gUvOffY;
    float gAlphaMode; // 0=none, 1=1-bit punch, 2=blend
    float gPointSample; // 1 = [useTextTexture] only (point, mip 0). Alpha-test uses aniso mips.
    float gOpacity; // 0..1 per-chunk opacity (editor)
    // OMSI-style dynamic lights (editor realtime approx of light_enh + maplight)
    float gAmbientScale; // 1=day, ~0.25 night
    float gLightCount;   // 0..4
    // BeamNG material params (root-constant indices 13..15; unused by the OMSI modes).
    float gAlphaRef;        // alphaRef / 255 -> alpha-test discard threshold (default 0.5)
    float gOpacityMapOn;    // 1 = opacityMap bound at t7 (mask in alpha and/or luminance)
    float gDetailDistance;  // BeamNG detailDistances far edge in metres (0 = 80 m legacy)
    float4 gLightPosRange[4];  // xyz world, w = range (full until range, then 1/d^2)
    float4 gLightColorInt[4];  // rgb, w = intensity
    float3 gSunDir;      // light direction toward scene (OMSI Z-up)
    float gSunShadowEnable;
    float3 gSunColor;
    float gSunPad;
};
Texture2D gAlbedo : register(t0);
Texture2D gEnvmap : register(t1);
// BeamNG materials (gMaterialMode 5): normal / detail / detail-normal maps (second range of the same table).
Texture2D gNormalMap : register(t4);
Texture2D gDetailMap : register(t5);
Texture2D gDetailNormal : register(t6);
Texture2D gOpacityMap : register(t7); // BeamNG opacityMap (mask in alpha and/or luminance)
Texture2DArray gShadowMap : register(t3);
SamplerState gSampler : register(s0);
SamplerState gSamplerPoint : register(s1);
SamplerState gSamplerClamp : register(s2);
SamplerComparisonState gShadowSampler : register(s3);
cbuffer ShadowCB : register(b3) {
    float4x4 gLightVP[3];
    float4 gCascadeSplits; // x,y,z far distances; w = texel size near
    float4 gCamPos;        // xyz camera world, w = shadow enable
    float4 gShadowBias;    // x=const bias, y=normal offset, z=pcf texel, w unused
    float4 gCamForward;    // xyz unit look dir (camera → target), w = fog scale (0 = no fog)
    float4 gEnvExtra;      // x = night blend, y/z = wind m/s world XY, w = time s (written per frame)
    float4 gXLightInfo;    // x = scenery light count, yz = light grid origin (world XY), w = grid cell (m)
    float4 gXLight[1024];  // 512 lights: (xyz, core m), (rgb x intensity, cut-off m)
    uint4 gXCell[576];     // 48 x 48 world grid cells, 4 per entry: (index offset << 10) | light count
    uint4 gXIdx[2478];     // the cells' light indices, 16 bit, 8 per entry (ends at 64 KB)
};
#define gNightBlend gEnvExtra.x
#define gWindX gEnvExtra.y
#define gWindY gEnvExtra.z
#define gTime gEnvExtra.w
struct VSIn {
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};
struct VSOut {
    float4 pos : SV_POSITION;
    float3 normal : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
    float viewZ : TEXCOORD3;
};
struct InstanceData {
    float4x4 wvp;
    float4x4 world;
};
StructuredBuffer<InstanceData> gInstance : register(t2);
// Interleaved gradient noise (Jimenez) — breaks fog / tonemap banding at distance.
float IgnNoise(float2 p) {
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}
// Compact AgX-ish tonemap (split from lighting): soft shoulder, no ACES wash.
float3 TonemapAgX(float3 c) {
    c = max(c, 0.0);
    float3 x = c / (1.0 + c * 0.85);
    x = saturate(x);
    return x * x * (3.0 - 2.0 * x);
}
float SampleShadowCascade(float3 worldPos, float3 N, int ci) {
    // normal offset grows with the cascade's texel (the far ones cover 10x the ground per texel)
    // slope-scaled normal offset: only as much as the surface turns from the sun (1 - N.L) - a flat
    // pavement under a high sun needs none, and a fixed lift moved the pole's shadow off its foot
    // Normal-offset shadows (Holbert): the offset is one texel of THIS cascade (its world size read off the
    // light matrix: x scale = 1/radius), times how far the surface turns from the sun - a flat pavement
    // under a high sun needs almost none. A fixed factor per cascade pushed far shadows a hand off the feet.
    const float ndl_s = saturate(dot(normalize(N), normalize(gSunDir)));
    const float3 xs = float3(gLightVP[ci][0][0], gLightVP[ci][1][0], gLightVP[ci][2][0]);
    const float3 zs = float3(gLightVP[ci][0][2], gLightVP[ci][1][2], gLightVP[ci][2][2]);
    const float texel_w = 2.0 * gShadowBias.z / max(length(xs), 1e-6);
    float3 offsetPos = worldPos + N * texel_w * (0.25 + 1.25 * (1.0 - ndl_s));
    float4 lp = mul(float4(offsetPos, 1.0), gLightVP[ci]);
    float3 ndc = lp.xyz / max(lp.w, 1e-5);
    float2 uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z < 0.0 || ndc.z > 1.0) return 1.0;
    // depth bias in metres along the light (z scale = 1 / depth range): 1.5 cm in every cascade
    float depth = ndc.z - 0.015 * length(zs);
    float dx = gShadowBias.z;
    float sh = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            sh += gShadowMap.SampleCmpLevelZero(gShadowSampler, float3(uv + float2(x, y) * dx, (float)ci), depth);
    return sh / 9.0;
}
float SampleShadowCSM(float3 worldPos, float3 N, float viewZ) {
    if (gCamPos.w < 0.5) return 1.0;
    float farEdge = gCascadeSplits.z;
    if (viewZ > farEdge) return 1.0;
    int ci = 2;
    if (viewZ < gCascadeSplits.x) ci = 0;
    else if (viewZ < gCascadeSplits.y) ci = 1;
    float sh = SampleShadowCascade(worldPos, N, ci);
    // Soft blend into next cascade to hide seam swimming (texel snap already on matrices).
    if (ci < 2) {
        float edge = (ci == 0) ? gCascadeSplits.x : gCascadeSplits.y;
        float band = max(edge * 0.12, 1.5);
        float t = saturate((viewZ - (edge - band)) / band);
        if (t > 0.0) sh = lerp(sh, SampleShadowCascade(worldPos, N, ci + 1), t);
    }
    // Soft fade last ~15% of far cascade so shadows do not hard-cut at visual radius.
    float fadeStart = farEdge * 0.85;
    if (viewZ > fadeStart) {
        float fade = saturate((viewZ - fadeStart) / max(farEdge - fadeStart, 1e-3));
        sh = lerp(sh, 1.0, fade);
    }
    return sh;
}
// World direction -> the instance's local frame (row-vector world: local = world x inverse(M)).
float3 WorldToLocalDir(float3 wd, float4x4 W) {
    const float3 r0 = W[0].xyz, r1 = W[1].xyz, r2 = W[2].xyz;
    const float3 c0 = cross(r1, r2), c1 = cross(r2, r0), c2 = cross(r0, r1);
    const float det = dot(r0, c0);
    if (abs(det) < 1e-12) return wd;
    return float3(dot(wd, c0), dot(wd, c1), dot(wd, c2)) / det;
}
// dz: a FIXED world-vertical offset of where the vertex is drawn (the ground layers' order: terrain under
// roads under the things lying on them). It does not depend on the camera at all, unlike the pull towards
// the eye it replaces; lighting and shadows keep the true position (worldPos).
VSOut vs_core(VSIn vin, uint iid, float dz) {
    VSOut o;
    InstanceData inst = gInstance[iid];
    float3 lp = vin.pos;
    // Vegetation wind sway (gOpacityMapOn bit 8 = tree billboard). mul(v,M) convention: row 0 / row 2 of
    // inst.world are the local X / Z axis images, so their lengths are width / height in metres.
    if ((((int)(gOpacityMapOn + 0.5)) & 8) != 0 && (gWindX * gWindX + gWindY * gWindY) > 1e-6) {
        const float wspd = sqrt(gWindX * gWindX + gWindY * gWindY);
        const float wid_m = max(length(inst.world[0].xyz), 0.5);
        const float h = saturate(lp.z);                  // 0 at root, 1 at crown (unit billboard)
        const float ph = inst.world[3].x * 0.031 + inst.world[3].y * 0.017 + lp.x * 0.7 + lp.y * 0.4;
        const float gust = 0.65 + 0.35 * sin(gTime * 0.23 + inst.world[3].x * 0.011);
        const float sway = sin(gTime * 1.7 + ph) + 0.4 * sin(gTime * 3.9 + ph * 1.7);
        const float disp_m = 0.10 * wspd * gust * sway * h * h; // crown offset in m (~0.3-0.5 m in a gale)
        lp.xy += (float2(gWindX, gWindY) / wspd) * (disp_m / wid_m);
    }
    const float3 lp_draw = dz != 0.0 ? lp + WorldToLocalDir(float3(0.0, 0.0, dz), inst.world) : lp;
    o.pos = mul(float4(lp_draw, 1.0f), inst.wvp);
    o.worldPos = mul(float4(lp, 1.0f), inst.world).xyz;
    o.normal = normalize(mul(vin.normal, (float3x3)inst.world));
    o.uv = vin.uv + float2(gUvOffX, gUvOffY);
    o.viewZ = max(0.0, dot(o.worldPos - gCamPos.xyz, gCamForward.xyz));
    return o;
}
VSOut vs_main(VSIn vin, uint iid : SV_InstanceID) { return vs_core(vin, iid, 0.0); }
)"
R"(
// openOMSI scene_clip: road surfaces are pulled towards the eye along the line of sight (depth
// only, the picture does not move) by a share of the distance plus a fixed amount, so they win
// over the flush terrain beside/under them at every distance. The raster DepthBias alone is
// sub-millimetre near the eye with reverse-Z D32, and the terrain won up close.
// Reverse-Z with a far plane >= 10 km: depth ~ k / viewZ, so moving the vertex to t * viewZ is
// clip.z / t (clip.w = viewZ, x/y unchanged).
// Grazing view: a step along the line of sight is worth only its cosine across the surface - looking
// along the street from just above it, the pull that keeps the road 1 cm over the ground at a steep look
// is a millimetre, and terrain, roads and objects a centimetre apart fought, worse the lower the eye.
// The pull is stretched by 1 / cos (up to 5x), so it is the same height over the surface at every angle.
float GrazeScale(float3 worldPos, float3 N) {
    const float3 vd = normalize(worldPos - gCamPos.xyz);
    const float c = abs(dot(vd, normalize(N)));
    return 1.0 / max(c, 0.2);
}
float4 SurfacePull(float4 clip, float3 N, float share, float fixedm, float graze) {
    const float w = clip.w;
    if (w <= 1e-3) return clip;
    const bool flat = abs(N.z) > 0.5;
    const float pull = min((min(share * w, flat ? 0.5 : 0.05) + fixedm) * graze, w * 0.3);
    const float t = 1.0 - pull / w;
    clip.z = min(clip.z / t, w); // never in front of the near plane
    return clip;
}
// Distance-relative pull (no cap but the near plane): ground layers keep the same order at every
// distance (terrain < splines < surface objects), and far roads still beat the coarser terrain LOD,
// which can rise above the real ground on slopes (a fixed cap let it cover roads far away).
float4 RelPull(float4 clip, float share, float fixedm) {
    const float w = clip.w;
    if (w <= 1e-3) return clip;
    const float pull = min(share * w + fixedm, w * 0.3);
    clip.z = min(clip.z / (1.0 - pull / w), w);
    return clip;
}
// Ground layers, FIXED world-vertical draw offsets (camera-independent; see vs_core):
//   terrain base under a cut tile -40 cm < terrain -2 mm < roads 0 < [surface] objects / decals +2 mm
//   < editor path rails +6 cm. Millimetres only break exact ties (reverse-Z D32 resolves them at any
//   distance): wherever one surface is really higher, even by a centimetre, it wins - the true geometry. Only where things are drawn moves - heights, picking and shadows keep the
//   true positions - so nothing changes with the viewing angle or distance.
VSOut vs_surface(VSIn vin, uint iid : SV_InstanceID) { return vs_core(vin, iid, 0.002); }
VSOut vs_terrain_under(VSIn vin, uint iid : SV_InstanceID) { return vs_core(vin, iid, -0.40); }
VSOut vs_terrain(VSIn vin, uint iid : SV_InstanceID) { return vs_core(vin, iid, -0.002); }
VSOut vs_spline(VSIn vin, uint iid : SV_InstanceID) { return vs_core(vin, iid, 0.0); }
VSOut vs_rail(VSIn vin, uint iid : SV_InstanceID) { return vs_core(vin, iid, 0.06); }
// OMSI vehicle ground shadow ([isshadow] shadow.o3d, Shadow_car.tga): a flat quad at the wheels' contact
// height, which is the road's own height - drawn 5 cm up so it never fights the road (Omsi.exe draws it with
// noZcheck; the depth test stays on here so walls and kerbs in front still cover it).
VSOut vs_vehshadow(VSIn vin, uint iid : SV_InstanceID) { return vs_core(vin, iid, 0.05); }
// openOMSI tex_address: pass UVs through; hardware WRAP (s0) / CLAMP (s2) does addressing.
float4 ps_main(VSOut pin) : SV_TARGET {
    if (gMaterialMode > 3.5 && gMaterialMode < 4.5) {
        // Path rails fixed_lit:
        // amount < ~1 → MULTIPLY tint (texture pattern stays, color washes over it).
        // amount ≥ ~1 → solid lerp (modern solid-colored lines / full hover).
        float4 a = gAlbedo.Sample(gSampler, pin.uv);
        float3 col = a.rgb * 0.85;
        if (gHighlightAmount > 0.001) {
            const float3 tint = float3(gHighlightR, gHighlightG, gHighlightB);
            const float amt = saturate(gHighlightAmount);
            if (amt >= 0.99)
                col = lerp(col, tint, amt);
            else
                col *= lerp(float3(1.0, 1.0, 1.0), tint, amt);
        }
        return float4(col, saturate(gOpacity));
    }
    const bool cutout = (gAlphaMode > 0.5 && gAlphaMode < 1.5);
    float2 uv = pin.uv;
    const bool uv_unit = all(uv >= 0.0) && all(uv <= 1.0);
    float4 albedo = (gPointSample > 0.5 && gMaterialMode < 4.5) ? gAlbedo.Sample(gSamplerPoint, uv)
                  : (cutout && uv_unit) ? gAlbedo.Sample(gSamplerClamp, uv)
                                        : gAlbedo.Sample(gSampler, uv);
    // [matl_transmap] (Omsi.exe material setter 0x7fd9b0 sets gMatlTransMapOn; OMSI SDK: the transmap's alpha
    // is the transparency, the texture's own alpha stays the reflection mask). Bound at t5 (slot+3), bit 15.
    float reflMask = albedo.a;
    if (gMaterialMode < 4.5 && ((((int)(gOpacityMapOn + 0.5)) & 32768) != 0))
        albedo.a = gDetailMap.Sample(gSampler, uv).a;
    // BeamNG material params: t1 = 1x1 solid, rgb = baseColorFactor, a = opacityFactor (always bound in
    // mode 5). gOpacityMapOn bit 0 = opacityMap bound at t7, bit 1 = t0 is a colorMap (baseColorFactor tint).
    if (gMaterialMode > 4.5) {
        const int mflags = (int)(gOpacityMapOn + 0.5);
        const float4 mt = gEnvmap.Sample(gSamplerPoint, float2(0.5, 0.5));
        if ((mflags & 2) != 0) albedo.rgb *= mt.rgb; // colorMap x baseColorFactor
        if ((mflags & 1) != 0) {
            // Opacity mask: alpha = min(opacityMap alpha, luminance) x colorMap alpha.
            const float4 om = gOpacityMap.Sample(gSampler, uv);
            const float chroma = max(max(om.r, om.g), om.b);
            float mask = min(saturate(om.a), saturate(chroma));
            // Masks without a usable alpha channel (BC4 = R only, a = 1; or mask authored in rgb).
            if (om.a > 0.999) mask = saturate(chroma);
            else if (chroma < 0.004) mask = saturate(om.a); // mask in alpha over a black rgb
            albedo.a *= mask;
        } else if ((int(gPointSample + 0.5) & 8) != 0) {
            // No colorMap: t0 carries the mask, the solid colour comes from baseColorFactor.
            const float chroma = max(max(albedo.r, albedo.g), albedo.b);
            float mask = min(saturate(albedo.a), saturate(chroma));
            if (albedo.a > 0.999) mask = saturate(chroma);
            else if (chroma < 0.004) mask = saturate(albedo.a);
            albedo = float4(mt.rgb, mask);
        }
        albedo.a *= saturate(mt.a); // opacityFactor
    }
    if (cutout && albedo.a < gAlphaRef) discard;
    // alpha mode 3 = opaque core of a [matl_alpha] 2 material: keep only texels that are (nearly) opaque,
    // they write depth here so the translucent remainder cannot show back faces through solid parts.
    if (gAlphaMode > 2.5 && albedo.a < 0.98) discard;
    float3 Nw = normalize(pin.normal);
    if (gMaterialMode > 4.5) {
        // BeamNG: gEnvStrength = detail U scale; gPointSample = flags (1 normal, 2 detail, 4 detail normal,
        // 8 opacity mask) + 16 * round(detail V scale * 64)
        const float packed = gPointSample + 0.5;
        const int fl = ((int)packed) & 15;
        const float dsy = floor(packed / 16.0) / 64.0;
        const float2 ds = float2(max(gEnvStrength, 0.0001), dsy > 0.0 ? dsy : max(gEnvStrength, 0.0001));
        // detail fades out with distance (BeamNG detailDistances) so it never aliases into streaks
        const float detail_w = saturate(1.0 - pin.viewZ / (gDetailDistance > 0.5 ? gDetailDistance : 80.0));
        if ((fl & 2) != 0 && detail_w > 0.0) {
            const float3 dt = gDetailMap.Sample(gSampler, uv * ds).rgb;
            albedo.rgb = saturate(albedo.rgb * lerp(float3(1, 1, 1), dt * 2.0, detail_w));
        }
        if ((fl & 5) != 0) {
            // Cotangent frame from screen derivatives (no tangent attribute needed).
            const float3 dp1 = ddx(pin.worldPos), dp2 = ddy(pin.worldPos);
            const float2 duv1 = ddx(uv), duv2 = ddy(uv);
            const float3 dp2perp = cross(dp2, Nw), dp1perp = cross(Nw, dp1);
            const float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
            const float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
            const float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
            float2 nxy = (fl & 1) != 0 ? gNormalMap.Sample(gSampler, uv).xy * 2.0 - 1.0 : float2(0, 0);
            if ((fl & 4) != 0)
                nxy += (gDetailNormal.Sample(gSampler, uv * ds).xy * 2.0 - 1.0) * ((fl & 1) != 0 ? 0.5 : 1.0) * detail_w;
            const float3 nt = float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
            Nw = normalize(T * invmax * nt.x - B * invmax * nt.y + Nw * nt.z);
        }
    }
    float3 base = albedo.rgb;
    if (gAlphaMode < 0.5 && albedo.a < 0.05 && dot(base, float3(1, 1, 1)) < 0.001)
        base = float3(0.15, 0.15, 0.18);
    if (gHighlightAmount > 0.001) {
        const float3 tint = float3(gHighlightR, gHighlightG, gHighlightB);
        base = lerp(base, tint, saturate(gHighlightAmount) * 0.65);
    }
    float3 N = Nw;
    const bool veg = (((int)(gOpacityMapOn + 0.5)) & 8) != 0;
    const float3 Ngeom = N;
    // Flat billboards: tilt the lighting normal toward up so leaves catch sun/sky like foliage.
    // A vertical quad has N.z = 0 (no sun term, dark crowns); both faces get the same tilt (two-sided).
    if (veg) N = normalize(lerp(N, float3(0.0, 0.0, 1.0), 0.55));
    // Warm cozy hemi + underside bounce (Quilez-ish) before AgX.
    float hemi_t = saturate(N.z * 0.5 + 0.5);
    float3 hemi = lerp(float3(0.22, 0.16, 0.12), float3(0.42, 0.50, 0.62), hemi_t);
    float3 lit = base * hemi * max(gAmbientScale, 0.12);
    lit += base * float3(0.40, 0.28, 0.20) * saturate(-N.z) * 0.35 * max(gAmbientScale, 0.12);
    const int lc = (int)gLightCount;
    for (int i = 0; i < 4; ++i) {
        if (i >= lc) break;
        float3 Lpos = gLightPosRange[i].xyz;
        float range = max(gLightPosRange[i].w, 0.5);
        float3 toL = Lpos - pin.worldPos;
        float dist = length(toL);
        if (dist >= range * 6.0) continue;
        float att = (dist <= range) ? 1.0 : saturate((range * range) / max(dist * dist, 1e-4));
        float3 Ldir = toL / max(dist, 1e-4);
        float ndl = saturate(dot(N, Ldir));
        float3 col = gLightColorInt[i].rgb * gLightColorInt[i].a;
        lit += base * col * att * ndl;
    }
)"
R"(
    // Scenery [maplight]s (openOMSI): full colour within the core, inverse-square beyond, cut off at six
    // times the core. Lights terrain, splines, objects alike (they all shade here).
    {
        // every light of the view radius, through a world grid: a pixel loops only the lights whose
        // reach covers its cell (nearest to the camera first), not the whole list
        const uint xl = (uint)gXLightInfo.x;
        const float2 xg = (pin.worldPos.xy - gXLightInfo.yz) / max(gXLightInfo.w, 1.0);
        uint xoff = 0u, xcnt = 0u;
        if (xl > 0u && xg.x >= 0.0 && xg.y >= 0.0 && xg.x < 48.0 && xg.y < 48.0) {
            const uint ci = (uint)xg.y * 48u + (uint)xg.x;
            const uint cell = gXCell[ci >> 2][ci & 3u];
            xoff = cell >> 10;
            xcnt = min(cell & 1023u, 64u);
        }
        for (uint k = 0u; k < xcnt; ++k) {
            const uint ii = xoff + k;
            const uint xw = gXIdx[ii >> 3][(ii >> 1) & 3u];
            const uint li = (ii & 1u) != 0u ? (xw >> 16) : (xw & 0xFFFFu);
            const float4 pr = gXLight[li * 2u];
            const float4 cc = gXLight[li * 2u + 1u];
            const float3 toL = pr.xyz - pin.worldPos;
            const float dist = length(toL);
            // a vehicle's [interiorlight] (negative reach) lights only the meshes of its [illumination_interior]
            if (cc.w < 0.0 && ((((int)(gOpacityMapOn + 0.5)) & 131072) == 0)) continue;
            const float reach = abs(cc.w);
            if (dist >= reach) continue;
            // the player's bus's own headlight pool (negative core) lights neither the bus nor any saloon
            if (pr.w < 0.0 && ((((int)(gOpacityMapOn + 0.5)) & (131072 | 262144)) != 0)) continue;
            const float core = max(abs(pr.w), 0.5);
            float att = dist <= core ? 1.0 : (core * core) / (dist * dist);
            att *= saturate((reach - dist) / (0.25 * reach));
            const float ndl = saturate(dot(N, toL / max(dist, 1e-4)));
            // (x3 at night: openOMSI's eye adapts to the dark - its night exposure makes the same lamp
            //  pools read bright; at our fixed exposure a 2.5 m lamp 5 m up left a dim smudge)
            lit += base * cc.rgb * att * (0.2 + 0.8 * ndl) * (1.0 + 2.0 * gNightBlend);
        }
    }
    {
        float3 L = normalize(gSunDir);
        float ndl = saturate(dot(N, L));
        float sh = SampleShadowCSM(pin.worldPos, N, pin.viewZ);
        lit += base * gSunColor * ndl * sh;
        // Translucency: sun through the far side of a leaf (back-lit foliage glows).
        if (veg) lit += base * gSunColor * saturate(dot(-Ngeom, L)) * 0.5 * sh;
        // Soft floor in shadow so AgX does not crush blacks.
        lit += base * float3(0.04, 0.035, 0.03) * (1.0 - sh) * max(gAmbientScale, 0.12);
    }
    // moonlight: a cool, faint light from high up at night (openOMSI lights the street with the moon as a
    // directional light of its own); without it the unlit streets were black
    if (gNightBlend > 0.001)
        lit += base * float3(0.12, 0.14, 0.19) * gNightBlend * (0.30 + 0.20 * saturate(N.z)); // sky fill (the moon is the directional light, with shadows)
    // [matl_envmap] (openOMSI / Omsi.exe environment stage): a sphere map looked up by the reflection in
    // the camera's frame (D3DTSS_TCI_SPHEREMAP) and laid over the lit texture (D3DTOP_LERP) by the
    // material's factor times the light the object stands in - glass and paint mirror the sky by day
    // and fade out at night; the alpha stays as it was (a clear pane reflects as faintly as it shows).
    if (gEnvStrength > 0.001 && gMaterialMode < 4.5) {
        const float3 fwd = normalize(gCamForward.xyz);
        float3 rgt = cross(fwd, float3(0.0, 0.0, 1.0));
        rgt = dot(rgt, rgt) > 1e-6 ? normalize(rgt) : float3(1.0, 0.0, 0.0);
        const float3 upv = cross(rgt, fwd);
        const float3 e = normalize(pin.worldPos - gCamPos.xyz);
        const float3 r = reflect(e, normalize(Ngeom));
        const float3 rv = float3(dot(r, rgt), dot(r, upv), -dot(r, fwd));
        const float m = 2.0 * sqrt(rv.x * rv.x + rv.y * rv.y + (rv.z + 1.0) * (rv.z + 1.0));
        const float2 envUV = float2(rv.x / max(m, 1e-4) + 0.5, -rv.y / max(m, 1e-4) + 0.5);
        const float3 env = gEnvmap.Sample(gSamplerClamp, envUV).rgb;
        // (with a [matl_transmap] / [matl_envmap_mask] the stage blends by the texture's alpha times the
        //  light alone - D3DTOP_BLENDCURRENTALPHA; without, by the material's factor. Unmasked, 3_ggr_7's
        //  walls mirrored the sky wall to wall: chrome)
        const float daylight = saturate(gAmbientScale + 0.05);
        // Paint that says nothing about how much it mirrors - [matl_envmap] ... 1 (or 10) with no mask, the
        // stock car bodies, or a mask read from a texture with no alpha channel (a 24-bit repaint samples 255
        // everywhere: the MAN EN92's .bmp schemes) - is a clear coat, as openOMSI shades it: a lacquer's 4 %
        // rising towards grazing (Schlick), not the factor's whole sphere map (every car a chrome mirror).
        // Authored masks (the EN92's own 15 / 74 of 255), factors under 1 (panes, dashboards, building
        // windows) and blended glass keep Omsi.exe's blend.
        const bool env_masked = ((((int)(gOpacityMapOn + 0.5)) & 32) != 0);
        const bool env_glass = gAlphaMode > 1.5 && gAlphaMode < 2.5;
        const bool env_unsaid = !env_glass && (env_masked ? reflMask > 0.998 : gEnvStrength > 0.99);
        float k = env_masked ? saturate(reflMask) * daylight : saturate(gEnvStrength) * daylight;
        if (env_unsaid) {
            const float nv = saturate(abs(dot(normalize(Ngeom), -e)));
            const float f = 1.0 - nv;
            k = min(0.04 + 0.96 * f * f * f * f * f, 0.45) * daylight;
        }
        lit = lerp(lit, env, k);
    }
    // Quilez squared fog: cool→warm by sunAmount.
    {
        float dist = length(pin.worldPos - gCamPos.xyz);
        float fog = 1.0 - exp(-1.1e-6 * gCamForward.w * dist * dist); // (w: Vista > Niebla, 0 = off)
        fog = saturate(fog);
        float3 viewDir = normalize(pin.worldPos - gCamPos.xyz);
        float sunAmount = saturate(dot(viewDir, normalize(gSunDir)));
        float3 fogRgb = lerp(float3(0.55, 0.64, 0.78), float3(1.0, 0.9, 0.7), pow(sunAmount, 8.0));
        fogRgb *= saturate(0.35 + gAmbientScale);
        lit = lerp(lit, fogRgb, fog);
        lit += (IgnNoise(pin.pos.xy) - 0.5) * (1.0 / 255.0);
    }
    // OMSI night map (matl_nightmap -> t4 for non-PBR batches, gOpacityMapOn bit 4). Emissive, fades in at night.
    if (((((int)(gOpacityMapOn + 0.5)) & 4) != 0) && gMaterialMode < 4.5) {
        const float3 nm = gNormalMap.Sample(gSampler, uv).rgb;
        if ((((int)(gOpacityMapOn + 0.5)) & 16) != 0) {
            // [matl_lightmap]: D3DTOP_ADDSMOOTH on the light, then x texture - glows in the dark
            const float v = saturate(gAmbientScale);
            // (a lightmap with a variable - a vehicle's lamps - shines as the variable says: the flags carry its
            //  value as (q - 1) / 255 above 64)
            const int lq = (((int)(gOpacityMapOn + 0.5)) >> 6) & 511; // (bits 6-14; bit 15 = transmap)
            const float lk = lq > 0 ? saturate((lq - 1) / 255.0) * (1.0 - 0.6 * v) : (1.0 - v);
            lit += albedo.rgb * nm * lk * 1.6;
        } else if ((((int)(gOpacityMapOn + 0.5)) & 65536) != 0) {
            // a [matl_item] night map, its variable on: glows whatever the daylight (openOMSI 09_vanilla:
            // warning lamps, lit displays, the saloon lights by day as well)
            lit += nm * 1.6;
        } else if (gNightBlend > 0.001) {
            lit += nm * gNightBlend * 1.6;
        }
    }
    // o3d material emissive: the texture shows at least at that level whatever the light (D3D9 clamps
    // emissive + lighting to 1 before the texture) - lit traffic light lenses, displays, lamps
    if (gDetailDistance > 0.5 && (gMaterialMode < 0.5 || (gMaterialMode > 3.5 && gMaterialMode < 4.5))) {
        const uint ep = (uint)(gDetailDistance + 0.5);
        const float3 em = float3(ep & 255u, (ep >> 8) & 255u, (ep >> 16) & 255u) * (1.0 / 255.0);
        // (at the texture's own level, as D3D clamps emissive + light to 1: boosted, a lens glowed like a
        //  lamp by day and stood out flat from afar)
        lit = max(lit, albedo.rgb * em * (0.55 + 0.45 * saturate(1.0 - gAmbientScale)));
    }
    lit *= 2.0; // +1 stop exposure before AgX
    lit = TonemapAgX(lit);
    // [matl_alpha] 2: blend with the texture's alpha (OMSI glass, rain overlays). gOpacity carries the
    // editor ghost opacity times [alphascale].
    const float cov = gAlphaMode > 1.5 ? saturate(albedo.a) : 1.0;
    return float4(lit, cov * saturate(gOpacity));
}
)";

const char* kShaderSourceAlpha = R"(
cbuffer SceneCB : register(b0) {
    float4x4 gWorldViewProj;
};
cbuffer EnvCB : register(b2) {
    float gEnvStrength;
    float gMaterialMode; // 0=normal, 1=shadow decal, 2=invert-color overlay (white PS)
    float gHighlightAmount;
    float gHighlightR;
    float gHighlightG;
    float gHighlightB;
    float gUvOffX;
    float gUvOffY;
    float gAlphaMode; // 0=none, 1=1-bit punch, 2=blend
    float gPointSample; // 1 = [useTextTexture] only (point, mip 0). Alpha-test uses aniso mips.
    float gOpacity; // 0..1 per-chunk opacity (editor)
    // OMSI-style dynamic lights (editor realtime approx of light_enh + maplight)
    float gAmbientScale; // 1=day, ~0.25 night
    float gLightCount;   // 0..4
    // BeamNG material params (root-constant indices 13..15; unused by the OMSI modes).
    float gAlphaRef;        // alphaRef / 255 -> alpha-test discard threshold (default 0.5)
    float gOpacityMapOn;    // 1 = opacityMap bound at t7 (mask in alpha and/or luminance)
    float gDetailDistance;  // BeamNG detailDistances far edge in metres (0 = 80 m legacy)
    float4 gLightPosRange[4];  // xyz world, w = range (full until range, then 1/d^2)
    float4 gLightColorInt[4];  // rgb, w = intensity
    float3 gSunDir;
    float gSunShadowEnable;
    float3 gSunColor;
    float gSunPad;
};
Texture2D gAlbedo : register(t0);
Texture2D gEnvmap : register(t1);
// BeamNG materials (gMaterialMode 5): normal / detail / detail-normal maps (second range of the same table).
Texture2D gNormalMap : register(t4);
Texture2D gDetailMap : register(t5);
Texture2D gDetailNormal : register(t6);
Texture2D gOpacityMap : register(t7); // BeamNG opacityMap (mask in alpha and/or luminance)
Texture2DArray gShadowMap : register(t3);
SamplerState gSampler : register(s0);
SamplerState gSamplerPoint : register(s1);
SamplerState gSamplerClamp : register(s2);
SamplerComparisonState gShadowSampler : register(s3);
cbuffer ShadowCB : register(b3) {
    float4x4 gLightVP[3];
    float4 gCascadeSplits;
    float4 gCamPos;
    float4 gShadowBias;
    float4 gCamForward;
    float4 gEnvExtra;      // x = night blend, y/z = wind m/s world XY, w = time s (written per frame)
    float4 gXLightInfo;    // x = scenery light count, yz = light grid origin (world XY), w = grid cell (m)
    float4 gXLight[1024];  // 512 lights: (xyz, core m), (rgb x intensity, cut-off m)
    uint4 gXCell[576];     // 48 x 48 world grid cells, 4 per entry: (index offset << 10) | light count
    uint4 gXIdx[2478];     // the cells' light indices, 16 bit, 8 per entry (ends at 64 KB)
};
#define gNightBlend gEnvExtra.x
#define gWindX gEnvExtra.y
#define gWindY gEnvExtra.z
#define gTime gEnvExtra.w
struct VSIn {
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};
struct VSOut {
    float4 pos : SV_POSITION;
    float3 normal : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
    float viewZ : TEXCOORD3;
};
struct InstanceData {
    float4x4 wvp;
    float4x4 world;
};
StructuredBuffer<InstanceData> gInstance : register(t2);
// Interleaved gradient noise (Jimenez) — breaks fog / tonemap banding at distance.
float IgnNoise(float2 p) {
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}
// Compact AgX-ish tonemap (split from lighting): soft shoulder, no ACES wash.
float3 TonemapAgX(float3 c) {
    c = max(c, 0.0);
    float3 x = c / (1.0 + c * 0.85);
    x = saturate(x);
    return x * x * (3.0 - 2.0 * x);
}
float SampleShadowCascade(float3 worldPos, float3 N, int ci) {
    // normal offset grows with the cascade's texel (the far ones cover 10x the ground per texel)
    // slope-scaled normal offset: only as much as the surface turns from the sun (1 - N.L) - a flat
    // pavement under a high sun needs none, and a fixed lift moved the pole's shadow off its foot
    // Normal-offset shadows (Holbert): the offset is one texel of THIS cascade (its world size read off the
    // light matrix: x scale = 1/radius), times how far the surface turns from the sun - a flat pavement
    // under a high sun needs almost none. A fixed factor per cascade pushed far shadows a hand off the feet.
    const float ndl_s = saturate(dot(normalize(N), normalize(gSunDir)));
    const float3 xs = float3(gLightVP[ci][0][0], gLightVP[ci][1][0], gLightVP[ci][2][0]);
    const float3 zs = float3(gLightVP[ci][0][2], gLightVP[ci][1][2], gLightVP[ci][2][2]);
    const float texel_w = 2.0 * gShadowBias.z / max(length(xs), 1e-6);
    float3 offsetPos = worldPos + N * texel_w * (0.25 + 1.25 * (1.0 - ndl_s));
    float4 lp = mul(float4(offsetPos, 1.0), gLightVP[ci]);
    float3 ndc = lp.xyz / max(lp.w, 1e-5);
    float2 uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z < 0.0 || ndc.z > 1.0) return 1.0;
    // depth bias in metres along the light (z scale = 1 / depth range): 1.5 cm in every cascade
    float depth = ndc.z - 0.015 * length(zs);
    float dx = gShadowBias.z;
    float sh = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            sh += gShadowMap.SampleCmpLevelZero(gShadowSampler, float3(uv + float2(x, y) * dx, (float)ci), depth);
    return sh / 9.0;
}
float SampleShadowCSM(float3 worldPos, float3 N, float viewZ) {
    if (gCamPos.w < 0.5) return 1.0;
    float farEdge = gCascadeSplits.z;
    if (viewZ > farEdge) return 1.0;
    int ci = 2;
    if (viewZ < gCascadeSplits.x) ci = 0;
    else if (viewZ < gCascadeSplits.y) ci = 1;
    float sh = SampleShadowCascade(worldPos, N, ci);
    // Soft blend into next cascade to hide seam swimming (texel snap already on matrices).
    if (ci < 2) {
        float edge = (ci == 0) ? gCascadeSplits.x : gCascadeSplits.y;
        float band = max(edge * 0.12, 1.5);
        float t = saturate((viewZ - (edge - band)) / band);
        if (t > 0.0) sh = lerp(sh, SampleShadowCascade(worldPos, N, ci + 1), t);
    }
    // Soft fade last ~15% of far cascade so shadows do not hard-cut at visual radius.
    float fadeStart = farEdge * 0.85;
    if (viewZ > fadeStart) {
        float fade = saturate((viewZ - fadeStart) / max(farEdge - fadeStart, 1e-3));
        sh = lerp(sh, 1.0, fade);
    }
    return sh;
}
VSOut vs_main(VSIn vin, uint iid : SV_InstanceID) {
    VSOut o;
    InstanceData inst = gInstance[iid];
    float3 lp = vin.pos;
    // Vegetation wind sway (gOpacityMapOn bit 8 = tree billboard). mul(v,M) convention: row 0 / row 2 of
    // inst.world are the local X / Z axis images, so their lengths are width / height in metres.
    if ((((int)(gOpacityMapOn + 0.5)) & 8) != 0 && (gWindX * gWindX + gWindY * gWindY) > 1e-6) {
        const float wspd = sqrt(gWindX * gWindX + gWindY * gWindY);
        const float wid_m = max(length(inst.world[0].xyz), 0.5);
        const float h = saturate(lp.z);                  // 0 at root, 1 at crown (unit billboard)
        const float ph = inst.world[3].x * 0.031 + inst.world[3].y * 0.017 + lp.x * 0.7 + lp.y * 0.4;
        const float gust = 0.65 + 0.35 * sin(gTime * 0.23 + inst.world[3].x * 0.011);
        const float sway = sin(gTime * 1.7 + ph) + 0.4 * sin(gTime * 3.9 + ph * 1.7);
        const float disp_m = 0.10 * wspd * gust * sway * h * h; // crown offset in m (~0.3-0.5 m in a gale)
        lp.xy += (float2(gWindX, gWindY) / wspd) * (disp_m / wid_m);
    }
    o.pos = mul(float4(lp, 1.0f), inst.wvp);
    o.worldPos = mul(float4(lp, 1.0f), inst.world).xyz;
    o.normal = normalize(mul(vin.normal, (float3x3)inst.world));
    o.uv = vin.uv + float2(gUvOffX, gUvOffY);
    o.viewZ = max(0.0, dot(o.worldPos - gCamPos.xyz, gCamForward.xyz));
    return o;
}
)"
R"(
float4 ps_main(VSOut pin) : SV_TARGET {
    if (gMaterialMode > 3.5 && gMaterialMode < 4.5) {
        // Same as opaque fixed_lit: multiply wash unless solid (amt>=0.99).
        float4 a = gAlbedo.Sample(gSampler, pin.uv);
        float3 col = a.rgb * 0.85;
        if (gHighlightAmount > 0.001) {
            const float3 tint = float3(gHighlightR, gHighlightG, gHighlightB);
            const float amt = saturate(gHighlightAmount);
            if (amt >= 0.99)
                col = lerp(col, tint, amt);
            else
                col *= lerp(float3(1.0, 1.0, 1.0), tint, amt);
        }
        return float4(col, saturate(gOpacity));
    }
    const bool cutout = (gAlphaMode > 0.5 && gAlphaMode < 1.5);
    float2 uv = pin.uv;
    const bool uv_unit = all(uv >= 0.0) && all(uv <= 1.0);
    float4 albedo = (gPointSample > 0.5 && gMaterialMode < 4.5) ? gAlbedo.Sample(gSamplerPoint, uv)
                  : (cutout && uv_unit) ? gAlbedo.Sample(gSamplerClamp, uv)
                                        : gAlbedo.Sample(gSampler, uv);
    // [matl_transmap] (Omsi.exe material setter 0x7fd9b0 sets gMatlTransMapOn; OMSI SDK: the transmap's alpha
    // is the transparency, the texture's own alpha stays the reflection mask). Bound at t5 (slot+3), bit 15.
    float reflMask = albedo.a;
    if (gMaterialMode < 4.5 && ((((int)(gOpacityMapOn + 0.5)) & 32768) != 0))
        albedo.a = gDetailMap.Sample(gSampler, uv).a;
    // BeamNG material params: t1 = 1x1 solid, rgb = baseColorFactor, a = opacityFactor (always bound in
    // mode 5). gOpacityMapOn bit 0 = opacityMap bound at t7, bit 1 = t0 is a colorMap (baseColorFactor tint).
    if (gMaterialMode > 4.5) {
        const int mflags = (int)(gOpacityMapOn + 0.5);
        const float4 mt = gEnvmap.Sample(gSamplerPoint, float2(0.5, 0.5));
        if ((mflags & 2) != 0) albedo.rgb *= mt.rgb; // colorMap x baseColorFactor
        if ((mflags & 1) != 0) {
            // Opacity mask: alpha = min(opacityMap alpha, luminance) x colorMap alpha.
            const float4 om = gOpacityMap.Sample(gSampler, uv);
            const float chroma = max(max(om.r, om.g), om.b);
            float mask = min(saturate(om.a), saturate(chroma));
            // Masks without a usable alpha channel (BC4 = R only, a = 1; or mask authored in rgb).
            if (om.a > 0.999) mask = saturate(chroma);
            else if (chroma < 0.004) mask = saturate(om.a); // mask in alpha over a black rgb
            albedo.a *= mask;
        } else if ((int(gPointSample + 0.5) & 8) != 0) {
            // No colorMap: t0 carries the mask, the solid colour comes from baseColorFactor.
            const float chroma = max(max(albedo.r, albedo.g), albedo.b);
            float mask = min(saturate(albedo.a), saturate(chroma));
            if (albedo.a > 0.999) mask = saturate(chroma);
            else if (chroma < 0.004) mask = saturate(albedo.a);
            albedo = float4(mt.rgb, mask);
        }
        albedo.a *= saturate(mt.a); // opacityFactor
    }
    if (cutout) {
        if (albedo.a < gAlphaRef) discard;
    } else if (gAlphaMode > 2.1 && gAlphaMode < 2.5 && albedo.a >= 0.98) {
        discard; // 2.2 = blend remainder pass: opaque texels were already drawn (with depth) by the core pass
    } else if (gAlphaMode > 1.5 && albedo.a < 0.001 && gOpacity > 0.995) {
        discard;
    }
    if (gMaterialMode > 2.5 && gMaterialMode < 3.5) {
        // Wireframe pass: respect highlight (same as hover) so path lines can be colored.
        float3 col = float3(0.12, 0.12, 0.14);
        if (gHighlightAmount > 0.001)
            col = lerp(col, float3(gHighlightR, gHighlightG, gHighlightB), saturate(gHighlightAmount));
        return float4(col, 1);
    }
    if (gMaterialMode > 1.5 && gMaterialMode < 2.5) {
        return float4(1, 1, 1, 1);
    }
    if (gMaterialMode > 0.5 && gMaterialMode < 1.5) {
        return float4(0, 0, 0, albedo.a * 0.42);
    }
    float3 Nw = normalize(pin.normal);
    if (gMaterialMode > 4.5) {
        // BeamNG: gEnvStrength = detail U scale; gPointSample = flags (1 normal, 2 detail, 4 detail normal,
        // 8 opacity mask) + 16 * round(detail V scale * 64)
        const float packed = gPointSample + 0.5;
        const int fl = ((int)packed) & 15;
        const float dsy = floor(packed / 16.0) / 64.0;
        const float2 ds = float2(max(gEnvStrength, 0.0001), dsy > 0.0 ? dsy : max(gEnvStrength, 0.0001));
        // detail fades out with distance (BeamNG detailDistances) so it never aliases into streaks
        const float detail_w = saturate(1.0 - pin.viewZ / (gDetailDistance > 0.5 ? gDetailDistance : 80.0));
        if ((fl & 2) != 0 && detail_w > 0.0) {
            const float3 dt = gDetailMap.Sample(gSampler, uv * ds).rgb;
            albedo.rgb = saturate(albedo.rgb * lerp(float3(1, 1, 1), dt * 2.0, detail_w));
        }
        if ((fl & 5) != 0) {
            // Cotangent frame from screen derivatives (no tangent attribute needed).
            const float3 dp1 = ddx(pin.worldPos), dp2 = ddy(pin.worldPos);
            const float2 duv1 = ddx(uv), duv2 = ddy(uv);
            const float3 dp2perp = cross(dp2, Nw), dp1perp = cross(Nw, dp1);
            const float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
            const float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
            const float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
            float2 nxy = (fl & 1) != 0 ? gNormalMap.Sample(gSampler, uv).xy * 2.0 - 1.0 : float2(0, 0);
            if ((fl & 4) != 0)
                nxy += (gDetailNormal.Sample(gSampler, uv * ds).xy * 2.0 - 1.0) * ((fl & 1) != 0 ? 0.5 : 1.0) * detail_w;
            const float3 nt = float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
            Nw = normalize(T * invmax * nt.x - B * invmax * nt.y + Nw * nt.z);
        }
    }
    float3 base = albedo.rgb;
    if (gHighlightAmount > 0.001) {
        const float3 tint = float3(gHighlightR, gHighlightG, gHighlightB);
        base = lerp(base, tint, saturate(gHighlightAmount) * 0.65);
    }
    float3 N = Nw;
    const bool veg = (((int)(gOpacityMapOn + 0.5)) & 8) != 0;
    const float3 Ngeom = N;
    // Flat billboards: tilt the lighting normal toward up so leaves catch sun/sky like foliage.
    // A vertical quad has N.z = 0 (no sun term, dark crowns); both faces get the same tilt (two-sided).
    if (veg) N = normalize(lerp(N, float3(0.0, 0.0, 1.0), 0.55));
    float hemi_t = saturate(N.z * 0.5 + 0.5);
    float3 hemi = lerp(float3(0.22, 0.16, 0.12), float3(0.42, 0.50, 0.62), hemi_t);
    float3 lit = base * hemi * max(gAmbientScale, 0.12);
    lit += base * float3(0.40, 0.28, 0.20) * saturate(-N.z) * 0.35 * max(gAmbientScale, 0.12);
    const int lc = (int)gLightCount;
    for (int i = 0; i < 4; ++i) {
        if (i >= lc) break;
        float3 Lpos = gLightPosRange[i].xyz;
        float range = max(gLightPosRange[i].w, 0.5);
        float3 toL = Lpos - pin.worldPos;
        float dist = length(toL);
        if (dist >= range * 6.0) continue;
        float att = (dist <= range) ? 1.0 : saturate((range * range) / max(dist * dist, 1e-4));
        float3 Ldir = toL / max(dist, 1e-4);
        float ndl = saturate(dot(N, Ldir));
        float3 col = gLightColorInt[i].rgb * gLightColorInt[i].a;
        lit += base * col * att * ndl;
    }
    // Scenery [maplight]s (openOMSI): full colour within the core, inverse-square beyond, cut off at six
    // times the core. Lights terrain, splines, objects alike (they all shade here).
    {
        // every light of the view radius, through a world grid: a pixel loops only the lights whose
        // reach covers its cell (nearest to the camera first), not the whole list
        const uint xl = (uint)gXLightInfo.x;
        const float2 xg = (pin.worldPos.xy - gXLightInfo.yz) / max(gXLightInfo.w, 1.0);
        uint xoff = 0u, xcnt = 0u;
        if (xl > 0u && xg.x >= 0.0 && xg.y >= 0.0 && xg.x < 48.0 && xg.y < 48.0) {
            const uint ci = (uint)xg.y * 48u + (uint)xg.x;
            const uint cell = gXCell[ci >> 2][ci & 3u];
            xoff = cell >> 10;
            xcnt = min(cell & 1023u, 64u);
        }
        for (uint k = 0u; k < xcnt; ++k) {
            const uint ii = xoff + k;
            const uint xw = gXIdx[ii >> 3][(ii >> 1) & 3u];
            const uint li = (ii & 1u) != 0u ? (xw >> 16) : (xw & 0xFFFFu);
            const float4 pr = gXLight[li * 2u];
            const float4 cc = gXLight[li * 2u + 1u];
            const float3 toL = pr.xyz - pin.worldPos;
            const float dist = length(toL);
            // a vehicle's [interiorlight] (negative reach) lights only the meshes of its [illumination_interior]
            if (cc.w < 0.0 && ((((int)(gOpacityMapOn + 0.5)) & 131072) == 0)) continue;
            const float reach = abs(cc.w);
            if (dist >= reach) continue;
            // the player's bus's own headlight pool (negative core) lights neither the bus nor any saloon
            if (pr.w < 0.0 && ((((int)(gOpacityMapOn + 0.5)) & (131072 | 262144)) != 0)) continue;
            const float core = max(abs(pr.w), 0.5);
            float att = dist <= core ? 1.0 : (core * core) / (dist * dist);
            att *= saturate((reach - dist) / (0.25 * reach));
            const float ndl = saturate(dot(N, toL / max(dist, 1e-4)));
            // (x3 at night: openOMSI's eye adapts to the dark - its night exposure makes the same lamp
            //  pools read bright; at our fixed exposure a 2.5 m lamp 5 m up left a dim smudge)
            lit += base * cc.rgb * att * (0.2 + 0.8 * ndl) * (1.0 + 2.0 * gNightBlend);
        }
    }
    {
        float3 L = normalize(gSunDir);
        float ndl = saturate(dot(N, L));
        float sh = SampleShadowCSM(pin.worldPos, N, pin.viewZ);
        lit += base * gSunColor * ndl * sh;
        // Translucency: sun through the far side of a leaf (back-lit foliage glows).
        if (veg) lit += base * gSunColor * saturate(dot(-Ngeom, L)) * 0.5 * sh;
        lit += base * float3(0.04, 0.035, 0.03) * (1.0 - sh) * max(gAmbientScale, 0.12);
    }
    // moonlight: a cool, faint light from high up at night (openOMSI lights the street with the moon as a
    // directional light of its own); without it the unlit streets were black
    if (gNightBlend > 0.001)
        lit += base * float3(0.12, 0.14, 0.19) * gNightBlend * (0.30 + 0.20 * saturate(N.z)); // sky fill (the moon is the directional light, with shadows)
    // [matl_envmap] (openOMSI / Omsi.exe environment stage): a sphere map looked up by the reflection in
    // the camera's frame (D3DTSS_TCI_SPHEREMAP) and laid over the lit texture (D3DTOP_LERP) by the
    // material's factor times the light the object stands in - glass and paint mirror the sky by day
    // and fade out at night; the alpha stays as it was (a clear pane reflects as faintly as it shows).
    if (gEnvStrength > 0.001 && gMaterialMode < 4.5) {
        const float3 fwd = normalize(gCamForward.xyz);
        float3 rgt = cross(fwd, float3(0.0, 0.0, 1.0));
        rgt = dot(rgt, rgt) > 1e-6 ? normalize(rgt) : float3(1.0, 0.0, 0.0);
        const float3 upv = cross(rgt, fwd);
        const float3 e = normalize(pin.worldPos - gCamPos.xyz);
        const float3 r = reflect(e, normalize(Ngeom));
        const float3 rv = float3(dot(r, rgt), dot(r, upv), -dot(r, fwd));
        const float m = 2.0 * sqrt(rv.x * rv.x + rv.y * rv.y + (rv.z + 1.0) * (rv.z + 1.0));
        const float2 envUV = float2(rv.x / max(m, 1e-4) + 0.5, -rv.y / max(m, 1e-4) + 0.5);
        const float3 env = gEnvmap.Sample(gSamplerClamp, envUV).rgb;
        // (with a [matl_transmap] / [matl_envmap_mask] the stage blends by the texture's alpha times the
        //  light alone - D3DTOP_BLENDCURRENTALPHA; without, by the material's factor. Unmasked, 3_ggr_7's
        //  walls mirrored the sky wall to wall: chrome)
        const float daylight = saturate(gAmbientScale + 0.05);
        // Paint that says nothing about how much it mirrors - [matl_envmap] ... 1 (or 10) with no mask, the
        // stock car bodies, or a mask read from a texture with no alpha channel (a 24-bit repaint samples 255
        // everywhere: the MAN EN92's .bmp schemes) - is a clear coat, as openOMSI shades it: a lacquer's 4 %
        // rising towards grazing (Schlick), not the factor's whole sphere map (every car a chrome mirror).
        // Authored masks (the EN92's own 15 / 74 of 255), factors under 1 (panes, dashboards, building
        // windows) and blended glass keep Omsi.exe's blend.
        const bool env_masked = ((((int)(gOpacityMapOn + 0.5)) & 32) != 0);
        const bool env_glass = gAlphaMode > 1.5 && gAlphaMode < 2.5;
        const bool env_unsaid = !env_glass && (env_masked ? reflMask > 0.998 : gEnvStrength > 0.99);
        float k = env_masked ? saturate(reflMask) * daylight : saturate(gEnvStrength) * daylight;
        if (env_unsaid) {
            const float nv = saturate(abs(dot(normalize(Ngeom), -e)));
            const float f = 1.0 - nv;
            k = min(0.04 + 0.96 * f * f * f * f * f, 0.45) * daylight;
        }
        lit = lerp(lit, env, k);
    }
    {
        float dist = length(pin.worldPos - gCamPos.xyz);
        float fog = 1.0 - exp(-1.1e-6 * gCamForward.w * dist * dist); // (w: Vista > Niebla, 0 = off)
        fog = saturate(fog);
        float3 viewDir = normalize(pin.worldPos - gCamPos.xyz);
        float sunAmount = saturate(dot(viewDir, normalize(gSunDir)));
        float3 fogRgb = lerp(float3(0.55, 0.64, 0.78), float3(1.0, 0.9, 0.7), pow(sunAmount, 8.0));
        fogRgb *= saturate(0.35 + gAmbientScale);
        lit = lerp(lit, fogRgb, fog);
        lit += (IgnNoise(pin.pos.xy) - 0.5) * (1.0 / 255.0);
    }
    // OMSI night map (matl_nightmap -> t4 for non-PBR batches, gOpacityMapOn bit 4). Emissive, fades in at night.
    if (((((int)(gOpacityMapOn + 0.5)) & 4) != 0) && gMaterialMode < 4.5) {
        const float3 nm = gNormalMap.Sample(gSampler, uv).rgb;
        if ((((int)(gOpacityMapOn + 0.5)) & 16) != 0) {
            // [matl_lightmap]: D3DTOP_ADDSMOOTH on the light, then x texture - glows in the dark
            const float v = saturate(gAmbientScale);
            // (a lightmap with a variable - a vehicle's lamps - shines as the variable says: the flags carry its
            //  value as (q - 1) / 255 above 64)
            const int lq = (((int)(gOpacityMapOn + 0.5)) >> 6) & 511; // (bits 6-14; bit 15 = transmap)
            const float lk = lq > 0 ? saturate((lq - 1) / 255.0) * (1.0 - 0.6 * v) : (1.0 - v);
            lit += albedo.rgb * nm * lk * 1.6;
        } else if ((((int)(gOpacityMapOn + 0.5)) & 65536) != 0) {
            // a [matl_item] night map, its variable on: glows whatever the daylight (openOMSI 09_vanilla:
            // warning lamps, lit displays, the saloon lights by day as well)
            lit += nm * 1.6;
        } else if (gNightBlend > 0.001) {
            lit += nm * gNightBlend * 1.6;
        }
    }
    // o3d material emissive: the texture shows at least at that level whatever the light (D3D9 clamps
    // emissive + lighting to 1 before the texture) - lit traffic light lenses, displays, lamps
    if (gDetailDistance > 0.5 && (gMaterialMode < 0.5 || (gMaterialMode > 3.5 && gMaterialMode < 4.5))) {
        const uint ep = (uint)(gDetailDistance + 0.5);
        const float3 em = float3(ep & 255u, (ep >> 8) & 255u, (ep >> 16) & 255u) * (1.0 / 255.0);
        // (at the texture's own level, as D3D clamps emissive + light to 1: boosted, a lens glowed like a
        //  lamp by day and stood out flat from afar)
        lit = max(lit, albedo.rgb * em * (0.55 + 0.45 * saturate(1.0 - gAmbientScale)));
    }
    lit *= 2.0; // +1 stop exposure before AgX
    lit = TonemapAgX(lit);
    float cov = 1.0;
    if (gAlphaMode > 1.5) cov = saturate(albedo.a);
    if (veg) cov = saturate(albedo.a); // vegetation: alpha-to-coverage edges (MSAA)
    return float4(lit, cov * saturate(gOpacity));
}
)";

// Depth-only portal for [rendertype] presurface + [matl_alpha] 2 (TubeEntrance hole.tga).
// hole.dds is 4x4 DXT3: 1 red texel, rest a=0. Aniso mips smear that into a pink lid;
// kShaderSourceAlpha also discards a<0.02 so those pixels never write Z and terrain fills the pit.
const char* kShaderSourcePresurfaceDepth = R"(
cbuffer SceneCB : register(b0) {
    float4x4 gWorldViewProj;
};
struct VSIn {
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};
struct VSOut {
    float4 pos : SV_POSITION;
};
struct InstanceData {
    float4x4 wvp;
    float4x4 world;
};
StructuredBuffer<InstanceData> gInstance : register(t2);
VSOut vs_main(VSIn vin, uint iid : SV_InstanceID) {
    VSOut o;
    o.pos = mul(float4(vin.pos, 1.0f), gInstance[iid].wvp);
    return o;
}
float4 ps_main(VSOut pin) : SV_TARGET {
    return float4(0, 0, 0, 0);
}
)";

const char* kAxisShaderSource = R"(
cbuffer AxisCB : register(b0) {
    float4x4 gAxisViewProj;
};
struct VSIn {
    float3 pos : POSITION;
    float4 col : COLOR0;
};
struct VSOut {
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
};
VSOut vs_main(VSIn vin) {
    VSOut o;
    o.pos = mul(float4(vin.pos, 1.0f), gAxisViewProj);
    o.col = vin.col;
    return o;
}
float4 ps_main(VSOut pin) : SV_TARGET {
    return pin.col;
}
)";

} // namespace

using Microsoft::WRL::ComPtr;

struct HelloO3dRenderer::Impl {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    ComPtr<ID3D12DescriptorHeap> cbv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline_state;
    ComPtr<ID3D12PipelineState> pipeline_state_alpha;
    // BeamNG doubleSided: same states with CULL_NONE (opaque) and CULL_NONE + alpha punch.
    ComPtr<ID3D12PipelineState> pipeline_state_double_sided;
    ComPtr<ID3D12PipelineState> pipeline_state_double_sided_cutout;
    ComPtr<ID3D12PipelineState> pipeline_state_alpha_nozwrite;
    // Editor opacity < 1 on a spline / [surface] chunk: SurfacePull VS (still wins over the terrain)
    // + SRC_ALPHA blend, depth test, no depth write.
    ComPtr<ID3D12PipelineState> pipeline_state_ghost_surface;
    // Vegetation cutout (OMSI_VEG_CUTOUT): blend OFF + depth write ON, so alpha-tested foliage
    // writes depth and stops overdrawing (alpha-blend billboards were shading every layer).
    ComPtr<ID3D12PipelineState> pipeline_state_veg_cutout;
    ComPtr<ID3D12PipelineState> pipeline_state_on_surface;
    ComPtr<ID3D12PipelineState> pipeline_state_surface_bias;
    // content meshes (.o3d / .x) keep Direct3D's back-face culling (openOMSI culls_back_faces): BACK twins
    // of the PSOs that draw both sides (vegetation / double-sided materials keep those)
    ComPtr<ID3D12PipelineState> pipeline_state_alpha_cull;
    ComPtr<ID3D12PipelineState> pipeline_state_alpha_nozwrite_cull;
    ComPtr<ID3D12PipelineState> pipeline_state_surface_bias_cull;
    ComPtr<ID3D12PipelineState> pipeline_state_ghost_surface_cull;
    // translucent remainder of a [matl_alpha] 2 road: blended, no depth write, the road's own vertex shader
    // and culling (its opaque core was drawn with depth by the spline PSO - see draw.cpp ground_core_pass)
    ComPtr<ID3D12PipelineState> pipeline_state_ghost_spline;
    ComPtr<ID3D12PipelineState> pipeline_state_on_surface_cull;
    // scenery [maplight]s (up to kMaxXLights): x, y, z, core, r, g, b (x intensity), cut-off
    std::vector<float> xlights;
    int xlight_count = 0;
    // ShadowCB from byte 272 to 64 KB: light info, 512 lights, the 48 x 48 world light grid and its
    // index list (set_scene_lights); copied to the CB when it changed
    std::vector<std::uint8_t> xlight_blob;
    bool xlight_dirty = false;
    std::uint64_t xlight_hash = 0;
    const std::uint8_t* xlight_cb_written = nullptr;
    ComPtr<ID3D12PipelineState> pipeline_state_spline_bias;
    ComPtr<ID3D12PipelineState> pipeline_state_path_rail; // vs_rail, no depth write, both sides

    ComPtr<ID3D12PipelineState> pipeline_state_terrain_bias;
    ComPtr<ID3D12PipelineState> pipeline_state_terrain_underlay; // base mesh behind the cut detail mesh
    ComPtr<ID3D12PipelineState> pipeline_state_surface_depth;
    ComPtr<ID3D12PipelineState> pipeline_state_presurface;
    ComPtr<ID3D12PipelineState> pipeline_state_presurface_alpha;
    ComPtr<ID3D12PipelineState> pipeline_state_alpha_nozcheck;
    ComPtr<ID3D12PipelineState> pipeline_state_alpha_shadow;
    ComPtr<ID3D12PipelineState> pipeline_state_invert;
    ComPtr<ID3D12PipelineState> pipeline_state_wireframe;
    ComPtr<ID3D12RootSignature> axis_root_signature;
    ComPtr<ID3D12PipelineState> axis_pipeline_state;
    bool sky_enabled = true;
    float sky_zenith[3]{0.35f, 0.55f, 0.95f};
    float sky_horizon[3]{0.72f, 0.78f, 0.88f};
    float sky_sun_dir[3]{0.4f, 0.22f, 0.88f};
    float sky_sun_color[3]{1.f, 0.95f, 0.75f};
    float sky_sun_intensity = 1.f;
    float sky_sun_power = 256.f;
    // OMSI sky dome / clouds / stars / weather / water passes (see o3d/sky.cpp, o3d/water.cpp).
    SkyWeatherGpu sw;
    // Triple-buffered frame resources: CPU can record frame N+2 while GPU draws N.
    struct FrameResource {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> command_list;
        UINT64 fence_value = 0;

        // GPU timestamp pair for this in-flight frame. The readback is only
        // mapped after this frame resource's fence has completed.
        ComPtr<ID3D12Resource> timestamp_readback;
    };
    FrameResource frames[HelloO3dRenderer::kFrameCount]{};
    // [PERF] Multithreaded recording of the big opaque pass (draw.cpp): the frame's list is closed before it
    // ("head"), kRecWorkers lists record its parts on the job pool, and the rest of the frame goes on in a
    // second list on the same allocator ("tail", swapped into frames[].command_list). Submitted in that order.
    static constexpr int kRecWorkers = 4;
    ComPtr<ID3D12CommandAllocator> rec_alloc[HelloO3dRenderer::kFrameCount][kRecWorkers];
    ComPtr<ID3D12GraphicsCommandList> rec_list[kRecWorkers];
    ComPtr<ID3D12GraphicsCommandList> rec_tail[HelloO3dRenderer::kFrameCount];
    std::vector<ID3D12CommandList*> rec_pre; // this frame's head + worker lists, submitted before the main list
    // The shadow cascades recorded on a worker into their own list while the main thread records the picture
    // (submitted first): their own allocator per frame, the job's done flag, and whether this frame has one.
    ComPtr<ID3D12CommandAllocator> csm_alloc[HelloO3dRenderer::kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> csm_list[HelloO3dRenderer::kFrameCount];
    // 0 queued, 3 running, 1 done, 2 failed (nothing to submit); whoever moves it 0 -> 3 records it - the
    // main thread itself when it reaches the join first (the pool may be busy with slow disk reads)
    std::shared_ptr<std::atomic<int>> csm_job;
    std::function<void()> csm_job_body;
    bool csm_list_this_frame = false;
    // Offline texture/mesh uploads (not tied to swapchain index).
    // Prefer COPY queue for buffer/texture copies; GRAPHICS queue for draws.
    ComPtr<ID3D12CommandQueue> copy_queue;
    ComPtr<ID3D12CommandAllocator> copy_allocator;
    ComPtr<ID3D12GraphicsCommandList> copy_command_list;
    ComPtr<ID3D12Fence> copy_fence;
    UINT64 copy_fence_value = 0;
    HANDLE copy_fence_event = nullptr;
    // Batched proc-mesh uploads (splines / .x parts / tree billboards): recorded on COPY,
    // submitted once per frame; graphics queue waits GPU-side on copy_fence.
    struct ProcUploadSlot {
        ComPtr<ID3D12CommandAllocator> allocator;
        UINT64 fence_value = 0;
        std::vector<ComPtr<ID3D12Resource>> keep_alive;
        // Persistently mapped staging ring (reset when the slot is reused, i.e. after its fence):
        // avoids one CreateCommittedResource per staging buffer (~1-2 ms each on a 1 GHz CPU).
        ComPtr<ID3D12Resource> ring;
        std::uint8_t* ring_ptr = nullptr;
        UINT64 ring_cap = 0;
        UINT64 ring_off = 0;
    };
    static constexpr std::uint32_t kProcUploadSlots = 16;
    ProcUploadSlot proc_slots[kProcUploadSlots]{};
    std::shared_ptr<VbArenaPool> vb_pool = std::make_shared<VbArenaPool>();
    struct LateTexture {
        std::uint32_t slot = 0;
        std::filesystem::path path;
    };
    std::vector<LateTexture> late_textures; // (see HelloO3dRenderer::pump_late_textures)
    ComPtr<ID3D12GraphicsCommandList> proc_command_list;
    std::uint32_t proc_slot_i = 0;
    std::uint32_t proc_pending = 0;
    bool proc_open = false;
    // Flip-model Present(0,0) without ALLOW_TEARING still waits on DWM vsync (frames >16.7 ms snap to 30 fps).
    bool allow_tearing = false;
    // DXGI Present sync interval: 0 = immediate (tearing if allowed), 1 = vsync to monitor refresh.
    UINT present_sync_interval = 0;
    bool vsync_explicit = false; // set_vsync() was called: FELIX_O3D_VSYNC must not override it
    // COMPUTE queue reserved for future GPU culling / LOD (created, ready to use).
    ComPtr<ID3D12CommandQueue> compute_queue;
    ComPtr<ID3D12CommandAllocator> compute_allocator;
    ComPtr<ID3D12GraphicsCommandList> compute_command_list;
    ComPtr<ID3D12Fence> compute_fence;
    UINT64 compute_fence_value = 0;
    // Legacy name: still used for DIRECT-list uploads when copy queue unavailable.
    ComPtr<ID3D12CommandAllocator> upload_allocator;
    ComPtr<ID3D12GraphicsCommandList> upload_command_list;
    // P6: per-frame instance matrix ring (UPLOAD). Max instances submitted in one draw.
    // Was 16384 (8192 with shadows) — overflowed on dense maps (>8k visible draws) and the
    // overflow path reused a stale matrix, drawing many objects at one position/scale
    // (huge/shifted/not clickable). 65536 (32768 w/ shadows) covers very dense scenes; if it
    // still overflows the draw is skipped instead of being drawn with a wrong matrix.
    static constexpr std::uint32_t kMaxInstancesPerDraw = 65536;
    static constexpr std::uint32_t kInstanceStride = 128; // float4x4 WVP + float4x4 world
    ComPtr<ID3D12Resource> instance_buffer; // UPLOAD, kFrameCount * kMaxInstancesPerDraw * stride
    struct InstanceMatrices {
        DirectX::XMFLOAT4X4 wvp{};
        DirectX::XMFLOAT4X4 world{};
    };
    std::uint8_t* instance_buffer_cpu = nullptr;
    std::uint32_t instance_write_cursor = 0; // within current frame region
    ComPtr<ID3D12Resource> render_targets[HelloO3dRenderer::kFrameCount];
    ComPtr<ID3D12Resource> scene_color[HelloO3dRenderer::kFrameCount];
    D3D12_CPU_DESCRIPTOR_HANDLE scene_rtv_cpu[HelloO3dRenderer::kFrameCount]{};
    D3D12_GPU_DESCRIPTOR_HANDLE scene_srv_gpu[HelloO3dRenderer::kFrameCount]{};
    // ---- Scene cache: reuse the last finished 3D image while nothing changed -----------------------
    // scene_epoch moves on every renderer change that alters the picture (mutators bump it only
    // when a value really changes). With the same epoch, view and size, populate copies the cached
    // image to the backbuffer and draws only the UI. Time-animated content (tree sway, water,
    // precipitation, sky clouds) still redraws at a reduced rate; a full redraw every 500 ms is
    // the safety net for any change made behind the mutators' back.
    std::uint64_t scene_epoch = 1;
    const char* scene_dirty_why = "";
    ComPtr<ID3D12Resource> scene_cache;
    D3D12_RESOURCE_STATES scene_cache_state = D3D12_RESOURCE_STATE_COPY_DEST;
    bool scene_cache_valid = false;
    std::uint64_t scene_cache_epoch = 0;
    DirectX::XMFLOAT4X4 scene_cache_vp{};
    float scene_cache_cam[3]{};
    UINT scene_cache_w = 0, scene_cache_h = 0;
    std::int64_t scene_last_full_ns = 0;
    std::uint64_t scene_last_tex_fence = 0;
    // previous full frame's signature (save the image only once the view stopped changing)
    std::uint64_t scene_prev_epoch = 0;
    DirectX::XMFLOAT4X4 scene_prev_vp{};
    bool frame_drew_sway = false;  // a wind-swayed chunk was drawn in the last full frame
    bool frame_drew_water = false;
    std::uint64_t scene_reused_frames = 0, scene_full_frames = 0;
    // set_chunk_visible changes of this frame: value before the first change (netted in populate)
    std::vector<std::pair<std::size_t, bool>> vis_before;
    std::unordered_set<std::size_t> vis_touched;
    // Offscreen MSAA (flip swapchain is always 1×). Resolved to scene_color or backbuffer.
    UINT msaa_samples = 1;
    UINT msaa_quality = 0;
    ComPtr<ID3D12Resource> msaa_color;
    D3D12_CPU_DESCRIPTOR_HANDLE msaa_rtv_cpu{};
    ComPtr<ID3D12RootSignature> fxaa_root_signature;
    ComPtr<ID3D12PipelineState> fxaa_pipeline_state; // alias: SMAA ready (neighborhood PSO)
    ComPtr<ID3D12PipelineState> smaa_edge_pso;
    ComPtr<ID3D12PipelineState> smaa_blend_pso;
    ComPtr<ID3D12PipelineState> smaa_neighborhood_pso;
    ComPtr<ID3D12Resource> smaa_edges;
    ComPtr<ID3D12Resource> smaa_blend;
    ComPtr<ID3D12Resource> smaa_area_tex;
    ComPtr<ID3D12Resource> smaa_search_tex;
    D3D12_CPU_DESCRIPTOR_HANDLE smaa_edges_rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE smaa_blend_rtv{};
    D3D12_GPU_DESCRIPTOR_HANDLE smaa_edges_srv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE smaa_blend_srv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE smaa_aux_srv_gpu{}; // edges..search table base
    ComPtr<ID3D12Resource> depth_buffer;
    D3D12_CPU_DESCRIPTOR_HANDLE depth_ro_dsv_cpu{}; // read-only DSV on depth_buffer (light shafts sample it)
    D3D12_CPU_DESCRIPTOR_HANDLE main_rtv_cpu{};     // scene RTV of the current main pass (shafts rebind it)
    D3D12_GPU_DESCRIPTOR_HANDLE depth_srv_gpu{};    // scene depth SRV (R32_FLOAT view of the typeless depth)
    ComPtr<ID3D12Resource> axis_vertex_buffer;
    ComPtr<ID3D12Resource> constant_buffer;
    ComPtr<ID3D12Resource> axis_constant_buffer;
    ComPtr<ID3D12Resource> white_texture;
    ComPtr<ID3D12Fence> fence;

    D3D12_VERTEX_BUFFER_VIEW axis_vertex_buffer_view{};
    std::uint32_t axis_vertex_count = 0;
    std::uint8_t* cb_ring_cpu = nullptr;
    AxisConstantBuffer* axis_cb_cpu = nullptr;
    XMFLOAT4X4 view_proj_transposed_{};
    bool has_view_proj_ = false;
    std::uint32_t scene_cb_slot = 0;
    D3D12_GPU_VIRTUAL_ADDRESS scene_cb_gpu_base = 0;
    bool draw_shadow_decals = true;
    bool clip_x_mirrored = false;
    std::uint32_t cb_overflow_frames = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE axis_cbv_gpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_heap_gpu{};
    float tile_cull_ox = 0.f;
    float tile_cull_oy = 0.f;
    float tile_cull_w = 300.f;
    float tile_cull_h = 300.f;
    float ambient_scale = 1.f;
    int scene_light_count = 0;
    float scene_lights[4][8]{};
    float sun_dir[3]{0.4f, 0.22f, 0.88f};
    // The sun direction the shadow cascades are fitted to: it follows sun_dir in steps of 1 degree, and every
    // cascade is redrawn in the frame it steps. With the clock running the sun moved every frame: every
    // cascade's matrix was refitted while only one cascade's map was redrawn (shadows flickered on a camera
    // turn) and the caster cache never held (12 fps).
    float shadow_sun_dir[3]{0.4f, 0.22f, 0.88f};
    bool shadow_dir_dirty = true;
    float sun_color[3]{1.f, 0.96f, 0.9f};
    float sun_shadow_enable = 1.f;
    std::uint32_t sun_shadow_map_size = 4096;
    int sun_shadow_cascade_i = 0; // staggered rebuild index 0..2
    float camera_world_pos[3]{0.f, 0.f, 20.f};
    // Render-only distance cull (metres; 0 = disabled). Chunks whose cached world AABB
    // centre is farther than this are not transformed/recorded. Terrain is exempt.
    float cull_max_distance = 0.f;
    float camera_forward[3]{0.f, 1.f, 0.f}; // unit look (eye → target)
    float camera_aspect = 1.6f;
    float camera_fov_y = 0.25f * 3.14159265f;
    float min_screen_size_pct = 0.f; // see set_min_screen_size_pct
    // Terrain tile LOD state (worker jobs hand finished index lists back through the shared block).
    struct LodJobResult {
        std::size_t chunk = 0;
        std::uint32_t gen = 0;
        std::vector<std::uint32_t> idx[GpuMeshChunk::kLodLevels];
    };
    struct LodShared {
        std::mutex mu;
        std::vector<LodJobResult> ready;
        std::atomic<int> inflight{0};
    };
    std::shared_ptr<LodShared> lod_shared = std::make_shared<LodShared>();
    bool terrain_lod_enabled = true;
    std::vector<std::size_t> lod_chunks; // chunk ids with lod_capable
    std::vector<std::size_t> detail_chunks; // chunk ids with a terrain detail mesh
    std::uint64_t lod_frame_counter = 0;
    // Replaced LOD index buffers stay alive a few frames (GPU may still be reading them).
    std::vector<std::pair<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>> lod_graveyard;
    // Cascaded shadow maps (3 cascades, Texture2DArray D32).
    static constexpr int kCsmCascades = 3;
    ComPtr<ID3D12Resource> csm_depth;
    ComPtr<ID3D12DescriptorHeap> csm_dsv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE csm_dsv_cpu[kCsmCascades]{};
    D3D12_GPU_DESCRIPTOR_HANDLE csm_srv_gpu{}; // in main srv_heap
    std::uint32_t csm_srv_slot = 0;
    ComPtr<ID3D12Resource> shadow_cb;
    std::uint8_t* shadow_cb_cpu = nullptr;     // this frame's copy (see shadow_cb_base_cpu)
    D3D12_GPU_VIRTUAL_ADDRESS shadow_cb_gpu = 0;
    // One shadow CB per frame in flight: rewritten in place, the GPU still drawing the frame before sampled the
    // old shadow map with the cascade matrices of the new frame (black patches flickering whenever the
    // matrices moved - the clock running). Each frame starts from the previous frame's copy.
    static constexpr UINT kShadowCbFrameBytes = 65536;
    float fog_scale = 1.f; // scene fog (Vista > Niebla), written to the shadow CB's gCamForward.w
    static constexpr std::uint32_t kFramesInFlight = HelloO3dRenderer::kFrameCount;
    std::uint8_t* shadow_cb_base_cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS shadow_cb_base_gpu = 0;
    ComPtr<ID3D12RootSignature> csm_root_signature;
    ComPtr<ID3D12PipelineState> csm_pso_opaque;
    ComPtr<ID3D12PipelineState> csm_pso_cutout;
    DirectX::XMFLOAT4X4 csm_light_vp[kCsmCascades]{};
    float csm_splits[3]{12.f, 40.f, 120.f};
    // Shadow maps are cached while both the snapped cascade matrices and all shadow-caster
    // inputs remain unchanged. This prevents clearing and drawing all cascades every frame.
    std::uint64_t csm_cached_matrix_hash = 0;
    std::uint64_t csm_cached_scene_hash = 0;
    bool csm_shadow_cache_valid = false;
    // Per-cascade stable light volume + cache validity (see csm_cache.hpp). The three legacy fields
    // above are unused now and only kept so nothing else that references them breaks.
    CsmCascadeState csm_cascade[kCsmCascades]{};
    std::uint64_t csm_basis_hash = 0;  // sun dir / res / fov / aspect / splits the volumes were fitted for
    float csm_light_axes[9]{};         // light right, up, dir (world) - used to cull casters per cascade
    bool csm_ready = false;
    // Per-frame scene state for the EnvCB globals (night blend 0..1, wind m/s in XY).
    float night_blend = 0.f;
    float wind_x = 0.f;
    float wind_y = 0.f;
    std::vector<std::uint32_t> season_rebind_queue;
    std::size_t season_rebind_cursor = 0;
    // (set when the pool has read the new season's files: the time-sliced uploads wait for it)
    std::shared_ptr<std::atomic<bool>> season_prefetch_ready = std::make_shared<std::atomic<bool>>(true);
    float clear_rgb[3]{0.f, 0.f, 0.f}; // black until the editor sets the sky colour (boot screen)
    std::vector<GpuMeshChunk> mesh_chunks;
    std::vector<std::size_t> highlighted_chunks;
    GpuMeshChunk ground_chunk{};
    bool has_ground_plane = false;
    std::size_t screen_rect_chunk = static_cast<std::size_t>(-1);
    bool screen_rect_enabled = false;
    std::vector<ComPtr<ID3D12Resource>> texture_resources;
    // Upload-heap copies; dropped after GPU wait (not kept for process lifetime).
    std::vector<ComPtr<ID3D12Resource>> texture_upload_scratch;
    // Live default-heap tex per SRV slot so [texttexture]/freetex replace can drop the previous one.
    std::unordered_map<std::uint32_t, ComPtr<ID3D12Resource>> texture_by_slot;
    // Replaced textures (reload_file_textures / season rebind). PBR tables and cache hits share SRV
    // descriptors via CopyDescriptorsSimple, so dropping the old default-heap resource made those
    // draws sample freed memory (pink frames). Retired resources stay alive here instead.
    std::vector<ComPtr<ID3D12Resource>> retired_textures;
    std::unordered_map<std::uint32_t, std::filesystem::path> texture_slot_files;
    /// Slots that must never season-rebind (UI thumbs, path rails, fixed previews).
    std::unordered_set<std::uint32_t> texture_slot_ignore_season;
    std::map<std::string, D3D12_GPU_DESCRIPTOR_HANDLE> texture_cache;
    std::map<std::string, std::uint32_t> texture_slot_cache;
    // set_chunk_freetex_variant: "<base slot>|<texture>" -> its descriptor table
    std::unordered_map<std::string, D3D12_GPU_DESCRIPTOR_HANDLE> freetex_variants;
    std::uint8_t editor_helper_mask = 0xFF; // helper kinds shown (Vista > objetos onlyeditor / Show carpark)
    // a variant texture: the slot it went up to (~0u: none to read), and the ones a worker still decodes
    std::unordered_map<std::string, std::uint32_t> variant_tex_slot;
    std::unordered_map<std::string, std::shared_ptr<void>> variant_jobs;
    std::uint64_t variant_frame = 0;            // frames drawn (bumped at the start of each frame's populate)
    std::vector<float> vehicle_coronas;         // set_vehicle_coronas: 12 floats each (pos+size, dir+flag, rgb+k)
    std::uint64_t variant_upload_frame = ~0ull; // variant_frame of the last variant texture uploaded
    std::uint32_t srv_stride = 0;
    std::uint32_t srv_next = 1;
    std::vector<std::uint32_t> text_slot_free; // slot pairs of released clones' text textures (AI buses' displays)
    bool srv_exhausted_logged = false;
    UINT64 fence_value = 1;
    UINT64 pending_tex_upload_fence = 0;
    bool pending_upload_reset = false;
    // Texture upload ring: several batches in flight instead of one per GPU round trip. The
    // current slot is mirrored in upload_allocator / upload_command_list / pending_tex_upload_fence.
    struct TexUploadSlot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        UINT64 fence = 0;
        std::vector<ComPtr<ID3D12Resource>> scratch;
    };
    static constexpr std::uint32_t kTexUploadSlots = 6;
    std::array<TexUploadSlot, kTexUploadSlots> tex_slots{};
    std::uint32_t tex_slot_cur = 0;
    HANDLE fence_event = nullptr;

    // GPU timestamp diagnostics.
    ComPtr<ID3D12QueryHeap> timestamp_query_heap;
    UINT64 timestamp_frequency = 0;
    double perf_last_gpu_ms = 0.0;
    double perf_last_gpu_scene_ms = 0.0;
    double perf_last_gpu_ui_ms = 0.0;

    // CPU/render diagnostics. Values are for the current frame; draw.cpp
    // accumulates and periodically emits a compact report.
    double perf_wait_texture_ms = 0.0;
    double perf_wait_frame_ms = 0.0;
    double perf_populate_ms = 0.0;
    double perf_cull_ms = 0.0;
    double perf_sort_ms = 0.0;
    double perf_classify_ms = 0.0;
    double perf_record_ms = 0.0;
    double perf_submit_ms = 0.0;
    double perf_present_ms = 0.0;
    std::uint64_t perf_chunks_tested = 0;
    std::uint64_t perf_chunks_visible = 0;
    std::uint64_t perf_chunks_culled = 0;
    std::uint64_t perf_tiles_seen = 0;
    std::uint64_t perf_tiles_visible = 0;
    std::uint64_t perf_vertices_visible = 0;
    std::uint64_t perf_indices_visible = 0;
    std::uint64_t perf_geometry_bytes_visible = 0;
    std::uint64_t perf_material_batches = 0;
    std::uint64_t perf_draws = 0;
    std::uint64_t perf_vb_binds = 0;
    std::uint64_t perf_instance_slots = 0;
    std::uint64_t perf_instanced_draws = 0;
    std::uint64_t perf_visible_objects_est = 0;
    // Instance-matrix ring diagnostics: proves/disproves "many objects → wrong transform".
    std::uint32_t perf_instance_cap = 0;           // effective cap this frame (half when shadows on)
    std::uint32_t perf_instance_overflow = 0;      // draws that reused a stale matrix this frame
    std::uint32_t perf_cb_slots_used = 0;          // scene CB slots consumed this frame
    std::uint32_t perf_cb_slots_cap = 0;           // per-frame scene CB budget
    std::uint64_t perf_report_frames = 0;
    double perf_sum_frame_ms = 0.0;
    double perf_sum_gpu_ms = 0.0;
    double perf_max_frame_ms = 0.0;
    double perf_max_gpu_ms = 0.0;
    double perf_sum_streaming_ms = 0.0;
    double perf_sum_prep_ms = 0.0; // populated by app-side streaming logs when available
    double perf_sum_populate_ms = 0.0;
    double perf_sum_cull_ms = 0.0;
    double perf_sum_sort_ms = 0.0;
    double perf_sum_classify_ms = 0.0;
    double perf_sum_record_ms = 0.0;
    double perf_sum_submit_ms = 0.0;
    double perf_sum_present_ms = 0.0;
    double perf_sum_wait_texture_ms = 0.0;
    double perf_sum_wait_frame_ms = 0.0;
    std::uint64_t perf_sum_tested = 0;
    std::uint64_t perf_sum_visible = 0;
    std::uint64_t perf_sum_culled = 0;
    std::uint64_t perf_sum_tiles = 0;
    std::uint64_t perf_sum_tiles_visible = 0;
    std::uint64_t perf_sum_vertices = 0;
    std::uint64_t perf_sum_indices = 0;
    std::uint64_t perf_sum_geometry_bytes = 0;
    std::uint64_t perf_sum_batches = 0;
    std::uint64_t perf_sum_draws = 0;
    std::uint64_t perf_sum_vb_binds = 0;
    std::uint64_t perf_sum_instance_slots = 0;
    std::uint64_t perf_sum_instanced_draws = 0;
    std::uint64_t perf_last_draw_log_frame = 0;
    std::string perf_hotspots;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_cpu{};
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    ComPtr<ID3D12Resource> screenshot_readback;
    UINT screenshot_readback_width = 0;
    UINT screenshot_readback_height = 0;
    std::filesystem::path pending_screenshot_path;
};

// ---- Scene cache dirty marking (see HelloO3dRenderer::Impl::scene_epoch) ----------------------------
inline void scene_dirty(HelloO3dRenderer::Impl& d, const char* why) {
    ++d.scene_epoch;
    d.scene_dirty_why = why;
}
inline std::uint64_t scene_hash_bytes(const void* p, std::size_t n, std::uint64_t h = 1469598103934665603ull) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
// Floats quantized to 1/1024 so slow drifts (sun, sidereal time) do not dirty every frame.
inline std::uint64_t scene_hash_floats_q(const float* v, std::size_t n, std::uint64_t h = 1469598103934665603ull) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::int64_t q = static_cast<std::int64_t>(std::llround(static_cast<double>(v[i]) * 1024.0));
        h = scene_hash_bytes(&q, sizeof q, h);
    }
    return h;
}
inline void scene_dirty_if(HelloO3dRenderer::Impl& d, std::uint64_t& last, std::uint64_t h, const char* why) {
    if (h == last) return;
    last = h;
    scene_dirty(d, why);
}

bool ensure_csm_resources(HelloO3dRenderer::Impl& d);
void update_csm_matrices(HelloO3dRenderer::Impl& d, int only_cascade = -1);
// only_cascade >= 0: that cascade alone this frame (the others keep their slice and matrix); -1 all stale ones
void render_csm_pass(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd, std::uint32_t frame_index,
                     int only_cascade = -1, bool serial = false, int also_cascade = -1);
void csm_end_frame_barrier(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd);
bool ensure_sky_resources(HelloO3dRenderer::Impl& d);
void draw_sky_pass(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd, std::uint32_t frame, float vp_w,
                   float vp_h);
void draw_water_pass(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd, std::uint32_t frame, float vp_w,
                     float vp_h);
void draw_precip_pass(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd, std::uint32_t frame, float vp_w,
                      float vp_h);
void draw_shaft_pass(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd, std::uint32_t frame, float vp_w,
                     float vp_h);
bool sw_create_water_precip_pipelines(HelloO3dRenderer::Impl& d);
void draw_corona_pass(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd, std::uint32_t frame, float vp_w,
                      float vp_h);
ComPtr<ID3D12Resource> sw_upload_buffer(HelloO3dRenderer::Impl& d, const void* src, UINT bytes);
bool sw_compile(const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& out);
void sw_default_pso_desc(HelloO3dRenderer::Impl& d, D3D12_GRAPHICS_PIPELINE_STATE_DESC& pso);
ComPtr<ID3D12PipelineState> sw_make_pso(HelloO3dRenderer::Impl& d, const char* body, const char* vs_entry,
                                        const char* ps_entry, D3D12_GRAPHICS_PIPELINE_STATE_DESC& pso);
D3D12_GPU_VIRTUAL_ADDRESS sw_bind_common(HelloO3dRenderer::Impl& d, std::uint32_t frame, std::uint32_t pass,
                                         float vp_w, float vp_h);

} // namespace omsi::gfx::dx12
