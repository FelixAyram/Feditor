#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "f4f23ma/math.hpp"
#include "gfx/debug_mesh.hpp"
#include "omsi/map_types.hpp"
#include "omsi/spline_types.hpp"

namespace omsi::editor {

// Closed hole ring in tile-local XY (mesh space 0..size). Used by in-RAM polycut
// (OMSI TD3DTerrain_RefreshTerrain / IdCharsets polycut) — never writes .terrain.
struct TerrainHoleRing {
    std::vector<f4f23ma::f32> x;
    std::vector<f4f23ma::f32> y;
    // Optional height for each contour vertex. AlignTerrain rings carry exact spline
    // profile heights so newly-created cut-edge vertices meet the spline surface;
    // object holes and legacy callers can leave this empty to sample the heightfield.
    std::vector<f4f23ma::f32> z;
};

// Build hole outlines for AlignTerrain splines (modes 1–4 pad start/end like mask).
void collect_spline_align_hole_rings(std::vector<TerrainHoleRing>& out, f4f23ma::f32 tile_rel_x,
                                    f4f23ma::f32 tile_rel_y, f4f23ma::f32 size_m,
                                    const std::vector<map::SplineInst>& splines);
// Same output as above, reusing a prebuilt plan (see spline::build_terrain_align_plan).
void collect_spline_align_hole_rings(std::vector<TerrainHoleRing>& out, f4f23ma::f32 tile_rel_x,
                                    f4f23ma::f32 tile_rel_y, f4f23ma::f32 size_m,
                                    const spline::TerrainAlignPlan& plan);

// Axis-aligned (in local heading) object hole as a 4-vert ring in tile-local XY.
void append_object_align_hole_ring(std::vector<TerrainHoleRing>& out, f4f23ma::f32 tile_rel_x,
                                  f4f23ma::f32 tile_rel_y, f4f23ma::f32 wx, f4f23ma::f32 wy,
                                  f4f23ma::f32 heading_deg, f4f23ma::f32 half_x, f4f23ma::f32 half_y);

// Rebuild mesh verts/indices: cells cut by rings get seam verts and triangulation.
// Does not write .terrain; AlignTerrain contour vertices may use exact profile Z at the seam.
void apply_terrain_polycut_to_mesh(gfx::BufferMesh& mesh, const std::vector<float>& heights,
                                   float size_meters, const std::vector<TerrainHoleRing>& rings,
                                   int dim = 61);

} // namespace omsi::editor
