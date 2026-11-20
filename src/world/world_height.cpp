#include "omsi/world_height.hpp"

#include "omsi/sco_types.hpp"
#include "omsi/spline_types.hpp"

#include <algorithm>
#include <cmath>

namespace omsi::world_height {
namespace {

float lerp(float a, float b, float t) { return a + (b - a) * t; }

bool sco_skip_collision(const sco::SceneryObject& sco) {
    if (sco.tree.valid) return true;
    if (sco.special == sco::ScoSpecial::Tree) return true;
    if (sco.only_editor_mesh) return true;
    if (sco.no_collision) return true;
    return false;
}

float sco_surface_z(const map::MapObjectInst& o, const sco::SceneryObject& sco, float ex, float ey) {
    if (!sco.surface && sco.collision_mesh.empty() && !sco.has_boundingbox) return -1.e9f;
    const float yaw = o.rotation_deg.x * 3.14159265f / 180.f;
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    const float dx = ex - o.position.x;
    const float dy = ey - o.position.y;
    const float lx = dx * c + dy * s;
    const float ly = -dx * s + dy * c;
    float hx = 2.f, hy = 4.f, hz = 0.2f, ox = 0.f, oy = 0.f, oz = 0.f;
    if (sco.has_boundingbox) {
        hx = sco.bbox_w * 0.5f;
        hy = sco.bbox_l * 0.5f;
        hz = sco.bbox_h * 0.5f;
        ox = sco.bbox_off_x;
        oy = sco.bbox_off_y;
        oz = sco.bbox_off_z;
    }
    if (std::abs(lx - ox) > hx + 0.05f || std::abs(ly - oy) > hy + 0.05f) return -1.e9f;
    return o.position.z + oz + hz;
}

} // namespace

float get_height_above_all(float ex, float ey, float z_hint, const TerrainSample& terrain,
                           const std::vector<map::SplineInst>& splines,
                           const std::vector<map::MapObjectInst>& objects,
                           const std::filesystem::path& omsi_root) {
    (void)omsi_root;
    float best = terrain ? terrain(ex, ey) : 0.f;
    if (z_hint < best - 50.f) {
        // keep
    }

    for (const auto& sp : splines) {
        if (sp.sli_path.empty()) continue;
        if (std::abs(sp.position.x - ex) > 80.f || std::abs(sp.position.y - ey) > 80.f) continue;
        const auto def = spline::load_sli_shared(sp.sli_path);
        if (!def || def->only_editor || def->height_profiles.empty()) continue;
        float along = 0.f, lat = 0.f;
        if (!spline::world_xy_to_spline_local(sp, ex, ey, along, lat)) continue;
        float z_off = -1.e9f;
        for (const auto& hp : def->height_profiles) {
            const float lo = (std::min)(hp.left, hp.right);
            const float hi = (std::max)(hp.left, hp.right);
            if (lat < lo - 0.05f || lat > hi + 0.05f) continue;
            const float span = hi - lo;
            const float t = span > 1.e-5f ? (lat - hp.left) / (hp.right - hp.left + 1.e-8f) : 0.f;
            const float h = lerp(hp.height, hp.skew, (std::clamp)(t, 0.f, 1.f));
            z_off = (std::max)(z_off, h);
        }
        if (z_off < -1.e8f) continue;
        const auto samp = spline::evaluate_spline(sp, (std::clamp)(along, 0.f, (std::max)(sp.length, 0.01f)), 0.f, z_off);
        // TMapKachel_GetHeightAboveAll / GetHeightAbove: downward ray from the query, keep the
        // highest hit with z <= z_hint. An +8 m slop let overpasses and roofs steal the car.
        if (samp.z > best && samp.z <= z_hint) best = samp.z;
    }

    for (const auto& o : objects) {
        if (std::abs(o.position.x - ex) > 40.f || std::abs(o.position.y - ey) > 40.f) continue;
        const auto* sco = sco::load_sco(o.sco_path);
        if (!sco || sco_skip_collision(*sco)) continue;
        if (!sco->surface && sco->collision_mesh.empty()) continue;
        const float z = sco_surface_z(o, *sco, ex, ey);
        if (z > best && z <= z_hint) best = z;
    }
    return best;
}

} // namespace omsi::world_height
