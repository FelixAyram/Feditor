#pragma once
// COLLADA (.dae) reader for BeamNG shapes: triangles/polylist geometry, node transforms, bound materials,
// Torque detail levels (trailing number in node names: "mesh500" = LOD for 500px, negative = collision).
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bng {

struct DaeVertex {
    float pos[3];
    float normal[3];
    float uv0[2];
    float uv1[2];
    std::uint32_t color; // RGBA8, 0xFFFFFFFF when absent
};

struct DaeSubmesh {
    std::string material;            // material symbol name as BeamNG maps it (material "name" or id)
    // Diffuse texture declared inside the .dae (library_images + effect sampler), relative to the shape.
    // Fallback only: the BeamNG material file wins when the level resolves it.
    std::string texture;
    std::vector<DaeVertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct DaeLod {
    int detail_size = 0;             // pixel size this LOD is used from (Torque detail size); <0 = collision
    std::vector<DaeSubmesh> submeshes;
    std::size_t triangle_count() const;
};

struct DaeShape {
    std::vector<DaeLod> lods;        // visible LODs, sorted by detail_size descending (lods[0] = most detailed)
    std::vector<DaeLod> collision;   // collision meshes (negative detail)
    // "bb__autobillboard<N>" impostor nodes. They are NOT a detail LOD (Torque draws a camera-facing quad from
    // the imposter texture at N px), so they never compete with real geometry in the LOD pick. When the shape
    // has no real visual geometry at all they are promoted to lods so the object is still drawable.
    std::vector<DaeLod> billboards;
    bool has_billboard = false;
    int billboard_px = 25;           // N of the first bb__autobillboard<N> node (BeamNG's usual impostor size)
    float bounds_min[3]{0, 0, 0}, bounds_max[3]{0, 0, 0};
    float radius = 0.f;
    float unit_scale = 1.f;          // COLLADA <unit meter>; 0.01 for the centimetre assets (gridmap cubes…)
    std::string error;
};

// Parses a .dae buffer. Geometry is converted to Z-up metres in shape space: the asset unit
// (<unit meter="0.01">) is folded into the root transform, so node translations are metres too.
std::optional<DaeShape> parse_dae(std::string_view xml);

// Minimal XML DOM (also usable for other small XML files).
struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> children;
    std::string text;
    const std::string* attr(std::string_view k) const;
    const XmlNode* child(std::string_view n) const;
    std::vector<const XmlNode*> children_named(std::string_view n) const;
};
bool xml_parse(std::string_view s, XmlNode& root, std::string* err = nullptr);

} // namespace bng
