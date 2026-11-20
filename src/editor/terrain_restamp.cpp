#include "editor/terrain_restamp.hpp"

#include "editor/editor_ground.hpp"
#include "omsi/job_system.hpp"
#include "omsi/map_attachment.hpp"
#include "omsi/sco_types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>

namespace omsi::editor {
namespace {

// Rings of a [terrainhole] outline in engine object space: x, y (forward), z (up).
using HoleOutline = std::vector<std::vector<std::array<f4f23ma::f32, 3>>>;

// ---- .sco hole metadata (thread-safe, shared by every context) --------------------------------
// Mirrors the old per-session cache in the app: companion .hole probe + bounding box. Each unique
// .sco is probed once for the lifetime of the process instead of once per tile restamp, and the
// probe now happens on worker threads, never on the frame thread.
struct ScoHoleMeta {
    bool has_hole = false;
    f4f23ma::f32 half = 14.f;
    // [absheight] + [terrainhole] with the OMSI editor outline cache (see load_hole_outline).
    std::shared_ptr<const HoleOutline> outline;
};

// The OMSI editor caches the outline of a [terrainhole] mesh next to it as "<mesh>.hole":
//   [date] <stamp>, then per ring: [hole] <count> followed by count * (x, y, z) lines,
// in the mesh's OMSI object space (x right, y up, z forward; ~3 cm inset from the mesh border).
std::shared_ptr<const HoleOutline> load_hole_outline(const std::filesystem::path& hole_file) {
    std::ifstream in(hole_file, std::ios::binary);
    if (!in) return nullptr;
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) {
        while (!l.empty() && std::isspace(static_cast<unsigned char>(l.back()))) l.pop_back();
        std::size_t b = 0;
        while (b < l.size() && std::isspace(static_cast<unsigned char>(l[b]))) ++b;
        lines.push_back(l.substr(b));
    }
    auto is_hole_tag = [](const std::string& s) {
        if (s.size() != 6) return false;
        std::string t = s;
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t == "[hole]";
    };
    auto parse_f = [](std::string s, bool& ok) {
        std::replace(s.begin(), s.end(), ',', '.');
        char* end = nullptr;
        const float v = std::strtof(s.c_str(), &end);
        if (end == s.c_str() || !std::isfinite(v)) ok = false;
        return v;
    };
    auto out = std::make_shared<HoleOutline>();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!is_hole_tag(lines[i]) || i + 1 >= lines.size()) continue;
        const long n = std::strtol(lines[i + 1].c_str(), nullptr, 10);
        if (n < 3 || n > 100000 || i + 2 + 3 * static_cast<std::size_t>(n) > lines.size()) continue;
        std::vector<std::array<f4f23ma::f32, 3>> ring;
        ring.reserve(static_cast<std::size_t>(n));
        bool ok = true;
        for (long k = 0; k < n; ++k) {
            const std::size_t at = i + 2 + 3 * static_cast<std::size_t>(k);
            const float x = parse_f(lines[at], ok);
            const float y_up = parse_f(lines[at + 1], ok);
            const float z_fwd = parse_f(lines[at + 2], ok);
            // Same Y<->Z swap the GPU path applies to the mesh (o3d_mesh_draw_matrix).
            ring.push_back({x, z_fwd, y_up});
        }
        // Polygon routines close rings implicitly: drop a repeated closing vertex (zero-length edge).
        while (ring.size() > 3 && std::fabs(ring.front()[0] - ring.back()[0]) < 1.e-4f &&
               std::fabs(ring.front()[1] - ring.back()[1]) < 1.e-4f)
            ring.pop_back();
        if (ok && ring.size() >= 3) out->push_back(std::move(ring));
        i += 1 + 3 * static_cast<std::size_t>(n);
    }
    if (out->empty()) return nullptr;
    return out;
}

