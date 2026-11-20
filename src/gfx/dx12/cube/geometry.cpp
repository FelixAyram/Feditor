#include "gfx/dx12/hello_cube_renderer.hpp"

#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/cube/internal.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "omsi/logger.hpp"

#include <DirectXMath.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace omsi::gfx::dx12 {

bool HelloCubeRenderer::create_geometry() {
    auto& d = *impl_;

    // Frank Luna d3d12book Chapter 6 box (8 corner colors).
    const Vertex vertices[8] = {
        {{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f, 1.f}},
        {{-1.f, +1.f, -1.f}, {0.f, 0.f, 0.f, 1.f}},
        {{+1.f, +1.f, -1.f}, {1.f, 0.f, 0.f, 1.f}},
        {{+1.f, -1.f, -1.f}, {0.f, 1.f, 0.f, 1.f}},
        {{-1.f, -1.f, +1.f}, {0.f, 0.f, 1.f, 1.f}},
        {{-1.f, +1.f, +1.f}, {1.f, 1.f, 0.f, 1.f}},
        {{+1.f, +1.f, +1.f}, {0.f, 1.f, 1.f, 1.f}},
        {{+1.f, -1.f, +1.f}, {1.f, 0.f, 1.f, 1.f}},
    };
    const std::uint16_t indices[36] = {
        0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 4, 5, 1, 4, 1, 0,
        3, 2, 6, 3, 6, 7, 1, 5, 6, 1, 6, 2, 4, 0, 3, 4, 3, 7,
    };
    d.index_count = 36;

    const UINT vertex_buffer_size = static_cast<UINT>(sizeof(vertices));
    const UINT index_buffer_size = static_cast<UINT>(sizeof(indices));

    const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC vb_desc = CD3DX12_RESOURCE_DESC::Buffer(vertex_buffer_size);
    throw_hr(d.device->CreateCommittedResource(
                 &upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(d.vertex_buffer.GetAddressOf())),
             "vertex buffer");
    const CD3DX12_RESOURCE_DESC ib_desc = CD3DX12_RESOURCE_DESC::Buffer(index_buffer_size);
    throw_hr(d.device->CreateCommittedResource(
                 &upload_heap, D3D12_HEAP_FLAG_NONE, &ib_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(d.index_buffer.GetAddressOf())),
             "index buffer");

    void* mapped = nullptr;
    throw_hr(d.vertex_buffer->Map(0, nullptr, &mapped), "vb map");
    std::memcpy(mapped, vertices, vertex_buffer_size);
    d.vertex_buffer->Unmap(0, nullptr);

    throw_hr(d.index_buffer->Map(0, nullptr, &mapped), "ib map");
    std::memcpy(mapped, indices, index_buffer_size);
    d.index_buffer->Unmap(0, nullptr);

    d.vertex_buffer_view.BufferLocation = d.vertex_buffer->GetGPUVirtualAddress();
    d.vertex_buffer_view.StrideInBytes = sizeof(Vertex);
    d.vertex_buffer_view.SizeInBytes = vertex_buffer_size;

    d.index_buffer_view.BufferLocation = d.index_buffer->GetGPUVirtualAddress();
    d.index_buffer_view.SizeInBytes = index_buffer_size;
    d.index_buffer_view.Format = DXGI_FORMAT_R16_UINT;

    const CD3DX12_RESOURCE_DESC cb_desc = CD3DX12_RESOURCE_DESC::Buffer(kConstantBufferSize);
    throw_hr(d.device->CreateCommittedResource(
                 &upload_heap, D3D12_HEAP_FLAG_NONE, &cb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(d.constant_buffer.GetAddressOf())),
             "constant buffer");

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc{};
    cbv_desc.BufferLocation = d.constant_buffer->GetGPUVirtualAddress();
    cbv_desc.SizeInBytes = kConstantBufferSize;
    d.device->CreateConstantBufferView(&cbv_desc, d.cbv_heap->GetCPUDescriptorHandleForHeapStart());

    throw_hr(d.constant_buffer->Map(0, nullptr, reinterpret_cast<void**>(&d.cb_cpu)), "cb map");
    std::memset(d.cb_cpu, 0, kConstantBufferSize);
    return true;
}

void HelloCubeRenderer::create_depth_buffer(std::uint32_t width, std::uint32_t height) {
    auto& d = *impl_;
    d.depth_buffer.Reset();
    if (!d.dsv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
        dsv_heap_desc.NumDescriptors = 1;
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        throw_hr(d.device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(d.dsv_heap.GetAddressOf())), "DSV heap");
        d.dsv_cpu = d.dsv_heap->GetCPUDescriptorHandleForHeapStart();
    }

    CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC depth_desc{};
    depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_desc.Width = width;
    depth_desc.Height = height;
    depth_desc.DepthOrArraySize = 1;
    depth_desc.MipLevels = 1;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = DXGI_FORMAT_D32_FLOAT;
    clear_value.DepthStencil.Depth = 1.f;

    throw_hr(d.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value,
                                               IID_PPV_ARGS(d.depth_buffer.GetAddressOf())),
             "depth buffer");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    d.device->CreateDepthStencilView(d.depth_buffer.Get(), &dsv_desc, d.dsv_cpu);
}

} // namespace omsi::gfx::dx12
