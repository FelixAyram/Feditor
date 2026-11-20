// Ported from openOMSI (crates/omsi-sim/src/collision.rs, https://github.com/openOMSI-org/openOMSI),
// GNU AGPL-3.0. See NOTICE.md.
#include "omsi/vehicle/collision.hpp"

#include "o3d/o3d_loader.hpp"
#include "omsi/logger.hpp"
#include "omsi/o3d_format.hpp"
#include "omsi/utf8_win32.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>

namespace omsi::vehicle {
namespace {

// Cell of a mesh shape's own grid (m).
constexpr double kPartCell = 8.0;
// A part no thinner than this (m): a wall's triangle has no depth of its own.
constexpr double kPartHalfMin = 0.05;
// A sloping triangle whose box would hold more than this much air (m^2 footprint x height) is cut
// into bands: a roof pitched over a house would otherwise be a solid block from eaves to ridge.
constexpr double kPartSlack = 0.25;
// How high a band of a sloping triangle is (m).
constexpr double kPartBand = 0.1;
// Objects whose collision faces stay under this over their origin are no obstacle (openOMSI LOW_OBJECT).
constexpr double kLowObject = 0.3;

std::int64_t cell_key(int x, int y) {
    return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(y);
}

std::optional<Vec2d> centroid(const std::vector<Vec2d>& poly) {
    if (poly.empty()) return std::nullopt;
    double area = 0.0;
    Vec2d c{};
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const Vec2d p = poly[i], q = poly[(i + 1) % poly.size()];
        const double a = p.perp_dot(q);
        area += a;
        c = c + (p + q) * a;
    }
    if (std::fabs(area) < 1e-6) {
        Vec2d s{};
        for (const auto& p : poly) s = s + p;
        return s * (1.0 / static_cast<double>(poly.size()));
    }
    return c * (1.0 / (3.0 * area));
}

// The part of the convex polygon subject inside the convex polygon clipper (Sutherland-Hodgman).
std::vector<Vec2d> clip(const std::vector<Vec2d>& subject, const std::vector<Vec2d>& clipper) {
    std::vector<Vec2d> out = subject;
    const std::size_t n = clipper.size();
    Vec2d centre{};
    for (const auto& p : clipper) centre = centre + p;
    centre = centre * (1.0 / static_cast<double>(n));
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2d a = clipper[i], b = clipper[(i + 1) % n];
        const Vec2d edge = b - a;
        const auto side = [&](Vec2d p) { return edge.perp_dot(p - a); };
        const double inside_sign = side(centre) >= 0.0 ? 1.0 : -1.0;
        std::vector<Vec2d> input;
        input.swap(out);
        if (input.empty()) break;
        for (std::size_t j = 0; j < input.size(); ++j) {
            const Vec2d p = input[j], q = input[(j + 1) % input.size()];
            const double sp = side(p) * inside_sign, sq = side(q) * inside_sign;
            if (sp >= 0.0) out.push_back(p);
            if ((sp >= 0.0) != (sq >= 0.0)) out.push_back(p + (q - p) * (sp / (sp - sq)));
        }
    }
    return out;
}

// The part of a convex polygon above (keep_above) or below the height z.
std::vector<Vec3d> clip_z(const std::vector<Vec3d>& poly, double z, bool keep_above) {
    const auto inside = [&](const Vec3d& p) { return keep_above ? p.z >= z : p.z <= z; };
    std::vector<Vec3d> out;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const Vec3d p = poly[i], q = poly[(i + 1) % poly.size()];
        if (inside(p)) out.push_back(p);
        if (inside(p) != inside(q)) {
            const double t = (z - p.z) / (q.z - p.z);
            out.push_back({p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t});
        }
    }
    return out;
}

