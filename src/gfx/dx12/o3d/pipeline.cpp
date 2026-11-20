#include "gfx/dx12/hello_o3d_renderer.hpp"

#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/o3d/internal.hpp"
#include "omsi/visual_quality.hpp"
#include "gfx/dx12/dx12_helpers.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/logger.hpp"
#include "omsi/model_cfg.hpp"
#include "omsi/visible_runtime.hpp"
#include "omsi/o3d_format.hpp"
#include "o3d/o3d_loader.hpp"

#include <DirectXMath.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <objbase.h>
#include <wrl/client.h>

#include <fstream>
#include <cstring>
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")

namespace omsi::gfx::dx12 {

bool HelloO3dRenderer::create_pipeline() {
    auto& d = *impl_;

    // Material table: t0 albedo, t1 env (or the BeamNG 1x1 material params), then t4..t7 (normal /
    // detail / detail normal / opacityMap) for BeamNG materials. OMSI batches only fill the first two
    // slots; their shaders never read t4..t7.
    D3D12_DESCRIPTOR_RANGE srv_ranges[2]{};
    srv_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_ranges[0].NumDescriptors = 2;
    srv_ranges[0].BaseShaderRegister = 0;
    srv_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    srv_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_ranges[1].NumDescriptors = 4;
    srv_ranges[1].BaseShaderRegister = 4;
    srv_ranges[1].OffsetInDescriptorsFromTableStart = 2;

    D3D12_DESCRIPTOR_RANGE shadow_srv_range{};
    shadow_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadow_srv_range.NumDescriptors = 1;
    shadow_srv_range.BaseShaderRegister = 3;

    D3D12_ROOT_PARAMETER root_params[6]{};
    // Root CBV kept for legacy; WVP for draws comes from instance StructuredBuffer (t2).
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_params[0].Descriptor.ShaderRegister = 0;
    root_params[0].Descriptor.RegisterSpace = 0;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[1].DescriptorTable.NumDescriptorRanges = 2;
    root_params[1].DescriptorTable.pDescriptorRanges = srv_ranges;
    root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    root_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    // 56 = 13 floats + 3 pad (HLSL float4 align) + 32 lights + 8 sun
    root_params[2].Constants.Num32BitValues = 56; // 56 + 2+1+2+2+1 = 64 DWORDs, the D3D12 limit
    root_params[2].Constants.ShaderRegister = 2;
    root_params[2].Constants.RegisterSpace = 0;
    root_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // P6: StructuredBuffer<InstanceData> gInstance : register(t2)
    root_params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    root_params[3].Descriptor.ShaderRegister = 2;
    root_params[3].Descriptor.RegisterSpace = 0;
    root_params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    // ShadowCB (b3) — cascade VP + camera for CSM PCF
    root_params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_params[4].Descriptor.ShaderRegister = 3;
    root_params[4].Descriptor.RegisterSpace = 0;
    root_params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // gShadowMap Texture2DArray : register(t3)
    root_params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[5].DescriptorTable.NumDescriptorRanges = 1;
    root_params[5].DescriptorTable.pDescriptorRanges = &shadow_srv_range;
    root_params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[4]{};
    // Aniso 16× WRAP for tiling ground/walls (MS Learn AF).
    samplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[0].MipLODBias = omsi::visual_quality_mip_lod_bias();
    samplers[0].MaxAnisotropy = 16;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samplers[0].MinLOD = 0.f;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // s1: [useTextTexture] point, no mips.
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    // WRAP, as Omsi.exe samples every texture: text planes are often mapped at negative V (the route
    // arrows' board at v -0.87..-0.13) - clamped, every texel read the empty top row and the text vanished
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].MipLODBias = 0.f;
    samplers[1].MaxAnisotropy = 1;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samplers[1].MinLOD = 0.f;
    samplers[1].MaxLOD = 0.f;
    samplers[1].ShaderRegister = 1;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // s2: alpha-cutout (trees/fences) — CLAMP so UV edges don't sample the opposite side.
    samplers[2] = samplers[0];
    samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].ShaderRegister = 2;
    // s3: CSM PCF — standard depth (clear=1, write LESS_EQUAL): SampleCmp LESS_EQUAL → 1 = lit.
    samplers[3].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[3].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[3].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[3].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[3].MipLODBias = 0.f;
    samplers[3].MaxAnisotropy = 1;
    samplers[3].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[3].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[3].MinLOD = 0.f;
    samplers[3].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[3].ShaderRegister = 3;
    samplers[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 6;
    rs_desc.pParameters = root_params;
    rs_desc.NumStaticSamplers = 4;
    rs_desc.pStaticSamplers = samplers;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig_blob;
    ComPtr<ID3DBlob> sig_error;
    throw_hr(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.GetAddressOf(),
                                         sig_error.GetAddressOf()),
             "SerializeRootSignature");
    throw_hr(d.device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                           IID_PPV_ARGS(d.root_signature.GetAddressOf())),
             "CreateRootSignature");

    auto vs = compile_shader(kShaderSource, "vs_main", "vs_5_1");
    auto ps = compile_shader(kShaderSource, "ps_main", "ps_5_1");
    auto vs_surface = compile_shader(kShaderSource, "vs_surface", "vs_5_1");
    auto vs_spline = compile_shader(kShaderSource, "vs_spline", "vs_5_1");
    auto vs_rail = compile_shader(kShaderSource, "vs_rail", "vs_5_1");
    auto vs_terrain_under = compile_shader(kShaderSource, "vs_terrain_under", "vs_5_1");
    auto vs_terrain = compile_shader(kShaderSource, "vs_terrain", "vs_5_1");

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {layout, _countof(layout)};
    pso.pRootSignature = d.root_signature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // Opaque BACK. VIEW_MIRROR_X flips clip X → FrontCCW. O3D Y↔Z (det<0) flips indices at upload.
    // Alpha / on_surface stay NONE (glass, trees, decals). If a mesh looks hollow, fix that winding.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso.RasterizerState.FrontCounterClockwise = d.clip_x_mirrored ? TRUE : FALSE;
    // MSAA: MultisampleEnable + AntialiasedLineEnable (MS docs / flip-model resolve path).
    pso.RasterizerState.MultisampleEnable = d.msaa_samples > 1 ? TRUE : FALSE;
    pso.RasterizerState.AntialiasedLineEnable = d.msaa_samples > 1 ? TRUE : FALSE;
    // Reverse-Z: positive bias is closer to camera (near=1/far=0, GREATER_EQUAL).
    // Give ordinary SCO meshes a tiny tie-break over terrain on almost-coplanar triangles.
    if (omsi::visual_quality_enabled()) {
        pso.RasterizerState.DepthBias = 0;          // (no angle-dependent bias anywhere: layers are fixed offsets)
        pso.RasterizerState.SlopeScaledDepthBias = 0.f;
        pso.RasterizerState.DepthBiasClamp = 0.f;
    }
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    // Reverse-Z (luna_proj_matrix swaps near/far): near=1 far=0 → GREATER_EQUAL + clear 0.
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = (std::max)(1u, d.msaa_samples);
    pso.SampleDesc.Quality = d.msaa_samples > 1 ? d.msaa_quality : 0;
    throw_hr(d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(d.pipeline_state.GetAddressOf())), "CreatePSO");

    // BeamNG doubleSided materials (leaves, bus-stop glass, grass): same states, no back-face culling.
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC ds = pso;
        ds.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        throw_hr(d.device->CreateGraphicsPipelineState(&ds,
                                                       IID_PPV_ARGS(d.pipeline_state_double_sided.GetAddressOf())),
                 "CreateDoubleSidedPSO");
    }

    auto ps_alpha = compile_shader(kShaderSourceAlpha, "ps_main", "ps_5_1");
    // Alpha-test + doubleSided: alpha PS (1-bit punch, no blend) with CULL_NONE and depth write ON.
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC ds_cut = pso;
        ds_cut.PS = {ps_alpha->GetBufferPointer(), ps_alpha->GetBufferSize()};
        ds_cut.BlendState.RenderTarget[0].BlendEnable = FALSE;
        ds_cut.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        ds_cut.DepthStencilState.DepthEnable = TRUE;
        ds_cut.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        ds_cut.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        throw_hr(d.device->CreateGraphicsPipelineState(
                     &ds_cut, IID_PPV_ARGS(d.pipeline_state_double_sided_cutout.GetAddressOf())),
                 "CreateDoubleSidedCutoutPSO");
    }
    auto make_alpha_pso = [&](bool no_zwrite, bool no_zcheck, ID3D12PipelineState** out,
                              ID3D12PipelineState** out_cull = nullptr) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC alpha_pso = pso;
        alpha_pso.PS = {ps_alpha->GetBufferPointer(), ps_alpha->GetBufferSize()};
        auto& blend = alpha_pso.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        alpha_pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        alpha_pso.DepthStencilState.DepthWriteMask =
            no_zwrite ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
        alpha_pso.DepthStencilState.DepthEnable = no_zcheck ? FALSE : TRUE;
        throw_hr(d.device->CreateGraphicsPipelineState(&alpha_pso, IID_PPV_ARGS(out)), "CreateAlphaPSO");
        if (out_cull) {
            alpha_pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            throw_hr(d.device->CreateGraphicsPipelineState(&alpha_pso, IID_PPV_ARGS(out_cull)), "CreateAlphaCullPSO");
        }
    };
    make_alpha_pso(false, false, d.pipeline_state_alpha.GetAddressOf(), d.pipeline_state_alpha_cull.GetAddressOf());
    make_alpha_pso(true, false, d.pipeline_state_alpha_nozwrite.GetAddressOf(),
                   d.pipeline_state_alpha_nozwrite_cull.GetAddressOf());
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC g = pso;
        g.VS = {vs_surface->GetBufferPointer(), vs_surface->GetBufferSize()};
        g.PS = {ps_alpha->GetBufferPointer(), ps_alpha->GetBufferSize()};
        auto& blend = g.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        g.DepthStencilState.DepthEnable = TRUE;
        g.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        g.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        throw_hr(d.device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(d.pipeline_state_ghost_surface.GetAddressOf())),
                 "CreateGhostSurfacePSO");
        g.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        throw_hr(d.device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(d.pipeline_state_ghost_surface_cull.GetAddressOf())),
                 "CreateGhostSurfaceCullPSO");
        g.VS = {vs_spline->GetBufferPointer(), vs_spline->GetBufferSize()};
        throw_hr(d.device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(d.pipeline_state_ghost_spline.GetAddressOf())),
                 "CreateGhostSplinePSO");
    }
    // Vegetation cutout (OMSI_VEG_CUTOUT): blend OFF + depth write ON so alpha-tested foliage
    // writes depth and stops overdrawing (alpha-blend billboards shade every layer).
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC cut = pso;
        cut.PS = {ps_alpha->GetBufferPointer(), ps_alpha->GetBufferSize()};
        cut.BlendState.RenderTarget[0].BlendEnable = FALSE;
        // Vegetation edges: alpha-to-coverage when MSAA is on (ps_alpha writes coverage alpha for wind-tagged batches).
        cut.BlendState.AlphaToCoverageEnable = d.msaa_samples > 1 ? TRUE : FALSE;
        cut.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        cut.DepthStencilState.DepthEnable = TRUE;
        cut.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        cut.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        throw_hr(d.device->CreateGraphicsPipelineState(&cut,
                                                      IID_PPV_ARGS(d.pipeline_state_veg_cutout.GetAddressOf())),
                 "CreateVegCutoutPSO");
    }
    {
        // openOMSI (omsi-render frame_plan instance_depth_bias / scene_clip): roads and [surface] objects get
        // NO raster depth bias - only the planar pull of their vertex shader (vs_spline / vs_surface, the
        // 0.3 % / 0.4 % + 1 cm share of the way to the eye) - and write their depth like everything else.
        // (Our own slope-scaled biases and two-pass depth grew with the viewing angle and cut the feet of
        // whatever stood on a .sli; all removed.)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC biased = pso;
        biased.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        biased.RasterizerState.DepthBias = 0;
        biased.RasterizerState.SlopeScaledDepthBias = 0.f;
        biased.RasterizerState.DepthBiasClamp = 0.f;
        biased.DepthStencilState.DepthEnable = TRUE;
        biased.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        biased.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        biased.VS = {vs_surface->GetBufferPointer(), vs_surface->GetBufferSize()};
        throw_hr(d.device->CreateGraphicsPipelineState(&biased,
                                                       IID_PPV_ARGS(d.pipeline_state_surface_bias.GetAddressOf())),
                 "CreateSurfaceBiasPSO");
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC biased_cull = biased;
            biased_cull.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
            throw_hr(d.device->CreateGraphicsPipelineState(
                         &biased_cull, IID_PPV_ARGS(d.pipeline_state_surface_bias_cull.GetAddressOf())),
                     "CreateSurfaceBiasCullPSO");
        }

        // Spline/road surfaces: openOMSI road code 1.0 (vs_spline pull), back faces culled.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC spline_bias = biased;
        spline_bias.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        spline_bias.VS = {vs_spline->GetBufferPointer(), vs_spline->GetBufferSize()};
        throw_hr(d.device->CreateGraphicsPipelineState(
                     &spline_bias, IID_PPV_ARGS(d.pipeline_state_spline_bias.GetAddressOf())),
                 "CreateSplineBiasPSO");

        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC rail = pso;
            rail.VS = {vs_rail->GetBufferPointer(), vs_rail->GetBufferSize()};
            rail.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            rail.RasterizerState.DepthBias = 0;
            rail.RasterizerState.SlopeScaledDepthBias = 0.f;
            rail.DepthStencilState.DepthEnable = TRUE;
            rail.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            rail.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            throw_hr(d.device->CreateGraphicsPipelineState(&rail, IID_PPV_ARGS(d.pipeline_state_path_rail.GetAddressOf())),
                     "CreatePathRailPSO");
        }
        // Reverse-Z: negative bias moves terrain away, so near-coplanar SCOs win depth tests.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC terrain_bias = pso;
        terrain_bias.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        terrain_bias.VS = {vs_terrain->GetBufferPointer(), vs_terrain->GetBufferSize()};
        // (openOMSI: the terrain is drawn as it is - no push, no bias)
        terrain_bias.RasterizerState.DepthBias = 0;
        terrain_bias.RasterizerState.SlopeScaledDepthBias = 0.f;
        terrain_bias.RasterizerState.DepthBiasClamp = 0.f;
        terrain_bias.DepthStencilState.DepthEnable = TRUE;
        terrain_bias.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        terrain_bias.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        throw_hr(d.device->CreateGraphicsPipelineState(
                     &terrain_bias, IID_PPV_ARGS(d.pipeline_state_terrain_bias.GetAddressOf())),
                 "CreateTerrainBiasPSO");

        // Underlay: the uncut base mesh of a tile whose cut detail mesh is drawn on top. Pushed further back than the
        // detail terrain (-32) so the detail wins where both exist, and any hole in the cut shows this surface.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC terrain_underlay = terrain_bias;
        terrain_underlay.VS = {vs_terrain_under->GetBufferPointer(), vs_terrain_under->GetBufferSize()};
        terrain_underlay.RasterizerState.DepthBias = 0; // (fixed 40 cm down in its vertex shader)
        terrain_underlay.RasterizerState.SlopeScaledDepthBias = 0.f;
        throw_hr(d.device->CreateGraphicsPipelineState(
                     &terrain_underlay, IID_PPV_ARGS(d.pipeline_state_terrain_underlay.GetAddressOf())),
                 "CreateTerrainUnderlayPSO");

        // [rendertype] on_surface: terrain decal. D32 depth bias + no Z write (Unit128 priority=2).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC decal = biased;
        decal.PS = {ps_alpha->GetBufferPointer(), ps_alpha->GetBufferSize()};
        auto& blend = decal.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        decal.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        decal.DepthStencilState.DepthEnable = TRUE;
        decal.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        // openOMSI decal code 1.25: the vs_surface pull alone, no raster bias (a slope bias grew with the
        // viewing angle and cut through what stood on the decal)
        decal.VS = {vs_surface->GetBufferPointer(), vs_surface->GetBufferSize()};
        decal.RasterizerState.DepthBias = 0;
        decal.RasterizerState.SlopeScaledDepthBias = 0.f;
        decal.RasterizerState.DepthBiasClamp = 0.f;
        throw_hr(d.device->CreateGraphicsPipelineState(&decal, IID_PPV_ARGS(d.pipeline_state_on_surface.GetAddressOf())),
                 "CreateOnSurfacePSO");
        decal.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        throw_hr(d.device->CreateGraphicsPipelineState(&decal, IID_PPV_ARGS(d.pipeline_state_on_surface_cull.GetAddressOf())),
                 "CreateOnSurfaceCullPSO");
    }
    {
        // [rendertype] presurface is a TMap pass (before terrain). Opaque copy = normal Z.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pre = pso;
        throw_hr(d.device->CreateGraphicsPipelineState(&pre, IID_PPV_ARGS(d.pipeline_state_presurface.GetAddressOf())),
                 "CreatePresurfacePSO");
        // Hole mesh: write Z, no color. Reverse-Z: +bias toward camera so terrain cannot fill the hole.
        auto ps_pre_depth = compile_shader(kShaderSourcePresurfaceDepth, "ps_main", "ps_5_1");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pre_a = pre;
        pre_a.PS = {ps_pre_depth->GetBufferPointer(), ps_pre_depth->GetBufferSize()};
        pre_a.BlendState.RenderTarget[0].BlendEnable = FALSE;
        pre_a.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        pre_a.DepthStencilState.DepthEnable = TRUE;
        pre_a.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pre_a.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        pre_a.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pre_a.RasterizerState.DepthBias = 8;
        pre_a.RasterizerState.SlopeScaledDepthBias = 2.f;
        pre_a.RasterizerState.DepthBiasClamp = 0.f;
        throw_hr(d.device->CreateGraphicsPipelineState(&pre_a,
                                                       IID_PPV_ARGS(d.pipeline_state_presurface_alpha.GetAddressOf())),
                 "CreatePresurfaceAlphaPSO");
    }
    make_alpha_pso(true, true, d.pipeline_state_alpha_nozcheck.GetAddressOf());
    // OMSI vehicle shadow: matl_alpha=2 + matl_noZwrite; depth test ON in viewer (noZcheck breaks without terrain).
    make_alpha_pso(true, false, d.pipeline_state_alpha_shadow.GetAddressOf());
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC inv_pso = pso;
        inv_pso.PS = {ps_alpha->GetBufferPointer(), ps_alpha->GetBufferSize()};
        auto& blend = inv_pso.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_INV_DEST_COLOR;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        inv_pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        inv_pso.DepthStencilState.DepthEnable = FALSE;
        inv_pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        throw_hr(d.device->CreateGraphicsPipelineState(&inv_pso, IID_PPV_ARGS(d.pipeline_state_invert.GetAddressOf())),
                 "CreateInvertPSO");
    }
    {
        // Reuse triangle VB/IB; rasterizer draws edges only (MS D3D12_FILL_MODE_WIREFRAME).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC wf_pso = pso;
        wf_pso.PS = {ps_alpha->GetBufferPointer(), ps_alpha->GetBufferSize()};
        wf_pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        wf_pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        wf_pso.RasterizerState.AntialiasedLineEnable = TRUE;
        // Lines drawn over their own faces (mesh edit face outlines): reverse-Z, positive bias = nearer,
        // so the outline wins the depth test instead of flickering against the surface.
        wf_pso.RasterizerState.DepthBias = 64;
        wf_pso.RasterizerState.SlopeScaledDepthBias = 1.5f;
        wf_pso.RasterizerState.DepthBiasClamp = 0.f;
        throw_hr(d.device->CreateGraphicsPipelineState(&wf_pso, IID_PPV_ARGS(d.pipeline_state_wireframe.GetAddressOf())),
                 "CreateWireframePSO");
    }
    // Pre-create CSM so main-pass root[4]/[5] always have valid CB/SRV bindings.
    if (!ensure_csm_resources(d)) omsi::log::warn("CSM resources unavailable — sun shadows disabled");
    return true;
}

