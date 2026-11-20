#include "gfx/dx12/hello_o3d_renderer.hpp"

#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/o3d/internal.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "omsi/logger.hpp"
#include "omsi/visual_quality.hpp"

#include <d3d12.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// iryoku SMAA lookup tables (R8G8 area, R8 search)
#include "../../../../third_party/smaa/AreaTex.h"
#include "../../../../third_party/smaa/SearchTex.h"

namespace omsi::gfx::dx12 {
namespace {

constexpr std::uint32_t kSceneSrvBaseSlot =
    kCbvDescriptorCount + kSrvDescriptorCount - HelloO3dRenderer::kSwapchainBuffers;

// Contiguous SRVs: edges, blend, area, search (t1..t4). CSM sits at scene_base-1.
constexpr std::uint32_t kSmaaEdgesSrvSlot = kSceneSrvBaseSlot - 1u - 4u;
constexpr std::uint32_t kSmaaBlendSrvSlot = kSmaaEdgesSrvSlot + 1u;
constexpr std::uint32_t kSmaaAreaSrvSlot = kSmaaEdgesSrvSlot + 2u;
constexpr std::uint32_t kSmaaSearchSrvSlot = kSmaaEdgesSrvSlot + 3u;

constexpr UINT kRtvSceneBase = HelloO3dRenderer::kSwapchainBuffers;            // 2..3
constexpr UINT kRtvMsaa = HelloO3dRenderer::kSwapchainBuffers * 2u;            // 4
constexpr UINT kRtvSmaaEdges = HelloO3dRenderer::kSwapchainBuffers * 2u + 1u;  // 5
constexpr UINT kRtvSmaaBlend = HelloO3dRenderer::kSwapchainBuffers * 2u + 2u;  // 6

struct SmaaInclude : public ID3DInclude {
    std::filesystem::path dir;
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID, LPCVOID* ppData,
                                   UINT* pBytes) override {
        const auto path = dir / pFileName;
        FILE* f = nullptr;
#if defined(_MSC_VER)
        if (fopen_s(&f, path.string().c_str(), "rb") != 0 || !f) return E_FAIL;
#else
        f = fopen(path.string().c_str(), "rb");
        if (!f) return E_FAIL;
#endif
        fseek(f, 0, SEEK_END);
        const long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) {
            fclose(f);
            return E_FAIL;
        }
        auto* buf = new char[static_cast<size_t>(sz)];
        if (fread(buf, 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
            delete[] buf;
            fclose(f);
            return E_FAIL;
        }
        fclose(f);
        *ppData = buf;
        *pBytes = static_cast<UINT>(sz);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID pData) override {
        delete[] static_cast<const char*>(pData);
        return S_OK;
    }
};

ComPtr<ID3DBlob> compile_smaa_shader(const std::filesystem::path& hlsl, const char* entry, const char* profile) {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    SmaaInclude include{};
    include.dir = hlsl.parent_path();
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const std::wstring wpath = hlsl.wstring();
    const HRESULT hr =
        D3DCompileFromFile(wpath.c_str(), nullptr, &include, entry, profile, flags, 0, blob.GetAddressOf(),
                           errors.GetAddressOf());
    if (FAILED(hr)) {
        if (errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        throw_hr(hr, "SMAA D3DCompileFromFile failed");
    }
    return blob;
}

void create_tex2d_rt(ID3D12Device* device, UINT w, UINT h, DXGI_FORMAT fmt, ComPtr<ID3D12Resource>& out,
                     D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu,
                     D3D12_GPU_DESCRIPTOR_HANDLE* srv_gpu, UINT srv_stride, ID3D12DescriptorHeap* srv_heap,
                     UINT srv_slot) {
    out.Reset();
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = fmt;
    clear.Color[0] = clear.Color[1] = clear.Color[2] = clear.Color[3] = 0.f;
    throw_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                             IID_PPV_ARGS(out.GetAddressOf())),
             "SMAA RT");
    device->CreateRenderTargetView(out.Get(), nullptr, rtv);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = fmt;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(out.Get(), &srv, srv_cpu);
    if (srv_gpu)
        *srv_gpu = offset_gpu_handle(srv_heap->GetGPUDescriptorHandleForHeapStart(), srv_stride, srv_slot);
}

