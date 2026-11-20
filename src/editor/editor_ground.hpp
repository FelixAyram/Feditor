#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "f4f23ma/math.hpp"
#include "gfx/debug_mesh.hpp"
#include "omsi/map_tile_layout.hpp"
#include "omsi/map_types.hpp"

namespace omsi::editor {

// Mirrors Blender-O3D-IO io_omsi_tile.import_terrain_mesh: 61×61 verts.
// Spacing is size_meters / 60 (classic 300 → 5 m; world/lat tiles use 611.5·cos(lat)).
constexpr int kTerrainDim = 61;
// OMSI editor default paint-mask resolution (Berlin Spandau masks are 256×256 A8 DDS).
constexpr int kGroundMaskDefaultPx = 256;

// Local OMSI Z-up tile with origin at (0,0,0) — same space as HelloO3d / LunaCamera.
struct LocalTerrainTile {
    gfx::BufferMesh mesh;
    std::filesystem::path texture_path;
    f4f23ma::f32 max_height = 0.f;
    f4f23ma::f32 size_meters = map::kStandardTileMeters;
    f4f23ma::f32 step_meters = 5.f;
    // File is [y][x] fill; sample with flat index x*dim+y like Blender-O3D-IO.
    std::vector<float> heights;
};

[[nodiscard]] std::optional<std::vector<float>> load_terrain_heights(const std::filesystem::path& map_path);

// Bilinear sample matching Blender-O3D-IO get_interpolated_height(x,y) on tile-local XY.
[[nodiscard]] float sample_terrain_height(const LocalTerrainTile& tile, float x, float y);
// Same sample without copying the heightfield (hot path for editor placement).
[[nodiscard]] float sample_terrain_height(const std::vector<float>& heights, float size_meters, float x,
                                          float y);

// Overwrite vertex Z/normals of a 61×61 terrain mesh from a heightfield (UVs kept).
void apply_heightfield_to_terrain_mesh(gfx::BufferMesh& mesh, const std::vector<float>& heights, float size_meters);

// Drop quads whose 4 corners are hole-masked (openOMSI-style terrain hole under SCO/SLI).
void apply_terrain_hole_mask_to_mesh(gfx::BufferMesh& mesh, const std::vector<std::uint8_t>& hole_mask,
                                     int dim = kTerrainDim);

// Build one tile mesh at local (0..size, 0..size, height). size_meters from map_tile_layout
// (tt-viewer: WORLD_EQUATOR_TILE_M=611.5 × cos(lat)).
// If ground_textures has 2+ entries (or OMSI masks exist), bakes a composite albedo for the mesh.
[[nodiscard]] std::optional<LocalTerrainTile> build_local_terrain_tile(
    const std::filesystem::path& map_path, const map::GroundTex& groundtex,
    const std::filesystem::path& omsi_root, f4f23ma::f32 size_meters = map::kStandardTileMeters,
    std::span<const map::GroundTex> ground_textures = {});

// OMSI Terrain tab: Higher / Lower / Set Height brush (soft falloff).
enum class TerrainBrushMode { Higher = 0, Lower = 1, SetHeight = 2 };

// Paint into a 61×61 heightfield. local_x/y in tile meters. Returns true if any sample changed.
bool paint_terrain_brush(std::vector<float>& heights, float size_meters, float local_x, float local_y,
                         float radius_m, TerrainBrushMode mode, float amount, float set_height);

// OMSI 2 parity: maps/<map>/texture/map/<tile.map>.<N>.dds — 8-bit alpha mask (layer N≥1).
// TMapKachel.GetGroundTexPath: ExtractFilePath + "texture\\map\\" + FileName + "." + N + ".dds".
[[nodiscard]] std::filesystem::path ground_layer_mask_path(const std::filesystem::path& map_path, int layer_index);
[[nodiscard]] bool tile_has_omsi_ground_masks(const std::filesystem::path& map_path, int max_layer_exclusive);

// Paint / rubber into OMSI mask DDS for layer_index (≥1). Creates A8 DDS if missing.
// Returns true if any texel changed.
bool paint_omsi_ground_layer_mask(const std::filesystem::path& map_path, int layer_index, float size_meters,
                                  float local_x, float local_y, float radius_m, bool rubber,
                                  int mask_px = kGroundMaskDefaultPx, float hardness = 0.5f,
                                  float strength = 1.f);

// Paint / rubber an arbitrary polygon (tile-local XY meters) into OMSI mask DDS for layer_index.
// Loads the mask once, rasterizes the polygon, writes once (fast lasso fill).
bool paint_omsi_ground_layer_mask_polygon(const std::filesystem::path& map_path, int layer_index,
                                          float size_meters,
                                          const std::vector<std::pair<float, float>>& pts_local, bool rubber,
                                          int mask_px = kGroundMaskDefaultPx);

// Bake "<tile.map>.ground_paint.bmp" from OMSI masks + [groundtex] list (editor preview albedo).
[[nodiscard]] std::filesystem::path bake_ground_layer_paint_bmp(const std::filesystem::path& map_path,
                                                                const std::vector<map::GroundTex>& ground_textures,
                                                                const std::filesystem::path& omsi_root,
                                                                float size_meters, int out_px = 512);
[[nodiscard]] std::filesystem::path ground_paint_bmp_path(const std::filesystem::path& map_path);

[[nodiscard]] std::vector<float> make_flat_terrain_heights(float height);

} // namespace omsi::editor
