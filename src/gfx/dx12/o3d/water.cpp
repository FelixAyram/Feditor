// OMSI water tiles (TFileWater: one quad per tile, 4 corner heights) and OMSI weather particles
// (rain = raindrop-style streaks, snow = flakes). Water uses OMSI's own water.tga (base colour),
// water_bump.bmp (normal waves) and water_envmap.bmp (reflection), with fresnel, sky reflection,
// sun glint, body tint and distance fog from the weather preset.

#include "gfx/dx12/hello_o3d_renderer.hpp"
#include "gfx/dx12/o3d/internal.hpp"
#include "gfx/dx12/o3d/sky_weather.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
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

// Volumetric light shafts: per pixel, march the view ray up to kShaftDist metres and accumulate sun
// visibility from the cascaded shadow map (same CSM the scene samples). Henyey-Greenstein forward
// scattering toward the sun, warm sun colour from the environment. Additive, full screen.
const char* const kShaftBody = R"(
cbuffer ShadowCBSw : register(b3) {
    float4x4 gLightVP[3];
    float4 gSplits;   // xyz view-space cascade ends
    float4 gShCam;
    float4 gShBias;   // x depth bias
    float4 gCamFwd;
};
Texture2DArray gShadowSw : register(t11);
#if SHAFT_MS
Texture2DMS<float> gSceneDepth : register(t12); // scene depth (reverse-Z), MSAA: sample 0 only
#else
Texture2D<float> gSceneDepth : register(t12);   // scene depth (reverse-Z, 0 = nothing drawn)
#endif
SamplerComparisonState gShCmp : register(s1);
struct ShVS { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };
ShVS vs_shaft(uint vid : SV_VertexID) {
    ShVS o;
    float2 p = float2((vid << 1) & 2, vid & 2);
    o.ndc = p * float2(2.0, -2.0) + float2(-1.0, 1.0);
    o.pos = float4(o.ndc, 0.0, 1.0);
    return o;
}
// Returns (visibility, coverage). Samples outside the shadow map footprint are NOT counted as lit: the
// old code treated them as sun, which filled every far / sideways ray with uniform glow and hid the
// canopy shadows. They are dropped from the integral instead.
float2 shaftShadow(float3 p, float viewZ) {
    if (viewZ > gSplits.z || viewZ < 0.0) return float2(0.0, 0.0);
    int ci = 2;
    if (viewZ < gSplits.x) ci = 0;
    else if (viewZ < gSplits.y) ci = 1;
    float4 lp = mul(float4(p, 1.0), gLightVP[ci]);
    float3 ndc = lp.xyz / max(lp.w, 1e-5);
    float2 uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z < 0.0 || ndc.z > 1.0) return float2(0.0, 0.0);
    float vis = gShadowSw.SampleCmpLevelZero(gShCmp, float3(uv, (float)ci), ndc.z - gShBias.x);
    return float2(vis, 1.0);
}
float shaftHash(float2 c) { return frac(sin(dot(c, float2(127.1, 311.7))) * 43758.5453); }
float shaftNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = shaftHash(i);
    float b = shaftHash(i + float2(1.0, 0.0));
    float c = shaftHash(i + float2(0.0, 1.0));
    float d = shaftHash(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
// Distance along the view ray to this pixel's nearest opaque surface (metres). Reverse-Z: the cleared value is
// 0 where nothing was drawn (sky), which means "no limit". Beams stop there, so trees and poles stay dark in
// front of the light instead of being washed out by the haze behind them.
float shaftSurfaceDist(float2 ndc, float3 dir) {
    float2 uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
#if SHAFT_MS
    uint dw = 0, dh = 0, dn = 0;
    gSceneDepth.GetDimensions(dw, dh, dn);
    int2 px = (int2)clamp(uv * float2(dw, dh), float2(0.0, 0.0), float2(dw, dh) - 1.0);
    float z = gSceneDepth.Load(px, 0);
#else
    uint dw = 0, dh = 0;
    gSceneDepth.GetDimensions(dw, dh);
    int2 px = (int2)clamp(uv * float2(dw, dh), float2(0.0, 0.0), float2(dw, dh) - 1.0);
    float z = gSceneDepth.Load(int3(px, 0));
#endif
    if (z <= 0.0) return 1e9;
    float4 wp = mul(float4(ndc.x, ndc.y, z, 1.0), gInvViewProj);
    float3 ws = wp.xyz / max(wp.w, 1e-6);
    return dot(ws - gCam.xyz, dir);
}
float4 ps_shaft(ShVS pin) : SV_TARGET {
    float strength = gExtra.w;
    if (strength <= 0.001) return float4(0.0, 0.0, 0.0, 0.0);
    float4 wp = mul(float4(pin.ndc, 0.5, 1.0), gInvViewProj);
    float3 wpos = wp.xyz / max(wp.w, 1e-6);
    float3 dir = normalize(wpos - gCam.xyz);
    // Beams fade toward the horizon instead of ending on a hard line at dir.z = 0.
    const float horizonFade = smoothstep(0.0, 0.2, abs(dir.z));
    if (horizonFade <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    const float maxT = gShaftCfg.z;
    const int steps = (int)gShaftCfg.x;
    float tmin = 0.0;
    float tmax = maxT;
    // Near cascades only (view depth < gSplits.y): the far cascades are too coarse for beams between trunks.
    const float cosF = dot(dir, gCamFwd.xyz);
    if (cosF > 1e-3) tmax = min(tmax, gSplits.y / cosF);
    // Downward rays end at the ground under the camera. Below it the terrain is in the CSM and would read as shadow.
    if (dir.z < -1e-3) tmax = min(tmax, max(0.0, (gShaftCfg3.x + 0.2 - gCam.z) / dir.z));
    // Scene depth: stop just in front of this pixel's first opaque surface.
    tmax = min(tmax, shaftSurfaceDist(pin.ndc, dir) - 0.3);
    if (tmax <= tmin) return float4(0.0, 0.0, 0.0, 0.0);
    float L = tmax - tmin;
    float stepLen = L / (float)steps;
    float jit = frac(52.9829189 * frac(dot(pin.pos.xy, float2(0.06711056, 0.00583715))));
    float cs = dot(dir, gSunDir.xyz);
    float g = gShaftCfg.w;
    float g2 = g * g;
    // Phase capped: the forward peak near the sun was blooming instead of reading as beams.
    float phase = min((1.0 - g2) / (4.0 * 3.14159265 * pow(1.0 + g2 - 2.0 * g * cs, 1.5)), 0.8);
    phase += gShaftCfg2.x;   // side visibility floor (Plus)
    float vis[32];           // per-step visibility (dust-modulated), for the contrast pass
    float cov[32];           // per-step shadow-map coverage
    float visSum = 0.0;      // sum of visibility over covered samples
    float covSum = 0.0;      // number of covered samples
    [loop] for (int i = 0; i < 32; ++i) {
        if (i >= steps) break;
        float t = tmin + (float(i) + jit) * stepLen;
        float3 p = gCam.xyz + dir * t;
        float dust = 1.0;
        if (gShaftCfg2.y > 0.0) {
            // Drifting dust / haze density: the beams seem to move slightly.
            float2 drift = gCloudWind.xy * gCam.w * gShaftCfg2.z;
            dust = 1.0 - gShaftCfg2.y * shaftNoise(p.xy * 0.03 + drift);
        }
        float2 sv = shaftShadow(p, dot(p - gCam.xyz, gCamFwd.xyz));
        vis[i] = sv.x * sv.y * dust;
        cov[i] = sv.y;
        visSum += vis[i];
        covSum += sv.y;
    }
    if (gShaftCfg2.w > 1.5) {
        // OMSI_SHAFT_PROBE: one sample at mid-ray. R = cascade (0 / 0.5 / 1), G = light-space depth of the
        // sample, B = depth stored in the shadow map at its UV. Shadowed where G > B; B = 1 means nothing drawn.
        float tm = 0.5 * (tmin + tmax);
        float3 pm = gCam.xyz + dir * tm;
        float vz = dot(pm - gCam.xyz, gCamFwd.xyz);
        if (vz < 0.0 || vz > gSplits.z) return float4(0.0, 0.0, 0.25, 1.0);
        int pc = 2;
        if (vz < gSplits.x) pc = 0;
        else if (vz < gSplits.y) pc = 1;
        float4 plp = mul(float4(pm, 1.0), gLightVP[pc]);
        float3 pndc = plp.xyz / max(plp.w, 1e-5);
        float2 puv = float2(pndc.x * 0.5 + 0.5, -pndc.y * 0.5 + 0.5);
        uint tw = 0, th = 0, te = 0;
        gShadowSw.GetDimensions(tw, th, te);
        float stored = gShadowSw.Load(int4((int2)(puv * float2((float)tw, (float)th)), pc, 0)).r;
        return float4((float)pc * 0.5, saturate(pndc.z), saturate(stored), 1.0);
    }
    if (gShaftCfg2.w > 0.5) {
        // OMSI_SHAFT_DEBUG: R/Y = visibility over covered samples, B = shadow-map coverage along the ray.
        float vm = visSum / max(covSum, 1.0);
        return float4(vm, vm * 0.5, covSum / (float)steps, 1.0);
    }
    // Contrast: only the lit steps above the ray's average count, so the shadowed gaps between trunks stay dark
    // and the light reads as bands. The uniform part of the ray is kept small.
    const float avg = visSum / max(covSum, 1.0);
    float hp = 0.0;
    [loop] for (int j = 0; j < 32; ++j) {
        if (j >= steps) break;
        hp += cov[j] * max(vis[j] - avg, 0.0);
    }
    // Soft clamp 1 - exp(-x): bounded per pixel, so it cannot white out the frame. High sun (noon): almost off.
    const float density = 0.010 * strength * gShaftCfg.y;
    const float x = phase * density * stepLen * (0.2 * visSum + hp);
    const float sunScale = 1.0 - 0.85 * smoothstep(35.0, 65.0, gDayW.w);
    float3 scat = gSunColor.rgb * gSunDir.w * (1.0 - exp(-x)) * 0.5 * sunScale * horizonFade;
    return float4(scat, 1.0);
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

    D3D12_GRAPHICS_PIPELINE_STATE_DESC sp{};
    sw_default_pso_desc(d, sp);
    sp.DepthStencilState.DepthEnable = FALSE; // full screen, no depth interaction
    sp.InputLayout = {nullptr, 0};
    auto& srt = sp.BlendState.RenderTarget[0];
    srt.BlendEnable = TRUE;
    srt.SrcBlend = D3D12_BLEND_ONE;
    srt.DestBlend = D3D12_BLEND_ONE;
    srt.BlendOp = D3D12_BLEND_OP_ADD;
    srt.SrcBlendAlpha = D3D12_BLEND_ONE;
    srt.DestBlendAlpha = D3D12_BLEND_ONE;
    srt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    d.sw.shaft_pso = sw_make_pso(d, kShaftBody, "vs_shaft", "ps_shaft", sp); // (optional: no shafts if it fails)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC sdp = sp; // debug readouts: the same shader, written over the scene
    sdp.BlendState.RenderTarget[0].BlendEnable = FALSE;
    d.sw.shaft_debug_pso = sw_make_pso(d, kShaftBody, "vs_shaft", "ps_shaft", sdp);
    // MSAA variants: the scene depth is Texture2DMS and is read at sample 0.
    const std::string shaft_ms = std::string("#define SHAFT_MS 1\n") + kShaftBody;
    d.sw.shaft_pso_ms = sw_make_pso(d, shaft_ms.c_str(), "vs_shaft", "ps_shaft", sp);
    d.sw.shaft_debug_pso_ms = sw_make_pso(d, shaft_ms.c_str(), "vs_shaft", "ps_shaft", sdp);
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

namespace omsi::gfx::dx12 {
// Light shafts. Needs the CSM (sun shadows on): the cascade depth SRV and ShadowCB are bound directly.
void draw_shaft_pass(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd, std::uint32_t frame, float vp_w,
                     float vp_h) {
    auto& sw = d.sw;
    if (!cmd || !d.has_view_proj_ || !sw.shaft_pso || !sw.env_set) return;
    if (sw.env.god_ray_strength <= 0.001f || !d.csm_ready || !d.shadow_cb_gpu) return;
    if (d.sun_shadow_enable < 0.5f || d.csm_srv_gpu.ptr == 0) return;
    if (!d.depth_buffer || d.depth_srv_gpu.ptr == 0 || d.depth_ro_dsv_cpu.ptr == 0 || d.main_rtv_cpu.ptr == 0) return;
    const bool ms = d.msaa_samples > 1;
    const bool dbg = sw.env.shaft_debug;
    ID3D12PipelineState* pso = nullptr;
    if (ms) pso = (dbg && sw.shaft_debug_pso_ms) ? sw.shaft_debug_pso_ms.Get() : sw.shaft_pso_ms.Get();
    else pso = (dbg && sw.shaft_debug_pso) ? sw.shaft_debug_pso.Get() : sw.shaft_pso.Get();
    if (!pso) return;
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        omsi::log::info(std::string("light shafts: active (plus=") + (sw.env.shaft_plus ? "1" : "0") +
                        ", strength=" + std::to_string(sw.env.god_ray_strength) + ")");
    }
    const D3D12_GPU_VIRTUAL_ADDRESS cb = sw_bind_common(d, frame, kSwPassShafts, vp_w, vp_h);
    // The scene depth is sampled while bound read-only: DEPTH_READ state plus the read-only DSV (no write hazard).
    const D3D12_RESOURCE_BARRIER to_read = dx12_transition_barrier(
        d.depth_buffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
    cmd->ResourceBarrier(1, &to_read);
    cmd->OMSetRenderTargets(1, &d.main_rtv_cpu, FALSE, &d.depth_ro_dsv_cpu);
    cmd->SetGraphicsRootSignature(sw.root.Get());
    cmd->SetPipelineState(pso);
    cmd->SetGraphicsRootConstantBufferView(0, cb);
    cmd->SetGraphicsRootDescriptorTable(1, sw.tex_table);
    cmd->SetGraphicsRootConstantBufferView(2, d.shadow_cb_gpu);
    cmd->SetGraphicsRootDescriptorTable(3, d.csm_srv_gpu);
    cmd->SetGraphicsRootDescriptorTable(4, d.depth_srv_gpu);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
    // Writable DSV and DEPTH_WRITE again: later passes and the next frame's depth clear need them.
    cmd->OMSetRenderTargets(1, &d.main_rtv_cpu, FALSE, &d.dsv_cpu);
    const D3D12_RESOURCE_BARRIER to_write = dx12_transition_barrier(
        d.depth_buffer.Get(), D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(1, &to_write);
    if (d.root_signature) cmd->SetGraphicsRootSignature(d.root_signature.Get());
}
} // namespace omsi::gfx::dx12
