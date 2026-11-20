#pragma once
// BeamNG ribbons: DecalRoad (projected on the terrain), River (water ribbon at node height) and water planes.
// Pure CPU geometry (safe on worker threads); the editor uploads the result.
#include "beamng/bng_level.hpp"
#include "omsi/o3d_format.hpp"

#include <array>
#include <string>
#include <vector>

namespace bng {

struct RoadNode {
    float x = 0.f, y = 0.f, z = 0.f, w = 1.f;
};

struct RibbonMesh {
    std::vector<omsi::o3d::Vertex> vertices;
    std::vector<std::uint32_t> indices;
    float center[2]{0.f, 0.f};
    float radius = 0.f;
    float zmin = 0.f, zmax = 0.f;
};

std::vector<RoadNode> read_road_nodes(const JsonValue& nodes);

// DecalRoad: Catmull-Rom through the nodes, edges dropped onto the terrain unless the road runs well above it
// (bridges / overObjects), v = distance / textureLength.
RibbonMesh build_decal_road(const std::vector<RoadNode>& nodes, float texture_length, bool looped, const TerrainData* terrain,
                            float z_offset = 0.05f);
// River / flat ribbon at node height (water surface).
RibbonMesh build_flat_ribbon(const std::vector<RoadNode>& nodes, float texture_length, bool looped);
// Axis-aligned (rotated) water rectangle (WaterBlock top / WaterPlane patch).
RibbonMesh build_water_quad(const float center[3], const Mat3& rot, float half_x, float half_y, float uv_scale);

// Decals (main.decals.json): one quad per instance, side = instance size, in the plane of its normal
// (X = tangent, Y = normal x tangent), lifted `rise` metres along the normal; UVs from textureCoords[rect].
RibbonMesh build_decal_quads(const std::vector<DecalInstance>& instances,
                             const std::vector<std::array<float, 4>>& rects, float rise = 0.03f);

} // namespace bng
