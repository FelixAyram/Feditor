#include "terrain_polycut.hpp"

#include "editor_ground.hpp"
#include "omsi/spline_types.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace omsi::editor {
namespace {

struct P2 {
    float x = 0.f;
    float y = 0.f;
    float z = std::numeric_limits<float>::quiet_NaN();
};

bool point_in_ring(float x, float y, const TerrainHoleRing& r) {
    const std::size_t n = (std::min)(r.x.size(), r.y.size());
    if (n < 3) return false;
    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = r.x[i], yi = r.y[i];
        const float xj = r.x[j], yj = r.y[j];
        const bool hit = ((yi > y) != (yj > y)) &&
                         (x < (xj - xi) * (y - yi) / ((yj - yi) + (yj == yi ? 1.e-12f : 0.f)) + xi);
        if (hit) inside = !inside;
    }
    return inside;
}

bool point_in_any(float x, float y, const std::vector<TerrainHoleRing>& rings) {
    for (const auto& r : rings)
        if (point_in_ring(x, y, r)) return true;
    return false;
}

bool seg_intersect(P2 a, P2 b, P2 c, P2 d, float& t, float& u, P2& out) {
    const float rx = b.x - a.x, ry = b.y - a.y;
    const float sx = d.x - c.x, sy = d.y - c.y;
    const float den = rx * sy - ry * sx;
    if (std::fabs(den) < 1.e-12f) return false;
    const float qpx = c.x - a.x, qpy = c.y - a.y;
    t = (qpx * sy - qpy * sx) / den;
    u = (qpx * ry - qpy * rx) / den;
    if (t < -1.e-5f || t > 1.f + 1.e-5f || u < -1.e-5f || u > 1.f + 1.e-5f) return false;
    t = std::clamp(t, 0.f, 1.f);
    u = std::clamp(u, 0.f, 1.f);
    out.x = a.x + t * rx;
    out.y = a.y + t * ry;
    // During terrain cutting, c-d is the AlignTerrain contour edge. Preserve its
    // interpolated target height at the newly-created intersection with a tile edge.
    if (std::isfinite(c.z) && std::isfinite(d.z)) {
        out.z = c.z + (d.z - c.z) * u;
    } else if (std::isfinite(a.z) && std::isfinite(b.z)) {
        out.z = a.z + (b.z - a.z) * t;
    }
    return true;
}

bool ring_is_simple(const TerrainHoleRing& r) {
    const std::size_t n = (std::min)(r.x.size(), r.y.size());
    if (n < 3) return false;
    double area2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = (i + 1) % n;
        const float ax = r.x[i], ay = r.y[i], bx = r.x[j], by = r.y[j];
        if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(bx) || !std::isfinite(by))
            return false;
        area2 += static_cast<double>(ax) * by - static_cast<double>(bx) * ay;
    }
    if (std::abs(area2) < 1.e-5) return false;

    // OMSI treats self-crossing hole outlines as invalid and cuts nothing.
    // Check non-adjacent edges; the shared endpoints of adjacent edges are legal.
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t in = (i + 1) % n;
        const P2 a{r.x[i], r.y[i]}, b{r.x[in], r.y[in]};
        for (std::size_t j = i + 1; j < n; ++j) {
            const std::size_t jn = (j + 1) % n;
            if (j == i || j == in || jn == i) continue;
            const P2 c{r.x[j], r.y[j]}, d{r.x[jn], r.y[jn]};
            float t = 0.f, u = 0.f;
            P2 p{};
            if (seg_intersect(a, b, c, d, t, u, p)) return false;
        }
    }
    return true;
}

float sample_h(const std::vector<float>& heights, float size, float lx, float ly) {
    return sample_terrain_height(heights, size, lx, ly);
}

// The stock terrain is rendered as two triangles per 5 m quad (diagonal c00-c11),
// not as a bilinear patch. Newly-created contour vertices must lie on that same
// piecewise-planar surface or they can lift/sink along the cut edge.
float sample_mesh_h(const std::vector<float>& heights, int dim, float size, float lx, float ly) {
    if (dim < 2 || heights.size() < static_cast<std::size_t>(dim * dim)) return 0.f;
    const float step = size / static_cast<float>(dim - 1);
    const float gx = std::clamp(lx / step, 0.f, static_cast<float>(dim - 1));
    const float gy = std::clamp(ly / step, 0.f, static_cast<float>(dim - 1));
    const int x0 = std::clamp(static_cast<int>(std::floor(gx)), 0, dim - 2);
    const int y0 = std::clamp(static_cast<int>(std::floor(gy)), 0, dim - 2);
    const float tx = std::clamp(gx - static_cast<float>(x0), 0.f, 1.f);
    const float ty = std::clamp(gy - static_cast<float>(y0), 0.f, 1.f);
    const auto at = [&](int x, int y) {
        return heights[static_cast<std::size_t>(y) * static_cast<std::size_t>(dim) +
                       static_cast<std::size_t>(x)];
    };
    const float h00 = at(x0, y0);
    const float h10 = at(x0 + 1, y0);
    const float h11 = at(x0 + 1, y0 + 1);
    const float h01 = at(x0, y0 + 1);
    if (ty <= tx) {
        // Triangle (c00,c10,c11).
        return h00 + tx * (h10 - h00) + ty * (h11 - h10);
    }
    // Triangle (c00,c11,c01).
    return h00 + tx * (h11 - h01) + ty * (h01 - h00);
}

void sample_normal(const std::vector<float>& heights, int dim, float size, float lx, float ly, float& nx,
                   float& ny, float& nz) {
    const float step = size / static_cast<float>((std::max)(2, dim) - 1);
    const float hL = sample_h(heights, size, lx - step, ly);
    const float hR = sample_h(heights, size, lx + step, ly);
    const float hD = sample_h(heights, size, lx, ly - step);
    const float hU = sample_h(heights, size, lx, ly + step);
    nx = -(hR - hL) / (2.f * step);
    ny = -(hU - hD) / (2.f * step);
    nz = 1.f;
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 1.e-6f) {
        nx /= len;
        ny /= len;
        nz /= len;
    }
}

