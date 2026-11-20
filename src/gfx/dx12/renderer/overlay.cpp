#include "gfx/dx12/dx12_renderer.hpp"
#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/o3d_format.hpp"
#include "omsi/logger.hpp"

#include <algorithm>
#include <cstring>
#include <d3dcompiler.h>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")

namespace omsi::gfx::dx12 {

void Dx12Renderer::draw_ndc_overlay() {
    if (!ndc_overlay_ready_ || draw_cb_count_ >= kMaxDrawsPerFrame) return;

    DrawCB& slot = draw_cb_cpu_[draw_cb_count_];
    slot.view_proj = f4f23ma::ma4::identity();
    slot.object = f4f23ma::ma4::identity();

    auto& f = frames_[frame_index_];
    const auto cb_addr =
        draw_cb_upload_->GetGPUVirtualAddress() + static_cast<UINT64>(draw_cb_count_) * kDrawCbStride;
    f.cmd_list->SetGraphicsRootConstantBufferView(0, cb_addr);
    f.cmd_list->SetGraphicsRootDescriptorTable(1, srv_heap_gpu_);
    f.cmd_list->IASetVertexBuffers(0, 1, &ndc_overlay_.vbv);
    f.cmd_list->IASetIndexBuffer(&ndc_overlay_.ibv);
    f.cmd_list->DrawIndexedInstanced(ndc_overlay_.index_count, 1, 0, 0, 0);
    ++draw_cb_count_;
    ++last_frame_draws_;
}

bool Dx12Renderer::create_overlay_geometry() {
    const o3d::Vertex verts[3] = {
        {{-0.98f, -0.98f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f}},
        {{-0.92f, -0.98f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f}},
        {{-0.95f, -0.92f, 0.f}, {0.f, 0.f, 1.f}, {0.5f, 1.f}},
    };
    const std::uint16_t indices[3] = {0, 1, 2};

    const std::size_t vb_size = sizeof(verts);
    const std::size_t ib_size = sizeof(indices);

    ComPtr<ID3D12Resource> upload;
    CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC upload_desc = CD3DX12_RESOURCE_DESC::Buffer(vb_size + ib_size);
    throw_hr(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(upload.GetAddressOf())),
             "debug upload");

    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    std::memcpy(mapped, verts, vb_size);
    std::memcpy(static_cast<std::uint8_t*>(mapped) + vb_size, indices, ib_size);
    upload->Unmap(0, nullptr);

    CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC vb_desc = CD3DX12_RESOURCE_DESC::Buffer(vb_size);
    throw_hr(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(ndc_overlay_.vertex_buffer.GetAddressOf())),
             "overlay vb");
    CD3DX12_RESOURCE_DESC ib_desc = CD3DX12_RESOURCE_DESC::Buffer(ib_size);
    throw_hr(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &ib_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(ndc_overlay_.index_buffer.GetAddressOf())),
             "overlay ib");

    auto& f = frames_[0];
    throw_hr(f.allocator->Reset(), "debug alloc reset");
    throw_hr(f.cmd_list->Reset(f.allocator.Get(), nullptr), "debug cmd reset");
    f.cmd_list->CopyBufferRegion(ndc_overlay_.vertex_buffer.Get(), 0, upload.Get(), 0, vb_size);
    f.cmd_list->CopyBufferRegion(ndc_overlay_.index_buffer.Get(), 0, upload.Get(), vb_size, ib_size);

    D3D12_RESOURCE_BARRIER barriers[2] = {
        dx12_transition_barrier(ndc_overlay_.vertex_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
        dx12_transition_barrier(ndc_overlay_.index_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_INDEX_BUFFER),
    };
    f.cmd_list->ResourceBarrier(2, barriers);
    throw_hr(f.cmd_list->Close(), "debug cmd close");

    ID3D12CommandList* lists[] = {f.cmd_list.Get()};
    queue_->ExecuteCommandLists(1, lists);
    wait_gpu();

    ndc_overlay_.vbv.BufferLocation = ndc_overlay_.vertex_buffer->GetGPUVirtualAddress();
    ndc_overlay_.vbv.SizeInBytes = static_cast<UINT>(vb_size);
    ndc_overlay_.vbv.StrideInBytes = sizeof(o3d::Vertex);
    ndc_overlay_.ibv.BufferLocation = ndc_overlay_.index_buffer->GetGPUVirtualAddress();
    ndc_overlay_.ibv.SizeInBytes = static_cast<UINT>(ib_size);
    ndc_overlay_.ibv.Format = DXGI_FORMAT_R16_UINT;
    ndc_overlay_.index_count = 3;
    ndc_overlay_ready_ = true;
    return true;
}

} // namespace omsi::gfx::dx12
