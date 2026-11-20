#include "beamng/bng_roads.hpp"

#include <algorithm>
#include <cmath>

namespace bng {

std::vector<RoadNode> read_road_nodes(const JsonValue& nodes) {
    std::vector<RoadNode> out;
    for (const auto& n : nodes.items()) {
        if (!n.is_array() || n.size() < 3) continue;
        RoadNode r;
        r.x = n[0].as_float();
        r.y = n[1].as_float();
        r.z = n[2].as_float();
        r.w = n.size() > 3 ? n[3].as_float(1.f) : 1.f;
        out.push_back(r);
    }
    return out;
}

namespace {
struct Sample {
    float x, y, z, w;
    float tx, ty; // unit tangent (xy)
    float dist;
};

// Centripetal-ish uniform Catmull-Rom (BeamNG improvedSpline is also Catmull-Rom based).
std::vector<Sample> sample_spline(const std::vector<RoadNode>& in, bool looped, float step) {
    std::vector<Sample> out;
    const std::size_t n = in.size();
    if (n < 2) return out;
    auto P = [&](long i) -> const RoadNode& {
        if (looped) return in[static_cast<std::size_t>(((i % static_cast<long>(n)) + static_cast<long>(n)) % static_cast<long>(n))];
        return in[static_cast<std::size_t>((std::clamp)(i, 0L, static_cast<long>(n) - 1))];
    };
    const long segs = looped ? static_cast<long>(n) : static_cast<long>(n) - 1;
    float dist = 0.f;
    float px = 0, py = 0, pz = 0;
    bool first = true;
    for (long s = 0; s < segs; ++s) {
        const auto &p0 = P(s - 1), &p1 = P(s), &p2 = P(s + 1), &p3 = P(s + 2);
        const float len = std::hypot(p2.x - p1.x, p2.y - p1.y);
        const int steps = (std::max)(1, static_cast<int>(std::ceil(len / step)));
        for (int k = (s == 0 ? 0 : 1); k <= steps; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(steps), t2 = t * t, t3 = t2 * t;
            auto cr = [&](float a, float b, float c, float d) {
                return 0.5f * (2.f * b + (-a + c) * t + (2.f * a - 5.f * b + 4.f * c - d) * t2 + (-a + 3.f * b - 3.f * c + d) * t3);
            };
            auto dcr = [&](float a, float b, float c, float d) {
                return 0.5f * ((-a + c) + 2.f * (2.f * a - 5.f * b + 4.f * c - d) * t + 3.f * (-a + 3.f * b - 3.f * c + d) * t2);
            };
            Sample q;
            q.x = cr(p0.x, p1.x, p2.x, p3.x);
            q.y = cr(p0.y, p1.y, p2.y, p3.y);
            q.z = cr(p0.z, p1.z, p2.z, p3.z);
            q.w = p1.w + (p2.w - p1.w) * t;
            float tx = dcr(p0.x, p1.x, p2.x, p3.x), ty = dcr(p0.y, p1.y, p2.y, p3.y);
            const float tl = std::hypot(tx, ty);
            if (tl > 1e-6f) {
                tx /= tl;
                ty /= tl;
            } else {
                tx = p2.x - p1.x;
                ty = p2.y - p1.y;
                const float l2 = (std::max)(1e-6f, std::hypot(tx, ty));
                tx /= l2;
                ty /= l2;
            }
            q.tx = tx;
            q.ty = ty;
            if (!first) dist += std::sqrt((q.x - px) * (q.x - px) + (q.y - py) * (q.y - py) + (q.z - pz) * (q.z - pz));
            first = false;
            px = q.x;
            py = q.y;
            pz = q.z;
            q.dist = dist;
            out.push_back(q);
        }
    }
    return out;
}

RibbonMesh ribbon(const std::vector<Sample>& s, float tex_len, const TerrainData* terrain, float z_offset, bool drape) {
    RibbonMesh m;
    if (s.size() < 2) return m;
    tex_len = tex_len > 0.01f ? tex_len : 5.f;
    float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
    m.zmin = 1e30f;
    m.zmax = -1e30f;
    for (const auto& q : s) {
        const float rx = q.ty, ry = -q.tx; // right of the travel direction
        const float hw = 0.5f * q.w;
        for (int side = 0; side < 2; ++side) {
            const float sgn = side == 0 ? -1.f : 1.f;
            omsi::o3d::Vertex v{};
            v.position[0] = q.x + rx * hw * sgn;
            v.position[1] = q.y + ry * hw * sgn;
            float z = q.z;
            if (drape && terrain && terrain->contains(v.position[0], v.position[1])) {
                const float tz = terrain->sample_world(v.position[0], v.position[1]);
                // decal roads hug the ground; well above it (bridges, overObjects) they keep the node height
                if (q.z - tz < 1.0f) z = tz;
            }
            v.position[2] = z + z_offset;
            v.normal[2] = 1.f;
            v.uv[0] = side == 0 ? 0.f : 1.f;
            v.uv[1] = q.dist / tex_len;
            mnx = (std::min)(mnx, v.position[0]);
            mxx = (std::max)(mxx, v.position[0]);
            mny = (std::min)(mny, v.position[1]);
            mxy = (std::max)(mxy, v.position[1]);
            m.zmin = (std::min)(m.zmin, v.position[2]);
            m.zmax = (std::max)(m.zmax, v.position[2]);
            m.vertices.push_back(v);
        }
    }
    for (std::uint32_t i = 0; i + 1 < s.size(); ++i) {
        const std::uint32_t a = i * 2, b = a + 1, c = a + 2, d = a + 3;
        // counter-clockwise seen from above (left, right, next left / right, next right, next left)
        m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
    }
    // fix winding if the ribbon was built clockwise (depends on the right vector convention)
    if (m.vertices.size() >= 3) {
        const auto& v0 = m.vertices[m.indices[0]].position;
        const auto& v1 = m.vertices[m.indices[1]].position;
        const auto& v2 = m.vertices[m.indices[2]].position;
        const float cz = (v1[0] - v0[0]) * (v2[1] - v0[1]) - (v1[1] - v0[1]) * (v2[0] - v0[0]);
        if (cz < 0.f)
            for (std::size_t k = 0; k + 2 < m.indices.size(); k += 3) std::swap(m.indices[k + 1], m.indices[k + 2]);
    }
    // per-vertex normals from the draped surface
    std::vector<float> acc(m.vertices.size() * 3, 0.f);
    for (std::size_t k = 0; k + 2 < m.indices.size(); k += 3) {
        const auto& p0 = m.vertices[m.indices[k]].position;
        const auto& p1 = m.vertices[m.indices[k + 1]].position;
        const auto& p2 = m.vertices[m.indices[k + 2]].position;
        const float ux = p1[0] - p0[0], uy = p1[1] - p0[1], uz = p1[2] - p0[2];
        const float vx = p2[0] - p0[0], vy = p2[1] - p0[1], vz = p2[2] - p0[2];
        const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        for (int j = 0; j < 3; ++j) {
            acc[m.indices[k + j] * 3 + 0] += nx;
            acc[m.indices[k + j] * 3 + 1] += ny;
            acc[m.indices[k + j] * 3 + 2] += nz;
        }
    }
    for (std::size_t i = 0; i < m.vertices.size(); ++i) {
        float nx = acc[i * 3], ny = acc[i * 3 + 1], nz = acc[i * 3 + 2];
        const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (l > 1e-8f) {
            m.vertices[i].normal[0] = nx / l;
            m.vertices[i].normal[1] = ny / l;
            m.vertices[i].normal[2] = nz / l;
        }
    }
    m.center[0] = 0.5f * (mnx + mxx);
    m.center[1] = 0.5f * (mny + mxy);
    m.radius = 0.5f * std::hypot(mxx - mnx, mxy - mny);
    return m;
}
} // namespace

RibbonMesh build_decal_road(const std::vector<RoadNode>& nodes, float texture_length, bool looped, const TerrainData* terrain,
                            float z_offset) {
    // Denser sampling where the ribbon follows the terrain so it does not cut through bumps.
    return ribbon(sample_spline(nodes, looped, 1.5f), texture_length, terrain, z_offset, true);
}

RibbonMesh build_flat_ribbon(const std::vector<RoadNode>& nodes, float texture_length, bool looped) {
    return ribbon(sample_spline(nodes, looped, 3.f), texture_length, nullptr, 0.f, false);
}

RibbonMesh build_water_quad(const float c[3], const Mat3& r, float hx, float hy, float uv_scale) {
    RibbonMesh m;
    const float corners[4][2] = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
    for (const auto& k : corners) {
        omsi::o3d::Vertex v{};
        v.position[0] = c[0] + r[0] * k[0] + r[1] * k[1];
        v.position[1] = c[1] + r[3] * k[0] + r[4] * k[1];
        v.position[2] = c[2] + r[6] * k[0] + r[7] * k[1];
        v.normal[2] = 1.f;
        v.uv[0] = (k[0] + hx) * uv_scale;
        v.uv[1] = (k[1] + hy) * uv_scale;
        m.vertices.push_back(v);
    }
    m.indices = {0, 1, 2, 0, 2, 3};
    m.center[0] = c[0];
    m.center[1] = c[1];
    m.radius = std::hypot(hx, hy);
    m.zmin = m.zmax = c[2];
    return m;
}

RibbonMesh build_decal_quads(const std::vector<DecalInstance>& instances, const std::vector<std::array<float, 4>>& rects,
                             float rise) {
    RibbonMesh m;
    m.vertices.reserve(instances.size() * 4);
    m.indices.reserve(instances.size() * 6);
    float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
    float zmin = 1e30f, zmax = -1e30f;
    for (const auto& d : instances) {
        float n[3]{d.normal[0], d.normal[1], d.normal[2]};
        float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!(nl > 1e-6f)) { // missing / junk normal: face up
            n[0] = 0.f;
            n[1] = 0.f;
            n[2] = 1.f;
            nl = 1.f;
        }
        for (float& c : n) c /= nl;
        // X axis = tangent projected into the decal plane (vertex normals would tilt the quad)
        float t[3]{d.tangent[0], d.tangent[1], d.tangent[2]};
        const float tn = t[0] * n[0] + t[1] * n[1] + t[2] * n[2];
        for (int k = 0; k < 3; ++k) t[k] -= tn * n[k];
        float tl = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        if (!(tl > 1e-5f)) { // degenerate tangent: any axis in the plane
            const float ax[3] = {std::fabs(n[2]) < 0.9f ? 0.f : 1.f, 0.f, std::fabs(n[2]) < 0.9f ? 1.f : 0.f};
            const float an = ax[0] * n[0] + ax[1] * n[1] + ax[2] * n[2];
            for (int k = 0; k < 3; ++k) t[k] = ax[k] - an * n[k];
            tl = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            if (!(tl > 1e-5f)) continue;
        }
        for (float& c : t) c /= tl;
        // Y axis = normal x tangent (right-handed with X = t, Z = n)
        const float y[3]{n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]};
        const float h = 0.5f * (d.size > 0.f ? d.size : 1.f);
        std::array<float, 4> r{0.f, 0.f, 1.f, 1.f};
        if (d.rect >= 0 && static_cast<std::size_t>(d.rect) < rects.size()) r = rects[static_cast<std::size_t>(d.rect)];
        const float u0 = r[0], v0 = r[1], u1 = r[0] + r[2], v1 = r[1] + r[3];
        const float c[3]{d.pos[0] + n[0] * rise, d.pos[1] + n[1] * rise, d.pos[2] + n[2] * rise};
        const float sx[4]{-1.f, 1.f, 1.f, -1.f}, sy[4]{-1.f, -1.f, 1.f, 1.f};
        const float uu[4]{u0, u1, u1, u0}, vv[4]{v1, v1, v0, v0}; // v0 = top of the atlas rect
        const auto base = static_cast<std::uint32_t>(m.vertices.size());
        for (int k = 0; k < 4; ++k) {
            omsi::o3d::Vertex v{};
            for (int a = 0; a < 3; ++a) {
                v.position[a] = c[a] + t[a] * h * sx[k] + y[a] * h * sy[k];
                v.normal[a] = n[a];
            }
            v.uv[0] = uu[k];
            v.uv[1] = vv[k];
            m.vertices.push_back(v);
            mnx = (std::min)(mnx, v.position[0]);
            mxx = (std::max)(mxx, v.position[0]);
            mny = (std::min)(mny, v.position[1]);
            mxy = (std::max)(mxy, v.position[1]);
            zmin = (std::min)(zmin, static_cast<float>(v.position[2]));
            zmax = (std::max)(zmax, static_cast<float>(v.position[2]));
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    if (!m.vertices.empty()) {
        m.center[0] = 0.5f * (mnx + mxx);
        m.center[1] = 0.5f * (mny + mxy);
        m.radius = 0.5f * std::hypot(mxx - mnx, mxy - mny);
        m.zmin = zmin;
        m.zmax = zmax;
    }
    return m;
}

} // namespace bng