void biline_uv(const o3d::Vertex& c00, const o3d::Vertex& c10, const o3d::Vertex& c11, const o3d::Vertex& c01,
               float tx, float ty, float& u, float& v) {
    const float u0 = c00.uv[0] + (c10.uv[0] - c00.uv[0]) * tx;
    const float u1 = c01.uv[0] + (c11.uv[0] - c01.uv[0]) * tx;
    const float v0 = c00.uv[1] + (c10.uv[1] - c00.uv[1]) * tx;
    const float v1 = c01.uv[1] + (c11.uv[1] - c01.uv[1]) * tx;
    u = u0 + (u1 - u0) * ty;
    v = v0 + (v1 - v0) * ty;
}

// Ear-clip a simple polygon (tile XY); emit tris into out_i using vert indices already in verts.
bool ear_clip(std::vector<std::uint32_t>& poly, const std::vector<o3d::Vertex>& verts,
              std::vector<std::uint32_t>& out_i) {
    if (poly.size() < 3) return false;
    if (poly.size() == 3) {
        out_i.push_back(poly[0]);
        out_i.push_back(poly[1]);
        out_i.push_back(poly[2]);
        return true;
    }
    auto area2 = [&](std::uint32_t ia, std::uint32_t ib, std::uint32_t ic) {
        const auto& a = verts[ia].position;
        const auto& b = verts[ib].position;
        const auto& c = verts[ic].position;
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    };
    auto point_in_tri = [&](std::uint32_t ip, std::uint32_t ia, std::uint32_t ib, std::uint32_t ic) {
        const float a = area2(ia, ib, ip);
        const float b = area2(ib, ic, ip);
        const float c = area2(ic, ia, ip);
        const bool has_neg = (a < 0.f) || (b < 0.f) || (c < 0.f);
        const bool has_pos = (a > 0.f) || (b > 0.f) || (c > 0.f);
        return !(has_neg && has_pos);
    };
    // Ensure CCW
    float signed_area = 0.f;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const auto& p = verts[poly[i]].position;
        const auto& q = verts[poly[(i + 1) % poly.size()]].position;
        signed_area += p[0] * q[1] - q[0] * p[1];
    }
    if (signed_area < 0.f) std::reverse(poly.begin(), poly.end());

    std::vector<std::uint32_t> idx = poly;
    int guard = static_cast<int>(idx.size()) * static_cast<int>(idx.size()) + 8;
    while (idx.size() > 3 && guard-- > 0) {
        bool clipped = false;
        const int n = static_cast<int>(idx.size());
        for (int i = 0; i < n; ++i) {
            const int i0 = (i + n - 1) % n;
            const int i1 = i;
            const int i2 = (i + 1) % n;
            if (area2(idx[static_cast<std::size_t>(i0)], idx[static_cast<std::size_t>(i1)],
                      idx[static_cast<std::size_t>(i2)]) <= 1.e-8f)
                continue; // not a convex ear
            bool ear = true;
            for (int j = 0; j < n; ++j) {
                if (j == i0 || j == i1 || j == i2) continue;
                if (point_in_tri(idx[static_cast<std::size_t>(j)], idx[static_cast<std::size_t>(i0)],
                                 idx[static_cast<std::size_t>(i1)], idx[static_cast<std::size_t>(i2)])) {
                    ear = false;
                    break;
                }
            }
            if (!ear) continue;
            out_i.push_back(idx[static_cast<std::size_t>(i0)]);
            out_i.push_back(idx[static_cast<std::size_t>(i1)]);
            out_i.push_back(idx[static_cast<std::size_t>(i2)]);
            idx.erase(idx.begin() + i1);
            clipped = true;
            break;
        }
        if (!clipped) break;
    }
    if (idx.size() == 3) {
        out_i.push_back(idx[0]);
        out_i.push_back(idx[1]);
        out_i.push_back(idx[2]);
        return true;
    }
    // A fan can cross a concave/self-intersecting contour and recreate the very
    // triangles that should have been cut. Let the caller use adaptive clipping.
    return false;
}

struct EdgeHit {
    int edge = 0;
    float t = 0.f;
    P2 p{};
    int ring = 0;
    int redge = 0;
    float ru = 0.f;
};

void push_vert(std::vector<o3d::Vertex>& verts, const std::vector<float>& heights, int dim, float size,
               float lx, float ly, float u, float v_uv,
               float contour_z = std::numeric_limits<float>::quiet_NaN()) {
    o3d::Vertex vtx{};
    vtx.position[0] = lx;
    vtx.position[1] = ly;
    // The cut line is not an arbitrary point on the old planar heightfield: OMSI builds
    // it from the .sli contour and its vertical profile offset. Use that exact Z when
    // available, otherwise preserve the original triangular terrain interpolation.
    vtx.position[2] = std::isfinite(contour_z) ? contour_z : sample_mesh_h(heights, dim, size, lx, ly);
    sample_normal(heights, dim, size, lx, ly, vtx.normal[0], vtx.normal[1], vtx.normal[2]);
    vtx.uv[0] = u;
    vtx.uv[1] = v_uv;
    verts.push_back(vtx);
}

bool point_in_rect(const P2& p, float x0, float y0, float x1, float y1, float eps = 1.e-5f) {
    return p.x >= x0 - eps && p.x <= x1 + eps && p.y >= y0 - eps && p.y <= y1 + eps;
}

bool ring_vertex_in_rect(const TerrainHoleRing& r, float x0, float y0, float x1, float y1) {
    const std::size_t n = (std::min)(r.x.size(), r.y.size());
    for (std::size_t i = 0; i < n; ++i)
        if (point_in_rect(P2{r.x[i], r.y[i]}, x0, y0, x1, y1)) return true;
    return false;
}