void upload_r8_texture(HelloO3dRenderer::Impl& d, ComPtr<ID3D12Resource>& tex, UINT w, UINT h, DXGI_FORMAT fmt,
                       const unsigned char* bytes, UINT pitch_src, const char* label) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    throw_hr(d.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(tex.GetAddressOf())),
             label);

    UINT64 upload_size = 0;
    d.device->GetCopyableFootprints(&desc, 0, 1, 0, nullptr, nullptr, nullptr, &upload_size);
    ComPtr<ID3D12Resource> upload;
    D3D12_HEAP_PROPERTIES up_heap{};
    up_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC up_desc{};
    up_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    up_desc.Width = upload_size;
    up_desc.Height = 1;
    up_desc.DepthOrArraySize = 1;
    up_desc.MipLevels = 1;
    up_desc.SampleDesc.Count = 1;
    up_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    throw_hr(d.device->CreateCommittedResource(&up_heap, D3D12_HEAP_FLAG_NONE, &up_desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(upload.GetAddressOf())),
             "SMAA upload");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    d.device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &num_rows, &row_size, &upload_size);
    void* mapped = nullptr;
    throw_hr(upload->Map(0, nullptr, &mapped), "SMAA map");
    auto* dst = static_cast<std::uint8_t*>(mapped) + layout.Offset;
    for (UINT y = 0; y < h; ++y) {
        std::memcpy(dst + y * layout.Footprint.RowPitch, bytes + y * pitch_src, pitch_src);
    }
    upload->Unmap(0, nullptr);

    throw_hr(d.upload_allocator->Reset(), "SMAA upload reset alloc");
    throw_hr(d.upload_command_list->Reset(d.upload_allocator.Get(), nullptr), "SMAA upload reset list");
    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = tex.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = upload.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint = layout;
    d.upload_command_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    D3D12_RESOURCE_BARRIER barrier =
        dx12_transition_barrier(tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    d.upload_command_list->ResourceBarrier(1, &barrier);
    throw_hr(d.upload_command_list->Close(), "SMAA upload close");
    ID3D12CommandList* lists[] = {d.upload_command_list.Get()};
    d.queue->ExecuteCommandLists(1, lists);
    // Init-time only: drain so upload staging can die with this stack frame.
    const UINT64 fence_value = ++d.fence_value;
    throw_hr(d.queue->Signal(d.fence.Get(), fence_value), "SMAA upload signal");
    if (d.fence->GetCompletedValue() < fence_value) {
        throw_hr(d.fence->SetEventOnCompletion(fence_value, d.fence_event), "SMAA fence event");
        WaitForSingleObject(d.fence_event, INFINITE);
    }
}

bool create_smaa_pso(HelloO3dRenderer::Impl& d, ID3D12RootSignature* rs, const std::filesystem::path& hlsl,
                     const char* vs, const char* ps, ComPtr<ID3D12PipelineState>& out, DXGI_FORMAT rtv_fmt) {
    auto vsb = compile_smaa_shader(hlsl, vs, "vs_5_1");
    auto psb = compile_smaa_shader(hlsl, ps, "ps_5_1");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rs;
    pso.VS = {vsb->GetBufferPointer(), vsb->GetBufferSize()};
    pso.PS = {psb->GetBufferPointer(), psb->GetBufferSize()};
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = rtv_fmt;
    pso.SampleDesc.Count = 1;
    throw_hr(d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(out.GetAddressOf())), "SMAA PSO");
    return true;
}

} // namespace

