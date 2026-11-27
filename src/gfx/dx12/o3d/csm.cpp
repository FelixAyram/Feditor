#include "gfx/dx12/hello_o3d_renderer.hpp"
#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/o3d/internal.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "omsi/logger.hpp"
#include "omsi/frustum.hpp"
#include "omsi/job_system.hpp"

#include <DirectXMath.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace omsi::gfx::dx12 {
namespace {

using Microsoft::WRL::ComPtr;
using DirectX::XMMATRIX;
using DirectX::XMVECTOR;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4X4;

// 256 B shadow data + 256 B gEnvExtra (night blend / wind / time, see draw.cpp): kept out of the root
// constants, which are at the 64-DWORD root-signature limit.
constexpr UINT kShadowCbBytes = 65536; // + scenery lights from 272: info, 512 lights, light grid (64 KB max)
// Permanent SRV slot just below scene-color slots (SMAA aux uses -2..-5). Relative to srv_heap_gpu.
constexpr std::uint32_t kCsmSrvRelativeSlot =
    kSrvDescriptorCount - HelloO3dRenderer::kSwapchainBuffers - 1u;

struct ShadowCbCpu {
    XMFLOAT4X4 light_vp[3]{};
    float splits[4]{12.f, 40.f, 120.f, 1.f / 2048.f};
    float cam_pos[4]{0.f, 0.f, 20.f, 0.f};
    float bias[4]{0.0015f, 0.04f, 1.f / 2048.f, 0.f};
    float cam_forward[4]{0.f, 1.f, 0.f, 0.f};
};

const char* kCsmVs = R"(
struct VSIn { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
struct InstanceData { float4x4 wvp; float4x4 world; };
StructuredBuffer<InstanceData> gInstance : register(t2);
VSOut vs_main(VSIn vin, uint iid : SV_InstanceID) {
    VSOut o;
    o.pos = mul(float4(vin.pos, 1), gInstance[iid].wvp);
    o.uv = vin.uv;
    return o;
}
)";

const char* kCsmPsCutout = R"(
Texture2D gAlbedo : register(t0);
SamplerState gSampler : register(s0);
cbuffer CutCB : register(b2) { float gAlphaMode; float3 gPad; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
void ps_main(PSIn pin) {
    if (gAlphaMode > 0.5) {
        float a = gAlbedo.Sample(gSampler, pin.uv).a;
        if (a < 0.5) discard;
    }
}
)";

void draw_chunk_shadow(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd, const GpuMeshChunk& chunk,
                       std::uint32_t& shadow_cursor, std::uint32_t inst_base, const XMMATRIX& light_vp) {
    if (!chunk.meta.visible || chunk.screen_space || chunk.fixed_lit) return;
    if (!chunk.vertex_buffer || !chunk.index_buffer) return;
    if (chunk.opaque_batches.empty() && chunk.alpha_batches.empty()) return;
    if (shadow_cursor >= HelloO3dRenderer::Impl::kMaxInstancesPerDraw) return;

    const std::uint32_t inst_slot = inst_base + shadow_cursor++;
    auto* dst = reinterpret_cast<HelloO3dRenderer::Impl::InstanceMatrices*>(
        d.instance_buffer_cpu + inst_slot * HelloO3dRenderer::Impl::kInstanceStride);
    const XMMATRIX world_t = DirectX::XMLoadFloat4x4(&chunk.local_world_transposed);
    const XMMATRIX world = DirectX::XMMatrixTranspose(world_t);
    // Terrain as a caster: (1) the CUT mesh where the picture shows it (the uncut base - drawn 40 cm down
    // in the picture - stood at full height in the shadow map wherever a road or object had cut it away
    // and shadowed them), (2) pushed 35 cm away from the sun: terrain a few cm from a road's or an
    // object's vertices shaded them (a coarse cascade texel of a sloped ground reaches over a surface
    // lying centimetres above it). A hill's shadow moves 35 cm along the sun ray - nothing visible.
    const bool terrain = chunk.is_terrain || chunk.meta.mesh_file == "__terrain_tile__" ||
                         chunk.meta.mesh_file == "__oo_terrain__";
    const bool terrain_detail = terrain && chunk.detail_ready && chunk.detail_active && chunk.detail_vb &&
                                chunk.detail_ib && !chunk.detail_batches.empty();
    XMMATRIX wvp = world * light_vp;
    if (terrain) {
        XMFLOAT4X4 lv;
        DirectX::XMStoreFloat4x4(&lv, light_vp);
        const float z_per_m = std::sqrt(lv._13 * lv._13 + lv._23 * lv._23 + lv._33 * lv._33);
        wvp = wvp * DirectX::XMMatrixTranslation(0.f, 0.f, 0.35f * z_per_m);
    }
    DirectX::XMStoreFloat4x4(&dst->wvp, DirectX::XMMatrixTranspose(wvp));
    dst->world = chunk.local_world_transposed;

    const D3D12_GPU_VIRTUAL_ADDRESS inst_gpu =
        d.instance_buffer->GetGPUVirtualAddress() +
        static_cast<UINT64>(inst_slot) * HelloO3dRenderer::Impl::kInstanceStride;
    cmd->SetGraphicsRootShaderResourceView(0, inst_gpu);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (terrain_detail) {
        cmd->IASetVertexBuffers(0, 1, &chunk.detail_vbv);
        cmd->IASetIndexBuffer(&chunk.detail_ibv);
        cmd->SetPipelineState(d.csm_pso_opaque.Get());
        const float consts[4]{0.f, 0.f, 0.f, 0.f};
        cmd->SetGraphicsRoot32BitConstants(2, 4, consts, 0);
        cmd->SetGraphicsRootDescriptorTable(1, d.srv_heap_gpu);
        for (const auto& b : chunk.detail_batches)
            if (b.index_count) cmd->DrawIndexedInstanced(b.index_count, 1, b.index_start, 0, 0);
        return;
    }
    cmd->IASetVertexBuffers(0, 1, &chunk.vertex_buffer_view);
    const bool shadow_lod = chunk.lod_level > 0 && chunk.lod_ib[chunk.lod_level - 1].buffer;
    cmd->IASetIndexBuffer(shadow_lod ? &chunk.lod_ib[chunk.lod_level - 1].view : &chunk.index_buffer_view);

    auto draw_batches = [&](const std::vector<MaterialDrawBatch>& batches, bool cutout) {
        cmd->SetPipelineState(cutout ? d.csm_pso_cutout.Get() : d.csm_pso_opaque.Get());
        for (const auto& b : batches) {
            if (b.index_count == 0) continue;
            float consts[4]{cutout ? 1.f : 0.f, 0.f, 0.f, 0.f};
            cmd->SetGraphicsRoot32BitConstants(2, 4, consts, 0);
            if (cutout && b.has_texture)
                cmd->SetGraphicsRootDescriptorTable(1, b.srv_gpu);
            else
                cmd->SetGraphicsRootDescriptorTable(1, d.srv_heap_gpu);
            if (shadow_lod && b.index_start == 0)
                cmd->DrawIndexedInstanced(chunk.lod_ib[chunk.lod_level - 1].index_count, 1, 0, 0, 0);
            else
                cmd->DrawIndexedInstanced(b.index_count, 1, b.index_start, 0, 0);
        }
    };
    draw_batches(chunk.opaque_batches, false);
    for (const auto& b : chunk.alpha_batches) {
        using AM = omsi::model_cfg::MatlAlphaMode;
        // Trees/fences often land as alpha_blend billboards — still need cutout shadow casters
        // or they project solid floating quads.
        if (b.alpha_mode == AM::alpha_test || (b.alpha_mode == AM::alpha_blend && b.has_texture))
            draw_batches({b}, true);
    }
}

// A chunk that can cast a shadow, with its world-space bounding sphere. Built once per frame; the
// three cascades then test spheres instead of hashing / drawing every chunk.
struct ShadowCaster {
    const GpuMeshChunk* chunk = nullptr;
    float c[3]{};
    float r = 0.f;
};

inline bool chunk_can_cast_shadow(const GpuMeshChunk& chunk) {
    if (!chunk.meta.visible || chunk.screen_space || chunk.fixed_lit || chunk.path_rail) return false;
    if (!chunk.vertex_buffer || !chunk.index_buffer) return false;
    if (chunk.opaque_batches.empty() && chunk.alpha_batches.empty()) return false;
    // openOMSI: the ground layers cast no sun shadow - a road spline lies on the ground (only one standing
    // clear of it, a bridge, does), and a [surface] / paint / decal object only with what rises over 1.5 m
    // (place_step: "tagged || tall"). Cast, a zebra crossing's stripes - millimetres over the asphalt -
    // threw a shadow under each stripe and looked raised off the road.
    const auto& mf = chunk.meta.mesh_file;
    const bool spline = chunk.render_phase == WorldRenderPhase::Spline || mf == "__spline__" ||
                        mf == "__spline_merged__";
    const bool ground_layer = spline || chunk.decal || chunk.is_presurface || chunk.surface_bias ||
                              chunk.render_phase == WorldRenderPhase::Surface ||
                              chunk.render_phase == WorldRenderPhase::OnSurface ||
                              chunk.render_phase == WorldRenderPhase::PreSurface;
    if (ground_layer) {
        const float h = chunk.meta.bbox_max[2] - chunk.meta.bbox_min[2];
        if (spline || !(h > 1.5f)) return false;
    }
    // (test switch OMSI_CSM_SKIP=trees,terrain,objects: which kind of caster costs the cascades what)
    static const int s_skip = [] {
        const char* e = std::getenv("OMSI_CSM_SKIP");
        if (!e) return 0;
        const std::string v(e);
        return (v.find("trees") != std::string::npos ? 1 : 0) | (v.find("terrain") != std::string::npos ? 2 : 0) |
               (v.find("objects") != std::string::npos ? 4 : 0);
    }();
    if (s_skip) {
        const bool terrain = chunk.is_terrain || mf == "__terrain_tile__" || mf == "__oo_terrain__";
        const int kind = chunk.wind_sway ? 1 : (terrain ? 2 : 4);
        if (s_skip & kind) return false;
    }
    return true;
}

inline void shadow_hash_caster(std::uint64_t& h, const GpuMeshChunk& c) {
    csm_mix_floats(h, &c.local_world_transposed._11, 16);
    csm_mix_ptr(h, c.vertex_buffer.Get());
    csm_mix_ptr(h, c.index_buffer.Get());
    csm_mix(h, c.vertex_buffer_view.BufferLocation); // (arena-shared buffers: the address tells the meshes apart)
    if (c.detail_ready && c.detail_active) csm_mix_ptr(h, c.detail_ib.Get());
    csm_mix(h, (static_cast<std::uint64_t>(c.opaque_batches.size()) << 32) | c.alpha_batches.size());
    for (const auto& b : c.opaque_batches) csm_mix(h, (static_cast<std::uint64_t>(b.index_count) << 32) | b.index_start);
    for (const auto& b : c.alpha_batches) {
        csm_mix(h, (static_cast<std::uint64_t>(b.index_count) << 32) | b.index_start);
        csm_mix(h, (static_cast<std::uint64_t>(static_cast<std::uint32_t>(b.alpha_mode)) << 1) | (b.has_texture ? 1u : 0u));
        csm_mix(h, static_cast<std::uint64_t>(b.srv_gpu.ptr));
    }
}

} // namespace

