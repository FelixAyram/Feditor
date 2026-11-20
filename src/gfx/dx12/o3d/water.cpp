// OMSI water tiles (TFileWater: one quad per tile, 4 corner heights) and OMSI weather particles
// (rain = raindrop-style streaks, snow = flakes). Water uses OMSI's own water.tga (base colour),
// water_bump.bmp (normal waves) and water_envmap.bmp (reflection), with fresnel, sky reflection,
// sun glint, body tint and distance fog from the weather preset.

#include "gfx/dx12/hello_o3d_renderer.hpp"
#include "gfx/dx12/o3d/internal.hpp"
#include "gfx/dx12/o3d/sky_weather.hpp"
#include "omsi/logger.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace omsi::gfx::dx12 {
namespace {

const char* const kWaterBody = R"(
struct WaVS { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct WaPS { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float2 uv : TEXCOORD1; };
WaPS vs_water(WaVS v) {
    WaPS o;
    o.wpos = v.pos;
    o.uv = v.uv;
    o.pos = mul(float4(v.pos, 1.0), gViewProj);
    return o;
}
float wBump(float2 uv) { return gTex[8].SampleLevel(gSamp, uv, 0).r; }
float4 ps_water(WaPS i) : SV_TARGET {
    float t = gCam.w;
    float2 wd = gCloudWind.xy;                 // direction the wind pushes the waves
    float spd = 0.6 + gCloudWind.z * 0.15;
    float2 p = i.wpos.xy;
    // Two scrolling layers of OMSI water_bump (large slow swell + small fast chop).
    float2 uvA = p / 18.0 + wd * t * 0.020 * spd;
    float2 uvB = p / 7.0 + float2(-wd.y, wd.x) * t * 0.035 * spd + 0.37;
    float e = 1.0 / 256.0;
    float2 gA = float2(wBump(uvA + float2(e, 0)) - wBump(uvA - float2(e, 0)),
                       wBump(uvA + float2(0, e)) - wBump(uvA - float2(0, e)));
    float2 gB = float2(wBump(uvB + float2(e, 0)) - wBump(uvB - float2(e, 0)),
                       wBump(uvB + float2(0, e)) - wBump(uvB - float2(0, e)));
    float S = 20.0 * (0.6 + 0.4 * saturate(gCloudWind.z * 0.2));
    float3 n = normalize(float3(-(gA + gB * 0.6) * S, 1.0));

    float3 v = normalize(gCam.xyz - i.wpos);
    float ndv = saturate(dot(n, v));
    float fres = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
    float3 r = reflect(-v, n);
    // Sky reflection: same zenith/horizon gradient as the dome, modulated by OMSI envmap.
    float3 sky = lerp(gHorizon.rgb, gZenith.rgb, pow(saturate(r.z), 0.6));
    float3 env = gTex[9].SampleLevel(gSamp, r.xy * 0.5 + 0.5, 0).rgb;
    sky *= lerp(float3(1.0, 1.0, 1.0), env * 1.6, 0.35);
    float sp = pow(saturate(dot(r, gSunDir.xyz)), 350.0) * gSunDir.w;
    sky += gSunColor.rgb * sp * 4.0;
    // Body colour: OMSI water.tga tinted; darker at night and when the sun is low.
    float3 base = gTex[7].SampleLevel(gSamp, p / 24.0 + wd * t * 0.005, 0).rgb;
    float light = saturate(gSunDir.z * 1.2 + 0.15) * (1.0 - 0.7 * gDayW.z);
    float3 body = lerp(float3(0.02, 0.10, 0.13), base * 1.6, 0.5) * (0.25 + 0.75 * light);
    float3 col = lerp(body, sky, saturate(fres + 0.06));
    // Distance fog from the weather preset.
    float dist = length(gCam.xyz - i.wpos);
    col = lerp(col, gFog.rgb, saturate(1.0 - exp(-gFog.w * dist)));
    float alpha = saturate(0.62 + 0.38 * fres);
    return float4(col, alpha);
}
)";

const char* const kPrecipBody = R"(
struct PrPS { float4 pos : SV_POSITION; float4 col : COLOR0; };
PrPS vs_precip(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    PrPS o;
    o.pos = float4(0.0, 0.0, 2.0, 1.0); // off-screen unless an active particle
    o.col = float4(0.0, 0.0, 0.0, 0.0);
    int type = (int)gExtra.z;
    float inten = gExtra.y;
    if (type <= 0 || inten <= 0.0) return o;
    if (swHash(iid * 7u + 5u) > inten) return o;
    float speed = (type == 1) ? 9.0 : 1.1;
    float bx = (swHash(iid * 3u) * 2.0 - 1.0) * 22.0;
    float by = (swHash(iid * 3u + 1u) * 2.0 - 1.0) * 22.0;
    float fall = frac(swHash(iid * 3u + 2u) - gCam.w * speed / 28.0);
    float3 p = float3(gCam.x + bx, gCam.y + by, gCam.z + 14.0 - fall * 28.0);
    if (type == 2) {
        p.x += sin(gCam.w * 1.3 + float(iid)) * 0.35;
        p.y += cos(gCam.w * 1.1 + float(iid) * 0.7) * 0.35;
    }
    float3 toCam = normalize(gCam.xyz - p);
    float3 side = normalize(cross(float3(0.0, 0.0, 1.0), toCam) + float3(1e-5, 0.0, 0.0));
    float u = (vid == 2 || vid == 3 || vid == 5) ? 1.0 : 0.0;
    float s = (vid == 0 || vid == 2 || vid == 3) ? -1.0 : 1.0;
    float3 world;
    if (type == 1) {
        // Rain streak: thin camera-facing quad along the vertical.
        float halfLen = 0.45;
        world = p + float3(0.0, 0.0, 1.0) * ((u * 2.0 - 1.0) * halfLen) + side * (s * 0.012);
        o.col = float4(float3(0.72, 0.78, 0.9), 0.42 * inten);
    } else {
        // Snow flake: small square billboard.
        float sz = 0.035;
        world = p + float3(0.0, 0.0, 1.0) * ((u * 2.0 - 1.0) * sz) + side * (s * sz);
        o.col = float4(float3(0.95, 0.97, 1.0), 0.85 * inten);
    }
    o.pos = mul(float4(world, 1.0), gViewProj);
    return o;
}
float4 ps_precip(PrPS i) : SV_TARGET { return i.col; }
)";

