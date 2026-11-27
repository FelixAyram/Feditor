// OMSI sky: THimmel-style dome (himmel01 day / himmel04 dusk / himmel05 night, blended by sun altitude
// exactly like OMSI's sky_w_* weights), cloud layers (Weather\clouds.cfg textures), sun + moon discs,
// star field (helper\stars.dat) and horizon haze from the weather preset.
// Water, rain and snow live in water.cpp; they share the root signature and constant layout from here.

#include "gfx/dx12/hello_o3d_renderer.hpp"
#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/o3d/internal.hpp"
#include "gfx/dx12/o3d/sky_weather.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/logger.hpp"

#include <DirectXMath.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace omsi::gfx::dx12 {
namespace {

using DirectX::XMFLOAT4X4;

constexpr float kPiF = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPiF / 180.f;
constexpr float kDomeRadius = 8000.f;

const char* const kSkyBody = R"(
struct SkyVS { float3 pos : POSITION; };
struct SkyPSIn { float4 pos : SV_POSITION; float3 dir : TEXCOORD0; };
SkyPSIn vs_sky(SkyVS vin) {
    SkyPSIn o;
    float3 world = gCam.xyz + vin.pos * 8000.0;
    o.pos = mul(float4(world, 1.0), gViewProj);
    o.pos.z = 0.0; // reverse-Z far plane
    o.dir = normalize(vin.pos);
    return o;
}
// OMSI himmel textures: image rows run horizon (bottom, ~0.017) to zenith (top, ~0.976).
float2 skyUV(float3 d) {
    float e = asin(clamp(d.z, 0.0, 1.0));
    float frac = 0.0167 + 0.9596 * (e / 1.5707963);
    float u = atan2(d.y, d.x) / 6.2831853 + 0.5;
    return float2(u, 1.0 - frac);
}
float4 cloudTex(int ti, float2 uv) {
    if (ti == 4) return gTex[4].SampleLevel(gSamp, uv, 0);
    if (ti == 5) return gTex[5].SampleLevel(gSamp, uv, 0);
    if (ti == 6) return gTex[6].SampleLevel(gSamp, uv, 0);
    return gTex[3].SampleLevel(gSamp, uv, 0);
}
float4 ps_sky(SkyPSIn pin) : SV_TARGET {
    float3 d = normalize(pin.dir);
    float2 uv = skyUV(d);
    float3 day   = gTex[0].SampleLevel(gSamp, uv, 0).rgb;
    float3 dusk  = gTex[1].SampleLevel(gSamp, uv, 0).rgb;
    float3 night = gTex[2].SampleLevel(gSamp, uv, 0).rgb;
    float3 col = day * gDayW.x + dusk * gDayW.y + night * gDayW.z;
    // Horizon haze from the weather preset (fog range).
    float hz = gStarCfg.w * (1.0 - saturate(d.z * 4.0));
    col = lerp(col, gFog.rgb, hz);
    // Sun: hard disc + soft glow (intensity 0 when the sun is below the horizon).
    float sd = saturate(dot(d, gSunDir.xyz));
    float disk = smoothstep(0.99985, 0.99995, sd);
    float glow = pow(sd, 400.0) * 0.8 + pow(sd, 24.0) * 0.12;
    col += gSunColor.rgb * gSunDir.w * (disk * 6.0 + glow);
    // Moon (approximate position, no phase).
    float md = saturate(dot(d, gMoonDir.xyz));
    float moon = smoothstep(0.9993, 0.9997, md) * gStarCfg.z;
    col += float3(0.85, 0.9, 1.0) * moon * 1.1 + float3(0.12, 0.14, 0.18) * pow(md, 60.0) * gStarCfg.z;
    // Cloud layer: intersect the view ray with the base plane, sample the cloud texture, light it.
    float cover = gCloud0.x;
    if (cover > 0.001 && d.z > 0.004) {
        float h = gCloud0.y - gCam.z;
        if (h > 0.0) {
            float t = h / d.z;
            float2 p = gCam.xy + d.xy * t;
            float2 cuv = (p + gCloudWind.xy * gCloudWind.z * gCam.w) / gCloud0.z;
            float4 cs = cloudTex((int)gCloud0.w, cuv);
            float dens = (gExtra.x > 0.5) ? cs.a : cs.r;
            float c = saturate((dens - (1.0 - cover)) * 3.0);
            float fade = saturate(1.0 - t / 25000.0) * saturate(d.z * 6.0);
            float a = c * fade;
            float lit = saturate((gDayW.w + 6.0) / 16.0);
            float3 cc = lerp(float3(0.30, 0.32, 0.36), gSunColor.rgb, lit) * gCloudWind.w;
            cc *= 1.0 - 0.25 * saturate((cover - 0.8) * 5.0); // overcast reads darker
            col = lerp(col, cc, a);
        }
    }
    return float4(col, 1.0);
}

struct StarIn { float4 sd : STAR; uint vid : SV_VertexID; };
struct StarOut { float4 pos : SV_POSITION; float4 col : COLOR0; };
// Equatorial (RA, Dec) -> local ENU (x east, y north, z up) from local sidereal time and latitude.
StarOut vs_star(StarIn i) {
    StarOut o;
    float H = gStar.x - i.sd.x;
    float cd = cos(i.sd.y), sdn = sin(i.sd.y);
    float cH = cos(H), sH = sin(H);
    float cp = cos(gStar.y), sp = sin(gStar.y);
    float3 dir = float3(-cd * sH, cp * sdn - sp * cd * cH, cp * cd * cH + sp * sdn);
    float mag = saturate(0.25 + 0.75 * i.sd.z / 3.84);
    float alpha = gStarCfg.y * mag * saturate(dir.z * 25.0) * (1.0 - gStarCfg.w) * (1.0 - 0.7 * gStarCfg.z);
    float4 p = mul(float4(gCam.xyz + dir * 7000.0, 1.0), gViewProj);
    float2 corner = float2((i.vid == 1 || i.vid == 4 || i.vid == 5) ? 1.0 : -1.0,
                           (i.vid == 2 || i.vid == 3 || i.vid == 5) ? 1.0 : -1.0);
    float rad = gStarCfg.x * (0.6 + 0.9 * mag);
    p.xy += corner * rad * 2.0 / gStar.zw * p.w;
    p.z = 0.0;
    o.pos = p;
    o.col = float4(float3(0.85, 0.9, 1.0) * alpha, 1.0);
    return o;
}
float4 ps_star(StarOut i) : SV_TARGET { return i.col; }
)";