bool HelloO3dRenderer::create_axis_pipeline() {
    auto& d = *impl_;

    D3D12_DESCRIPTOR_RANGE cbv_range{};
    cbv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    cbv_range.NumDescriptors = 1;
    cbv_range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER root_param{};
    root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_param.DescriptorTable.NumDescriptorRanges = 1;
    root_param.DescriptorTable.pDescriptorRanges = &cbv_range;
    root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 1;
    rs_desc.pParameters = &root_param;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig_blob;
    ComPtr<ID3DBlob> sig_error;
    throw_hr(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.GetAddressOf(),
                                         sig_error.GetAddressOf()),
             "SerializeAxisRootSignature");
    throw_hr(d.device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                                           IID_PPV_ARGS(d.axis_root_signature.GetAddressOf())),
             "CreateAxisRootSignature");

    auto vs = compile_shader(kAxisShaderSource, "vs_main", "vs_5_0");
    auto ps = compile_shader(kAxisShaderSource, "ps_main", "ps_5_0");

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {layout, _countof(layout)};
    pso.pRootSignature = d.axis_root_signature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FrontCounterClockwise = FALSE;
    pso.RasterizerState.MultisampleEnable = d.msaa_samples > 1 ? TRUE : FALSE;
    pso.RasterizerState.AntialiasedLineEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = (std::max)(1u, d.msaa_samples);
    pso.SampleDesc.Quality = d.msaa_samples > 1 ? d.msaa_quality : 0;
    throw_hr(d.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(d.axis_pipeline_state.GetAddressOf())),
             "CreateAxisPSO");
    return true;
}