bool ensure_csm_resources(HelloO3dRenderer::Impl& d) {
    if (d.csm_ready && d.csm_depth) return true;
    if (!d.device || !d.srv_heap || d.srv_stride == 0) return false;

    const UINT res = (std::max)(512u, (std::min)(d.sun_shadow_map_size, 4096u));
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = res;
    desc.Height = res;
    desc.DepthOrArraySize = HelloO3dRenderer::Impl::kCsmCascades;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.f;

    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(d.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                 IID_PPV_ARGS(d.csm_depth.ReleaseAndGetAddressOf())))) {
        omsi::log::warn("CSM: failed to create depth array");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
    dsv_heap_desc.NumDescriptors = HelloO3dRenderer::Impl::kCsmCascades;
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(d.device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(d.csm_dsv_heap.ReleaseAndGetAddressOf()))))
        return false;
    const UINT dsv_stride = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (int i = 0; i < HelloO3dRenderer::Impl::kCsmCascades; ++i) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.ArraySize = 1;
        dsv.Texture2DArray.FirstArraySlice = static_cast<UINT>(i);
        d.device->CreateDepthStencilView(d.csm_depth.Get(), &dsv,
                                         offset_cpu_handle(d.csm_dsv_heap->GetCPUDescriptorHandleForHeapStart(),
                                                           dsv_stride, i));
        d.csm_dsv_cpu[i] =
            offset_cpu_handle(d.csm_dsv_heap->GetCPUDescriptorHandleForHeapStart(), dsv_stride, i);
    }

    d.csm_srv_slot = kCsmSrvRelativeSlot;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2DArray.MipLevels = 1;
    srv.Texture2DArray.ArraySize = HelloO3dRenderer::Impl::kCsmCascades;
    const D3D12_CPU_DESCRIPTOR_HANDLE cpu =
        offset_cpu_handle(d.srv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride,
                          kCbvDescriptorCount + d.csm_srv_slot);
    d.device->CreateShaderResourceView(d.csm_depth.Get(), &srv, cpu);
    d.csm_srv_gpu = offset_gpu_handle(d.srv_heap_gpu, d.srv_stride, d.csm_srv_slot);

    const CD3DX12_HEAP_PROPERTIES upload(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC cb_desc =
        CD3DX12_RESOURCE_DESC::Buffer(static_cast<UINT64>(kShadowCbBytes) * HelloO3dRenderer::Impl::kFramesInFlight);
    if (FAILED(d.device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &cb_desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(d.shadow_cb.ReleaseAndGetAddressOf()))))
        return false;
    throw_hr(d.shadow_cb->Map(0, nullptr, reinterpret_cast<void**>(&d.shadow_cb_base_cpu)), "shadow cb map");
    d.shadow_cb_base_gpu = d.shadow_cb->GetGPUVirtualAddress();
    std::memset(d.shadow_cb_base_cpu, 0, static_cast<std::size_t>(kShadowCbBytes) * HelloO3dRenderer::Impl::kFramesInFlight);
    d.shadow_cb_cpu = d.shadow_cb_base_cpu;
    d.shadow_cb_gpu = d.shadow_cb_base_gpu;

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER rp[3]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[0].Descriptor.ShaderRegister = 2;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &range;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[2].Constants.Num32BitValues = 4;
    rp[2].Constants.ShaderRegister = 2;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3;
    rs.pParameters = rp;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return false;
    if (FAILED(d.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(d.csm_root_signature.ReleaseAndGetAddressOf()))))
        return false;

    auto compile = [](const char* src, const char* entry, const char* target) -> ComPtr<ID3DBlob> {
        ComPtr<ID3DBlob> out, errb;
        const HRESULT hr =
            D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                       0, &out, &errb);
        if (FAILED(hr)) {
            if (errb) omsi::log::warn(std::string("CSM shader: ") + static_cast<const char*>(errb->GetBufferPointer()));
            return {};
        }
        return out;
    };
    auto vs = compile(kCsmVs, "vs_main", "vs_5_0");
    auto ps = compile(kCsmPsCutout, "ps_main", "ps_5_0");
    if (!vs || !ps) {
        omsi::log::warn("CSM: shader compile failed");
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = d.csm_root_signature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {nullptr, 0};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.DepthBias = 300;
    pso.RasterizerState.SlopeScaledDepthBias = 1.5f;
    pso.RasterizerState.DepthBiasClamp = 0.f;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pso.InputLayout = {layout, 3};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 0;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    if (FAILED(d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(d.csm_pso_opaque.ReleaseAndGetAddressOf()))))
        return false;

    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    if (FAILED(d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(d.csm_pso_cutout.ReleaseAndGetAddressOf()))))
        return false;

    for (auto& cs : d.csm_cascade) cs = CsmCascadeState{}; // fresh depth array: nothing cached yet
    d.csm_basis_hash = 0;
    d.csm_ready = true;
    omsi::log::info("CSM ready res=" + std::to_string(res) + " cascades=3");
    return true;
}