// The boxes standing in for one triangle: one when it is upright (a wall) or flat (a floor),
// else a slab per 10 cm band of its height, each laid along the band.
void triangle_parts(const std::array<Vec3d, 3>& t, std::vector<Obb>& out) {
    const Vec2d q[3] = {{t[0].x, t[0].y}, {t[1].x, t[1].y}, {t[2].x, t[2].y}};
    const double area = 0.5 * std::fabs((q[1] - q[0]).perp_dot(q[2] - q[0]));
    const double z0 = (std::min)({t[0].z, t[1].z, t[2].z});
    const double z1 = (std::max)({t[0].z, t[1].z, t[2].z});
    const auto part = [&](const std::vector<Vec3d>& poly, Vec2d along) {
        Obb b;
        b.heading = along.length() < 1e-6 ? 0.0 : std::atan2(along.x, along.y);
        b.z0 = std::numeric_limits<double>::max();
        b.z1 = std::numeric_limits<double>::lowest();
        Vec2d r, f;
        b.axes(r, f);
        double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
        for (const auto& p : poly) {
            const Vec2d p2{p.x, p.y};
            u0 = (std::min)(u0, p2.dot(r));
            u1 = (std::max)(u1, p2.dot(r));
            v0 = (std::min)(v0, p2.dot(f));
            v1 = (std::max)(v1, p2.dot(f));
            b.z0 = (std::min)(b.z0, p.z);
            b.z1 = (std::max)(b.z1, p.z);
        }
        b.center = r * ((u0 + u1) * 0.5) + f * ((v0 + v1) * 0.5);
        b.half = {(std::max)((u1 - u0) * 0.5, kPartHalfMin), (std::max)((v1 - v0) * 0.5, kPartHalfMin)};
        out.push_back(b);
    };
    if (area * (z1 - z0) <= kPartSlack) {
        // along the longest edge in plan view
        const Vec2d edges[3] = {q[1] - q[0], q[2] - q[1], q[0] - q[2]};
        Vec2d d = edges[0];
        for (const auto& e : edges)
            if (e.dot(e) > d.dot(d)) d = e;
        part({t[0], t[1], t[2]}, d);
        return;
    }
    // bands between contour lines, laid along them
    const Vec3d e1{t[1].x - t[0].x, t[1].y - t[0].y, t[1].z - t[0].z};
    const Vec3d e2{t[2].x - t[0].x, t[2].y - t[0].y, t[2].z - t[0].z};
    const Vec3d n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
    const Vec2d contour{-n.y, n.x};
    const int bands = std::clamp(static_cast<int>(std::ceil((z1 - z0) / kPartBand)), 1, 64);
    const double dz = (z1 - z0) / bands;
    for (int k = 0; k < bands; ++k) {
        const double lo = z0 + dz * k, hi = z0 + dz * (k + 1);
        const auto poly = clip_z(clip_z({t[0], t[1], t[2]}, lo, true), hi, false);
        if (poly.size() >= 3) part(poly, contour);
    }
}

} // namespace

double Vec2d::length() const { return std::sqrt(x * x + y * y); }

Obb Obb::from_box(const float bb[6], Vec3d pos, double heading_deg) {
    const double h = heading_deg * 3.14159265358979323846 / 180.0;
    const double sh = std::sin(h), ch = std::cos(h);
    const double cx = bb[3], cy = bb[4];
    Obb o;
    // local (x right, y forward) -> world for a clockwise heading
    o.center = {pos.x + cx * ch + cy * sh, pos.y - cx * sh + cy * ch};
    o.half = {bb[0] * 0.5, bb[1] * 0.5};
    o.heading = h;
    o.z0 = pos.z + bb[5] - bb[2] * 0.5;
    o.z1 = pos.z + bb[5] + bb[2] * 0.5;
    return o;
}

void Obb::axes(Vec2d& right, Vec2d& forward) const {
    const double s = std::sin(heading), c = std::cos(heading);
    right = {c, -s};
    forward = {s, c};
}

std::vector<Vec2d> Obb::corners() const {
    Vec2d r, f;
    axes(r, f);
    return {center + r * half.x + f * half.y, center - r * half.x + f * half.y, center - r * half.x - f * half.y,
            center + r * half.x - f * half.y};
}

void Obb::interval(Vec2d axis, double& lo, double& hi) const {
    Vec2d r, f;
    axes(r, f);
    const double c = center.dot(axis);
    const double e = half.x * std::fabs(r.dot(axis)) + half.y * std::fabs(f.dot(axis));
    lo = c - e;
    hi = c + e;
}

bool Obb::overlaps(const Obb& o) const {
    if (z1 < o.z0 || o.z1 < z0) return false;
    Vec2d a[4];
    axes(a[0], a[1]);
    o.axes(a[2], a[3]);
    for (const auto& axis : a) {
        double min_a, max_a, min_b, max_b;
        interval(axis, min_a, max_a);
        o.interval(axis, min_b, max_b);
        if (max_a < min_b || max_b < min_a) return false;
    }
    return true;
}

