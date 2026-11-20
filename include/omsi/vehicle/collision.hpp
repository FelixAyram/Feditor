#pragma once
// Vehicle collision world: oriented boxes in the ground plane plus a height range.
// Ported from openOMSI (crates/omsi-sim/src/collision.rs, https://github.com/openOMSI-org/openOMSI),
// GNU AGPL-3.0. See NOTICE.md.

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace omsi::vehicle {

struct Vec2d {
    double x = 0.0, y = 0.0;
    Vec2d operator+(Vec2d o) const { return {x + o.x, y + o.y}; }
    Vec2d operator-(Vec2d o) const { return {x - o.x, y - o.y}; }
    Vec2d operator*(double s) const { return {x * s, y * s}; }
    Vec2d operator-() const { return {-x, -y}; }
    [[nodiscard]] double dot(Vec2d o) const { return x * o.x + y * o.y; }
    [[nodiscard]] double perp_dot(Vec2d o) const { return x * o.y - y * o.x; }
    [[nodiscard]] double length() const;
};

struct Vec3d {
    double x = 0.0, y = 0.0, z = 0.0;
};

// Where two boxes touch: push the tested box by normal * depth to separate them.
struct Contact {
    Vec2d normal; // unit, ground plane, from the obstacle towards the tested box
    double depth = 0.0;
    Vec2d point; // middle of the overlap
    double z0 = 0.0, z1 = 0.0;
};

struct Obb {
    Vec2d center;
    Vec2d half;           // x across, y along the heading
    double heading = 0.0; // radians, clockwise from north (like vehicles)
    double z0 = 0.0, z1 = 0.0;
    std::int64_t id = -1; // map object id of a placed obstacle, -1 for anything else

    // OMSI [boundingbox] w l h cx cy cz in a frame at pos turned by heading_deg.
    static Obb from_box(const float bb[6], Vec3d pos, double heading_deg);

    void axes(Vec2d& right, Vec2d& forward) const;
    [[nodiscard]] std::vector<Vec2d> corners() const;
    [[nodiscard]] bool overlaps(const Obb& o) const;
    [[nodiscard]] bool overlaps_plan(const Obb& o) const { return separation(o) < 0.0; }
    // The push-out direction is the axis with the shortest way *out* (to the nearer side), so a
    // thin post in front of the bumper is not "separated" sideways by its own width.
    [[nodiscard]] std::optional<Contact> contact(const Obb& o) const;
    [[nodiscard]] double separation(const Obb& o) const;
    [[nodiscard]] double radius() const { return half.length(); }

private:
    void interval(Vec2d axis, double& lo, double& hi) const;
};

// A [collision_mesh] in its object's frame (x right, y forward, z up): every triangle becomes a
// thin box (a wall's slab, a floor's plate; sloping faces cut in 10 cm bands), so a vehicle meets
// the faces it really runs into instead of one box around the whole mesh.
class MeshShape {
public:
    // Triangles whose top stays below min_top over the origin (kerbs, floors) are no obstacle.
    static std::shared_ptr<MeshShape> from_triangles(const std::vector<std::array<Vec3d, 3>>& tris, double min_top);

    std::vector<Obb> parts;
    Vec3d lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};

    // Indices of the parts within r of the local point c (each once).
    [[nodiscard]] std::vector<std::uint32_t> parts_within(Vec2d c, double r) const;

private:
    void add(const Obb& b);
    std::unordered_map<std::int64_t, std::vector<std::uint32_t>> grid_;
};

// A placed object's collision mesh.
struct MeshObstacle {
    std::shared_ptr<const MeshShape> shape;
    Vec3d pos;
    double heading = 0.0; // radians, clockwise
    std::int64_t id = -1;
    Obb bounds; // around the whole mesh, world

    MeshObstacle(std::shared_ptr<const MeshShape> s, Vec3d p, double heading_deg, std::int64_t id_);
    // Parts that may touch b, in the world.
    void parts_near(const Obb& b, std::vector<Obb>& out) const;
};

// [collision_mesh] .o3d → shape (cached per file, min_top = 0.3 m like openOMSI's LOW_OBJECT).
[[nodiscard]] std::shared_ptr<const MeshShape> load_collision_shape(const std::filesystem::path& o3d_path);

} // namespace omsi::vehicle
