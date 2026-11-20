// Included from spline_creator_dialog.cpp — DX12 spline preview (same path as map editor).

#include "gfx/dx12/hello_o3d_renderer.hpp"
#include "input/luna_camera.hpp"

#include <DirectXMath.h>
#include <memory>

std::filesystem::path preview_sli_base(const CreatorState* st) {
    if (!st->def.sli_path.empty()) return st->def.sli_path;
    if (!st->save_path.empty()) return st->save_path;
    if (!st->omsi_root.empty()) return st->omsi_root / L"Splines" / L"preview.sli";
    return L"preview.sli";
}

void expand_bounds(float& minx, float& miny, float& minz, float& maxx, float& maxy, float& maxz,
                   const omsi::o3d::Vertex& v) {
    minx = (std::min)(minx, v.position[0]);
    miny = (std::min)(miny, v.position[1]);
    minz = (std::min)(minz, v.position[2]);
    maxx = (std::max)(maxx, v.position[0]);
    maxy = (std::max)(maxy, v.position[1]);
    maxz = (std::max)(maxz, v.position[2]);
}

void frame_preview_camera(CreatorState* st, float minx, float miny, float minz, float maxx, float maxy,
                          float maxz) {
    st->preview_cam.z_up = true;
    st->preview_cam.mirror_x = true;
    st->preview_cam.frame_omsi_bounds(minx, miny, minz, maxx, maxy, maxz);
}

bool init_preview_rhi(CreatorState* st) {
    if (!st || !st->preview3d) return false;
    st->preview_rhi = std::make_unique<gfx::dx12::HelloO3dRenderer>();
    RECT rc{};
    GetClientRect(st->preview3d, &rc);
    const auto w = static_cast<std::uint32_t>((std::max)(rc.right - rc.left, 1L));
    const auto h = static_cast<std::uint32_t>((std::max)(rc.bottom - rc.top, 1L));
    if (!st->preview_rhi->initialize(st->preview3d, w, h, true)) {
        st->preview_rhi.reset();
        st->preview_rhi_ok = false;
        log::warn("spline creator: preview DX12 init failed — GDI fallback");
        return false;
    }
    st->preview_rhi->set_draw_shadow_decals(false);
    st->preview_rhi_ok = true;
    st->preview_mesh_dirty = true;
    st->preview_cam.theta = 1.5f * DirectX::XM_PI + 0.45f;
    st->preview_cam.phi = 0.55f;
    st->preview_cam.radius = 22.f;
    st->preview_cam.target_x = 3.f;
    st->preview_cam.target_y = kPreviewLenM * 0.5f;
    st->preview_cam.target_z = 0.15f;
    return true;
}

void shutdown_preview_rhi(CreatorState* st) {
    if (!st) return;
    if (st->preview_rhi) {
        st->preview_rhi->shutdown();
        st->preview_rhi.reset();
    }
    st->preview_chunks.clear();
    st->preview_rhi_ok = false;
}

void sync_preview_meshes(CreatorState* st, bool reframe_camera) {
    if (!st || !st->preview_rhi_ok || !st->preview_rhi) return;

    st->preview_rhi->clear_meshes();
    st->preview_chunks.clear();

    map::SplineInst sp{};
    sp.length = kPreviewLenM;
    sp.heading_deg = 0.f;
    const auto meshes = spline::build_spline_meshes(sp, st->def, st->omsi_root);

    const auto sli_base = preview_sli_base(st);
    float minx = 1.e30f, miny = 1.e30f, minz = 1.e30f;
    float maxx = -1.e30f, maxy = -1.e30f, maxz = -1.e30f;
    bool any = false;

    for (const auto& mesh : meshes) {
        std::filesystem::path tex_path;
        if (!mesh.texture_path.empty()) {
            const std::string token = mesh.texture_path.string();
            if (const auto from_src = resolve_tex_file(st, token))
                tex_path = *from_src;
            else if (const auto from_name = resolve_tex_file(st, mesh.texture_path.filename().string()))
                tex_path = *from_name;
            else if (const auto resolved =
                         gfx::dx12::resolve_texture_path(sli_base, token, st->omsi_root))
                tex_path = *resolved;
        }
        if (!st->preview_rhi->append_textured_mesh(mesh.vertices.data(),
                                                   static_cast<std::uint32_t>(mesh.vertices.size()),
                                                   mesh.indices.data(),
                                                   static_cast<std::uint32_t>(mesh.indices.size()), tex_path,
                                                   mesh.alpha_mode)) {
            log::warn("spline creator preview: mesh upload failed tex=" + mesh.texture_path.string());
            continue;
        }
        st->preview_chunks.push_back(st->preview_rhi->chunk_count() - 1);
        for (const auto& v : mesh.vertices) {
            expand_bounds(minx, miny, minz, maxx, maxy, maxz, v);
            any = true;
        }
    }

    if (reframe_camera) {
        if (any)
            frame_preview_camera(st, minx, miny, minz, maxx, maxy, maxz);
        else
            frame_preview_camera(st, -1.f, 0.f, -0.5f, 7.f, kPreviewLenM, 1.f);
    }

    st->preview_mesh_dirty = false;
}

void render_preview_rhi(CreatorState* st) {
    if (!st || !st->preview_rhi_ok || !st->preview_rhi || !st->preview3d) return;
    if (st->preview_mesh_dirty) sync_preview_meshes(st, st->preview_first_frame);
    if (st->preview_first_frame) st->preview_first_frame = false;

    RECT rc{};
    GetClientRect(st->preview3d, &rc);
    const int pw = (std::max)(static_cast<int>(rc.right - rc.left), 1);
    const int ph = (std::max)(static_cast<int>(rc.bottom - rc.top), 1);
    const float aspect = static_cast<float>(pw) / static_cast<float>(ph);

    float wvp_t[16]{};
    st->preview_cam.build_world_view_proj(aspect, wvp_t);
    st->preview_rhi->set_world_view_proj(wvp_t);
    st->preview_rhi->render();
}

void center_preview_view(CreatorState* st) {
    if (!st) return;
    if (st->preview_rhi_ok) {
        st->preview_mesh_dirty = true;
        sync_preview_meshes(st, true);
        render_preview_rhi(st);
        InvalidateRect(st->preview3d, nullptr, FALSE);
    }
}
