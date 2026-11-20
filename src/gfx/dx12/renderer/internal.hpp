#pragma once

#include "gfx/dx12/common/dx12_common.hpp"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>
#include <stdexcept>

namespace omsi::gfx::dx12 {
namespace {

using Microsoft::WRL::ComPtr;

ComPtr<ID3DBlob> compile_shader(const char* source, const char* entry, const char* profile) {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, profile, flags, 0,
                                  blob.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        if (errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        throw_hr(hr, "D3DCompile failed");
    }
    return blob;
}

const char* kShaderSource = R"(
cbuffer DrawCB : register(b0) {
    float4x4 view_proj;
    float4x4 object;
};
Texture2D albedo : register(t0);
SamplerState samp0 : register(s0);
struct VSIn { float3 pos:POSITION; float2 uv:TEXCOORD0; float3 nrm:TEXCOORD1; };
struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float3 nrm:TEXCOORD1; };
VSOut vs_main(VSIn vin) {
    VSOut o;
    float4 w = mul(object, float4(vin.pos, 1));
    o.pos = mul(view_proj, w);
    o.uv = vin.uv;
    o.nrm = mul((float3x3)object, vin.nrm);
    return o;
}
float4 ps_main(VSOut pin):SV_TARGET {
    float3 n = normalize(pin.nrm);
    float ndl = saturate(dot(n, normalize(float3(0.35, 0.25, 0.9))));
    float3 base = albedo.Sample(samp0, pin.uv).rgb;
    if (dot(base, float3(1,1,1)) < 0.001) base = float3(0.75, 0.2, 0.85);
    return float4(base * (0.35 + 0.65 * ndl), 1);
}
)";

} // namespace
} // namespace omsi::gfx::dx12