constexpr D3D12_INPUT_ELEMENT_DESC kSkyLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};
constexpr D3D12_INPUT_ELEMENT_DESC kStarLayout[] = {
    {"STAR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
};

bool create_sw_root(HelloO3dRenderer::Impl& d) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kSwTexCount;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    // CSM table for light shafts: the existing cascade depth SRV (Texture2DArray, t11).
    D3D12_DESCRIPTOR_RANGE csm_range{};
    csm_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    csm_range.NumDescriptors = 1;
    csm_range.BaseShaderRegister = 11;
    csm_range.OffsetInDescriptorsFromTableStart = 0;

    // Scene depth SRV for light shafts (R32 view of the depth buffer, t12).
    D3D12_DESCRIPTOR_RANGE depth_range{};
    depth_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    depth_range.NumDescriptors = 1;
    depth_range.BaseShaderRegister = 12;
    depth_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rp[5]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[0].Descriptor.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &range;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[2].Descriptor.ShaderRegister = 3; // ShadowCB (same buffer the scene uses)
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[3].DescriptorTable.NumDescriptorRanges = 1;
    rp[3].DescriptorTable.pDescriptorRanges = &csm_range;
    rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samps[2]{};
    D3D12_STATIC_SAMPLER_DESC& samp = samps[0];
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // s1: CSM comparison (LESS_EQUAL, reverse-free standard depth as the scene uses).
    D3D12_STATIC_SAMPLER_DESC& cmp = samps[1];
    cmp.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    cmp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    cmp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    cmp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    cmp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    cmp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    cmp.MaxLOD = D3D12_FLOAT32_MAX;
    cmp.ShaderRegister = 1;
    cmp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rp[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[4].DescriptorTable.NumDescriptorRanges = 1;
    rp[4].DescriptorTable.pDescriptorRanges = &depth_range;
    rp[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rs.NumParameters = 5;
    rs.pParameters = rp;
    rs.NumStaticSamplers = 2;
    rs.pStaticSamplers = samps;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig_blob, sig_err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.GetAddressOf(),
                                           sig_err.GetAddressOf()))) {
        omsi::log::warn("SkyWeather: SerializeRootSignature failed");
        return false;
    }
    if (FAILED(d.device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                             IID_PPV_ARGS(d.sw.root.GetAddressOf())))) {
        omsi::log::warn("SkyWeather: CreateRootSignature failed");
        return false;
    }
    return true;
}

