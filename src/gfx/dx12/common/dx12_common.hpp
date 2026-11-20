#pragma once

#include <d3d12.h>

#include <stdexcept>

namespace omsi::gfx::dx12 {

[[nodiscard]] inline D3D12_CPU_DESCRIPTOR_HANDLE offset_cpu_handle(D3D12_CPU_DESCRIPTOR_HANDLE base, UINT inc,
                                                                   UINT i) {
    base.ptr += static_cast<SIZE_T>(i) * inc;
    return base;
}

[[nodiscard]] inline D3D12_GPU_DESCRIPTOR_HANDLE offset_gpu_handle(D3D12_GPU_DESCRIPTOR_HANDLE base, UINT inc,
                                                                   UINT i) {
    base.ptr += static_cast<UINT64>(i) * inc;
    return base;
}

void throw_hr(HRESULT hr, const char* what);

} // namespace omsi::gfx::dx12