bool ring_boundary_near_rect(const std::vector<TerrainHoleRing>& rings, float x0, float y0, float x1,
                             float y1) {
    // A cheap segment-AABB test is sufficient here: false positives only cause
    // extra refinement; false negatives would re-introduce an uncut cell.
    for (const auto& r : rings) {
        const std::size_t n = (std::min)(r.x.size(), r.y.size());
        if (n < 2) continue;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const float ax = r.x[i], ay = r.y[i], bx = r.x[j], by = r.y[j];
            if ((std::max)(ax, bx) < x0 - 1.e-5f || (std::min)(ax, bx) > x1 + 1.e-5f ||
                (std::max)(ay, by) < y0 - 1.e-5f || (std::min)(ay, by) > y1 + 1.e-5f)
                continue;
            // Segment AABB overlaps this rectangle. Avoid refining the whole
            // interior of a long spline: require an endpoint in the rectangle
            // or a real intersection with one of its four sides.
            if (point_in_rect(P2{ax, ay}, x0, y0, x1, y1) ||
                point_in_rect(P2{bx, by}, x0, y0, x1, y1))
                return true;
            // A segment can be exactly collinear with a rectangle side. The
            // ordinary segment-intersection test intentionally rejects parallel
            // lines, so handle boundary overlap explicitly to avoid a false
            // "interior cell" result at shared terrain/spline edges.
            constexpr float kEdgeEps = 1.e-5f;
            const bool overlap_x = (std::max)(ax, bx) >= x0 - kEdgeEps &&
                                   (std::min)(ax, bx) <= x1 + kEdgeEps;
            const bool overlap_y = (std::max)(ay, by) >= y0 - kEdgeEps &&
                                   (std::min)(ay, by) <= y1 + kEdgeEps;
            if (overlap_x &&
                ((std::fabs(ay - y0) <= kEdgeEps && std::fabs(by - y0) <= kEdgeEps) ||
                 (std::fabs(ay - y1) <= kEdgeEps && std::fabs(by - y1) <= kEdgeEps)))
                return true;
            if (overlap_y &&
                ((std::fabs(ax - x0) <= kEdgeEps && std::fabs(bx - x0) <= kEdgeEps) ||
                 (std::fabs(ax - x1) <= kEdgeEps && std::fabs(bx - x1) <= kEdgeEps)))
                return true;
            const P2 a{ax, ay}, b{bx, by};
            const P2 corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
            for (int e = 0; e < 4; ++e) {
                float t = 0.f, u = 0.f;
                P2 hit{};
                if (seg_intersect(corners[e], corners[(e + 1) % 4], a, b, t, u, hit)) return true;
            }
        }
    }
    return false;
}

// Max subdivision depth of emit_adaptive_rect for the polycut running on THIS thread (workers build tiles in
// parallel, each sets its own value around the call).
thread_local int g_polycut_max_depth = 5;

// Robust bounded fallback for pathological cells (overlapping splines, a hole
// wholly contained by one coarse terrain quad, duplicate/near-collinear points,
// etc.). It recursively subdivides only ambiguous cells. The normal path keeps
// the exact contour; this path limits the worst-case edge error to < 8 cm while
// ensuring we never drop an entire 5 m terrain cell just because clipping failed.
void emit_adaptive_rect(std::vector<o3d::Vertex>& out_v, std::vector<std::uint32_t>& out_i,
                        const std::vector<float>& heights, int dim, float size,
                        const std::vector<TerrainHoleRing>& rings, float x0, float y0, float x1,
                        float y1, const o3d::Vertex& c00, const o3d::Vertex& c10,
                        const o3d::Vertex& c11, const o3d::Vertex& c01, int depth,
                        float root_x0, float root_y0, float root_step) {
    const P2 corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    bool in[4]{};
    int inside_count = 0;
    for (int i = 0; i < 4; ++i) {
        in[i] = point_in_any(corners[i].x, corners[i].y, rings);
        if (in[i]) ++inside_count;
    }
    const float mx = (x0 + x1) * 0.5f;
    const float my = (y0 + y1) * 0.5f;
    const bool center_in = point_in_any(mx, my, rings);
    const bool boundary = ring_boundary_near_rect(rings, x0, y0, x1, y1);
    bool vertex_inside = false;
    if (!boundary) {
        for (const auto& r : rings) {
            if (ring_vertex_in_rect(r, x0, y0, x1, y1)) {
                vertex_inside = true;
                break;
            }
        }
    }

    auto uv_at = [&](float x, float y, float& u, float& v) {
        // UVs are continuous across the regular grid. Interpolate from the four
        // vertices of the original 5 m cell rather than reset/normalize them.
        const float tx = std::clamp((x - root_x0) / root_step, 0.f, 1.f);
        const float ty = std::clamp((y - root_y0) / root_step, 0.f, 1.f);
        biline_uv(c00, c10, c11, c01, tx, ty, u, v);
    };
    auto emit_tri = [&](P2 a, P2 b, P2 c) {
        const float cx = (a.x + b.x + c.x) / 3.f;
        const float cy = (a.y + b.y + c.y) / 3.f;
        if (point_in_any(cx, cy, rings)) return;
        const std::uint32_t base = static_cast<std::uint32_t>(out_v.size());
        const P2 p[3] = {a, b, c};
        for (const auto& q : p) {
            float u = 0.f, v = 0.f;
            uv_at(q.x, q.y, u, v);
            push_vert(out_v, heights, dim, size, q.x, q.y, u, v, q.z);
        }
        out_i.push_back(base + 0);
        out_i.push_back(base + 1);
        out_i.push_back(base + 2);
    };
    auto emit_quad = [&]() {
        const std::uint32_t base = static_cast<std::uint32_t>(out_v.size());
        const P2 p[4] = {corners[0], corners[1], corners[2], corners[3]};
        for (const auto& q : p) {
            float u = 0.f, v = 0.f;
            uv_at(q.x, q.y, u, v);
            push_vert(out_v, heights, dim, size, q.x, q.y, u, v, q.z);
        }
        out_i.push_back(base + 0);
        out_i.push_back(base + 1);
        out_i.push_back(base + 2);
        out_i.push_back(base + 0);
        out_i.push_back(base + 2);
        out_i.push_back(base + 3);
    };

    if (!boundary && !vertex_inside && ((inside_count == 4 && center_in) || (inside_count == 0 && !center_in))) {
        if (!center_in) emit_quad();
        return;
    }

    // Only pathological boundary cells enter this fallback. Eight levels reduce
    // the last visible end-cap slivers to about 2 cm on a 5 m terrain quad.
    // Was a fixed 8 (1.95 cm cells): every cell crossed by a hole contour produced thousands of unshared
    // vertices, so a tile with roads reached 1-2 M vertices (30-150 ms upload each). The caller picks the depth
    // (default 5 = 15.6 cm) and lowers it further when a tile exceeds its triangle budget.
    const int kMaxDepth = g_polycut_max_depth;
    if (depth < kMaxDepth) {
        const float hx = (x0 + x1) * 0.5f;
        const float hy = (y0 + y1) * 0.5f;
        emit_adaptive_rect(out_v, out_i, heights, dim, size, rings, x0, y0, hx, hy,
                           c00, c10, c11, c01, depth + 1, root_x0, root_y0, root_step);
        emit_adaptive_rect(out_v, out_i, heights, dim, size, rings, hx, y0, x1, hy,
                           c00, c10, c11, c01, depth + 1, root_x0, root_y0, root_step);
        emit_adaptive_rect(out_v, out_i, heights, dim, size, rings, hx, hy, x1, y1,
                           c00, c10, c11, c01, depth + 1, root_x0, root_y0, root_step);
        emit_adaptive_rect(out_v, out_i, heights, dim, size, rings, x0, hy, hx, y1,
                           c00, c10, c11, c01, depth + 1, root_x0, root_y0, root_step);
        return;
    }

    // At the leaf, use four center fans rather than discarding a whole cell.
    // Each triangle is classified independently, so a narrow sliver cannot
    // remove a large patch of visible terrain.
    const P2 center{mx, my};
    for (int i = 0; i < 4; ++i) emit_tri(corners[i], corners[(i + 1) % 4], center);
}