bool HelloO3dRenderer::upload_axis_gpu() {
    auto& d = *impl_;
    constexpr float L = 1.05f;
    constexpr float tick = 0.14f;
    constexpr float letter_scale = 0.34f;
    constexpr float letter_offset = 0.22f;

    std::vector<AxisVertex> axis_vertices;
    axis_vertices.reserve(64);

    // OMSI world: X lateral, Y forward, Z = height (up).
    axis_add_segment(axis_vertices, 0.f, 0.f, 0.f, L, 0.f, 0.f, 1.f, 0.25f, 0.25f);
    axis_add_segment(axis_vertices, 0.f, 0.f, 0.f, 0.f, L, 0.f, 0.25f, 1.f, 0.25f);
    axis_add_segment(axis_vertices, 0.f, 0.f, 0.f, 0.f, 0.f, L, 0.35f, 0.55f, 1.f);

    axis_add_segment(axis_vertices, L - tick, -tick, 0.f, L - tick, tick, 0.f, 1.f, 0.35f, 0.35f);
    axis_add_segment(axis_vertices, -tick, L - tick, 0.f, tick, L - tick, 0.f, 0.35f, 1.f, 0.35f);
    axis_add_segment(axis_vertices, 0.f, -tick, L - tick, 0.f, tick, L - tick, 0.45f, 0.65f, 1.f);

    const float lx = L + letter_offset;
    const float ly = L + letter_offset;
    const float lz = L + letter_offset;

    axis_add_letter(axis_vertices, {{-0.5f, -0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f, -0.5f}}, lx, 0.f, 0.f,
                    AxisLetterPlane::YZ, letter_scale, 1.f, 0.35f, 0.35f);
    axis_add_letter(axis_vertices, {{-0.5f, 0.5f, 0.f, 0.f}, {0.5f, 0.5f, 0.f, 0.f}, {0.f, -0.08f, 0.f, -0.5f}}, 0.f,
                    ly, 0.f, AxisLetterPlane::XZ, letter_scale, 0.35f, 1.f, 0.35f);
    axis_add_letter(axis_vertices, {{-0.5f, 0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f, -0.5f}},
                    0.f, 0.f, lz, AxisLetterPlane::XY, letter_scale, 0.45f, 0.65f, 1.f);

    d.axis_vertex_buffer.Reset();
    d.axis_vertex_count = static_cast<std::uint32_t>(axis_vertices.size());

    const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    const UINT vb_size = static_cast<UINT>(axis_vertices.size() * sizeof(AxisVertex));
    const CD3DX12_RESOURCE_DESC vb_desc = CD3DX12_RESOURCE_DESC::Buffer(vb_size);
    throw_hr(d.device->CreateCommittedResource(
                 &upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(d.axis_vertex_buffer.GetAddressOf())),
             "axis vertex buffer");

    void* mapped = nullptr;
    throw_hr(d.axis_vertex_buffer->Map(0, nullptr, &mapped), "axis vb map");
    std::memcpy(mapped, axis_vertices.data(), vb_size);
    d.axis_vertex_buffer->Unmap(0, nullptr);

    d.axis_vertex_buffer_view.BufferLocation = d.axis_vertex_buffer->GetGPUVirtualAddress();
    d.axis_vertex_buffer_view.StrideInBytes = sizeof(AxisVertex);
    d.axis_vertex_buffer_view.SizeInBytes = vb_size;
    return true;
}