// [light_enh] / [light_enh_2] coronas (OMSI SDK: the glow of a vehicle's lamp - a camera-facing picture at the
// lamp, its size in metres, its colour; a [light_enh_2] with an axis shows towards that axis only, an omni one
// all round). They light nothing (Omsi.exe's lamps light the world with [spotlight] / [interiorlight] only).
const char* const kCoronaBody = R"(
struct CoPS { float4 pos : SV_POSITION; float2 q : TEXCOORD0; float4 col : COLOR0; };
CoPS vs_corona(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    CoPS o;
    o.pos = float4(0.0, 0.0, 2.0, 1.0);
    o.q = float2(0.0, 0.0);
    o.col = float4(0.0, 0.0, 0.0, 0.0);
    if (iid >= (uint)gExtra.w) return o;
    const float4 a = gCorona[iid * 3u];
    const float4 dr = gCorona[iid * 3u + 1u];
    const float4 c = gCorona[iid * 3u + 2u];
    const float3 p = a.xyz;
    float3 toCam = gCam.xyz - p;
    const float dist = max(length(toCam), 1e-3);
    toCam /= dist;
    // towards its axis only (the lens of a brake light is dark from the front)
    float k = 1.0;
    if (dr.w > 0.5) k = smoothstep(-0.05, 0.55, dot(normalize(dr.xyz), toCam));
    if (k <= 0.001) return o;
    const float3 side = normalize(cross(float3(0.0, 0.0, 1.0), toCam) + float3(1e-5, 0.0, 0.0));
    const float3 up = cross(toCam, side);
    const float u = (vid == 2 || vid == 3 || vid == 5) ? 1.0 : -1.0;
    const float v = (vid == 0 || vid == 2 || vid == 3) ? -1.0 : 1.0;
    // the glow keeps a few pixels' size far off (a lamp seen 200 m away still twinkles)
    const float sz = max(a.w, dist * 0.0025);
    const float3 w = p + toCam * 0.05 + side * (u * sz) + up * (v * sz);
    o.pos = mul(float4(w, 1.0), gViewProj);
    o.q = float2(u, v);
    o.col = float4(c.rgb, c.a * k);
    return o;
}
float4 ps_corona(CoPS i) : SV_TARGET {
    const float r2 = dot(i.q, i.q);
    if (r2 >= 1.0) discard;
    const float g = (1.0 - r2);
    const float core = g * g * g;
    return float4(i.col.rgb * (core * 1.6 + g * 0.25) * i.col.a, 1.0);
}
)";