// Walk hole ring from (ei,u) to (ej,v) picking the chain whose midpoint lies in the cell.
std::vector<P2> hole_chain_in_cell(const TerrainHoleRing& r, int ei, float u, int ej, float v, float x0,
                                   float y0, float x1, float y1) {
    const int n = static_cast<int>((std::min)(r.x.size(), r.y.size()));
    if (n < 2) return {};
    auto at = [&](int e, float t) -> P2 {
        const int a = ((e % n) + n) % n;
        const int b = (a + 1) % n;
        float z = std::numeric_limits<float>::quiet_NaN();
        if (r.z.size() == r.x.size()) {
            const float za = r.z[static_cast<std::size_t>(a)];
            const float zb = r.z[static_cast<std::size_t>(b)];
            if (std::isfinite(za) && std::isfinite(zb)) z = za + (zb - za) * t;
        }
        return {r.x[static_cast<std::size_t>(a)] +
                    (r.x[static_cast<std::size_t>(b)] - r.x[static_cast<std::size_t>(a)]) * t,
                r.y[static_cast<std::size_t>(a)] +
                    (r.y[static_cast<std::size_t>(b)] - r.y[static_cast<std::size_t>(a)]) * t,
                z};
    };
    auto walk = [&](bool forward) -> std::vector<P2> {
        std::vector<P2> chain;
        chain.push_back(at(ei, u));
        int e = ei;
        float t = u;
        for (int step = 0; step < n + 3; ++step) {
            if (e == ej && ((forward && t <= v + 1.e-4f) || (!forward && t >= v - 1.e-4f)) && step > 0) {
                chain.push_back(at(ej, v));
                break;
            }
            if (forward) {
                if (e == ej && t <= v) {
                    chain.push_back(at(ej, v));
                    break;
                }
                chain.push_back(at(e, 1.f));
                e = (e + 1) % n;
                t = 0.f;
                if (e == ej) {
                    chain.push_back(at(ej, v));
                    break;
                }
            } else {
                if (e == ej && t >= v) {
                    chain.push_back(at(ej, v));
                    break;
                }
                chain.push_back(at(e, 0.f));
                e = (e + n - 1) % n;
                t = 1.f;
                if (e == ej) {
                    chain.push_back(at(ej, v));
                    break;
                }
            }
        }
        return chain;
    };
    auto score = [&](const std::vector<P2>& c) {
        if (c.size() < 2) return -1.e9f;
        int vertices_in_cell = 0;
        float len = 0.f;
        for (const auto& p : c)
            if (point_in_rect(p, x0, y0, x1, y1, 1.e-3f)) ++vertices_in_cell;
        for (std::size_t i = 1; i < c.size(); ++i) {
            const float dx = c[i].x - c[i - 1].x, dy = c[i].y - c[i - 1].y;
            len += std::sqrt(dx * dx + dy * dy);
        }
        // A local contour arc has more of its vertices inside this cell and is
        // much shorter than the complementary walk around the rest of the road.
        return static_cast<float>(vertices_in_cell) * 10000.f - len;
    };
    auto a = walk(true);
    auto b = walk(false);
    return score(a) >= score(b) ? a : b;
}