bool HelloO3dRenderer::create_constant_buffer() {
    auto& d = *impl_;

    const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC cb_desc = CD3DX12_RESOURCE_DESC::Buffer(kSceneCbRingBytes);
    throw_hr(d.device->CreateCommittedResource(
                 &upload_heap, D3D12_HEAP_FLAG_NONE, &cb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(d.constant_buffer.GetAddressOf())),
             "constant buffer");
    d.scene_cb_gpu_base = d.constant_buffer->GetGPUVirtualAddress();
    d.scene_cb_slot = 0;

    // Slot 0 of the shader heap stays reserved (historical CBV layout); axis CBV still at index 1.
    const CD3DX12_RESOURCE_DESC axis_cb_desc = CD3DX12_RESOURCE_DESC::Buffer(kAxisConstantBufferSize);
    throw_hr(d.device->CreateCommittedResource(
                 &upload_heap, D3D12_HEAP_FLAG_NONE, &axis_cb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                 IID_PPV_ARGS(d.axis_constant_buffer.GetAddressOf())),
             "axis constant buffer");

    const D3D12_CPU_DESCRIPTOR_HANDLE axis_cbv_cpu =
        offset_cpu_handle(d.cbv_heap->GetCPUDescriptorHandleForHeapStart(), d.srv_stride, 1);
    D3D12_CONSTANT_BUFFER_VIEW_DESC axis_cbv_desc{};
    axis_cbv_desc.BufferLocation = d.axis_constant_buffer->GetGPUVirtualAddress();
    axis_cbv_desc.SizeInBytes = kAxisConstantBufferSize;
    d.device->CreateConstantBufferView(&axis_cbv_desc, axis_cbv_cpu);
    d.axis_cbv_gpu = offset_gpu_handle(d.cbv_heap->GetGPUDescriptorHandleForHeapStart(), d.srv_stride, 1);

    throw_hr(d.constant_buffer->Map(0, nullptr, reinterpret_cast<void**>(&d.cb_ring_cpu)), "cb map");
    std::memset(d.cb_ring_cpu, 0, kSceneCbRingBytes);
    throw_hr(d.axis_constant_buffer->Map(0, nullptr, reinterpret_cast<void**>(&d.axis_cb_cpu)), "axis cb map");
    std::memset(d.axis_cb_cpu, 0, kAxisConstantBufferSize);

    // P6: instance matrix ring (UPLOAD) — one region per in-flight frame.
    const UINT64 inst_bytes = static_cast<UINT64>(HelloO3dRenderer::kFrameCount) *
                              HelloO3dRenderer::Impl::kMaxInstancesPerDraw * HelloO3dRenderer::Impl::kInstanceStride;
    const CD3DX12_RESOURCE_DESC inst_desc = CD3DX12_RESOURCE_DESC::Buffer(inst_bytes);
    throw_hr(d.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &inst_desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(d.instance_buffer.GetAddressOf())),
             "instance buffer");
    throw_hr(d.instance_buffer->Map(0, nullptr, reinterpret_cast<void**>(&d.instance_buffer_cpu)), "instance map");
    std::memset(d.instance_buffer_cpu, 0, static_cast<size_t>(inst_bytes));
    d.instance_write_cursor = 0;
    return true;
}

} // namespace omsi::gfx::dx12