std::optional<Contact> Obb::contact(const Obb& o) const {
    if (z1 <= o.z0 || o.z1 <= z0) return std::nullopt;
    Vec2d a[4];
    axes(a[0], a[1]);
    o.axes(a[2], a[3]);
    bool have = false;
    double best_d = 0.0;
    Vec2d best_n{};
    for (const auto& axis : a) {
        double min_a, max_a, min_b, max_b;
        interval(axis, min_a, max_a);
        o.interval(axis, min_b, max_b);
        if (max_a <= min_b || max_b <= min_a) return std::nullopt;
        // move self along +axis by (max_b - min_a), or along -axis by (max_a - min_b)
        const bool plus = max_b - min_a < max_a - min_b;
        const double d = plus ? max_b - min_a : max_a - min_b;
        if (!have || d < best_d) {
            have = true;
            best_d = d;
            best_n = plus ? axis : -axis;
        }
    }
    // clipped around our own centre (large map coordinates drown a small overlap in rounding)
    auto local = [&](std::vector<Vec2d> c) {
        for (auto& p : c) p = p - center;
        return c;
    };
    const auto cen = centroid(clip(local(corners()), local(o.corners())));
    Contact c;
    c.normal = best_n;
    c.depth = best_d;
    c.point = cen ? *cen + center : (center + o.center) * 0.5;
    c.z0 = (std::max)(z0, o.z0);
    c.z1 = (std::min)(z1, o.z1);
    return c;
}

double Obb::separation(const Obb& o) const {
    const Vec2d d = o.center - center;
    Vec2d ra, fa, rb, fb;
    axes(ra, fa);
    o.axes(rb, fb);
    const auto extent = [](Vec2d r, Vec2d f, Vec2d h, Vec2d axis) {
        return std::fabs(r.dot(axis)) * h.x + std::fabs(f.dot(axis)) * h.y;
    };
    double best = std::numeric_limits<double>::lowest();
    for (const Vec2d axis : {ra, fa, rb, fb}) {
        const double gap = std::fabs(d.dot(axis)) - extent(ra, fa, half, axis) - extent(rb, fb, o.half, axis);
        best = (std::max)(best, gap);
    }
    return best;
}

std::shared_ptr<MeshShape> MeshShape::from_triangles(const std::vector<std::array<Vec3d, 3>>& tris, double min_top) {
    auto shape = std::make_shared<MeshShape>();
    std::set<std::array<std::int64_t, 6>> seen;
    std::vector<Obb> out;
    for (const auto& t : tris) {
        out.clear();
        triangle_parts(t, out);
        for (const auto& part : out) {
            if (part.z1 <= min_top) continue;
            // the two halves of a flat wall make the same slab
            const auto q = [](double v) { return static_cast<std::int64_t>(std::llround(v * 100.0)); };
            if (!seen.insert({q(part.center.x), q(part.center.y), q(part.half.x), q(part.half.y), q(part.z0), q(part.z1)})
                     .second)
                continue;
            shape->add(part);
        }
    }
    return shape;
}

void MeshShape::add(const Obb& b) {
    const auto i = static_cast<std::uint32_t>(parts.size());
    parts.push_back(b);
    const double r = b.radius();
    lo = {(std::min)(lo.x, b.center.x - r), (std::min)(lo.y, b.center.y - r), (std::min)(lo.z, b.z0)};
    hi = {(std::max)(hi.x, b.center.x + r), (std::max)(hi.y, b.center.y + r), (std::max)(hi.z, b.z1)};
    const int y0 = static_cast<int>(std::floor((b.center.y - r) / kPartCell));
    const int y1 = static_cast<int>(std::floor((b.center.y + r) / kPartCell));
    const int x0 = static_cast<int>(std::floor((b.center.x - r) / kPartCell));
    const int x1 = static_cast<int>(std::floor((b.center.x + r) / kPartCell));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) grid_[cell_key(x, y)].push_back(i);
}