void emit_cut_cell(std::vector<o3d::Vertex>& out_v, std::vector<std::uint32_t>& out_i,
                   const std::vector<o3d::Vertex>& grid, int dim, const std::vector<float>& heights,
                   float size, int cx, int cy, const std::vector<TerrainHoleRing>& rings) {
    const float step = size / static_cast<float>(dim - 1);
    const float x0 = static_cast<float>(cx) * step;
    const float y0 = static_cast<float>(cy) * step;
    const float x1 = static_cast<float>(cx + 1) * step;
    const float y1 = static_cast<float>(cy + 1) * step;
    const std::uint32_t gi0 = static_cast<std::uint32_t>(cy * dim + cx);
    const std::uint32_t gi1 = static_cast<std::uint32_t>(cy * dim + cx + 1);
    const std::uint32_t gi2 = static_cast<std::uint32_t>((cy + 1) * dim + cx + 1);
    const std::uint32_t gi3 = static_cast<std::uint32_t>((cy + 1) * dim + cx);
    const o3d::Vertex& c00 = grid[gi0];
    const o3d::Vertex& c10 = grid[gi1];
    const o3d::Vertex& c11 = grid[gi2];
    const o3d::Vertex& c01 = grid[gi3];
    P2 corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    bool cin[4]{};
    int n_in = 0;
    for (int i = 0; i < 4; ++i) {
        cin[i] = point_in_any(corners[i].x, corners[i].y, rings);
        if (cin[i]) ++n_in;
    }
    P2 e0[4] = {corners[0], corners[1], corners[2], corners[3]};
    P2 e1[4] = {corners[1], corners[2], corners[3], corners[0]};
    const bool center_in = point_in_any((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, rings);
    bool has_ring_vertex = false;
    for (const auto& r : rings) {
        if (ring_vertex_in_rect(r, x0, y0, x1, y1)) {
            has_ring_vertex = true;
            break;
        }
    }
    auto fallback = [&]() {
        emit_adaptive_rect(out_v, out_i, heights, dim, size, rings, x0, y0, x1, y1,
                           c00, c10, c11, c01, 0, x0, y0, step);
    };

    std::vector<EdgeHit> hits;
    for (std::size_t ri = 0; ri < rings.size(); ++ri) {
        const auto& r = rings[ri];
        const int rn = static_cast<int>((std::min)(r.x.size(), r.y.size()));
        if (rn < 2) continue;
        for (int re = 0; re < rn; ++re) {
            const std::size_t ra = static_cast<std::size_t>(re);
            const std::size_t rb = static_cast<std::size_t>((re + 1) % rn);
            const float za = r.z.size() == r.x.size() ? r.z[ra] : std::numeric_limits<float>::quiet_NaN();
            const float zb = r.z.size() == r.x.size() ? r.z[rb] : std::numeric_limits<float>::quiet_NaN();
            P2 a{r.x[ra], r.y[ra], za};
            P2 b{r.x[rb], r.y[rb], zb};
            for (int ed = 0; ed < 4; ++ed) {
                float t = 0.f, u = 0.f;
                P2 p{};
                if (!seg_intersect(e0[ed], e1[ed], a, b, t, u, p)) continue;
                EdgeHit h{};
                h.edge = ed;
                h.t = t;
                h.p = p;
                h.ring = static_cast<int>(ri);
                h.redge = re;
                h.ru = u;
                hits.push_back(h);
            }
        }
    }
    if (hits.empty()) {
        // Do not assume that four outside corners mean this cell is intact: a
        // narrow road/end-cap can be fully contained inside a 5 m cell without
        // crossing its perimeter. Likewise, all four inside corners are only a
        // safe full cut when the hole boundary doesn't pass through the cell.
        const bool boundary_near = ring_boundary_near_rect(rings, x0, y0, x1, y1);
        if (!boundary_near && !has_ring_vertex && n_in >= 4 && center_in) return;
        if (!boundary_near && !has_ring_vertex && n_in == 0 && !center_in) {
            // Intact cell — use local copies for simplicity.
            const std::uint32_t base = static_cast<std::uint32_t>(out_v.size());
            out_v.push_back(c00);
            out_v.push_back(c10);
            out_v.push_back(c11);
            out_v.push_back(c01);
            out_i.push_back(base + 0);
            out_i.push_back(base + 1);
            out_i.push_back(base + 2);
            out_i.push_back(base + 0);
            out_i.push_back(base + 2);
            out_i.push_back(base + 3);
            return;
        }
        fallback();
        return;
    }

    std::sort(hits.begin(), hits.end(), [](const EdgeHit& a, const EdgeHit& b) {
        if (a.edge != b.edge) return a.edge < b.edge;
        return a.t < b.t;
    });
    // Dedup near-identical hits
    {
        std::vector<EdgeHit> uniq;
        for (const auto& h : hits) {
            if (!uniq.empty()) {
                const auto& p = uniq.back();
                if (p.edge == h.edge && std::fabs(p.t - h.t) < 1.e-4f) continue;
            }
            uniq.push_back(h);
        }
        hits.swap(uniq);
    }

    // The original tessellator rejects crossing/ambiguous hole polygons rather
    // than producing triangles through a hole. The editor should be equally
    // conservative, but must not leave those cells empty: use bounded adaptive
    // refinement whenever several cutters meet inside one coarse quad.
    bool multiple_rings_hit = false;
    for (std::size_t i = 1; i < hits.size(); ++i)
        if (hits[i].ring != hits[0].ring) {
            multiple_rings_hit = true;
            break;
        }
    if (hits.size() > 4 || multiple_rings_hit) {
        fallback();
        return;
    }

    // Walk cell boundary; at enter/exit pairs splice hole chain.
    std::vector<P2> poly;
    auto emit_p = [&](P2 p) {
        if (!poly.empty()) {
            const float dx = poly.back().x - p.x, dy = poly.back().y - p.y;
            if (dx * dx + dy * dy < 1.e-10f) return;
        }
        poly.push_back(p);
    };

    bool inside = cin[0];
    EdgeHit* pending_enter = nullptr;
    static thread_local EdgeHit enter_store{};

    for (int ed = 0; ed < 4; ++ed) {
        if (!inside) emit_p(corners[ed]);
        std::vector<EdgeHit> on_edge;
        for (auto& h : hits)
            if (h.edge == ed) on_edge.push_back(h);
        for (auto& h : on_edge) {
            if (!inside) {
                // outside → entering hole
                emit_p(h.p);
                enter_store = h;
                pending_enter = &enter_store;
                inside = true;
            } else {
                // inside → exiting hole: splice hole arc then exit point
                if (pending_enter) {
                    auto chain = hole_chain_in_cell(rings[static_cast<std::size_t>(pending_enter->ring)],
                                                    pending_enter->redge, pending_enter->ru, h.redge, h.ru, x0,
                                                    y0, x1, y1);
                    if (chain.size() >= 2) {
                        for (std::size_t k = 1; k + 1 < chain.size(); ++k) emit_p(chain[k]);
                    }
                }
                emit_p(h.p);
                pending_enter = nullptr;
                inside = false;
            }
        }
    }
    // If walk ended inside, open contour — fall back to mask-style skip of full cell.
    if (inside || poly.size() < 3) {
        fallback();
        return;
    }

    // Drop polygon if centroid is inside a hole (wrong winding / hole face).
    float cxp = 0.f, cyp = 0.f;
    for (const auto& p : poly) {
        cxp += p.x;
        cyp += p.y;
    }
    cxp /= static_cast<float>(poly.size());
    cyp /= static_cast<float>(poly.size());
    if (point_in_any(cxp, cyp, rings)) {
        // A concave valid polygon can have its centroid in the hole, and the old
        // code dropped the entire remaining terrain cell in that case. Refine
        // instead of confusing a bad centroid with a fully cut cell.
        fallback();
        return;
    }

    std::vector<std::uint32_t> poly_idx;
    poly_idx.reserve(poly.size());
    const std::size_t vertex_mark = out_v.size();
    const std::size_t index_mark = out_i.size();
    for (const auto& p : poly) {
        const float tx = (x1 > x0) ? (p.x - x0) / (x1 - x0) : 0.f;
        const float ty = (y1 > y0) ? (p.y - y0) / (y1 - y0) : 0.f;
        float u = 0.f, v = 0.f;
        biline_uv(c00, c10, c11, c01, std::clamp(tx, 0.f, 1.f), std::clamp(ty, 0.f, 1.f), u, v);
        poly_idx.push_back(static_cast<std::uint32_t>(out_v.size()));
        push_vert(out_v, heights, dim, size, p.x, p.y, u, v, p.z);
    }
    if (!ear_clip(poly_idx, out_v, out_i)) {
        out_v.resize(vertex_mark);
        out_i.resize(index_mark);
        fallback();
    }
}

// Same cross-section mapping as TSplineSegment.Generate in OMSI _Unit84.c:
// actual along = station + clamp(interpolated skew, -32, 32) * profile.x.
float align_skew_at(const map::SplineInst& sp, float station) {
    const float len = (std::max)(sp.length, 1.e-4f);
    const float t = std::clamp(station / len, 0.f, 1.f);
    return std::clamp(sp.skew_start * (1.f - t) + sp.skew_end * t, -32.f, 32.f);
}

float align_actual_along(const map::SplineInst& sp, float station, float lateral) {
    return station + align_skew_at(sp, station) * lateral;
}

float align_near_station(int mode, const spline::HoleProfilePoint& p) {
    return mode > 2 ? 0.f : p.z;
}

float align_far_station(const map::SplineInst& sp, int mode, const spline::HoleProfilePoint& p) {
    const float len = (std::max)(sp.length, 1.e-4f);
    return ((mode & 1) == 0) ? len : (len - p.z);
}

void append_ring_point(TerrainHoleRing& ring, float x, float y, float z) {
    // Avoid zero-length edges, which make ear clipping unstable on skewed end caps.
    if (!ring.x.empty()) {
        const std::size_t i = ring.x.size() - 1;
        const float dx = ring.x[i] - x;
        const float dy = ring.y[i] - y;
        if (dx * dx + dy * dy < 1.e-8f) return;
    }
    ring.x.push_back(x);
    ring.y.push_back(y);
    ring.z.push_back(z);
}

} // namespace