void HelloO3dRenderer::create_scene_color_buffers(std::uint32_t width, std::uint32_t height) {
    auto& d = *impl_;
    if (!d.device || width == 0 || height == 0) return;
    for (std::uint32_t i = 0; i < kFrameCount; ++i) {
        d.scene_color[i].Reset();
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_CLEAR_VALUE clear_value{};
        clear_value.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        clear_value.Color[0] = 0.69f;
        clear_value.Color[1] = 0.77f;
        clear_value.Color[2] = 0.87f;
        clear_value.Color[3] = 1.f;
        throw_hr(d.device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
                                                   IID_PPV_ARGS(d.scene_color[i].GetAddressOf())),
                 "scene color RT");

        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            offset_cpu_handle(d.rtv_heap->GetCPUDescriptorHandleForHeapStart(), rtv_descriptor_size_,
                              kRtvSceneBase + i);
        d.device->CreateRenderTargetView(d.scene_color[i].Get(), nullptr, rtv);
        d.scene_rtv_cpu[i] = rtv;

        const std::uint32_t srv_slot = kSceneSrvBaseSlot + i;
        D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu =
            offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, srv_slot);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;
        d.device->CreateShaderResourceView(d.scene_color[i].Get(), &srv_desc, srv_cpu);
        d.scene_srv_gpu[i] = offset_gpu_handle(d.srv_heap->GetGPUDescriptorHandleForHeapStart(), d.srv_stride,
                                               srv_slot);
    }

    // SMAA edges + blend intermediates (non-sRGB).
    d.smaa_edges_rtv = offset_cpu_handle(d.rtv_heap->GetCPUDescriptorHandleForHeapStart(), rtv_descriptor_size_,
                                         kRtvSmaaEdges);
    d.smaa_blend_rtv = offset_cpu_handle(d.rtv_heap->GetCPUDescriptorHandleForHeapStart(), rtv_descriptor_size_,
                                         kRtvSmaaBlend);
    create_tex2d_rt(d.device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, d.smaa_edges, d.smaa_edges_rtv,
                    offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                      kSmaaEdgesSrvSlot),
                    &d.smaa_edges_srv_gpu, d.srv_stride, d.srv_heap.Get(), kSmaaEdgesSrvSlot);
    create_tex2d_rt(d.device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM, d.smaa_blend, d.smaa_blend_rtv,
                    offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                                      kSmaaBlendSrvSlot),
                    &d.smaa_blend_srv_gpu, d.srv_stride, d.srv_heap.Get(), kSmaaBlendSrvSlot);
}

