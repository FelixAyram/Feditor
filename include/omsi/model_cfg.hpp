#pragma once

#include "omsi/model_cfg_types.hpp"

#include <filesystem>
#include <optional>

namespace omsi::model_cfg {

// Sequential parser matching OMSI TMaterialMan_ReadMaterialCommands + Blender-O3D-IO o3d_cfg_parser.
[[nodiscard]] std::optional<ModelCfgDocument> load_model_cfg(const std::filesystem::path& path);

[[nodiscard]] const ModelCfgLod* primary_lod(const ModelCfgDocument& doc);

// OMSI screensize (0..1+). visual_radius = bounding-sphere radius in world units.
[[nodiscard]] float compute_screensize(float visual_radius, float distance_m, float vertical_fov_rad);

// Highest LOD whose threshold <= screensize (OMSI: list high-detail first). Falls back to last.
[[nodiscard]] const ModelCfgLod* select_lod(const ModelCfgDocument& doc, float screensize);

// Index into doc.lods for select_lod; -1 if empty. hysteresis_eps keeps prior_index when near edge.
[[nodiscard]] int select_lod_index(const ModelCfgDocument& doc, float screensize, int prior_index = -1,
                                   float hysteresis_eps = 0.02f);

// Resolve cfg material for an O3D slot: match mat_id + embedded texture name, else first mat_id match.
[[nodiscard]] const ModelCfgMaterial* resolve_material_for_o3d_slot(const ModelCfgMeshEntry& mesh,
                                                                    int o3d_mat_id,
                                                                    std::string_view o3d_texture_name);

// Blender-O3D-IO generate_materials: key = matl[4].lower() must exist in cfg matls dict.
[[nodiscard]] const ModelCfgMaterial* resolve_cfg_material_by_o3d_texture(const ModelCfgMeshEntry& mesh,
                                                                            std::string_view o3d_texture_name);

// All [matl] entries for a given O3D material index (multi-stage: diffuse + detail).
[[nodiscard]] std::vector<const ModelCfgMaterial*> materials_for_o3d_slot(const ModelCfgMeshEntry& mesh,
                                                                            int o3d_mat_id);

// Apply TMaterialMan_LoadMaterial rules: matl_alpha=2 always disables Z-write.
void apply_omsi_material_state(MatlAlphaMode& alpha_mode, bool& no_zwrite, bool& no_zcheck);

} // namespace omsi::model_cfg