void collect_spline_align_hole_rings(std::vector<TerrainHoleRing>& out, f4f23ma::f32 tile_rel_x,
                                    f4f23ma::f32 tile_rel_y, f4f23ma::f32 size_m,
                                    const spline::TerrainAlignPlan& plan) {
    const float tile_size = size_m > 0.01f ? size_m : 300.f;

    for (const auto& entry : plan.entries) {
        // The plan already holds the compiled hole profiles, lateral/end extents and chain-end Z of
        // every aligned spline, so nothing is loaded, copied or hashed per tile any more.
        if (!spline::terrain_align_entry_may_touch(entry, tile_rel_x, tile_rel_y, tile_size)) continue;
        const auto& sp = *entry.sp;
        const auto& def = entry.comp->def;
        const auto& hole_profiles = entry.comp->raw_rings;
        const float lateral_extent = entry.comp->lateral_extent;
        const float end_extent = entry.comp->end_extent;

        // Exact broad-phase reject BEFORE expensive contour tessellation (same conservative reach
        // as before; the AABB above is only a superset used to skip most splines cheaply).
        const float dx_tile = (std::max)(0.f, (std::max)(tile_rel_x - sp.position.x,
                                                          sp.position.x - (tile_rel_x + tile_size)));
        const float dy_tile = (std::max)(0.f, (std::max)(tile_rel_y - sp.position.y,
                                                          sp.position.y - (tile_rel_y + tile_size)));
        const float reach = (std::max)(0.f, sp.length) + std::fabs(sp.local_length) +
                            std::fabs(sp.skew_start) + std::fabs(sp.skew_end) + lateral_extent + end_extent;
        if (dx_tile * dx_tile + dy_tile * dy_tile > reach * reach) continue;

        const int mode = static_cast<int>(sp.terrain_align);
        for (const auto& source_profile : hole_profiles) {
            std::vector<spline::HoleProfilePoint> hp;
            hp.reserve(source_profile.size());
            for (const auto& p : source_profile) {
                if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) hp.push_back(p);
            }
            std::stable_sort(hp.begin(), hp.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
            if (hp.size() < 2 || hp.back().x <= hp.front().x + 1.e-3f) continue;

            // The profile's third value is a per-point end offset. Do not reduce it to
            // one global [along0, along1] rectangle: the synthesized OMSI profile is
            // narrower at the actual ends and extends only its bottom 0.5 m beyond them.
            const auto& left = hp.front();
            const auto& right = hp.back();
            const float near_left = align_near_station(mode, left);
            const float near_right = align_near_station(mode, right);
            const float far_left = align_far_station(sp, mode, left);
            const float far_right = align_far_station(sp, mode, right);
            const auto& chain_end_z = entry.chain_end_z;

            // OMSI extrudes the contour using the same longitudinal section stations
            // as the spline mesh. Sampling every arbitrary 0.75 m made our cutter a
            // different polyline than the road at its first/last rows (especially on
            // curved or gradient-heavy splines), which left hairline wedges at the caps.
            int contour_sections = std::max(1, spline::omis_teile(sp, chain_end_z));
            // Mirror build_spline_meshes' patchwork section-count adjustment as well.
            if (def->patchwork.valid && !def->patchwork.factors.empty()) {
                int groups = static_cast<int>(std::trunc(
                    sp.length / std::max(def->patchwork.seg_length, 1.e-3f))) + 1;
                groups = std::max(groups, 1);
                const int per_group = contour_sections / groups + 1;
                contour_sections = std::max(groups, per_group * groups);
                if (contour_sections > 200) {
                    const int per_group_limited = std::max(1, 200 / groups);
                    contour_sections = per_group_limited * groups;
                }
            }
            const float station_step = std::max(sp.length / static_cast<float>(contour_sections), 1.e-3f);
            const auto side_steps_for = [&](float near_station, float far_station) {
                return std::clamp(static_cast<int>(std::ceil(
                                      std::abs(far_station - near_station) / station_step)),
                                  1, 4096);
            };
            const int side_steps_right = side_steps_for(near_right, far_right);
            const int side_steps_left = side_steps_for(near_left, far_left);

            TerrainHoleRing ring;
            ring.x.reserve(hp.size() * 2 + static_cast<std::size_t>(side_steps_left + side_steps_right + 2));
            ring.y.reserve(ring.x.capacity());
            ring.z.reserve(ring.x.capacity());
            auto append_spline_point = [&](float station, float lateral, float height_off) {
                const float actual = align_actual_along(sp, station, lateral);
                const auto sample = spline::evaluate_spline(sp, actual, lateral, height_off, chain_end_z);
                if (!std::isfinite(sample.x) || !std::isfinite(sample.y) || !std::isfinite(sample.z)) return;
                append_ring_point(ring, sample.x - tile_rel_x, sample.y - tile_rel_y, sample.z);
            };

            // Start at the near/right corner. Follow the right side to the far end.
            for (int i = 0; i <= side_steps_right; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(side_steps_right);
                append_spline_point(near_right + (far_right - near_right) * t, right.x, right.y);
            }
            // Far cap, following each profile point's own end offset from right to left.
            for (std::size_t i = hp.size() - 1; i-- > 0;) {
                append_spline_point(align_far_station(sp, mode, hp[i]), hp[i].x, hp[i].y);
            }
            // Left side back to the near end. Include the far-left corner explicitly:
            // the far cap only emits interior profile vertices, so skipping i==steps
            // used to connect the last cap vertex to a point partway down the side.
            for (int i = side_steps_left; i >= 0; --i) {
                const float t = static_cast<float>(i) / static_cast<float>(side_steps_left);
                append_spline_point(far_left + (near_left - far_left) * (1.f - t), left.x, left.y);
            }
            // Near cap from left to right, excluding the duplicate right corner which closes the ring.
            for (std::size_t i = 1; i + 1 < hp.size(); ++i) {
                append_spline_point(align_near_station(mode, hp[i]), hp[i].x, hp[i].y);
            }

            // Remove a possible repeated closing vertex; the polygon routines close rings implicitly.
            if (ring.x.size() > 2) {
                const float dx = ring.x.front() - ring.x.back();
                const float dy = ring.y.front() - ring.y.back();
                if (dx * dx + dy * dy < 1.e-8f) {
                    ring.x.pop_back();
                    ring.y.pop_back();
                    if (ring.z.size() == ring.x.size() + 1) ring.z.pop_back();
                }
            }
            if (ring.x.size() >= 3) out.push_back(std::move(ring));
        }
    }
}