// Dome mesh (unit sphere, Z-up). Scaled to kDomeRadius in the shader.
bool upload_dome(HelloO3dRenderer::Impl& d) {
    if (d.sw.dome_index_count > 0) return true;
    constexpr int kStacks = 24;
    constexpr int kSlices = 48;
    std::vector<float> verts;
    verts.reserve(static_cast<size_t>((kStacks + 1) * (kSlices + 1) * 3));
    for (int i = 0; i <= kStacks; ++i) {
        const float phi = static_cast<float>(i) / static_cast<float>(kStacks) * kPiF;
        const float sp = std::sin(phi), cp = std::cos(phi);
        for (int j = 0; j <= kSlices; ++j) {
            const float theta = static_cast<float>(j) / static_cast<float>(kSlices) * 2.f * kPiF;
            verts.push_back(sp * std::sin(theta));
            verts.push_back(sp * std::cos(theta));
            verts.push_back(cp);
        }
    }
    std::vector<std::uint16_t> idx;
    idx.reserve(static_cast<size_t>(kStacks * kSlices * 6));
    for (int i = 0; i < kStacks; ++i)
        for (int j = 0; j < kSlices; ++j) {
            const auto a = static_cast<std::uint16_t>(i * (kSlices + 1) + j);
            const auto b = static_cast<std::uint16_t>(a + 1);
            const auto c = static_cast<std::uint16_t>(a + (kSlices + 1));
            const auto e = static_cast<std::uint16_t>(c + 1);
            idx.insert(idx.end(), {a, c, b, b, c, e});
        }
    const UINT vb_bytes = static_cast<UINT>(verts.size() * sizeof(float));
    const UINT ib_bytes = static_cast<UINT>(idx.size() * sizeof(std::uint16_t));
    d.sw.dome_vb = sw_upload_buffer(d, verts.data(), vb_bytes);
    d.sw.dome_ib = sw_upload_buffer(d, idx.data(), ib_bytes);
    if (!d.sw.dome_vb || !d.sw.dome_ib) return false;
    d.sw.dome_vbv.BufferLocation = d.sw.dome_vb->GetGPUVirtualAddress();
    d.sw.dome_vbv.StrideInBytes = sizeof(float) * 3;
    d.sw.dome_vbv.SizeInBytes = vb_bytes;
    d.sw.dome_ibv.BufferLocation = d.sw.dome_ib->GetGPUVirtualAddress();
    d.sw.dome_ibv.Format = DXGI_FORMAT_R16_UINT;
    d.sw.dome_ibv.SizeInBytes = ib_bytes;
    d.sw.dome_index_count = static_cast<std::uint32_t>(idx.size());
    return true;
}