void update_csm_matrices(HelloO3dRenderer::Impl& d, int only_cascade) {
    if (!d.csm_ready || !d.shadow_cb_cpu) return;
    const float* Ldir = d.shadow_sun_dir; // (stepped: see Impl::shadow_sun_dir)
    XMVECTOR light_dir = DirectX::XMVector3Normalize(DirectX::XMVectorSet(Ldir[0], Ldir[1], Ldir[2], 0.f));
    // Fixed light basis (world) so texel snap stays stable across camera orbit.
    XMVECTOR world_up = DirectX::XMVectorSet(0.f, 0.f, 1.f, 0.f);
    if (std::fabs(DirectX::XMVectorGetZ(light_dir)) > 0.95f) world_up = DirectX::XMVectorSet(0.f, 1.f, 0.f, 0.f);
    XMVECTOR light_right =
        DirectX::XMVector3Normalize(DirectX::XMVector3Cross(world_up, light_dir));
    XMVECTOR light_up = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(light_dir, light_right));
    DirectX::XMFLOAT3 lr{}, lu{}, ld{};
    DirectX::XMStoreFloat3(&lr, light_right);
    DirectX::XMStoreFloat3(&lu, light_up);
    DirectX::XMStoreFloat3(&ld, light_dir);
    d.csm_light_axes[0] = lr.x; d.csm_light_axes[1] = lr.y; d.csm_light_axes[2] = lr.z;
    d.csm_light_axes[3] = lu.x; d.csm_light_axes[4] = lu.y; d.csm_light_axes[5] = lu.z;
    d.csm_light_axes[6] = ld.x; d.csm_light_axes[7] = ld.y; d.csm_light_axes[8] = ld.z;

    XMVECTOR cam_pos = DirectX::XMVectorSet(d.camera_world_pos[0], d.camera_world_pos[1], d.camera_world_pos[2], 0.f);
    XMVECTOR cam_fwd = DirectX::XMVector3Normalize(
        DirectX::XMVectorSet(d.camera_forward[0], d.camera_forward[1], d.camera_forward[2], 0.f));
    XMVECTOR cam_right = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(cam_fwd, world_up));
    if (DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(cam_right)) < 1e-8f)
        cam_right = DirectX::XMVectorSet(1.f, 0.f, 0.f, 0.f);
    XMVECTOR cam_up = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(cam_right, cam_fwd));

    const float fov_y = (d.camera_fov_y > 0.05f) ? d.camera_fov_y : (0.25f * 3.14159265f);
    const float aspect = (d.camera_aspect > 0.1f) ? d.camera_aspect : 1.6f;
    const float tan_half_v = std::tan(fov_y * 0.5f);
    const float tan_half_h = tan_half_v * aspect;

    const float splits[3] = {d.csm_splits[0], d.csm_splits[1], d.csm_splits[2]};
    const UINT res = d.csm_depth ? static_cast<UINT>(d.csm_depth->GetDesc().Width) : 1024;

    // Everything that changes the cascade FITTING problem except the camera pose. If any of it changes
    // (new hour -> new sun direction, FOV/aspect/resolution/split distances) every volume is refitted.
    // Camera position and rotation are deliberately NOT part of it: they are handled by hysteresis below.
    std::uint64_t basis = 1469598103934665603ull;
    csm_mix_floats(basis, d.shadow_sun_dir, 3);
    csm_mix_floats(basis, splits, 3);
    csm_mix_float(basis, fov_y);
    csm_mix_float(basis, aspect);
    csm_mix(basis, res);
    if (basis != d.csm_basis_hash) {
        d.csm_basis_hash = basis;
        for (auto& cs : d.csm_cascade) {
            cs.volume_valid = false;
            cs.shadow_valid = false;
        }
    }

    for (int ci = 0; ci < HelloO3dRenderer::Impl::kCsmCascades; ++ci) {
        // (one cascade a frame: the others keep the matrix their depth slice was drawn with)
        if (only_cascade >= 0 && ci != only_cascade) continue;
        const float near_d = (ci == 0) ? 0.5f : splits[ci - 1];
        const float far_d = splits[ci];
        // 8 frustum-slice corners -> bounding sphere (rotation-stable).
        XMVECTOR corners[8];
        int k = 0;
        for (float z : {near_d, far_d}) {
            const float hy = tan_half_v * z;
            const float hx = tan_half_h * z;
            for (float sy : {-1.f, 1.f}) {
                for (float sx : {-1.f, 1.f}) {
                    corners[k++] = DirectX::XMVectorAdd(
                        cam_pos, DirectX::XMVectorAdd(DirectX::XMVectorScale(cam_fwd, z),
                                                      DirectX::XMVectorAdd(DirectX::XMVectorScale(cam_right, sx * hx),
                                                                           DirectX::XMVectorScale(cam_up, sy * hy))));
                }
            }
        }
        XMVECTOR center = DirectX::XMVectorZero();
        for (int i = 0; i < 8; ++i) center = DirectX::XMVectorAdd(center, corners[i]);
        center = DirectX::XMVectorScale(center, 1.f / 8.f);
        float radius = 0.f;
        for (int i = 0; i < 8; ++i) {
            const float dist =
                DirectX::XMVectorGetX(DirectX::XMVector3Length(DirectX::XMVectorSubtract(corners[i], center)));
            radius = (std::max)(radius, dist);
        }
        radius = std::ceil(radius * 16.f) / 16.f; // stabilize radius quantization
        radius = (std::max)(radius, 4.f);

        // HYSTERESIS: if this cascade's cached light volume still fully contains the slice the camera
        // needs, keep the cached matrix (and therefore the cached shadow map). Without this, any camera
        // move or rotation larger than one texel changed the matrix and redrew the whole cascade.
        DirectX::XMFLOAT3 needed_center{};
        DirectX::XMStoreFloat3(&needed_center, center);
        const float needed_c[3] = {needed_center.x, needed_center.y, needed_center.z};
        auto& cs = d.csm_cascade[ci];
        const float cached_texel = (2.f * cs.radius) / static_cast<float>(res);
        if (csm_volume_reusable(cs, needed_c, radius, 2.f, cached_texel)) continue; // csm_light_vp[ci] unchanged

        // Refit with a safety margin so the next frames of camera motion hit the cache.
        radius = std::ceil(radius * csm_volume_margin(ci) * 16.f) / 16.f;

        // World-space texel snap along light right/up (before LookAt).
        const float texel = (2.f * radius) / static_cast<float>(res);
        const float snap_r = DirectX::XMVectorGetX(DirectX::XMVector3Dot(center, light_right));
        const float snap_u = DirectX::XMVectorGetX(DirectX::XMVector3Dot(center, light_up));
        const float snapped_r = std::floor(snap_r / texel) * texel;
        const float snapped_u = std::floor(snap_u / texel) * texel;
        center = DirectX::XMVectorAdd(center, DirectX::XMVectorScale(light_right, snapped_r - snap_r));
        center = DirectX::XMVectorAdd(center, DirectX::XMVectorScale(light_up, snapped_u - snap_u));
        // Also quantize motion along the light axis (keeps the matrix identical for sub-texel motion).
        const float snap_l = DirectX::XMVectorGetX(DirectX::XMVector3Dot(center, light_dir));
        const float snapped_l = std::floor(snap_l / texel) * texel;
        center = DirectX::XMVectorAdd(center, DirectX::XMVectorScale(light_dir, snapped_l - snap_l));

        // sun_dir points toward the sun (lighting N.L); place the light eye on that side looking at the slice.
        XMVECTOR eye = DirectX::XMVectorAdd(center, DirectX::XMVectorScale(light_dir, radius + 80.f));
        const XMMATRIX light_view = DirectX::XMMatrixLookAtLH(eye, center, light_up);
        const XMMATRIX ortho =
            DirectX::XMMatrixOrthographicOffCenterLH(-radius, radius, -radius, radius, 0.5f, radius * 2.f + 160.f);
        DirectX::XMStoreFloat4x4(&d.csm_light_vp[ci], DirectX::XMMatrixTranspose(light_view * ortho));

        DirectX::XMFLOAT3 final_center{};
        DirectX::XMStoreFloat3(&final_center, center);
        cs.volume_valid = true;
        cs.center[0] = final_center.x;
        cs.center[1] = final_center.y;
        cs.center[2] = final_center.z;
        cs.radius = radius;
    }

    auto* cb = reinterpret_cast<ShadowCbCpu*>(d.shadow_cb_cpu);
    for (int i = 0; i < 3; ++i) cb->light_vp[i] = d.csm_light_vp[i];
    cb->splits[0] = splits[0];
    cb->splits[1] = splits[1];
    cb->splits[2] = splits[2];
    cb->splits[3] = 1.f / static_cast<float>(res);
    cb->cam_pos[0] = d.camera_world_pos[0];
    cb->cam_pos[1] = d.camera_world_pos[1];
    cb->cam_pos[2] = d.camera_world_pos[2];
    cb->cam_pos[3] = d.sun_shadow_enable;
    // (depth bias in the light's depth range of 2r + 160 m: 0.001 was 18-36 cm, so a shadow began that far
    //  off the foot of whatever cast it - a gap at every contact, objects floating from afar. ~3-5 cm now;
    //  acne is held off by the per-cascade normal offset in SampleShadowCascade instead)
    cb->bias[0] = 0.00009f;
    cb->bias[1] = 0.025f;
    cb->bias[2] = 1.f / static_cast<float>(res);
    cb->bias[3] = 0.f;
    cb->cam_forward[0] = d.camera_forward[0];
    cb->cam_forward[1] = d.camera_forward[1];
    cb->cam_forward[2] = d.camera_forward[2];
    cb->cam_forward[3] = d.fog_scale;
}

