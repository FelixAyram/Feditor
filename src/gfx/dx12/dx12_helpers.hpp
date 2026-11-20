#pragma once

#include <d3d12.h>

inline D3D12_RESOURCE_BARRIER dx12_transition_barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                                                      D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

struct CD3DX12_HEAP_PROPERTIES : public D3D12_HEAP_PROPERTIES {
    explicit CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE type) {
        Type = type;
        CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        CreationNodeMask = 1;
        VisibleNodeMask = 1;
    }
};

struct CD3DX12_RESOURCE_DESC : public D3D12_RESOURCE_DESC {
    static inline CD3DX12_RESOURCE_DESC Buffer(UINT64 width) {
        CD3DX12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = width;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return d;
    }
};