bool HelloO3dRenderer::create_fxaa_pipeline() {
    auto& d = *impl_;
    if (!d.device) return false;

    D3D12_DESCRIPTOR_RANGE color_range{};
    color_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    color_range.NumDescriptors = 1;
    color_range.BaseShaderRegister = 0;

    D3D12_DESCRIPTOR_RANGE aux_range{};
    aux_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    aux_range.NumDescriptors = 4;
    aux_range.BaseShaderRegister = 1;

    D3D12_ROOT_PARAMETER root_params[3]{};
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_params[0].Constants.Num32BitValues = 4;
    root_params[0].Constants.ShaderRegister = 0;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[1].DescriptorTable.NumDescriptorRanges = 1;
    root_params[1].DescriptorTable.pDescriptorRanges = &color_range;
    root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    root_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[2].DescriptorTable.NumDescriptorRanges = 1;
    root_params[2].DescriptorTable.pDescriptorRanges = &aux_range;
    root_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for (auto& s : samplers) {
        s.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplers[0].ShaderRegister = 0;
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 3;
    rs_desc.pParameters = root_params;
    rs_desc.NumStaticSamplers = 2;
    rs_desc.pStaticSamplers = samplers;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig_blob;
    ComPtr<ID3DBlob> sig_error;
    throw_hr(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.GetAddressOf(),
                                          sig_error.GetAddressOf()),
             "SMAA root sig");
    throw_hr(d.device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                           IID_PPV_ARGS(d.fxaa_root_signature.GetAddressOf())),
             "SMAA root sig create");

    std::filesystem::path hlsl;
    {
        // beside the .exe first (a release folder started from anywhere), then the working / source tree
        wchar_t exe_buf[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
        const std::filesystem::path exe_dir = std::filesystem::path(exe_buf).parent_path();
        const std::filesystem::path candidates[] = {
            exe_dir / "third_party" / "smaa" / "smaa_dx12.hlsl",
            exe_dir / "shaders" / "smaa_dx12.hlsl",
            "third_party/smaa/smaa_dx12.hlsl",
            "../third_party/smaa/smaa_dx12.hlsl",
            "../../third_party/smaa/smaa_dx12.hlsl",
        };
        std::error_code ec;
        for (const auto& c : candidates) {
            if (std::filesystem::exists(c, ec)) {
                hlsl = std::filesystem::absolute(c, ec);
                break;
            }
        }
    }
    if (hlsl.empty() || !std::filesystem::exists(hlsl)) {
        omsi::log::error("SMAA shader missing: third_party/smaa/smaa_dx12.hlsl");
        return false;
    }

    create_smaa_pso(d, d.fxaa_root_signature.Get(), hlsl, "EdgeVS", "EdgePS", d.smaa_edge_pso,
                    DXGI_FORMAT_R8G8B8A8_UNORM);
    create_smaa_pso(d, d.fxaa_root_signature.Get(), hlsl, "BlendVS", "BlendPS", d.smaa_blend_pso,
                    DXGI_FORMAT_R8G8B8A8_UNORM);
    create_smaa_pso(d, d.fxaa_root_signature.Get(), hlsl, "NeighborhoodVS", "NeighborhoodPS",
                    d.smaa_neighborhood_pso, DXGI_FORMAT_R8G8B8A8_UNORM);
    // Keep fxaa_pipeline_state as "ready" flag for scene_postfx_active().
    d.fxaa_pipeline_state = d.smaa_neighborhood_pso;

    // Upload AreaTex (R8G8) + SearchTex (R8) once.
    upload_r8_texture(d, d.smaa_area_tex, AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, areaTexBytes,
                      AREATEX_PITCH, "SMAA AreaTex");
    upload_r8_texture(d, d.smaa_search_tex, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM,
                      searchTexBytes, SEARCHTEX_PITCH, "SMAA SearchTex");

    D3D12_SHADER_RESOURCE_VIEW_DESC area_srv{};
    area_srv.Format = DXGI_FORMAT_R8G8_UNORM;
    area_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    area_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    area_srv.Texture2D.MipLevels = 1;
    d.device->CreateShaderResourceView(
        d.smaa_area_tex.Get(), &area_srv,
        offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kSmaaAreaSrvSlot));

    D3D12_SHADER_RESOURCE_VIEW_DESC search_srv{};
    search_srv.Format = DXGI_FORMAT_R8_UNORM;
    search_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    search_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    search_srv.Texture2D.MipLevels = 1;
    d.device->CreateShaderResourceView(
        d.smaa_search_tex.Get(), &search_srv,
        offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, kSmaaSearchSrvSlot));

    d.smaa_aux_srv_gpu =
        offset_gpu_handle(d.srv_heap->GetGPUDescriptorHandleForHeapStart(), d.srv_stride, kSmaaEdgesSrvSlot);

    omsi::log::info("SMAA 1x HIGH post-process pipeline ready");
    return true;
}