void collect_spline_align_hole_rings(std::vector<TerrainHoleRing>& out, f4f23ma::f32 tile_rel_x,
                                    f4f23ma::f32 tile_rel_y, f4f23ma::f32 size_m,
                                    const std::vector<map::SplineInst>& splines) {
    const auto plan = spline::build_terrain_align_plan(splines);
    collect_spline_align_hole_rings(out, tile_rel_x, tile_rel_y, size_m, *plan);
}

void append_object_align_hole_ring(std::vector<TerrainHoleRing>& out, f4f23ma::f32 tile_rel_x,
                                  f4f23ma::f32 tile_rel_y, f4f23ma::f32 wx, f4f23ma::f32 wy,
                                  f4f23ma::f32 heading_deg, f4f23ma::f32 half_x, f4f23ma::f32 half_y) {
    const float rad = heading_deg * (3.14159265f / 180.f);
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const float hx = (std::max)(0.5f, half_x);
    const float hy = (std::max)(0.5f, half_y);
    const float local[4][2] = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
    TerrainHoleRing ring;
    ring.x.resize(4);
    ring.y.resize(4);
    for (int i = 0; i < 4; ++i) {
        const float lx = local[i][0], ly = local[i][1];
        // Inverse of mask_object: world = R * local + origin (forward=(sin,cos) basis used in mask)
        const float px = wx + lx * c - ly * s;
        const float py = wy + lx * s + ly * c;
        ring.x[static_cast<std::size_t>(i)] = px - tile_rel_x;
        ring.y[static_cast<std::size_t>(i)] = py - tile_rel_y;
    }
    out.push_back(std::move(ring));
}

namespace {

// Merge vertices with the same position and UV (1 mm / 1e-4 UV quantization) and drop triangles that collapse.
struct WeldKey {
    std::int32_t v[5];
    bool operator==(const WeldKey& o) const {
        return v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2] && v[3] == o.v[3] && v[4] == o.v[4];
    }
};
struct WeldKeyHash {
    std::size_t operator()(const WeldKey& k) const noexcept {
        std::uint64_t h = 1469598103934665603ull;
        for (const std::int32_t x : k.v) {
            h ^= static_cast<std::uint32_t>(x);
            h *= 1099511628211ull;
        }
        return static_cast<std::size_t>(h);
    }
};

void weld_polycut_mesh(std::vector<o3d::Vertex>& verts, std::vector<std::uint32_t>& idx) {
    if (verts.size() < 8) return;
    std::unordered_map<WeldKey, std::uint32_t, WeldKeyHash> table;
    table.reserve(verts.size() / 2 + 16);
    std::vector<std::uint32_t> remap(verts.size());
    std::vector<o3d::Vertex> out;
    out.reserve(verts.size() / 2 + 16);
    for (std::size_t i = 0; i < verts.size(); ++i) {
        const auto& v = verts[i];
        WeldKey k{{static_cast<std::int32_t>(std::lround(v.position[0] * 1000.f)),
                   static_cast<std::int32_t>(std::lround(v.position[1] * 1000.f)),
                   static_cast<std::int32_t>(std::lround(v.position[2] * 1000.f)),
                   static_cast<std::int32_t>(std::lround(v.uv[0] * 10000.f)),
                   static_cast<std::int32_t>(std::lround(v.uv[1] * 10000.f))}};
        const auto [it, inserted] = table.try_emplace(k, static_cast<std::uint32_t>(out.size()));
        if (inserted) out.push_back(v);
        remap[i] = it->second;
    }
    std::vector<std::uint32_t> out_idx;
    out_idx.reserve(idx.size());
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        const std::uint32_t a = remap[idx[t]], b = remap[idx[t + 1]], c = remap[idx[t + 2]];
        if (a == b || b == c || a == c) continue;
        out_idx.push_back(a);
        out_idx.push_back(b);
        out_idx.push_back(c);
    }
    verts = std::move(out);
    idx = std::move(out_idx);
}

} // namespace