struct WaterVertex {
    float x, y, z;
    float u, v;
};

constexpr D3D12_INPUT_ELEMENT_DESC kWaterLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};

void set_blend_alpha(D3D12_GRAPHICS_PIPELINE_STATE_DESC& pso) {
    auto& rt = pso.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
}

} // namespace

bool sw_create_water_precip_pipelines(HelloO3dRenderer::Impl& d) {
    if (!d.sw.root) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC wp{};
    sw_default_pso_desc(d, wp);
    wp.InputLayout = {kWaterLayout, 2};
    set_blend_alpha(wp);
    d.sw.water_pso = sw_make_pso(d, kWaterBody, "vs_water", "ps_water", wp);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pp{};
    sw_default_pso_desc(d, pp);
    pp.InputLayout = {nullptr, 0}; // procedural quads from SV_VertexID / SV_InstanceID
    set_blend_alpha(pp);
    d.sw.precip_pso = sw_make_pso(d, kPrecipBody, "vs_precip", "ps_precip", pp);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC cp{};
    sw_default_pso_desc(d, cp);
    cp.InputLayout = {nullptr, 0};
    {
        auto& rt = cp.BlendState.RenderTarget[0]; // additive: a glow over what is behind it
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ONE;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
        rt.DestBlendAlpha = D3D12_BLEND_ONE;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    d.sw.corona_pso = sw_make_pso(d, kCoronaBody, "vs_corona", "ps_corona", cp); // (optional: no glows if it fails)
    return d.sw.water_pso && d.sw.precip_pso;
}

// Builds one mesh for all visible water tiles (world space, 8x8 cells per tile, both sides via CULL_NONE).
void HelloO3dRenderer::set_water_quads(const WaterQuad* quads, std::uint32_t count) {
    if (impl_) scene_dirty(*impl_, "set_water_quads");
    if (!impl_) return;
    auto& d = *impl_;
    if (!d.device) return;
    auto& sw = d.sw;
    if (sw.water_vb) {
        // Keep the previous buffers alive while frames that reference them are still in flight.
        sw.retired_water.push_back(sw.water_vb);
        sw.retired_water.push_back(sw.water_ib);
        while (sw.retired_water.size() > 12) sw.retired_water.pop_front();
    }
    sw.water_vb.Reset();
    sw.water_ib.Reset();
    sw.water_index_count = 0;
    if (!quads || count == 0) return;

    constexpr std::uint32_t kSeg = 8;
    std::vector<WaterVertex> verts;
    std::vector<std::uint32_t> inds;
    verts.reserve(static_cast<std::size_t>(count) * (kSeg + 1) * (kSeg + 1));
    inds.reserve(static_cast<std::size_t>(count) * kSeg * kSeg * 6);
    for (std::uint32_t q = 0; q < count; ++q) {
        const WaterQuad& wq = quads[q];
        const std::uint32_t base = static_cast<std::uint32_t>(verts.size());
        for (std::uint32_t j = 0; j <= kSeg; ++j)
            for (std::uint32_t i = 0; i <= kSeg; ++i) {
                const float u = static_cast<float>(i) / kSeg;
                const float v = static_cast<float>(j) / kSeg;
                const float z = wq.h[0] * (1.f - u) * (1.f - v) + wq.h[1] * u * (1.f - v) +
                                wq.h[2] * (1.f - u) * v + wq.h[3] * u * v;
                verts.push_back({wq.origin_x + u * wq.size_m, wq.origin_y + v * wq.size_m, z, u, v});
            }
        for (std::uint32_t j = 0; j < kSeg; ++j)
            for (std::uint32_t i = 0; i < kSeg; ++i) {
                const std::uint32_t a = base + j * (kSeg + 1) + i, b = a + 1, c = a + (kSeg + 1), dd = c + 1;
                inds.insert(inds.end(), {a, c, b, b, c, dd});
            }
    }
    const UINT vb_bytes = static_cast<UINT>(verts.size() * sizeof(WaterVertex));
    const UINT ib_bytes = static_cast<UINT>(inds.size() * sizeof(std::uint32_t));
    sw.water_vb = sw_upload_buffer(d, verts.data(), vb_bytes);
    sw.water_ib = sw_upload_buffer(d, inds.data(), ib_bytes);
    if (!sw.water_vb || !sw.water_ib) {
        sw.water_vb.Reset();
        sw.water_ib.Reset();
        return;
    }
    sw.water_vbv.BufferLocation = sw.water_vb->GetGPUVirtualAddress();
    sw.water_vbv.StrideInBytes = sizeof(WaterVertex);
    sw.water_vbv.SizeInBytes = vb_bytes;
    sw.water_ibv.BufferLocation = sw.water_ib->GetGPUVirtualAddress();
    sw.water_ibv.Format = DXGI_FORMAT_R32_UINT;
    sw.water_ibv.SizeInBytes = ib_bytes;
    sw.water_index_count = static_cast<std::uint32_t>(inds.size());
}

void draw_water_pass(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd, std::uint32_t frame, float vp_w,
                     float vp_h) {
    auto& sw = d.sw;
    if (!cmd || !d.has_view_proj_ || !sw.water_pso || !sw.water_vb || sw.water_index_count == 0) return;
    const D3D12_GPU_VIRTUAL_ADDRESS cb = sw_bind_common(d, frame, kSwPassWater, vp_w, vp_h);
    cmd->SetGraphicsRootSignature(sw.root.Get());
    cmd->SetPipelineState(sw.water_pso.Get());
    cmd->SetGraphicsRootConstantBufferView(0, cb);
    cmd->SetGraphicsRootDescriptorTable(1, sw.tex_table);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &sw.water_vbv);
    cmd->IASetIndexBuffer(&sw.water_ibv);
    cmd->DrawIndexedInstanced(sw.water_index_count, 1, 0, 0, 0);
    if (d.root_signature) cmd->SetGraphicsRootSignature(d.root_signature.Get());
}