bool create_sky_and_star_pipelines(HelloO3dRenderer::Impl& d) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    sw_default_pso_desc(d, pso);
    pso.InputLayout = {kSkyLayout, 1};
    pso.DepthStencilState.DepthEnable = FALSE; // sky sits at the far plane, drawn first
    d.sw.sky_pso = sw_make_pso(d, kSkyBody, "vs_sky", "ps_sky", pso);
    if (!d.sw.sky_pso) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC spso{};
    sw_default_pso_desc(d, spso);
    spso.InputLayout = {kStarLayout, 1};
    spso.DepthStencilState.DepthEnable = FALSE;
    auto& rt = spso.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_ONE;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    d.sw.stars_pso = sw_make_pso(d, kSkyBody, "vs_star", "ps_star", spso);
    return static_cast<bool>(d.sw.stars_pso);
}

// Reserve the 10-descriptor SRV block once; fill it with white until textures are bound.
void reserve_texture_block(HelloO3dRenderer::Impl& d) {
    if (d.sw.tex_table.ptr != 0) return;
    d.sw.tex_base_slot = d.srv_next;
    d.srv_next += kSwTexCount;
    const D3D12_CPU_DESCRIPTOR_HANDLE white = offset_cpu_handle(
        d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount);
    for (std::uint32_t i = 0; i < kSwTexCount; ++i) {
        const D3D12_CPU_DESCRIPTOR_HANDLE dst = offset_cpu_handle(
            d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
            kCbvDescriptorCount + d.sw.tex_base_slot + i);
        d.device->CopyDescriptorsSimple(1, dst, white, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    d.sw.tex_table = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, d.sw.tex_base_slot);
}

void load_sky_textures(HelloO3dRenderer& r, HelloO3dRenderer::Impl& d, const std::filesystem::path& root) {
    const std::filesystem::path t = root / "Texture";
    const std::vector<std::filesystem::path> paths = {
        t / "himmel01.bmp", t / "himmel04.bmp", t / "himmel05.bmp", // day, dusk, night (envir.cfg order)
        t / "Cumulus_1.tga", t / "Cumulus_3.tga", t / "OVC_1.bmp", t / "OVC_2.tga",
        t / "water.tga", t / "water_bump.bmp", t / "water_envmap.bmp"};
    if (d.sw.tex_attempts == 0) prefetch_textures_cpu(paths); // decode on worker threads; copies only do GPU work
    ++d.sw.tex_attempts;
    bool all_ok = true;
    for (std::uint32_t i = 0; i < kSwTexCount; ++i) {
        const D3D12_CPU_DESCRIPTOR_HANDLE dst = offset_cpu_handle(
            d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
            kCbvDescriptorCount + d.sw.tex_base_slot + i);
        std::error_code ec;
        bool ok = false;
        if (std::filesystem::is_regular_file(paths[i], ec))
            ok = r.copy_file_texture_srv(paths[i], static_cast<std::uint64_t>(dst.ptr));
        if (!ok) {
            all_ok = false;
            // Transient failures (upload allocator busy) retry on the next frame; meanwhile white.
            const D3D12_CPU_DESCRIPTOR_HANDLE white = offset_cpu_handle(
                d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kCbvDescriptorCount);
            d.device->CopyDescriptorsSimple(1, dst, white, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }
    if (all_ok) {
        d.sw.textures_ready = true;
    } else if (d.sw.tex_attempts >= 120) {
        omsi::log::warn("SkyWeather: some OMSI sky/water textures missing or undecodable; white fallback");
        d.sw.textures_ready = true;
    }
}

bool load_star_catalogue(HelloO3dRenderer::Impl& d, const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return false;
    std::vector<float> data; // per star: RA (rad), Dec (rad), brightness, pad
    data.reserve(6000 * 4);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        double ra = 0.0, dec = 0.0, mag = 0.0;
        if (!(ss >> ra >> dec >> mag)) continue;
        data.push_back(static_cast<float>(ra) * kDeg2Rad);
        data.push_back(static_cast<float>(dec) * kDeg2Rad);
        data.push_back(static_cast<float>(mag));
        data.push_back(0.f);
    }
    if (data.empty()) return false;
    const UINT bytes = static_cast<UINT>(data.size() * sizeof(float));
    d.sw.star_vb = sw_upload_buffer(d, data.data(), bytes);
    if (!d.sw.star_vb) return false;
    d.sw.star_vbv.BufferLocation = d.sw.star_vb->GetGPUVirtualAddress();
    d.sw.star_vbv.StrideInBytes = sizeof(float) * 4;
    d.sw.star_vbv.SizeInBytes = bytes;
    d.sw.star_count = static_cast<std::uint32_t>(data.size() / 4);
    return true;
}

float sw_time_seconds() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

ComPtr<ID3D12Resource> sw_upload_buffer(HelloO3dRenderer::Impl& d, const void* src, UINT bytes) {
    ComPtr<ID3D12Resource> buf;
    const CD3DX12_HEAP_PROPERTIES upload(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    if (FAILED(d.device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(buf.GetAddressOf()))))
        return {};
    void* mapped = nullptr;
    if (FAILED(buf->Map(0, nullptr, &mapped)) || !mapped) return {};
    std::memcpy(mapped, src, bytes);
    buf->Unmap(0, nullptr);
    return buf;
}

bool sw_compile(const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(src, std::strlen(src), "skyweather", nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) {
        if (err)
            omsi::log::warn(std::string("SkyWeather shader ") + entry + ": " +
                            static_cast<const char*>(err->GetBufferPointer()));
        return false;
    }
    return true;
}

void sw_default_pso_desc(HelloO3dRenderer::Impl& d, D3D12_GRAPHICS_PIPELINE_STATE_DESC& pso) {
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.MultisampleEnable = d.msaa_samples > 1 ? TRUE : FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    // Reverse-Z scene: cleared to 0, geometry passes when depth >= stored.
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = (std::max)(1u, d.msaa_samples);
    pso.SampleDesc.Quality = d.msaa_samples > 1 ? d.msaa_quality : 0;
}

ComPtr<ID3D12PipelineState> sw_make_pso(HelloO3dRenderer::Impl& d, const char* body, const char* vs_entry,
                                        const char* ps_entry, D3D12_GRAPHICS_PIPELINE_STATE_DESC& pso) {
    const std::string full = std::string(kSwCbHlsl) + body;
    ComPtr<ID3DBlob> vs, ps;
    if (!sw_compile(full.c_str(), vs_entry, "vs_5_0", vs) || !sw_compile(full.c_str(), ps_entry, "ps_5_0", ps))
        return {};
    pso.pRootSignature = d.sw.root.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    ComPtr<ID3D12PipelineState> out;
    if (FAILED(d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(out.GetAddressOf())))) {
        omsi::log::warn(std::string("SkyWeather: CreateGraphicsPipelineState failed (") + vs_entry + ")");
        return {};
    }
    return out;
}

bool ensure_sky_resources(HelloO3dRenderer::Impl& d) {
    if (!d.device) return false;
    auto& sw = d.sw;
    if (!sw.root && !create_sw_root(d)) return false;
    if (!sw.sky_pso || !sw.stars_pso) {
        if (!create_sky_and_star_pipelines(d)) return false;
    }
    if (!sw.water_pso || !sw.precip_pso || !sw.shaft_pso) {
        if (!sw_create_water_precip_pipelines(d)) return false;
    }
    if (!sw.cb_ring) {
        const UINT total = kSwCbStride * static_cast<UINT>(std::size(d.frames)) * kSwPassCount;
        const CD3DX12_HEAP_PROPERTIES upload(D3D12_HEAP_TYPE_UPLOAD);
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(total);
        if (FAILED(d.device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(sw.cb_ring.GetAddressOf()))))
            return false;
        throw_hr(sw.cb_ring->Map(0, nullptr, reinterpret_cast<void**>(&sw.cb_cpu)), "sky weather cb map");
        std::memset(sw.cb_cpu, 0, total);
    }
    if (!upload_dome(d)) return false;
    reserve_texture_block(d);
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS sw_bind_common(HelloO3dRenderer::Impl& d, std::uint32_t frame, std::uint32_t pass,
                                         float vp_w, float vp_h) {
    auto& sw = d.sw;
    const std::uint32_t slot = (frame % static_cast<std::uint32_t>(std::size(d.frames))) * kSwPassCount + pass;
    const std::uint64_t off = static_cast<std::uint64_t>(slot) * kSwCbStride;
    auto* cb = reinterpret_cast<SwCbCpu*>(sw.cb_cpu + off);
    const auto& e = sw.env;

    cb->view_proj = d.view_proj_transposed_;
    cb->cam[0] = d.camera_world_pos[0];
    cb->cam[1] = d.camera_world_pos[1];
    cb->cam[2] = d.camera_world_pos[2];
    cb->cam[3] = sw_time_seconds();
    cb->zenith[0] = d.sky_zenith[0];
    cb->zenith[1] = d.sky_zenith[1];
    cb->zenith[2] = d.sky_zenith[2];
    cb->zenith[3] = e.night_w;
    cb->horizon[0] = d.sky_horizon[0];
    cb->horizon[1] = d.sky_horizon[1];
    cb->horizon[2] = d.sky_horizon[2];
    cb->horizon[3] = e.dusk_w;
    cb->sun_dir[0] = e.sun_dir[0];
    cb->sun_dir[1] = e.sun_dir[1];
    cb->sun_dir[2] = e.sun_dir[2];
    cb->sun_dir[3] = e.sun_intensity;
    cb->sun_color[0] = e.sun_color[0];
    cb->sun_color[1] = e.sun_color[1];
    cb->sun_color[2] = e.sun_color[2];
    cb->sun_color[3] = 0.f;
    cb->day_w[0] = e.day_w;
    cb->day_w[1] = e.dusk_w;
    cb->day_w[2] = e.night_w;
    cb->day_w[3] = e.sun_alt_deg;

    const int ctype = e.cloud_type;
    const bool cloud_tga = (ctype == 1 || ctype == 2 || ctype == 4);
    const float cloud_tex_m = (ctype == 1 || ctype == 2) ? 2000.f : 1000.f; // clouds.cfg texturesize
    cb->cloud0[0] = e.cloud_cover;
    cb->cloud0[1] = e.cloud_base_m;
    cb->cloud0[2] = cloud_tex_m;
    cb->cloud0[3] = ctype >= 1 && ctype <= 4 ? static_cast<float>(kSwTexCloudFirst + ctype - 1) : 0.f;
    cb->cloud_wind[0] = std::sin(e.cloud_wind_to_rad);
    cb->cloud_wind[1] = std::cos(e.cloud_wind_to_rad);
    cb->cloud_wind[2] = e.cloud_wind_speed;
    cb->cloud_wind[3] = e.cloud_brightness;

    cb->star[0] = e.lst_rad;
    cb->star[1] = e.lat_rad;
    cb->star[2] = vp_w > 0.f ? vp_w : 1280.f;
    cb->star[3] = vp_h > 0.f ? vp_h : 720.f;
    cb->star_cfg[0] = 1.15f;
    cb->star_cfg[1] = e.star_brightness;
    cb->star_cfg[2] = e.moon_intensity;
    cb->star_cfg[3] = e.haze;
    cb->fog[0] = e.fog_rgb[0];
    cb->fog[1] = e.fog_rgb[1];
    cb->fog[2] = e.fog_rgb[2];
    cb->fog[3] = e.fog_density;
    cb->moon_dir[0] = e.moon_dir[0];
    cb->moon_dir[1] = e.moon_dir[1];
    cb->moon_dir[2] = e.moon_dir[2];
    cb->moon_dir[3] = 0.f;
    cb->extra[0] = cloud_tga ? 1.f : 0.f;
    cb->extra[1] = e.precip_intensity;
    cb->extra[2] = static_cast<float>(e.precip_type);
    cb->extra[3] = e.god_ray_strength;
    // Inverse view-projection in the same transposed storage convention as view_proj.
    const DirectX::XMMATRIX vp = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&d.view_proj_transposed_));
    DirectX::XMStoreFloat4x4(&cb->inv_vp, DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, vp)));
    // Light shafts: Normal = 8 steps / 260 m; Plus = 32 steps / 380 m with stronger in-scatter.
    cb->shaft[0] = e.shaft_plus ? 32.f : 8.f;
    cb->shaft[1] = e.shaft_plus ? 1.8f : 1.0f;
    cb->shaft[2] = e.shaft_plus ? 380.f : 260.f;
    cb->shaft[3] = e.shaft_plus ? 0.78f : 0.6f;          // Henyey-Greenstein g: strong forward glow in Plus
    cb->shaft2[0] = e.shaft_plus ? 0.06f : 0.f;          // visible from the side (phase floor)
    cb->shaft2[1] = e.shaft_plus ? 0.45f : 0.f;          // drifting dust: density modulation depth
    cb->shaft2[2] = e.shaft_plus ? 0.6f : 0.f;           // drift speed
    // OMSI_SHAFT_DEBUG=1: visibility. OMSI_SHAFT_PROBE=1 (with OMSI_SHAFT_DEBUG=1): depth probe.
    static const bool s_shaft_probe = [] {
        const char* p = std::getenv("OMSI_SHAFT_PROBE");
        return p && p[0] == '1';
    }();
    cb->shaft2[3] = e.shaft_debug ? (s_shaft_probe ? 2.f : 1.f) : 0.f;
    cb->shaft3[0] = e.ground_z;
    cb->shaft3[1] = cb->shaft3[2] = cb->shaft3[3] = 0.f;
    return sw.cb_ring->GetGPUVirtualAddress() + off;
}