std::vector<std::uint32_t> MeshShape::parts_within(Vec2d c, double r) const {
    std::vector<std::uint32_t> out;
    const int x0 = static_cast<int>(std::floor((c.x - r) / kPartCell));
    const int y0 = static_cast<int>(std::floor((c.y - r) / kPartCell));
    const int x1 = static_cast<int>(std::floor((c.x + r) / kPartCell));
    const int y1 = static_cast<int>(std::floor((c.y + r) / kPartCell));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const auto it = grid_.find(cell_key(x, y));
            if (it == grid_.end()) continue;
            for (const auto i : it->second) {
                const auto& p = parts[i];
                const double pr = p.radius();
                // listed in every cell it covers: taken in the first of them met here
                const bool first =
                    (std::max)(static_cast<int>(std::floor((p.center.x - pr) / kPartCell)), x0) == x &&
                    (std::max)(static_cast<int>(std::floor((p.center.y - pr) / kPartCell)), y0) == y;
                if (first && (p.center - c).length() <= r + pr) out.push_back(i);
            }
        }
    }
    return out;
}

MeshObstacle::MeshObstacle(std::shared_ptr<const MeshShape> s, Vec3d p, double heading_deg, std::int64_t id_)
    : shape(std::move(s)), pos(p), heading(heading_deg * 3.14159265358979323846 / 180.0), id(id_) {
    const Vec3d size{shape->hi.x - shape->lo.x, shape->hi.y - shape->lo.y, shape->hi.z - shape->lo.z};
    const Vec3d c{(shape->lo.x + shape->hi.x) * 0.5, (shape->lo.y + shape->hi.y) * 0.5, (shape->lo.z + shape->hi.z) * 0.5};
    const float bb[6] = {static_cast<float>(size.x), static_cast<float>(size.y), static_cast<float>(size.z),
                         static_cast<float>(c.x),    static_cast<float>(c.y),    static_cast<float>(c.z)};
    bounds = Obb::from_box(bb, pos, heading_deg);
    bounds.z0 = pos.z + shape->lo.z;
    bounds.z1 = pos.z + shape->hi.z;
    bounds.id = id;
}

void MeshObstacle::parts_near(const Obb& b, std::vector<Obb>& out) const {
    if (!bounds.overlaps_plan(b)) return;
    const double s = std::sin(heading), c = std::cos(heading);
    // world point -> object frame (plan view)
    const Vec2d d{b.center.x - pos.x, b.center.y - pos.y};
    const Vec2d local{d.x * c - d.y * s, d.x * s + d.y * c};
    const double z0 = b.z0 - pos.z, z1 = b.z1 - pos.z;
    for (const auto i : shape->parts_within(local, b.radius())) {
        const auto& p = shape->parts[i];
        if (!(p.z1 > z0 && p.z0 < z1)) continue;
        Obb w = p;
        w.center = {pos.x + p.center.x * c + p.center.y * s, pos.y - p.center.x * s + p.center.y * c};
        w.heading = p.heading + heading;
        w.z0 = p.z0 + pos.z;
        w.z1 = p.z1 + pos.z;
        w.id = id;
        out.push_back(w);
    }
}

std::shared_ptr<const MeshShape> load_collision_shape(const std::filesystem::path& o3d_path) {
    static std::mutex mu;
    static std::unordered_map<std::wstring, std::shared_ptr<const MeshShape>> cache;
    const std::wstring key = o3d_path.wstring();
    {
        std::lock_guard lk(mu);
        if (const auto it = cache.find(key); it != cache.end()) return it->second;
    }
    std::shared_ptr<const MeshShape> shape;
    try {
        const auto mapped = o3d::load_mapped_shared(o3d_path);
        if (mapped && mapped->valid()) {
            const auto& mesh = mapped->mesh();
            std::vector<o3d::Vertex> verts;
            o3d::prepare_o3d_mesh_vertices_for_gpu(mesh, verts);
            std::vector<std::array<Vec3d, 3>> tris;
            tris.reserve(mesh.triangle_count);
            const auto v = [&](std::uint32_t i) -> Vec3d {
                if (i >= verts.size()) return {};
                return {verts[i].position[0], verts[i].position[1], verts[i].position[2]};
            };
            if (mesh.long_triangle_indices) {
                for (const auto& t : mesh.triangle_u32_span()) tris.push_back({v(t.i0), v(t.i1), v(t.i2)});
            } else {
                for (const auto& t : mesh.triangle_u16_span()) tris.push_back({v(t.i0), v(t.i1), v(t.i2)});
            }
            auto s = MeshShape::from_triangles(tris, kLowObject);
            if (!s->parts.empty()) shape = std::move(s);
        }
    } catch (const std::exception& ex) {
        omsi::log::warn(std::string("collision mesh failed: ") + ex.what());
    }
    std::lock_guard lk(mu);
    cache.emplace(key, shape);
    return shape;
}

} // namespace omsi::vehicle