void render_csm_pass(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd, std::uint32_t frame_index,
                     int only_cascade, bool serial, int also_cascade) {
    if (!d.csm_ready || d.sun_shadow_enable < 0.5f || !cmd) return;
    if (!d.csm_pso_opaque || !d.instance_buffer_cpu) return;
    // Until every cascade has a depth slice (start, a new sun direction) all of them are drawn at once; after
    // that one a frame (see draw.cpp): every stale cascade redrawn together every other frame made a 13 ms
    // frame out of every second one with the AI traffic moving in all of them.
    for (const auto& cs : d.csm_cascade)
        if (!cs.shadow_valid || !cs.volume_valid) only_cascade = -1;
    if (only_cascade < 0 || also_cascade == only_cascade) also_cascade = -1;

    update_csm_matrices(d, only_cascade);
    if (also_cascade >= 0) update_csm_matrices(d, also_cascade);

    // Candidate casters with world-space bounding spheres (one cheap pass; reused by all cascades).
    static thread_local std::vector<ShadowCaster> casters;
    static thread_local std::vector<std::uint64_t> caster_hash;
    static thread_local std::vector<std::uint32_t> per_cascade[HelloO3dRenderer::Impl::kCsmCascades];
    // Per-chunk result of the parallel collection pass (index == mesh_chunks index).
    struct CasterSlot {
        ShadowCaster sc;
        std::uint64_t hash = 0;
        bool ok = false;
    };
    static thread_local std::vector<CasterSlot> slots;
    casters.clear();
    caster_hash.clear();
    casters.reserve(d.mesh_chunks.size() + 1);
    caster_hash.reserve(d.mesh_chunks.size() + 1);
    // Camera frustum + render distance: do NOT rasterize shadow casters the camera cannot see.
    // (Shadow maps are rendered from the light, so without this every off-screen chunk within the
    // load radius was still being drawn into the cascades.) A margin keeps shadows near screen edges.
    omsi::Frustum cam_frustum{};
    bool cam_frustum_ok = false;
    if (d.has_view_proj_) {
        const DirectX::XMMATRIX vp_t = DirectX::XMLoadFloat4x4(&d.view_proj_transposed_);
        cam_frustum = omsi::extract_frustum_planes_dx(vp_t);
        cam_frustum_ok = true;
    }
    const float cam_cull = d.cull_max_distance > 0.f ? d.cull_max_distance : 1.0e9f;
    // Thread-safe per chunk: only writes that chunk's own mutable sphere cache and the given slot.
    auto eval_caster = [&](const GpuMeshChunk& chunk, CasterSlot& out) {
        out.ok = false;
        if (!chunk_can_cast_shadow(chunk)) return;
        ShadowCaster& sc = out.sc;
        sc.chunk = &chunk;
        float bbox[6] = {chunk.meta.bbox_min[0], chunk.meta.bbox_min[1], chunk.meta.bbox_min[2],
                         chunk.meta.bbox_max[0], chunk.meta.bbox_max[1], chunk.meta.bbox_max[2]};
        if (chunk.csm_sphere_valid &&
            std::memcmp(&chunk.csm_sphere_matrix, &chunk.local_world_transposed, sizeof(XMFLOAT4X4)) == 0 &&
            std::memcmp(chunk.csm_sphere_bbox, bbox, sizeof(bbox)) == 0) {
            sc.c[0] = chunk.csm_sphere_c[0];
            sc.c[1] = chunk.csm_sphere_c[1];
            sc.c[2] = chunk.csm_sphere_c[2];
            sc.r = chunk.csm_sphere_r;
        } else {
            csm_chunk_world_sphere(&chunk.local_world_transposed._11, chunk.meta.bbox_min, chunk.meta.bbox_max,
                                   sc.c, sc.r);
            chunk.csm_sphere_matrix = chunk.local_world_transposed;
            std::memcpy(chunk.csm_sphere_bbox, bbox, sizeof(bbox));
            chunk.csm_sphere_c[0] = sc.c[0];
            chunk.csm_sphere_c[1] = sc.c[1];
            chunk.csm_sphere_c[2] = sc.c[2];
            chunk.csm_sphere_r = sc.r;
            chunk.csm_sphere_valid = true;
        }
        if (cam_frustum_ok) {
            const float dx = sc.c[0] - d.camera_world_pos[0];
            const float dy = sc.c[1] - d.camera_world_pos[1];
            const float dz = sc.c[2] - d.camera_world_pos[2];
            const float lim = cam_cull + sc.r;
            if (dx * dx + dy * dy + dz * dz > lim * lim) return;
            const float margin = sc.r + 25.f; // keep casters just outside the frame
            for (const auto& p : cam_frustum.planes) {
                if (p.a * sc.c[0] + p.b * sc.c[1] + p.c * sc.c[2] + p.d < -margin) return;
            }
        }
        // Hashed once per caster (was once per caster PER cascade, serially).
        std::uint64_t h = 1469598103934665603ull;
        shadow_hash_caster(h, chunk);
        out.hash = h;
        out.ok = true;
    };
    if (d.has_ground_plane) {
        CasterSlot gs;
        eval_caster(d.ground_chunk, gs);
        if (gs.ok) {
            casters.push_back(gs.sc);
            caster_hash.push_back(gs.hash);
        }
    }
    {
        const std::size_t n_chunks = d.mesh_chunks.size();
        slots.resize(n_chunks);
        // Capture a plain pointer: `slots` is thread_local and workers would otherwise see their own copy.
        CasterSlot* const slot_ptr = slots.data();
        auto eval_i = [&, slot_ptr](std::size_t i) { eval_caster(d.mesh_chunks[i], slot_ptr[i]); };
        // Tens of thousands of chunks: split across the pool (small scenes stay serial).
        // (serial when this runs on a pool worker itself - see draw.cpp - a nested parallel_for there could
        //  wait behind the main thread's own)
        if (n_chunks >= 2048 && !serial) omsi::JobSystem::global().parallel_for(0, n_chunks, eval_i, 1024);
        else for (std::size_t i = 0; i < n_chunks; ++i) eval_i(i);
        for (std::size_t i = 0; i < n_chunks; ++i) {
            if (!slots[i].ok) continue;
            casters.push_back(slots[i].sc);
            caster_hash.push_back(slots[i].hash);
        }
    }

    const float* right = d.csm_light_axes;
    const float* up = d.csm_light_axes + 3;
    const float* dir = d.csm_light_axes + 6;

    bool stale[HelloO3dRenderer::Impl::kCsmCascades]{};
    bool any_stale = false;
    const float csm_res = d.csm_depth ? static_cast<float>(d.csm_depth->GetDesc().Width) : 1024.f;
    for (int ci = 0; ci < HelloO3dRenderer::Impl::kCsmCascades; ++ci) {
        auto& cs = d.csm_cascade[ci];
        auto& list = per_cascade[ci];
        list.clear();
        if (only_cascade >= 0 && ci != only_cascade && ci != also_cascade) continue; // (not this frame's: its slice stays)
        // a caster under ~1.5 texels of this cascade's map casts no shadow anyone can see there
        const float min_r = 0.75f * (2.f * cs.radius / csm_res);
        std::uint64_t scene_hash = 1469598103934665603ull;
        for (std::uint32_t i = 0; i < casters.size(); ++i) {
            const auto& sc = casters[i];
            if (sc.r < min_r) continue;
            // trees cast into the near and middle cascades only: in the far one their shadow is a blob of a few
            // pixels, and the thousands of alpha-tested foliage quads there were most of the shadow cost (43 vs
            // 67 fps on a 100 km/h drive through Thuringer Wald)
            if (ci == HelloO3dRenderer::Impl::kCsmCascades - 1 && sc.chunk && sc.chunk->wind_sway) continue;
            // Only chunks that can land in THIS cascade's depth map matter: for hashing and for drawing.
            if (!csm_sphere_in_light_volume(sc.c, sc.r, cs.center, cs.radius, right, up, dir, 80.f)) continue;
            list.push_back(i);
            csm_mix(scene_hash, caster_hash[i]);
        }
        csm_mix(scene_hash, list.size());
        std::uint64_t matrix_hash = 1469598103934665603ull;
        csm_mix_floats(matrix_hash, &d.csm_light_vp[ci]._11, 16);
        stale[ci] = !cs.shadow_valid || matrix_hash != cs.cached_matrix_hash || scene_hash != cs.cached_scene_hash;
        if (stale[ci]) {
            cs.cached_matrix_hash = matrix_hash;
            cs.cached_scene_hash = scene_hash;
            any_stale = true;
        }
    }

    const UINT res = static_cast<UINT>(d.csm_depth->GetDesc().Width);
    // The caller's frame-end barrier returns this resource to DEPTH_WRITE every frame. Even when every
    // cascade is cached, transition it back to PIXEL_SHADER_RESOURCE for the main pass without drawing.
    if (any_stale) {
        cmd->SetGraphicsRootSignature(d.csm_root_signature.Get());
        ID3D12DescriptorHeap* heaps[] = {d.srv_heap.Get()};
        cmd->SetDescriptorHeaps(1, heaps);

        const std::uint32_t inst_base = frame_index * HelloO3dRenderer::Impl::kMaxInstancesPerDraw;
        // Upper half of the per-frame instance ring is reserved for shadow casters.
        std::uint32_t shadow_cursor = HelloO3dRenderer::Impl::kMaxInstancesPerDraw / 2;

        for (int ci = 0; ci < HelloO3dRenderer::Impl::kCsmCascades; ++ci) {
            if (!stale[ci]) continue; // this cascade's depth slice is still valid from an earlier frame
            D3D12_VIEWPORT vp{0.f, 0.f, static_cast<float>(res), static_cast<float>(res), 0.f, 1.f};
            D3D12_RECT sc{0, 0, static_cast<LONG>(res), static_cast<LONG>(res)};
            cmd->RSSetViewports(1, &vp);
            cmd->RSSetScissorRects(1, &sc);
            cmd->OMSetRenderTargets(0, nullptr, FALSE, &d.csm_dsv_cpu[ci]);
            cmd->ClearDepthStencilView(d.csm_dsv_cpu[ci], D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);

            const XMFLOAT4X4& light_vp_t = d.csm_light_vp[ci];
            const XMMATRIX light_vp = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&light_vp_t));

            for (const std::uint32_t idx : per_cascade[ci])
                draw_chunk_shadow(d, cmd, *casters[idx].chunk, shadow_cursor, inst_base, light_vp);
            d.csm_cascade[ci].shadow_valid = true;
        }
    }

    const D3D12_RESOURCE_BARRIER barrier =
        dx12_transition_barrier(d.csm_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &barrier);
}

void csm_end_frame_barrier(HelloO3dRenderer::Impl& d, ID3D12GraphicsCommandList* cmd) {
    if (!d.csm_ready || !d.csm_depth || !cmd || d.sun_shadow_enable < 0.5f) return;
    const D3D12_RESOURCE_BARRIER barrier =
        dx12_transition_barrier(d.csm_depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(1, &barrier);
}

} // namespace omsi::gfx::dx12