std::shared_ptr<const HoleOutline> find_terrain_hole_outline(const sco::SceneryObject& sco,
                                                             const std::filesystem::path& sco_path) {
    if (sco.terrain_hole_mesh.empty() || !sco::skip_terrain_add(sco)) return nullptr;
    const auto dir = sco_path.parent_path();
    // Same lookup order as [mesh] in sco_loader: next to the .sco, then model/.
    for (auto hole : {dir / sco.terrain_hole_mesh, dir / "model" / sco.terrain_hole_mesh}) {
        hole += ".hole";
        std::error_code ec;
        if (!std::filesystem::exists(hole, ec)) continue;
        if (auto o = load_hole_outline(hole)) return o;
    }
    return nullptr;
}

std::mutex g_sco_meta_mu;
std::unordered_map<std::string, ScoHoleMeta>& sco_meta_cache() {
    static std::unordered_map<std::string, ScoHoleMeta> cache;
    return cache;
}

ScoHoleMeta sco_hole_meta(const std::filesystem::path& sco_path) {
    if (sco_path.empty()) return {false, 14.f};
    const std::string key = sco_path.lexically_normal().string();
    {
        std::lock_guard<std::mutex> lk(g_sco_meta_mu);
        const auto& cache = sco_meta_cache();
        if (const auto it = cache.find(key); it != cache.end()) return it->second;
    }
    // Probe outside the lock: filesystem calls can be slow on HDD / network shares.
    const auto a = std::filesystem::path(sco_path.string() + ".hole");
    const auto b = sco_path.parent_path() / (sco_path.stem().wstring() + L".hole");
    std::error_code ec1, ec2;
    const bool has_hole = std::filesystem::exists(a, ec1) || std::filesystem::exists(b, ec2);
    ScoHoleMeta meta{has_hole, 14.f, nullptr};
    if (const auto* sco = sco::load_sco(sco_path)) {
        if (has_hole && sco->has_boundingbox)
            meta.half = (std::max)(0.5f, 0.5f * (std::max)(sco->bbox_w, sco->bbox_l));
        meta.outline = find_terrain_hole_outline(*sco, sco_path);
    }
    std::lock_guard<std::mutex> lk(g_sco_meta_mu);
    sco_meta_cache().emplace(key, meta);
    return meta;
}

} // namespace

bool sco_has_terrain_hole_outline(const std::filesystem::path& sco_path) {
    return sco_hole_meta(sco_path).outline != nullptr;
}

std::shared_ptr<TerrainRestampContext> TerrainRestampContext::build(const std::vector<map::SplineInst>& splines,
                                                                    std::vector<TerrainRestampObjectIn> objects,
                                                                    std::vector<TerrainRestampSaIn> spline_attachments) {
    std::shared_ptr<TerrainRestampContext> ctx(new TerrainRestampContext());
    ctx->plan_ = spline::build_terrain_align_plan(splines);
    ctx->objects_in_ = std::move(objects);
    ctx->sa_in_ = std::move(spline_attachments);
    return ctx;
}

void TerrainRestampContext::ensure_prepared() const { std::call_once(prepared_once_, [this] { prepare_impl(); }); }