void HelloO3dRenderer::record_fxaa_pass(ID3D12GraphicsCommandList* cmd, std::uint32_t frame_index) {
    auto& d = *impl_;
    if (!cmd || !d.smaa_neighborhood_pso || !d.scene_color[frame_index] || !d.smaa_edges || !d.smaa_blend)
        return;

    ID3D12Resource* scene = d.scene_color[frame_index].Get();
    ID3D12Resource* edges = d.smaa_edges.Get();
    ID3D12Resource* blend = d.smaa_blend.Get();
    ID3D12Resource* back = d.render_targets[frame_index].Get();

    D3D12_VIEWPORT vp{};
    D3D12_RECT sc{};
    scene_viewport_rect(width_, height_, vp, sc);
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    ID3D12DescriptorHeap* heaps[] = {d.srv_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(d.fxaa_root_signature.Get());

    const float metrics[4] = {1.f / static_cast<float>((std::max)(width_, 1u)),
                              1.f / static_cast<float>((std::max)(height_, 1u)),
                              static_cast<float>(width_), static_cast<float>(height_)};
    cmd->SetGraphicsRoot32BitConstants(0, 4, metrics, 0);
    cmd->SetGraphicsRootDescriptorTable(2, d.smaa_aux_srv_gpu);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const float clear_zero[] = {0.f, 0.f, 0.f, 0.f};

    // Pass A: scene → edges
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {
            dx12_transition_barrier(scene, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            dx12_transition_barrier(edges, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        cmd->ResourceBarrier(1, &barriers[0]);
        cmd->OMSetRenderTargets(1, &d.smaa_edges_rtv, FALSE, nullptr);
        cmd->ClearRenderTargetView(d.smaa_edges_rtv, clear_zero, 0, nullptr);
        cmd->SetPipelineState(d.smaa_edge_pso.Get());
        cmd->SetGraphicsRootDescriptorTable(1, d.scene_srv_gpu[frame_index]);
        cmd->DrawInstanced(3, 1, 0, 0);
    }

    // Pass B: edges → blend
    {
        D3D12_RESOURCE_BARRIER barriers[2] = {
            dx12_transition_barrier(edges, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            dx12_transition_barrier(blend, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        cmd->ResourceBarrier(1, &barriers[0]);
        cmd->OMSetRenderTargets(1, &d.smaa_blend_rtv, FALSE, nullptr);
        cmd->ClearRenderTargetView(d.smaa_blend_rtv, clear_zero, 0, nullptr);
        cmd->SetPipelineState(d.smaa_blend_pso.Get());
        cmd->DrawInstanced(3, 1, 0, 0);
    }

    // Pass C: neighborhood → backbuffer
    {
        D3D12_RESOURCE_BARRIER barriers[3] = {
            dx12_transition_barrier(blend, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            dx12_transition_barrier(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET),
            dx12_transition_barrier(scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        cmd->ResourceBarrier(2, barriers);
        D3D12_CPU_DESCRIPTOR_HANDLE back_rtv = d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        back_rtv.ptr += static_cast<SIZE_T>(frame_index) * rtv_descriptor_size_;
        cmd->OMSetRenderTargets(1, &back_rtv, FALSE, nullptr);
        const float clear_bb[] = {0.f, 0.f, 0.f, 1.f};
        cmd->ClearRenderTargetView(back_rtv, clear_bb, 0, nullptr);
        cmd->SetPipelineState(d.smaa_neighborhood_pso.Get());
        cmd->SetGraphicsRootDescriptorTable(1, d.scene_srv_gpu[frame_index]);
        cmd->DrawInstanced(3, 1, 0, 0);
    }

    // Restore RT states for next frame.
    {
        D3D12_RESOURCE_BARRIER barriers[3] = {
            dx12_transition_barrier(scene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET),
            dx12_transition_barrier(edges, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET),
            dx12_transition_barrier(blend, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        cmd->ResourceBarrier(3, barriers);
    }
}

bool HelloO3dRenderer::scene_postfx_active() const {
    return impl_ && omsi::visual_quality_fxaa_enabled() && impl_->fxaa_pipeline_state &&
           impl_->smaa_neighborhood_pso;
}

bool HelloO3dRenderer::scene_msaa_active() const {
    return impl_ && impl_->msaa_samples > 1 && impl_->msaa_color;
}

void HelloO3dRenderer::record_msaa_resolve(ID3D12GraphicsCommandList* cmd, ID3D12Resource* dest) {
    auto& d = *impl_;
    if (!cmd || !dest || !d.msaa_color) return;

    const bool dest_is_back = (dest == d.render_targets[frame_index_].Get());
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0] = dx12_transition_barrier(d.msaa_color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    if (dest_is_back) {
        barriers[1] = dx12_transition_barrier(dest, D3D12_RESOURCE_STATE_PRESENT,
                                              D3D12_RESOURCE_STATE_RESOLVE_DEST);
    } else {
        barriers[1] = dx12_transition_barrier(dest, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                              D3D12_RESOURCE_STATE_RESOLVE_DEST);
    }
    cmd->ResourceBarrier(2, barriers);

    cmd->ResolveSubresource(dest, 0, d.msaa_color.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);

    barriers[0] = dx12_transition_barrier(d.msaa_color.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &barriers[0]);
    if (!dest_is_back) {
        barriers[0] = dx12_transition_barrier(dest, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->ResourceBarrier(1, &barriers[0]);
    }
}

} // namespace omsi::gfx::dx12