void apply_terrain_polycut_to_mesh(gfx::BufferMesh& mesh, const std::vector<float>& heights, float size_meters,
                                   const std::vector<TerrainHoleRing>& rings, int dim) {
    if (rings.empty() || dim < 2) return;
    if (heights.size() < static_cast<std::size_t>(dim * dim)) return;
    if (mesh.vertices.size() < static_cast<std::size_t>(dim * dim)) return;
    const float size = size_meters > 0.01f ? size_meters : 300.f;

    // collect_spline_align_hole_rings receives all loaded splines so that the
    // same path works for streaming/reloaded tiles. Filter to this tile before
    // tessellating, and honour OMSI's rule that a self-crossing outline cuts
    // nothing. This also prevents off-tile splines from driving quadratic work.
    std::vector<TerrainHoleRing> active_rings;
    active_rings.reserve(rings.size());
    for (const auto& r : rings) {
        const std::size_t n = (std::min)(r.x.size(), r.y.size());
        if (n < 3) continue;
        float minx = r.x[0], maxx = r.x[0], miny = r.y[0], maxy = r.y[0];
        for (std::size_t i = 1; i < n; ++i) {
            minx = (std::min)(minx, r.x[i]);
            maxx = (std::max)(maxx, r.x[i]);
            miny = (std::min)(miny, r.y[i]);
            maxy = (std::max)(maxy, r.y[i]);
        }
        if (maxx < 0.f || maxy < 0.f || minx > size || miny > size) continue;
        if (!ring_is_simple(r)) continue;
        active_rings.push_back(r);
    }
    if (active_rings.empty()) return;

    const std::vector<o3d::Vertex> grid = mesh.vertices;
    std::vector<o3d::Vertex> out_v;
    std::vector<std::uint32_t> out_i;
    out_v.reserve(grid.size() + active_rings.size() * 128);
    out_i.reserve(static_cast<std::size_t>((dim - 1) * (dim - 1) * 6));

    // Quick reject: ring AABB vs cell
    struct Aabb {
        float minx, miny, maxx, maxy;
    };
    std::vector<Aabb> aabbs;
    aabbs.reserve(active_rings.size());
    for (const auto& r : active_rings) {
        Aabb b{1.e9f, 1.e9f, -1.e9f, -1.e9f};
        const std::size_t n = (std::min)(r.x.size(), r.y.size());
        for (std::size_t i = 0; i < n; ++i) {
            b.minx = std::min(b.minx, r.x[i]);
            b.miny = std::min(b.miny, r.y[i]);
            b.maxx = std::max(b.maxx, r.x[i]);
            b.maxy = std::max(b.maxy, r.y[i]);
        }
        aabbs.push_back(b);
    }

    const float step = size / static_cast<float>(dim - 1);
    // Triangle budget per tile: when the cut produces more, rebuild with a shallower adaptive depth.
    constexpr std::size_t kMaxTrisPerTile = 150000;
    constexpr int kStartDepth = 5;
    constexpr int kMinDepth = 3;
    int depth_limit = kStartDepth;
    for (;;) {
    g_polycut_max_depth = depth_limit;
    out_v.clear();
    out_i.clear();
    for (int cy = 0; cy < dim - 1; ++cy) {
        for (int cx = 0; cx < dim - 1; ++cx) {
            const float x0 = static_cast<float>(cx) * step;
            const float y0 = static_cast<float>(cy) * step;
            const float x1 = x0 + step;
            const float y1 = y0 + step;
            bool touches = false;
            for (const auto& b : aabbs) {
                if (x1 < b.minx || x0 > b.maxx || y1 < b.miny || y0 > b.maxy) continue;
                touches = true;
                break;
            }
            if (!touches) {
                const std::uint32_t base = static_cast<std::uint32_t>(out_v.size());
                const std::uint32_t gi0 = static_cast<std::uint32_t>(cy * dim + cx);
                const std::uint32_t gi1 = static_cast<std::uint32_t>(cy * dim + cx + 1);
                const std::uint32_t gi2 = static_cast<std::uint32_t>((cy + 1) * dim + cx + 1);
                const std::uint32_t gi3 = static_cast<std::uint32_t>((cy + 1) * dim + cx);
                out_v.push_back(grid[gi0]);
                out_v.push_back(grid[gi1]);
                out_v.push_back(grid[gi2]);
                out_v.push_back(grid[gi3]);
                out_i.push_back(base + 0);
                out_i.push_back(base + 1);
                out_i.push_back(base + 2);
                out_i.push_back(base + 0);
                out_i.push_back(base + 2);
                out_i.push_back(base + 3);
                continue;
            }
            emit_cut_cell(out_v, out_i, grid, dim, heights, size, cx, cy, active_rings);
        }
    }
    // Every cell emitted private copies of its corner/seam vertices: share identical ones.
    weld_polycut_mesh(out_v, out_i);
    if (out_i.size() / 3 <= kMaxTrisPerTile || depth_limit <= kMinDepth) break;
    --depth_limit;
    }
    g_polycut_max_depth = kStartDepth;
    mesh.vertices = std::move(out_v);
    mesh.indices = std::move(out_i);
}

} // namespace omsi::editor

// Compiled as part of this translation unit so no CMakeLists.txt change is needed
// (terrain_restamp.cpp must NOT be listed separately in CMake or symbols would be duplicated).
#include "editor/terrain_restamp.cpp"