void TerrainRestampContext::prepare_impl() const {
    objects_.reserve(objects_in_.size());
    for (const auto& o : objects_in_) {
        const auto meta = sco_hole_meta(o.sco_path);
        PreparedObject p{};
        p.x = o.x;
        p.y = o.y;
        p.z = o.z;
        p.rot_z = o.rot_z;
        p.has_hole = meta.has_hole;
        p.half = meta.half;
        p.overlap_radius = meta.has_hole ? (std::max)(14.f, meta.half * 1.5f) : 14.f;
        if (meta.outline) {
            // Object space -> world with the same yaw as sco_object_base_matrix (RotationZ(-yaw), row
            // vectors): world = origin + (x*c + y*s, -x*s + y*c). Z is absolute ([absheight]).
            const float rad = o.yaw_deg * (3.14159265f / 180.f);
            const float c = std::cos(rad);
            const float s = std::sin(rad);
            p.out_minx = p.out_miny = 1.e30f;
            p.out_maxx = p.out_maxy = -1.e30f;
            float r2 = 0.f;
            for (const auto& ring : *meta.outline) {
                auto& wr = p.outline.emplace_back();
                wr.reserve(ring.size());
                for (const auto& q : ring) {
                    const float wx = o.x + q[0] * c + q[1] * s;
                    const float wy = o.y - q[0] * s + q[1] * c;
                    wr.push_back({wx, wy, o.z + q[2]});
                    p.out_minx = (std::min)(p.out_minx, wx);
                    p.out_miny = (std::min)(p.out_miny, wy);
                    p.out_maxx = (std::max)(p.out_maxx, wx);
                    p.out_maxy = (std::max)(p.out_maxy, wy);
                    r2 = (std::max)(r2, q[0] * q[0] + q[1] * q[1]);
                }
            }
            p.overlap_radius = (std::max)(p.overlap_radius, std::sqrt(r2) + 10.f); // + max apron
        }
        objects_.push_back(std::move(p));
    }
    // [splineAttachement] + AlignTerrain: stamp at the attachment pose(s). Poses depend only on the
    // splines and attachments, never on the tile, so they are computed once here (the old code redid
    // the along-enumeration and pose math for every SA on every tile).
    const auto& splines = plan_->splines;
    for (const auto& e : sa_in_) {
        if (e.parent_index < 0 || static_cast<std::size_t>(e.parent_index) >= splines.size()) continue;
        const auto& parent = splines[static_cast<std::size_t>(e.parent_index)];
        const auto alongs = map::enumerate_spline_attachment_along(e.att, parent);
        const auto chain_z = spline::resolve_spline_mesh_chain_end_z(parent, {});
        for (const float along : alongs) {
            const auto pose = map::compute_spline_attachment_pose(parent, e.att, along, chain_z, e.att.tangential);
            sa_poses_.push_back({pose.x, pose.y, pose.z});
        }
    }
}