void draw_precip_pass(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd, std::uint32_t frame, float vp_w,
                      float vp_h) {
    auto& sw = d.sw;
    if (!cmd || !d.has_view_proj_ || !sw.precip_pso || !sw.env_set) return;
    if (sw.env.precip_type <= 0 || sw.env.precip_intensity <= 0.f) return;
    const D3D12_GPU_VIRTUAL_ADDRESS cb = sw_bind_common(d, frame, kSwPassPrecip, vp_w, vp_h);
    cmd->SetGraphicsRootSignature(sw.root.Get());
    cmd->SetPipelineState(sw.precip_pso.Get());
    cmd->SetGraphicsRootConstantBufferView(0, cb);
    cmd->SetGraphicsRootDescriptorTable(1, sw.tex_table);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(6, kSwPrecipMax, 0, 0);
    if (d.root_signature) cmd->SetGraphicsRootSignature(d.root_signature.Get());
}

void draw_corona_pass(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd, std::uint32_t frame, float vp_w,
                      float vp_h) {
    auto& sw = d.sw;
    if (!cmd || !d.has_view_proj_ || !sw.corona_pso || !sw.env_set) return;
    const std::size_t n = (std::min)(d.vehicle_coronas.size() / 12u, static_cast<std::size_t>(kSwCoronaMax));
    if (n == 0) return;
    const D3D12_GPU_VIRTUAL_ADDRESS cb = sw_bind_common(d, frame, kSwPassCorona, vp_w, vp_h);
    // the list right after the common part of this pass's slot (gExtra.w = how many)
    const std::uint32_t slot = (frame % static_cast<std::uint32_t>(std::size(d.frames))) * kSwPassCount + kSwPassCorona;
    std::uint8_t* base = sw.cb_cpu + static_cast<std::uint64_t>(slot) * kSwCbStride;
    reinterpret_cast<SwCbCpu*>(base)->extra[3] = static_cast<float>(n);
    std::memcpy(base + sizeof(SwCbCpu), d.vehicle_coronas.data(), n * 12u * sizeof(float));
    cmd->SetGraphicsRootSignature(sw.root.Get());
    cmd->SetPipelineState(sw.corona_pso.Get());
    cmd->SetGraphicsRootConstantBufferView(0, cb);
    cmd->SetGraphicsRootDescriptorTable(1, sw.tex_table);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(6, static_cast<UINT>(n), 0, 0);
    if (d.root_signature) cmd->SetGraphicsRootSignature(d.root_signature.Get());
}

} // namespace omsi::gfx::dx12
