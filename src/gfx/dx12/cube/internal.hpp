#pragma once

#include "gfx/dx12/hello_cube_renderer.hpp"
#include "gfx/dx12/common/dx12_common.hpp"

#include <DirectXMath.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace omsi::gfx::dx12 {
namespace {

using Microsoft::WRL::ComPtr;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;
using DirectX::XMFLOAT4X4;

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT4 color;
};

struct SceneConstantBuffer {
    XMFLOAT4X4 world_view_proj;
};

constexpr std::uint32_t kConstantBufferSize = (sizeof(SceneConstantBuffer) + 255u) & ~255u;

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
cbuffer SceneCB : register(b0) {
    float4x4 gWorldViewProj;
};
struct VSIn {
    float3 pos : POSITION;
    float4 color : COLOR;
};
struct VSOut {
    float4 pos : SV_POSITION;
    float4 color : COLOR;
};
VSOut vs_main(VSIn vin) {
    VSOut o;
    o.pos = mul(float4(vin.pos, 1.0f), gWorldViewProj);
    o.color = vin.color;
    return o;
}
float4 ps_main(VSOut pin) : SV_TARGET {
    return pin.color;
}
)";

} // namespace

using Microsoft::WRL::ComPtr;

struct HelloCubeRenderer::Impl {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    ComPtr<ID3D12DescriptorHeap> cbv_heap;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline_state;
    ComPtr<ID3D12CommandAllocator> command_allocator;
    ComPtr<ID3D12GraphicsCommandList> command_list;
    ComPtr<ID3D12Resource> render_targets[HelloCubeRenderer::kFrameCount];
    ComPtr<ID3D12Resource> depth_buffer;
    ComPtr<ID3D12Resource> vertex_buffer;
    ComPtr<ID3D12Resource> index_buffer;
    ComPtr<ID3D12Resource> constant_buffer;
    ComPtr<ID3D12Fence> fence;

    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view{};
    D3D12_INDEX_BUFFER_VIEW index_buffer_view{};
    SceneConstantBuffer* cb_cpu = nullptr;
    std::uint32_t index_count = 0;
    UINT64 fence_value = 1;
    HANDLE fence_event = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_cpu{};
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
};

} // namespace omsi::gfx::dx12