TerrainRestampResult TerrainRestampContext::stamp_tile(const std::vector<float>& heights_base, f4f23ma::f32 rel_x,
                                                       f4f23ma::f32 rel_y, f4f23ma::f32 size_m) const {
    ensure_prepared();
    constexpr int dim = kTerrainDim;
    TerrainRestampResult r;
    r.heights = heights_base;
    spline::stamp_terrain_align(r.heights, size_m, rel_x, rel_y, *plan_, dim);
    if (static_cast<int>(r.heights.size()) < dim * dim) return r; // has_cut_data stays false

    collect_spline_align_hole_rings(r.polycut_rings, rel_x, rel_y, size_m, *plan_);
    spline::mask_terrain_align_holes(r.hole_mask, size_m, rel_x, rel_y, *plan_, dim);

    const float size = size_m > 0.01f ? size_m : 300.f;
    const float step = size / static_cast<float>(dim - 1);
    auto stamp_circle = [&](float wx, float wy, float wz, float radius_m = 14.f) {
        if (!(step > 0.f) || !(radius_m > 0.f)) return;
        const float r2 = radius_m * radius_m;
        // Only visit the grid cells touched by the circle, rather than all 61x61 vertices.
        const int x0 = (std::max)(0, static_cast<int>(std::floor((wx - radius_m - rel_x) / step)));
        const int x1 = (std::min)(dim - 1, static_cast<int>(std::ceil((wx + radius_m - rel_x) / step)));
        const int y0 = (std::max)(0, static_cast<int>(std::floor((wy - radius_m - rel_y) / step)));
        const int y1 = (std::min)(dim - 1, static_cast<int>(std::ceil((wy + radius_m - rel_y) / step)));
        if (x0 > x1 || y0 > y1) return;
        for (int yi = y0; yi <= y1; ++yi) {
            const float py = rel_y + static_cast<float>(yi) * step;
            const float dy = py - wy;
            for (int xi = x0; xi <= x1; ++xi) {
                const float px = rel_x + static_cast<float>(xi) * step;
                const float dx = px - wx;
                const float d2 = dx * dx + dy * dy;
                if (d2 > r2) continue;
                const float fall = 1.f - std::sqrt(d2) / radius_m;
                const float w = fall * fall;
                auto& h = r.heights[static_cast<std::size_t>(yi) * dim + static_cast<std::size_t>(xi)];
                h = h * (1.f - w) + wz * w;
            }
        }
    };
    auto overlaps_tile = [&](float wx, float wy, float radius) {
        const float dx = (std::max)(0.f, (std::max)(rel_x - wx, wx - (rel_x + size)));
        const float dy = (std::max)(0.f, (std::max)(rel_y - wy, wy - (rel_y + size)));
        return dx * dx + dy * dy <= radius * radius;
    };

    // [terrainhole] outline (OMSI editor .hole cache): cut the terrain along the outline and pull the
    // ground around it to the outline heights, like the edge of an AlignTerrain spline. Inside the
    // outline the (cut) grid takes the nearest outline height so relative-Z children and the cut-edge
    // triangles stay at the object's surface. No ground lift happens for these objects (terrain_add).
    auto stamp_outline = [&](const PreparedObject& o) {
        const float apron = std::clamp(step * 1.75f, 1.0f, 10.0f);
        const int x0 = (std::max)(0, static_cast<int>(std::floor((o.out_minx - apron - rel_x) / step)));
        const int x1 = (std::min)(dim - 1, static_cast<int>(std::ceil((o.out_maxx + apron - rel_x) / step)));
        const int y0 = (std::max)(0, static_cast<int>(std::floor((o.out_miny - apron - rel_y) / step)));
        const int y1 = (std::min)(dim - 1, static_cast<int>(std::ceil((o.out_maxy + apron - rel_y) / step)));
        if (x0 > x1 || y0 > y1) return;
        if (r.hole_mask.size() < static_cast<std::size_t>(dim * dim))
            r.hole_mask.assign(static_cast<std::size_t>(dim * dim), 0);
        for (int yi = y0; yi <= y1; ++yi) {
            const float py = rel_y + static_cast<float>(yi) * step;
            for (int xi = x0; xi <= x1; ++xi) {
                const float px = rel_x + static_cast<float>(xi) * step;
                float best_d2 = 1.e30f, best_h = o.z;
                bool inside = false;
                for (const auto& ring : o.outline) {
                    const std::size_t n = ring.size();
                    bool in_ring = false; // even-odd per ring; rings are united (polycut point_in_any)
                    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
                        const auto& a = ring[j];
                        const auto& b = ring[i];
                        if ((a[1] > py) != (b[1] > py) &&
                            px < (b[0] - a[0]) * (py - a[1]) / (b[1] - a[1]) + a[0])
                            in_ring = !in_ring;
                        const float ex = b[0] - a[0], ey = b[1] - a[1];
                        const float len2 = ex * ex + ey * ey;
                        float t = len2 > 1.e-12f ? ((px - a[0]) * ex + (py - a[1]) * ey) / len2 : 0.f;
                        t = std::clamp(t, 0.f, 1.f);
                        const float dx = a[0] + ex * t - px, dy = a[1] + ey * t - py;
                        const float d2 = dx * dx + dy * dy;
                        if (d2 < best_d2) {
                            best_d2 = d2;
                            best_h = a[2] + (b[2] - a[2]) * t;
                        }
                    }
                    inside = inside || in_ring;
                }
                float w = 1.f;
                if (!inside) {
                    const float d = std::sqrt(best_d2);
                    if (d >= apron) continue;
                    const float u = 1.f - d / apron;
                    w = u * u * (3.f - 2.f * u);
                }
                const std::size_t k = static_cast<std::size_t>(yi) * dim + static_cast<std::size_t>(xi);
                r.heights[k] = r.heights[k] * (1.f - w) + best_h * w;
                if (inside) r.hole_mask[k] = 1;
            }
        }
        for (const auto& ring : o.outline) {
            TerrainHoleRing hr;
            hr.x.reserve(ring.size());
            hr.y.reserve(ring.size());
            hr.z.reserve(ring.size());
            for (const auto& q : ring) {
                hr.x.push_back(q[0] - rel_x);
                hr.y.push_back(q[1] - rel_y);
                hr.z.push_back(q[2]);
            }
            r.polycut_rings.push_back(std::move(hr));
        }
    };

    // Objects first, then attachment children (already merged in that order by the snapshot builder).
    for (const auto& o : objects_) {
        if (!overlaps_tile(o.x, o.y, o.overlap_radius)) continue;
        if (!o.outline.empty()) {
            stamp_outline(o);
            continue;
        }
        stamp_circle(o.x, o.y, o.z);
        if (!o.has_hole) continue;
        append_object_align_hole_ring(r.polycut_rings, rel_x, rel_y, o.x, o.y, o.rot_z, o.half, o.half);
        spline::mask_object_terrain_align_holes(r.hole_mask, size_m, rel_x, rel_y, dim, o.x, o.y, o.rot_z, o.half,
                                                o.half);
    }
    for (const auto& pose : sa_poses_) stamp_circle(pose[0], pose[1], pose[2]);
    r.has_cut_data = true;
    return r;
}

