#include "gfx/dx12/hello_o3d_renderer.hpp"

#include "gfx/dx12/common/dx12_common.hpp"
#include "gfx/dx12/o3d/internal.hpp"
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

void HelloO3dRenderer::apply_mesh_visibility(
    const std::function<bool(const omsi::model_cfg::ModelCfgMeshEntry&)>& is_visible) {
    if (impl_) scene_dirty(*impl_, "apply_mesh_visibility");
    if (!impl_ || !is_visible) return;
    for (auto& chunk : impl_->mesh_chunks) {
        omsi::model_cfg::ModelCfgMeshEntry entry{};
        entry.mesh_file = chunk.meta.mesh_file;
        entry.visible_var = chunk.meta.visible_var;
        entry.visible_default = chunk.meta.visible_required;
        entry.mouseevent_var = chunk.meta.mouseevent_var;
        chunk.meta.visible = is_visible(entry);
    }
}

void HelloO3dRenderer::apply_visible_runtime(const omsi::model_cfg::ModelCfgDocument& cfg,
                                               const omsi::visible::VisibleRuntime& rt) {
    if (impl_) scene_dirty(*impl_, "apply_visible_runtime");
    if (!impl_) return;
    const auto* lod = omsi::model_cfg::primary_lod(cfg);
    if (!lod) return;
    std::unordered_map<std::string, const omsi::model_cfg::ModelCfgMeshEntry*> by_file;
    for (const auto& mesh : lod->meshes) by_file[mesh.mesh_file] = &mesh;

    std::size_t shown = 0;
    for (auto& chunk : impl_->mesh_chunks) {
        const auto it = by_file.find(chunk.meta.mesh_file);
        const bool vis = (it != by_file.end()) ? rt.mesh_visible(*it->second) : true;
        chunk.meta.visible = vis;
        if (vis) ++shown;
    }
    omsi::log::info("visible runtime: showing " + std::to_string(shown) + "/" +
                    std::to_string(impl_->mesh_chunks.size()) + " mesh chunks");
}

std::vector<LoadedMeshChunkInfo> HelloO3dRenderer::chunk_info() const {
    std::vector<LoadedMeshChunkInfo> out;
    if (!impl_) return out;
    out.reserve(impl_->mesh_chunks.size());
    for (const auto& chunk : impl_->mesh_chunks) out.push_back(chunk.meta);
    return out;
}

} // namespace omsi::gfx::dx12