void draw_sky_pass(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd, std::uint32_t frame, float vp_w,
                   float vp_h) {
    if (!cmd || !d.sky_enabled || !d.has_view_proj_) return;
    if (!ensure_sky_resources(d)) return;
    const D3D12_GPU_VIRTUAL_ADDRESS cb = sw_bind_common(d, frame, kSwPassSky, vp_w, vp_h);

    cmd->SetGraphicsRootSignature(d.sw.root.Get());
    cmd->SetPipelineState(d.sw.sky_pso.Get());
    cmd->SetGraphicsRootConstantBufferView(0, cb);
    cmd->SetGraphicsRootDescriptorTable(1, d.sw.tex_table);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &d.sw.dome_vbv);
    cmd->IASetIndexBuffer(&d.sw.dome_ibv);
    cmd->DrawIndexedInstanced(d.sw.dome_index_count, 1, 0, 0, 0);

    const auto& e = d.sw.env;
    if (d.sw.stars_pso && d.sw.star_count > 0 && e.star_brightness > 0.01f) {
        cmd->SetPipelineState(d.sw.stars_pso.Get());
        cmd->IASetVertexBuffers(0, 1, &d.sw.star_vbv);
        cmd->DrawInstanced(6, d.sw.star_count, 0, 0);
    }

    // Restore the scene root signature for the geometry that follows.
    if (d.root_signature) cmd->SetGraphicsRootSignature(d.root_signature.Get());
}

// Called by the app once per frame (cheap: constants only) and once with the OMSI root to load assets.
void HelloO3dRenderer::set_sky_environment(const SkyEnvironment& env) {
    if (impl_) { static std::uint64_t s_h_ = 0; const auto* f_ = reinterpret_cast<const float*>(&env.sun_dir[0]); const std::size_t n_ = (sizeof(SkyEnvironment) - offsetof(SkyEnvironment, sun_dir)) / sizeof(float); scene_dirty_if(*impl_, s_h_, scene_hash_floats_q(f_, n_), "sky_env"); }
    if (!impl_) return;
    auto& d = *impl_;
    if (!ensure_sky_resources(d)) return;
    if (!env.asset_root.empty()) {
        if (!d.sw.textures_ready) load_sky_textures(*this, d, env.asset_root);
        if (d.sw.star_count == 0) load_star_catalogue(d, env.asset_root / "helper" / "stars.dat");
    }
    d.sw.env = env;
    d.sw.env_set = true;
}

} // namespace omsi::gfx::dx12