void TerrainRestampQueue::submit(const std::shared_ptr<const TerrainRestampContext>& ctx,
                                 std::vector<TerrainRestampTileIn> tiles, TerrainRestampPostHook post) {
    if (!ctx || tiles.empty()) return;
    const auto gen = state_->generation.load(std::memory_order_acquire);
    state_->outstanding.fetch_add(tiles.size(), std::memory_order_acq_rel);
    auto shared_post = post ? std::make_shared<TerrainRestampPostHook>(std::move(post)) : nullptr;
    for (auto& tile : tiles) {
        auto in = std::make_shared<TerrainRestampTileIn>(std::move(tile));
        JobSystem::global().submit(
            [st = state_, ctx, in, shared_post, gen] {
                TerrainRestampDone d;
                d.key = in->key;
                d.token = in->token;
                try {
                    if (in->prestamped) {
                        d.result = *in->prestamped;
                        d.stamped = false;
                    } else {
                        d.result = ctx->stamp_tile(in->heights_base, in->rel_x, in->rel_y, in->size_m);
                        d.stamped = true;
                    }
                    if (shared_post) d.payload = (*shared_post)(*in, d.result);
                } catch (...) {
                    // A failed tile still has to be reported so outstanding() reaches zero.
                    d.payload.reset();
                }
                std::lock_guard<std::mutex> lk(st->mu);
                if (st->generation.load(std::memory_order_acquire) == gen) {
                    st->done.push_back(std::move(d));
                } else {
                    st->outstanding.fetch_sub(1, std::memory_order_acq_rel);
                }
            },
            JobSystem::Priority::Low);
    }
}

std::vector<TerrainRestampDone> TerrainRestampQueue::drain(std::size_t max_items) {
    std::vector<TerrainRestampDone> out;
    std::lock_guard<std::mutex> lk(state_->mu);
    const std::size_t n = (std::min)(max_items, state_->done.size());
    if (n == 0) return out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) out.push_back(std::move(state_->done[i]));
    state_->done.erase(state_->done.begin(), state_->done.begin() + static_cast<std::ptrdiff_t>(n));
    state_->outstanding.fetch_sub(n, std::memory_order_acq_rel);
    return out;
}

void TerrainRestampQueue::cancel_all() {
    std::lock_guard<std::mutex> lk(state_->mu);
    // Jobs already finished and waiting are dropped here; running ones notice the new generation.
    state_->outstanding.fetch_sub(state_->done.size(), std::memory_order_acq_rel);
    state_->done.clear();
    state_->generation.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace omsi::editor
