#include "omsi/spline_types.hpp"

#include "omsi/logger.hpp"
#include "omsi/vfs_overlay.hpp"
#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/cfg_document.hpp"

#include <mutex>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <numbers>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace omsi::spline {
namespace {

constexpr f4f23ma::f32 kPi = std::numbers::pi_v<f4f23ma::f32>;
constexpr f4f23ma::f32 kDeg2Rad = kPi / 180.f;

struct GradCoeffs {
    f4f23ma::f32 c = 0.f;
    f4f23ma::f32 a = 0.f;
    f4f23ma::f32 b = 0.f;
};

// OMSI TCoordSystem.SetGradient @ 0x005ab1b8 (_Unit84): steigungA/B/C in CoordSystem+0x14..0x1c.
// z(t) = c*t^3 + a*t^2 + b*t  (b = start_grad/100).
// Cubic when [spline_h]/use_deltaheight only. Mesh never passes chain_end_z (see spline_mesh_chain_end_z).
GradCoeffs gradient_coeffs(const map::SplineInst& sp, std::optional<f4f23ma::f32> chain_end_z) {
    GradCoeffs g{};
    const f4f23ma::f32 len = std::max(sp.length, 1.e-4f);
    g.b = sp.start_grad * 0.01f;

    f4f23ma::f32 delta = sp.delta_height;
    bool cubic = sp.use_delta_height || sp.is_spline_h;
    if (!cubic && chain_end_z) {
        delta = *chain_end_z - sp.position.z;
        cubic = true;
    }

    if (cubic) {
        f4f23ma::f32 c = (sp.end_grad - sp.start_grad) * 0.01f * len -
                         2.f * (delta - sp.start_grad * 0.01f * len);
        c /= (len * len * len);
        f4f23ma::f32 a = -(sp.end_grad - sp.start_grad) * 0.01f * len +
                         3.f * (delta - sp.start_grad * 0.01f * len);
        a /= (len * len);
        g.c = c;
        g.a = a;
    } else {
        g.c = 0.f;
        g.a = (sp.end_grad - sp.start_grad) * 0.01f / (2.f * len);
    }
    return g;
}

// Gradient tessellation:
// Primary (user/OMSI practice): segments depend ONLY on |Grad_Start - Grad_End|
//   Cantidad_Segmentos = floor(2.25 * |ΔGrad|) + 1
// Length only scales each segment: Longitud_Segmento = Largo / Cantidad_Segmentos
// Also keep a floor from the reverse-engineered cubic curvature so extreme S-curves
// never under-tessellate.
int omis_teile_grad(const map::SplineInst& sp, std::optional<f4f23ma::f32> chain_end_z) {
    const f4f23ma::f32 len = std::max(sp.length, 1.e-4f);
    const f4f23ma::f32 delta_grad = std::abs(sp.end_grad - sp.start_grad);
    const int from_delta = std::max(1, static_cast<int>(std::floor(2.25f * delta_grad)) + 1);

    const GradCoeffs g = gradient_coeffs(sp, chain_end_z);
    int from_curve = 1;
    if (std::abs(g.c) < 1.e-12f && std::abs(g.a) < 1.e-12f) {
        const f4f23ma::f32 steig_a = (sp.end_grad - sp.start_grad) * 0.01f / (2.f * len);
        if (std::abs(steig_a) >= 1.e-8f) {
            const f4f23ma::f32 denom = std::abs(2.f * steig_a);
            from_curve = std::max(1, static_cast<int>(std::trunc(1.f / std::max(denom, 1.e-8f))) + 1);
        }
    } else {
        const f4f23ma::f32 denom =
            std::abs(2.f * g.a) + std::abs(3.f * g.c * len + 2.f * g.a);
        from_curve = std::max(1, static_cast<int>(std::trunc(1.f / std::max(denom, 1.e-8f))) + 1);
    }
    return std::max(from_delta, from_curve);
}

// Unit84 patchwork: signed 1-based cell id per seglen step (neg = inverse V).
std::vector<int> expand_patchwork_cells(const PatchworkChain& pw) {
    std::vector<int> cells;
    if (!pw.valid || pw.factors.empty()) return cells;
    const std::size_t n = pw.factors.size();
    cells.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        const char ch = (i < pw.transitions.size()) ? pw.transitions[i] : 'A';
        int cell = static_cast<int>(std::toupper(static_cast<unsigned char>(ch)) - 'A') + 1;
        if (cell < 1) cell = 1;
        if (cell > static_cast<int>(n)) cell = static_cast<int>(n);
        int reps = 1;
        if (pw.factors[i] >= '0' && pw.factors[i] <= '9') reps = pw.factors[i] - '0';
        if (reps < 1) reps = 1;
        const bool inv = (i < pw.inverse.size() && pw.inverse[i] == '1');
        for (int r = 0; r < reps; ++r) cells.push_back(inv ? -cell : cell);
    }
    return cells;
}

// Centerline in tile Z-up — Unity/tt-viewer splineLocalAt (X/Z map → our X/Y).
void centerline_xy(const map::SplineInst& sp, f4f23ma::f32 along, f4f23ma::f32& out_x,
                   f4f23ma::f32& out_y, f4f23ma::f32& out_heading) {
    const f4f23ma::f32 rot = sp.heading_deg * kDeg2Rad;
    if (std::abs(sp.radius) < 1.e-6f) {
        out_x = sp.position.x + along * std::sin(rot);
        out_y = sp.position.y + along * std::cos(rot);
        out_heading = rot;
        return;
    }
    const f4f23ma::f32 theta = along / sp.radius;
    const f4f23ma::f32 x_temp = sp.radius * (1.f - std::cos(theta));
    const f4f23ma::f32 y_temp = sp.radius * std::sin(theta);
    out_x = sp.position.x + x_temp * std::cos(rot) + y_temp * std::sin(rot);
    out_y = sp.position.y - x_temp * std::sin(rot) + y_temp * std::cos(rot);
    out_heading = rot + theta;
}

} // namespace

namespace {
std::mutex g_tm_mu;
std::filesystem::path g_tm_ground;
f4f23ma::f32 g_tm_scale = 60.f;
std::unordered_map<std::wstring, bool> g_tm_cache; // resolved texture -> has [terrainmapping]

bool texture_has_terrain_mapping(const std::filesystem::path& sli, const std::filesystem::path& tex,
                                 const std::filesystem::path& omsi_root) {
    if (tex.empty()) return false;
    const auto r = gfx::dx12::resolve_texture_path(sli, tex.string(), omsi_root);
    if (!r) return false;
    std::wstring key = r->lexically_normal().native();
    for (auto& c : key) c = static_cast<wchar_t>(towlower(c));
    {
        std::lock_guard<std::mutex> lk(g_tm_mu);
        if (const auto it = g_tm_cache.find(key); it != g_tm_cache.end()) return it->second;
    }
    bool tm = false;
    const std::filesystem::path cfgp(r->wstring() + L".cfg");
    if (omsi::vfs::exists(cfgp)) {
        for (const auto& line : cfg::read_cfg_lines(cfgp)) {
            std::string t = line;
            while (!t.empty() && (t.back() == '\r' || t.back() == ' ' || t.back() == '\t')) t.pop_back();
            for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (t.rfind("[terrainmapping", 0) == 0) {
                tm = true;
                break;
            }
        }
    }
    std::lock_guard<std::mutex> lk(g_tm_mu);
    g_tm_cache[key] = tm;
    return tm;
}
} // namespace

void set_terrain_mapping_ground(const std::filesystem::path& ground_texture, float scale_m) {
    std::lock_guard<std::mutex> lk(g_tm_mu);
    g_tm_ground = ground_texture;
    g_tm_scale = scale_m > 0.01f ? scale_m : 60.f;
}

SplineSample evaluate_spline(const map::SplineInst& sp, f4f23ma::f32 along_m, f4f23ma::f32 lateral_m,
                             f4f23ma::f32 height_off, std::optional<f4f23ma::f32> chain_end_z,
                             const bool mirror_lateral, const f4f23ma::f32 half_cant_width) {
    SplineSample out{};
    const f4f23ma::f32 len = std::max(sp.length, 1.e-4f);
    const f4f23ma::f32 oy = along_m;
    f4f23ma::f32 ox = lateral_m;
    if (mirror_lateral && sp.mirror) ox = -ox;

    const GradCoeffs g = gradient_coeffs(sp, chain_end_z);
    const f4f23ma::f32 cant_delta = (sp.cant_end - sp.cant_start) * 0.01f / len;
    const f4f23ma::f32 cant0 = sp.cant_start * 0.01f;

    f4f23ma::f32 hx = 0.f;
    f4f23ma::f32 hy = 0.f;
    f4f23ma::f32 heading = 0.f;
    centerline_xy(sp, oy, hx, hy, heading);

    // Lateral: left of tangent (tt-viewer perpOffset sign, Z-up XY plane).
    const f4f23ma::f32 dir_x = std::sin(heading);
    const f4f23ma::f32 dir_y = std::cos(heading);
    const f4f23ma::f32 nx = dir_y;  // = cos
    const f4f23ma::f32 ny = -dir_x; // = -sin

    out.x = hx + nx * ox;
    out.y = hy + ny * ox;
    const f4f23ma::f32 cx = (std::clamp)(ox, -half_cant_width, half_cant_width);
    out.z = sp.position.z + ((g.c * oy + g.a) * oy + g.b) * oy + height_off - cx * (oy * cant_delta + cant0);
    out.heading_rad = heading;
    out.pitch_rad = std::atan((3.f * g.c * oy + 2.f * g.a) * oy + g.b + cx * cant_delta);
    out.cant_rad = std::atan(oy * cant_delta + cant0);
    return out;
}

int omis_teile(f4f23ma::f32 length, f4f23ma::f32 radius, int max_teilung) {
    map::SplineInst dummy{};
    dummy.length = length;
    dummy.radius = radius;
    return omis_teile(dummy, std::nullopt, max_teilung);
}

int omis_teile(const map::SplineInst& sp, std::optional<f4f23ma::f32> chain_end_z, int max_teilung) {
    // TSplineSegment.Generate (_Unit84 + Omsi.exe floats @ 0x5b24c4..e0):
    const f4f23ma::f32 len = std::max(sp.length, 0.f);
    int parts = std::max(1, static_cast<int>(std::trunc(len / 10.f)));
    parts = std::max(parts, omis_teile_grad(sp, chain_end_z));
    if (std::abs(sp.radius) > 1.e-6f) {
        const f4f23ma::f32 inv_s = 1.f / sp.radius;
        const f4f23ma::f32 inv_e = inv_s;
        const f4f23ma::f32 a = std::abs(2.f * inv_s);
        const f4f23ma::f32 b = std::abs(3.f * inv_e * len + 2.f * inv_s);
        const f4f23ma::f32 denom = a + b;
        const int from_inv = std::max(1, static_cast<int>(std::trunc(len * denom)) + 1);
        const f4f23ma::f32 ang = std::abs(len * inv_s);
        const int from_ang = std::max(1, static_cast<int>(std::trunc(ang / (kPi / 36.f))) + 1);
        parts = std::max(parts, std::max(from_inv, from_ang));
        if (len < 5.f) parts = std::max(parts, std::max(from_ang, 8));
    }
    if (max_teilung > 0) parts = std::min(parts, max_teilung);
    return std::max(1, parts);
}

std::vector<SplineMesh> build_spline_meshes(const map::SplineInst& sp, const SplineDefinition& def,
                                            const std::filesystem::path& omsi_root, f4f23ma::f32 tess_dist,
                                            std::optional<f4f23ma::f32> chain_end_z,
                                            SplineTerrainSampler sample_terrain) {
    (void)sample_terrain;
    std::vector<SplineMesh> out;
    if (def.profiles.empty() || sp.length <= 1.e-4f) return out;

    const f4f23ma::f32 len = sp.length;
    int segs = (tess_dist > 0.f)
                   ? std::max(1, static_cast<int>(std::ceil(len / std::max(tess_dist, 0.5f))))
                   : omis_teile(sp, chain_end_z);

    // Unit84: with patchwork, bump teile to a multiple of trunc(L/seglen)+1 groups.
    const bool have_pw = def.patchwork.valid && def.patchwork.transitions.size() >= 2 &&
                         !def.patchwork.factors.empty();
    int n_pw_groups = 1;
    if (have_pw) {
        n_pw_groups = static_cast<int>(std::trunc(len / std::max(def.patchwork.seg_length, 1.e-3f))) + 1;
        if (n_pw_groups < 1) n_pw_groups = 1;
        const int per = segs / n_pw_groups + 1;
        segs = std::max(n_pw_groups, per * n_pw_groups);
        const int max_t = 200;
        if (segs > max_t) {
            const int per2 = std::max(1, max_t / n_pw_groups);
            segs = per2 * n_pw_groups;
        }
    }
    // [patchwork_chain] (Omsi.exe TSplineSegment.Generate): the texture is cut lengthwise into `count`
    // pieces; transitions[i] / transitions[i+1] are the edge types at the start / end of piece i (pieces with
    // the same edge letter join seamlessly), factors[i] its weight (0 = never picked), inverse[i] = 1 lets it
    // be laid backwards. Segment by segment a piece starting with the current edge is picked at random by
    // weight (seeded per spline: the same every time); the last segment must end on the first edge letter so
    // the next spline joins. (Was: the letters read as fixed pieces and the digits as repeat counts.)
    const int pw_count = have_pw ? static_cast<int>((std::min)(def.patchwork.transitions.size() - 1,
                                                               def.patchwork.factors.size()))
                                 : 0;
    std::vector<int> pw_seq; // per segment: piece 1..count, negative = laid backwards
    if (have_pw && pw_count > 0) {
        const std::string& T = def.patchwork.transitions;
        const std::string& F = def.patchwork.factors;
        const std::string& I = def.patchwork.inverse;
        auto weight = [&](int i) -> int {
            const char c = i < static_cast<int>(F.size()) ? F[static_cast<std::size_t>(i)] : '0';
            return (c >= '0' && c <= '9') ? c - '0' : 0;
        };
        auto inv_ok = [&](int i) { return i < static_cast<int>(I.size()) && I[static_cast<std::size_t>(i)] == '1'; };
        auto at = [&](int i) { return T[static_cast<std::size_t>(i)]; };
        std::uint64_t seed = 0x9E3779B97F4A7C15ull ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sp.id)) << 1);
        auto rnd = [&seed]() {
            seed += 0x9E3779B97F4A7C15ull;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z ^= z >> 31;
            return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
        };
        char state = at(0);
        std::vector<int> cand;
        for (int g = 0; g < n_pw_groups; ++g) {
            cand.clear();
            if (g == n_pw_groups - 1) { // last: end on the first edge letter
                for (int i = 0; i < pw_count; ++i)
                    if (at(i) == state && at(i + 1) == at(0))
                        for (int w = weight(i); w > 0; --w) cand.push_back(i + 1);
                for (int i = 0; i < pw_count; ++i)
                    if (inv_ok(i) && at(i + 1) == state && at(i) == at(0))
                        for (int w = weight(i); w > 0; --w) cand.push_back(-(i + 1));
            }
            if (cand.empty()) {
                for (int k = -pw_count; k <= pw_count; ++k) {
                    if (k == 0) continue;
                    const int i = (k < 0 ? -k : k) - 1;
                    if (k < 0 ? (!inv_ok(i) || at(i + 1) != state) : at(i) != state) continue;
                    for (int w = weight(i); w > 0; --w) cand.push_back(k);
                }
            }
            int piece;
            if (cand.empty()) // ("Problems with patch work chain compilation"): any piece
                piece = (std::min)(pw_count, 1 + static_cast<int>(rnd() * pw_count));
            else
                piece = cand[(std::min)(cand.size() - 1, static_cast<std::size_t>(rnd() * static_cast<double>(cand.size())))];
            pw_seq.push_back(piece);
            state = piece > 0 ? at(piece) : at(-piece - 1);
        }
    }
    // half cant width (Omsi.exe 0x5adb3c): as far as the [heightprofile]s reach; none = the cant does nothing
    f4f23ma::f32 hcw = 0.f;
    {
        f4f23ma::f32 lo = 0.f, hi = 0.f;
        for (const auto& hp : def.height_profiles) {
            lo = (std::min)(lo, (std::min)(hp.left, hp.right));
            hi = (std::max)(hi, (std::max)(hp.left, hp.right));
        }
        hcw = (std::max)(-lo, hi);
    }

    for (std::size_t strip_i = 0; strip_i < def.profiles.size(); ++strip_i) {
        const auto& profile = def.profiles[strip_i];
        if (profile.size() < 2) continue;

        SplineMesh mesh{};
        const std::int32_t mat = profile.front().material_id;
        // Keep .sli texture token; Blender find_image_path resolves via .sli base file.
        if (mat >= 0 && static_cast<std::size_t>(mat) < def.textures.size()) {
            mesh.texture_path = def.textures[static_cast<std::size_t>(mat)];
            if (static_cast<std::size_t>(mat) < def.texture_alpha.size())
                mesh.alpha_mode = def.texture_alpha[static_cast<std::size_t>(mat)];
        } else if (!def.textures.empty()) {
            mesh.texture_path = def.textures.front();
            if (!def.texture_alpha.empty()) mesh.alpha_mode = def.texture_alpha.front();
        }
        // [terrainmapping]: the map's first ground texture with world UVs instead of the placeholder
        bool terrain_map = false;
        f4f23ma::f32 tm_scale = 60.f;
        if (!mesh.texture_path.empty() && texture_has_terrain_mapping(def.sli_path, mesh.texture_path, omsi_root)) {
            std::lock_guard<std::mutex> lk(g_tm_mu);
            if (!g_tm_ground.empty()) {
                mesh.texture_path = g_tm_ground;
                tm_scale = g_tm_scale;
                terrain_map = true;
            }
        }

        const bool use_pw = have_pw && mat == def.patchwork.material_index && pw_count > 0 &&
                            static_cast<int>(pw_seq.size()) == n_pw_groups;

        // Unit84 CreateVerticesAndIndices: if ((local_11 ^ 1) == mirror) { tri A } else { tri B }.
        // TSplineSegment.Generate always passes local_11=0 → flip only when mirror.
        // Profile X order sets strip vertex order; do NOT XOR profile_rev (double-flips decr-X faces).
        const bool profile_rev = profile.front().x > profile.back().x;
        const bool flip_winding = sp.mirror;
        const int n_pt = static_cast<int>(profile.size());
        mesh.vertices.reserve(static_cast<std::size_t>((segs + 1) * n_pt));

        auto emit_row = [&](int s, int pw_group, f4f23ma::f32 pw_local) {
            const f4f23ma::f32 along =
                (static_cast<f4f23ma::f32>(s) / static_cast<f4f23ma::f32>(segs)) * len;
            f4f23ma::f32 skew =
                sp.skew_start * (1.f - along / len) + sp.skew_end * (along / len);
            // openOMSI / OMSI clamp — extreme mitre blows up along-shift.
            skew = (std::clamp)(skew, -32.f, 32.f);

            for (int p = 0; p < n_pt; ++p) {
                const auto& pp = profile[static_cast<std::size_t>(p)];
                // Omsi.exe sub_5aaef4 / openOMSI skewed_point: the shift is skew * x with x ALREADY
                // mirrored. Using the unmirrored profile x turned the skew round on mirrored splines
                // (the mitre leaned the opposite way to OMSI's).
                const f4f23ma::f32 along_pt = along + skew * (sp.mirror ? -pp.x : pp.x);
                SplineSample samp = evaluate_spline(sp, along_pt, pp.x, pp.z, chain_end_z, true, hcw);

                o3d::Vertex v{};
                v.position[0] = samp.x;
                v.position[1] = samp.y;
                v.position[2] = samp.z;
                v.normal[0] = 0.f;
                v.normal[1] = 0.f;
                v.normal[2] = 1.f;
                v.uv[0] = pp.uv_x;
                if (use_pw) {
                    const int piece = pw_seq[static_cast<std::size_t>(pw_group)];
                    const int abs_cell = piece < 0 ? -piece : piece;
                    const f4f23ma::f32 local = piece < 0 ? 1.f - pw_local : pw_local;
                    v.uv[1] = (static_cast<f4f23ma::f32>(abs_cell - 1) + local) / static_cast<f4f23ma::f32>(pw_count);
                } else {
                    const f4f23ma::f32 tiling = pp.y_tiling != 0.f ? pp.y_tiling : 1.f;
                    v.uv[1] = along_pt * tiling;
                }
                if (sp.mirror && !use_pw) v.uv[1] = -v.uv[1]; // (a patchwork piece keeps its V range)
                if (terrain_map) {
                    v.uv[0] = samp.x / tm_scale;
                    v.uv[1] = samp.y / tm_scale;
                }
                mesh.vertices.push_back(v);
            }
        };
        int pw_per = 1;
        if (!use_pw) {
            for (int s = 0; s <= segs; ++s) emit_row(s, 0, 0.f);
        } else {
            pw_per = (std::max)(1, segs / n_pw_groups);
            for (int g = 0; g < n_pw_groups; ++g)
                for (int r = 0; r <= pw_per; ++r)
                    emit_row(g * pw_per + r, g, static_cast<f4f23ma::f32>(r) / static_cast<f4f23ma::f32>(pw_per));
        }
        const int n_rows = static_cast<int>(mesh.vertices.size()) / n_pt;
        for (int s = 0; s + 1 < n_rows; ++s) {
            if (use_pw && (s + 1) % (pw_per + 1) == 0) continue; // last row of a segment: the next row starts another
            for (int p = 0; p < n_pt - 1; ++p) {
                const std::uint32_t i0 = static_cast<std::uint32_t>(s * n_pt + p);
                const std::uint32_t i1 = static_cast<std::uint32_t>(s * n_pt + p + 1);
                const std::uint32_t i2 = static_cast<std::uint32_t>((s + 1) * n_pt + p);
                const std::uint32_t i3 = static_cast<std::uint32_t>((s + 1) * n_pt + p + 1);
                // OMSI (local_11=0): mirror → (i0,i3,i2)+(i1,i3,i0);
                // else → (i0,i2,i3)+(i1,i0,i3). Invert for our Z-up + FCCW cull.
                if (flip_winding) {
                    mesh.indices.insert(mesh.indices.end(), {i0, i2, i3, i1, i0, i3});
                } else {
                    mesh.indices.insert(mesh.indices.end(), {i0, i3, i2, i1, i3, i0});
                }
            }
        }

        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            auto& ia = mesh.indices[i];
            auto& ib = mesh.indices[i + 1];
            auto& ic = mesh.indices[i + 2];
            const auto& a = mesh.vertices[ia];
            const auto& b = mesh.vertices[ib];
            const auto& c = mesh.vertices[ic];
            const f4f23ma::f32 e1x = b.position[0] - a.position[0];
            const f4f23ma::f32 e1y = b.position[1] - a.position[1];
            const f4f23ma::f32 e1z = b.position[2] - a.position[2];
            const f4f23ma::f32 e2x = c.position[0] - a.position[0];
            const f4f23ma::f32 e2y = c.position[1] - a.position[1];
            const f4f23ma::f32 e2z = c.position[2] - a.position[2];
            f4f23ma::f32 nx = e1y * e2z - e1z * e2y;
            f4f23ma::f32 ny = e1z * e2x - e1x * e2z;
            f4f23ma::f32 nz = e1x * e2y - e1y * e2x;
            // Winding from profile_rev/mirror selects visible face — do not force +Z (Unit84).
            const f4f23ma::f32 inv = 1.f / std::max(1.e-8f, std::sqrt(nx * nx + ny * ny + nz * nz));
            nx *= inv;
            ny *= inv;
            nz *= inv;
            for (int k = 0; k < 3; ++k) {
                auto& vv = mesh.vertices[mesh.indices[i + static_cast<std::size_t>(k)]];
                vv.normal[0] = nx;
                vv.normal[1] = ny;
                vv.normal[2] = nz;
            }
        }

        if (!mesh.vertices.empty() && !mesh.indices.empty()) {
            out.push_back(std::move(mesh));
        }
    }

    return out;
}

std::optional<SplineMesh> build_path_rail_mesh(const map::SplineInst& parent, f4f23ma::f32 path_lateral,
                                               f4f23ma::f32 path_height, const SplineDefinition& path_def,
                                               f4f23ma::f32 z_bias,
                                               std::optional<f4f23ma::f32> chain_end_z) {
    if (path_def.profiles.empty() || parent.length <= 1.e-4f) return std::nullopt;
    const auto& profile = path_def.profiles.front();
    if (profile.size() < 2) return std::nullopt;

    const f4f23ma::f32 len = parent.length;
    const int segs = omis_teile(parent, chain_end_z);
    const int n_pt = static_cast<int>(profile.size());
    const bool flip_winding = parent.mirror;

    SplineMesh mesh{};
    if (!path_def.textures.empty()) {
        mesh.texture_path = path_def.textures.front();
        if (!path_def.texture_alpha.empty()) mesh.alpha_mode = path_def.texture_alpha.front();
    }

    mesh.vertices.reserve(static_cast<std::size_t>((segs + 1) * n_pt));
    for (int s = 0; s <= segs; ++s) {
        const f4f23ma::f32 along = (static_cast<f4f23ma::f32>(s) / static_cast<f4f23ma::f32>(segs)) * len;
        for (int p = 0; p < n_pt; ++p) {
            const auto& pp = profile[static_cast<std::size_t>(p)];
            const SplineSample samp =
                evaluate_spline(parent, along, path_lateral + pp.x, path_height + pp.z, chain_end_z);
            o3d::Vertex v{};
            v.position[0] = samp.x;
            v.position[1] = samp.y;
            v.position[2] = samp.z + z_bias;
            v.normal[0] = 0.f;
            v.normal[1] = 0.f;
            v.normal[2] = 1.f;
            v.uv[0] = pp.uv_x;
            const f4f23ma::f32 tiling = pp.y_tiling != 0.f ? pp.y_tiling : 0.1f;
            v.uv[1] = along * tiling;
            if (parent.mirror) v.uv[1] = -v.uv[1];
            mesh.vertices.push_back(v);
        }
    }

    for (int s = 0; s < segs; ++s) {
        for (int p = 0; p < n_pt - 1; ++p) {
            const std::uint32_t i0 = static_cast<std::uint32_t>(s * n_pt + p);
            const std::uint32_t i1 = static_cast<std::uint32_t>(s * n_pt + p + 1);
            const std::uint32_t i2 = static_cast<std::uint32_t>((s + 1) * n_pt + p);
            const std::uint32_t i3 = static_cast<std::uint32_t>((s + 1) * n_pt + p + 1);
            if (flip_winding) {
                mesh.indices.insert(mesh.indices.end(), {i0, i2, i3, i1, i0, i3});
            } else {
                mesh.indices.insert(mesh.indices.end(), {i0, i3, i2, i1, i3, i0});
            }
        }
    }

    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        auto& ia = mesh.indices[i];
        auto& ib = mesh.indices[i + 1];
        auto& ic = mesh.indices[i + 2];
        const auto& a = mesh.vertices[ia];
        const auto& b = mesh.vertices[ib];
        const auto& c = mesh.vertices[ic];
        const f4f23ma::f32 e1x = b.position[0] - a.position[0];
        const f4f23ma::f32 e1y = b.position[1] - a.position[1];
        const f4f23ma::f32 e1z = b.position[2] - a.position[2];
        const f4f23ma::f32 e2x = c.position[0] - a.position[0];
        const f4f23ma::f32 e2y = c.position[1] - a.position[1];
        const f4f23ma::f32 e2z = c.position[2] - a.position[2];
        f4f23ma::f32 nx = e1y * e2z - e1z * e2y;
        f4f23ma::f32 ny = e1z * e2x - e1x * e2z;
        f4f23ma::f32 nz = e1x * e2y - e1y * e2x;
        const f4f23ma::f32 inv = 1.f / std::max(1.e-8f, std::sqrt(nx * nx + ny * ny + nz * nz));
        nx *= inv;
        ny *= inv;
        nz *= inv;
        for (int k = 0; k < 3; ++k) {
            auto& vv = mesh.vertices[mesh.indices[i + static_cast<std::size_t>(k)]];
            vv.normal[0] = nx;
            vv.normal[1] = ny;
            vv.normal[2] = nz;
        }
    }

    if (mesh.vertices.empty() || mesh.indices.empty()) return std::nullopt;
    return mesh;
}

void fit_spline_to_xy(f4f23ma::f32 ox, f4f23ma::f32 oy, f4f23ma::f32 heading_deg, f4f23ma::f32 tx,
                      f4f23ma::f32 ty, f4f23ma::f32& out_len, f4f23ma::f32& out_radius) {
    const f4f23ma::f32 rot = heading_deg * kDeg2Rad;
    const f4f23ma::f32 dx = tx - ox;
    const f4f23ma::f32 dy = ty - oy;
    const f4f23ma::f32 local_x = dx * std::cos(rot) - dy * std::sin(rot);
    const f4f23ma::f32 local_y = dx * std::sin(rot) + dy * std::cos(rot);
    out_radius = 0.f;
    if (std::fabs(local_x) < 0.02f) {
        out_len = (std::max)(0.05f, local_y);
        if (out_len < 0.05f) out_len = (std::max)(0.05f, std::sqrt(dx * dx + dy * dy));
        return;
    }
    out_radius = (local_x * local_x + local_y * local_y) / (2.f * local_x);
    if (std::fabs(out_radius) < 0.05f) {
        out_radius = 0.f;
        out_len = (std::max)(0.05f, local_y);
        return;
    }
    const f4f23ma::f32 cos_th = 1.f - local_x / out_radius;
    const f4f23ma::f32 sin_th = local_y / out_radius;
    const f4f23ma::f32 theta = std::atan2(sin_th, cos_th);
    out_len = out_radius * theta;
    if (out_len < 0.25f) {
        out_len = (std::max)(0.25f, std::fabs(out_radius * theta));
        if (out_len < 0.25f) {
            out_radius = 0.f;
            out_len = (std::max)(0.5f, std::sqrt(dx * dx + dy * dy));
        }
    }
}

bool fit_spline_to_end_heading(f4f23ma::f32 x0, f4f23ma::f32 y0, f4f23ma::f32 h0_deg, f4f23ma::f32 x1,
                               f4f23ma::f32 y1, f4f23ma::f32 h1_deg, f4f23ma::f32& out_len,
                               f4f23ma::f32& out_radius) {
    auto wrap180 = [](f4f23ma::f32 a) {
        while (a > 180.f) a -= 360.f;
        while (a < -180.f) a += 360.f;
        return a;
    };
    const f4f23ma::f32 dx = x1 - x0;
    const f4f23ma::f32 dy = y1 - y0;
    const f4f23ma::f32 chord = std::sqrt(dx * dx + dy * dy);
    if (chord < 0.05f) return false;
    const f4f23ma::f32 th = wrap180(h1_deg - h0_deg) * kDeg2Rad;
    const f4f23ma::f32 rot = h0_deg * kDeg2Rad;
    if (std::fabs(th) < 1.e-4f) {
        const f4f23ma::f32 fx = std::sin(rot);
        const f4f23ma::f32 fy = std::cos(rot);
        out_len = (std::max)(0.5f, dx * fx + dy * fy);
        if (out_len < 0.5f) out_len = chord;
        out_radius = 0.f;
        return true;
    }
    const f4f23ma::f32 ct = std::cos(th);
    const f4f23ma::f32 st = std::sin(th);
    const f4f23ma::f32 cx = std::cos(rot);
    const f4f23ma::f32 sx = std::sin(rot);
    const f4f23ma::f32 vx = (1.f - ct) * cx + st * sx;
    const f4f23ma::f32 vy = -(1.f - ct) * sx + st * cx;
    const f4f23ma::f32 den = vx * vx + vy * vy;
    if (den < 1.e-10f) return false;
    out_radius = (dx * vx + dy * vy) / den;
    out_len = out_radius * th;
    if (out_len < 0.05f) return false;
    return std::isfinite(out_len) && std::isfinite(out_radius);
}

f4f23ma::f32 spline_mesh_world_min_z(const map::SplineInst& sp, const SplineDefinition& def,
                                     const std::filesystem::path& omsi_root,
                                     std::optional<f4f23ma::f32> chain_end_z) {
    const auto meshes = build_spline_meshes(sp, def, omsi_root, 0.f, chain_end_z);
    f4f23ma::f32 min_z = sp.position.z;
    for (const auto& sm : meshes) {
        for (const auto& v : sm.vertices) min_z = (std::min)(min_z, v.position[2]);
    }
    return min_z;
}

namespace {

// Inverse of the OMSI segment's XY path without clipping the longitudinal coordinate.
// Terrain alignment needs samples slightly beyond the segment ends because synthetic
// [terrainholeprofile] points can carry a negative end-offset (usually -0.5 m).
bool world_xy_to_spline_local_raw(const map::SplineInst& sp, f4f23ma::f32 wx, f4f23ma::f32 wy,
                                  f4f23ma::f32& along, f4f23ma::f32& lateral) {
    const f4f23ma::f32 rot = sp.heading_deg * kDeg2Rad;
    const f4f23ma::f32 dx = wx - sp.position.x;
    const f4f23ma::f32 dy = wy - sp.position.y;
    if (std::abs(sp.radius) < 1.e-6f) {
        const f4f23ma::f32 fx = std::sin(rot);
        const f4f23ma::f32 fy = std::cos(rot);
        along = dx * fx + dy * fy;
        lateral = dx * fy + dy * (-fx);
        if (sp.mirror) lateral = -lateral;
        return std::isfinite(along) && std::isfinite(lateral);
    }
    const f4f23ma::f32 x_temp = dx * std::cos(rot) + dy * (-std::sin(rot));
    const f4f23ma::f32 y_temp = dx * std::sin(rot) + dy * std::cos(rot);
    const f4f23ma::f32 r = sp.radius;
    const f4f23ma::f32 cos_th = 1.f - x_temp / r;
    const f4f23ma::f32 sin_th = y_temp / r;
    const f4f23ma::f32 th = std::atan2(sin_th, cos_th);
    along = th * r;
    f4f23ma::f32 cx = 0.f, cy = 0.f, heading = 0.f;
    // Do not clamp here: OMSI shifts the lateral profile points along the spline by
    // skew*x, so their true centreline station can sit just outside [0, length].
    centerline_xy(sp, along, cx, cy, heading);
    const f4f23ma::f32 nx = std::cos(heading);
    const f4f23ma::f32 ny = -std::sin(heading);
    lateral = (wx - cx) * nx + (wy - cy) * ny;
    if (sp.mirror) lateral = -lateral;
    return std::isfinite(along) && std::isfinite(lateral);
}

} // namespace

bool world_xy_to_spline_local(const map::SplineInst& sp, f4f23ma::f32 wx, f4f23ma::f32 wy,
                              f4f23ma::f32& along, f4f23ma::f32& lateral) {
    if (!world_xy_to_spline_local_raw(sp, wx, wy, along, lateral)) return false;
    const f4f23ma::f32 len = std::max(sp.length, 1.e-4f);
    return along >= -0.35f && along <= len + 0.35f;
}

namespace {

// The third field of [terrainholeprofilepnt] is an end offset, not a height.
// Keep points together per ring; flattening several rings into one x/z array causes
// unrelated cross-sections to overwrite one another when a .sli contains multiple cuts.
std::vector<HoleProfilePoint> sorted_hole_profile(const std::vector<HoleProfilePoint>& ring) {
    std::vector<HoleProfilePoint> points;
    points.reserve(ring.size());
    for (const auto& p : ring) {
        if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) points.push_back(p);
    }
    std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
    return points;
}

bool hole_profile_limits(const std::vector<HoleProfilePoint>& profile, f4f23ma::f32& xmin,
                         f4f23ma::f32& xmax) {
    if (profile.size() < 2) return false;
    xmin = profile.front().x;
    xmax = profile.back().x;
    return std::isfinite(xmin) && std::isfinite(xmax) && xmax > xmin + 1.e-3f;
}

// Interpolate the actual [terrainholeprofilepnt] columns: y = vertical offset,
// z = longitudinal start/end offset. The caller passes points sorted by x.
void sample_hole_profile(const std::vector<HoleProfilePoint>& profile, f4f23ma::f32 lateral,
                         f4f23ma::f32& height_off, f4f23ma::f32& end_off) {
    if (profile.empty()) {
        height_off = 0.f;
        end_off = 0.f;
        return;
    }
    if (lateral <= profile.front().x) {
        height_off = profile.front().y;
        end_off = profile.front().z;
        return;
    }
    if (lateral >= profile.back().x) {
        height_off = profile.back().y;
        end_off = profile.back().z;
        return;
    }
    for (std::size_t i = 0; i + 1 < profile.size(); ++i) {
        const auto& a = profile[i];
        const auto& b = profile[i + 1];
        if (lateral > b.x) continue;
        const f4f23ma::f32 width = b.x - a.x;
        const f4f23ma::f32 t = std::abs(width) > 1.e-8f ? (lateral - a.x) / width : 0.f;
        height_off = a.y + (b.y - a.y) * t;
        end_off = a.z + (b.z - a.z) * t;
        return;
    }
    height_off = profile.back().y;
    end_off = profile.back().z;
}

f4f23ma::f32 spline_skew_at(const map::SplineInst& sp, f4f23ma::f32 station) {
    const f4f23ma::f32 len = std::max(sp.length, 1.e-4f);
    const f4f23ma::f32 t = std::clamp(station / len, 0.f, 1.f);
    const f4f23ma::f32 raw = sp.skew_start * (1.f - t) + sp.skew_end * t;
    return std::clamp(raw, -32.f, 32.f); // Same clamp as TSplineSegment.Generate / build_spline_meshes.
}

f4f23ma::f32 spline_actual_along(const map::SplineInst& sp, f4f23ma::f32 station,
                                 f4f23ma::f32 profile_lateral) {
    // The mesh generator shifts each profile vertex longitudinally by skew * x, x mirrored like the
    // lateral offset (Omsi.exe sub_5aaef4). profile_lateral is the unmirrored profile x.
    const f4f23ma::f32 x = sp.mirror ? -profile_lateral : profile_lateral;
    return station + spline_skew_at(sp, station) * x;
}

f4f23ma::f32 spline_station_from_actual_along(const map::SplineInst& sp, f4f23ma::f32 actual_along,
                                              f4f23ma::f32 profile_lateral_in) {
    // same mirrored x as spline_actual_along / the mesh generator
    const f4f23ma::f32 profile_lateral = sp.mirror ? -profile_lateral_in : profile_lateral_in;
    const f4f23ma::f32 len = std::max(sp.length, 1.e-4f);
    const f4f23ma::f32 slope = (sp.skew_end - sp.skew_start) / len;
    const f4f23ma::f32 denom = 1.f + slope * profile_lateral;

    // Exact solution while the interpolated skew is inside OMSI's +/-32 m clamp.
    if (std::abs(denom) > 1.e-5f) {
        const f4f23ma::f32 candidate = (actual_along - sp.skew_start * profile_lateral) / denom;
        if (candidate >= 0.f && candidate <= len) {
            const f4f23ma::f32 raw = sp.skew_start + slope * candidate;
            if (raw >= -32.f && raw <= 32.f) return candidate;
        }
    }

    // Exact solutions on the two saturated skew ranges.
    const f4f23ma::f32 pos_sat = actual_along - 32.f * profile_lateral;
    if (pos_sat >= 0.f && pos_sat <= len && sp.skew_start + slope * pos_sat >= 32.f) return pos_sat;
    const f4f23ma::f32 neg_sat = actual_along + 32.f * profile_lateral;
    if (neg_sat >= 0.f && neg_sat <= len && sp.skew_start + slope * neg_sat <= -32.f) return neg_sat;

    // The synthetic hole rim may extend beyond either endpoint. Outside the segment,
    // the mesh uses the endpoint skew rather than extrapolating the skew gradient.
    const f4f23ma::f32 before = actual_along - spline_skew_at(sp, 0.f) * profile_lateral;
    if (before < 0.f) return before;
    const f4f23ma::f32 after = actual_along - spline_skew_at(sp, len) * profile_lateral;
    if (after > len) return after;

    // Numerically safe fallback for near-singular combinations of lateral distance and skew.
    f4f23ma::f32 station = std::clamp(actual_along, 0.f, len);
    for (int i = 0; i < 10; ++i) {
        const f4f23ma::f32 raw = sp.skew_start + slope * station;
        const f4f23ma::f32 skew = std::clamp(raw, -32.f, 32.f);
        const f4f23ma::f32 f = station + skew * profile_lateral - actual_along;
        const f4f23ma::f32 derivative = (raw > -32.f && raw < 32.f) ? 1.f + slope * profile_lateral : 1.f;
        if (std::abs(derivative) < 1.e-4f) break;
        const f4f23ma::f32 next = std::clamp(station - f / derivative, -32.f, len + 32.f);
        if (std::abs(next - station) < 1.e-4f) return next;
        station = next;
    }
    return station;
}

f4f23ma::f32 hole_start_at(const map::SplineInst& sp, int mode, f4f23ma::f32 end_off) {
    (void)sp;
    return mode > 2 ? 0.f : end_off;
}

f4f23ma::f32 hole_end_at(const map::SplineInst& sp, int mode, f4f23ma::f32 end_off) {
    const f4f23ma::f32 len = std::max(sp.length, 1.e-4f);
    return ((mode & 1) == 0) ? len : (len - end_off);
}

} // namespace

std::vector<std::vector<HoleProfilePoint>> effective_terrain_hole_profiles(const SplineDefinition& def) {
    if (!def.hole_profiles.empty()) return def.hole_profiles;
    // OMSI _Unit84 @ 0x5adc52 / openOMSI terrain_hole_profiles: chain [profile]s that meet
    // within 1 cm; trough 3 cm in from ends, 3 mm below edges, bottom 10 cm under lowest,
    // ends stand 0.5 m past the spline (z on HoleProfilePoint).
    constexpr f4f23ma::f32 kJoin = 0.01f;
    constexpr f4f23ma::f32 kInset = 0.03f;
    const auto& profiles = def.profiles;
    if (profiles.empty()) return {};
    std::vector<char> used(profiles.size(), 0);
    for (std::size_t j = 0; j < profiles.size(); ++j)
        if (profiles[j].empty()) used[j] = 1;
    std::vector<std::vector<HoleProfilePoint>> out;
    while (true) {
        std::size_t start = profiles.size();
        for (std::size_t j = 0; j < profiles.size(); ++j) {
            if (!used[j]) {
                start = j;
                break;
            }
        }
        if (start >= profiles.size()) break;
        std::vector<std::size_t> chain;
        for (;;) {
            std::size_t next = profiles.size();
            for (std::size_t j = 0; j < profiles.size(); ++j) {
                if (used[j]) continue;
                if (chain.empty()) {
                    next = j;
                    break;
                }
                if (profiles[j].empty() || profiles[chain.back()].empty()) continue;
                const auto& a = profiles[chain.back()].back();
                const auto& b = profiles[j].front();
                if (std::abs(a.x - b.x) < kJoin && std::abs(a.z - b.z) < kJoin) {
                    next = j;
                    break;
                }
            }
            if (next >= profiles.size()) break;
            used[next] = 1;
            chain.push_back(next);
        }
        if (chain.empty() || profiles[chain[0]].empty()) continue;
        // Left edge = first profile's first point (OMSI quirk: min-scan then re-take first).
        const f4f23ma::f32 xl = profiles[chain[0]].front().x;
        const f4f23ma::f32 zl = profiles[chain[0]].front().z;
        f4f23ma::f32 xr = profiles[chain[0]].back().x;
        f4f23ma::f32 zr = profiles[chain[0]].back().z;
        for (std::size_t ci = 1; ci < chain.size(); ++ci) {
            const auto& last = profiles[chain[ci]].back();
            if (last.x > xr) {
                xr = last.x;
                zr = last.z;
            }
        }
        if (xl + kInset >= xr - kInset) continue; // thin wire / zero-width → skip
        f4f23ma::f32 low = std::min(zl, zr);
        for (std::size_t j : chain)
            for (const auto& p : profiles[j]) low = std::min(low, p.z);
        f4f23ma::f32 bottom_left = (xr - xl) * 0.25f + xl;
        if (low < zl) {
            bottom_left = xr;
            for (std::size_t j : chain) {
                for (const auto& p : profiles[j]) {
                    if (zl <= p.z) continue;
                    const f4f23ma::f32 den = zl - p.z;
                    if (std::abs(den) < 1.e-8f) continue;
                    bottom_left = std::min(bottom_left, (p.x - xl) / den * (zl - low) + xl);
                }
            }
        }
        f4f23ma::f32 bottom_right = (xr - xl) * 0.75f + xl;
        if (low < zr) {
            bottom_right = xl;
            for (std::size_t j : chain) {
                for (const auto& p : profiles[j]) {
                    if (zr <= p.z) continue;
                    const f4f23ma::f32 den = zr - p.z;
                    if (std::abs(den) < 1.e-8f) continue;
                    bottom_right = std::max(bottom_right, xr - (xr - p.x) / den * (zr - low));
                }
            }
        }
        low -= 0.1f;
        out.push_back({
            HoleProfilePoint{xl + kInset, zl - 0.003f, 0.f},
            HoleProfilePoint{bottom_left, low, -0.5f},
            HoleProfilePoint{bottom_right, low, -0.5f},
            HoleProfilePoint{xr - kInset, zr - 0.003f, 0.f},
        });
    }
    return out;
}

std::shared_ptr<const TerrainAlignPlan> build_terrain_align_plan(const std::vector<map::SplineInst>& splines) {
    auto plan = std::make_shared<TerrainAlignPlan>();
    plan->splines = splines; // snapshot; entries point into this vector, which is never resized again

    std::unordered_map<std::int32_t, f4f23ma::f32> spline_z;
    spline_z.reserve(plan->splines.size() * 2 + 1);
    for (const auto& candidate : plan->splines) spline_z[candidate.id] = candidate.position.z;

    // One compiled profile set per distinct .sli (hundreds of splines usually share a handful).
    // nullptr is cached too, so a missing/broken .sli is probed once per plan, not once per tile.
    std::unordered_map<std::filesystem::path::string_type, std::shared_ptr<const TerrainAlignCompiled>> by_path;
    constexpr f4f23ma::f32 kMaxApron = 10.f; // upper bound of clamp(step * 1.75, 1, 10) in stamp_terrain_align

    plan->entries.reserve(plan->splines.size());
    for (const auto& sp : plan->splines) {
        if (sp.terrain_align == 0 || sp.length <= 1.e-4f) continue;
        const auto& key = sp.sli_path.native();
        auto it = by_path.find(key);
        if (it == by_path.end()) {
            std::shared_ptr<const TerrainAlignCompiled> compiled;
            if (const auto def = load_sli_shared(sp.sli_path)) {
                auto c = std::make_shared<TerrainAlignCompiled>();
                c->def = def;
                c->raw_rings = effective_terrain_hole_profiles(*def);
                if (!c->raw_rings.empty()) {
                    c->rings.reserve(c->raw_rings.size());
                    for (const auto& raw : c->raw_rings) {
                        TerrainAlignRing ring;
                        ring.sorted = sorted_hole_profile(raw);
                        ring.valid = hole_profile_limits(ring.sorted, ring.xmin, ring.xmax);
                        c->rings.push_back(std::move(ring));
                        for (const auto& pt : raw) {
                            if (std::isfinite(pt.x)) c->lateral_extent = (std::max)(c->lateral_extent, std::fabs(pt.x));
                            if (std::isfinite(pt.z)) c->end_extent = (std::max)(c->end_extent, std::fabs(pt.z));
                        }
                    }
                    compiled = std::move(c);
                }
            }
            it = by_path.emplace(key, std::move(compiled)).first;
        }
        if (!it->second) continue;

        TerrainAlignEntry e;
        e.sp = &sp;
        e.comp = it->second;
        e.chain_end_z = resolve_spline_mesh_chain_end_z(sp, spline_z);
        const f4f23ma::f32 reach = (std::max)(0.f, sp.length) + std::fabs(sp.local_length) +
                                   std::fabs(sp.skew_start) + std::fabs(sp.skew_end) + e.comp->lateral_extent +
                                   (std::max)(kMaxApron, e.comp->end_extent) + 1.f;
        e.min_x = sp.position.x - reach;
        e.max_x = sp.position.x + reach;
        e.min_y = sp.position.y - reach;
        e.max_y = sp.position.y + reach;
        plan->entries.push_back(std::move(e));
    }
    return plan;
}

void stamp_terrain_align(std::vector<float>& heights, f4f23ma::f32 size_m, f4f23ma::f32 tile_rel_x,
                         f4f23ma::f32 tile_rel_y, const std::vector<map::SplineInst>& splines, int dim) {
    const auto plan = build_terrain_align_plan(splines);
    stamp_terrain_align(heights, size_m, tile_rel_x, tile_rel_y, *plan, dim);
}

void mask_terrain_align_holes(std::vector<std::uint8_t>& mask, f4f23ma::f32 size_m, f4f23ma::f32 tile_rel_x,
                              f4f23ma::f32 tile_rel_y, const std::vector<map::SplineInst>& splines, int dim) {
    const auto plan = build_terrain_align_plan(splines);
    mask_terrain_align_holes(mask, size_m, tile_rel_x, tile_rel_y, *plan, dim);
}

void stamp_terrain_align(std::vector<float>& heights, f4f23ma::f32 size_m, f4f23ma::f32 tile_rel_x,
                         f4f23ma::f32 tile_rel_y, const TerrainAlignPlan& plan, int dim) {
    if (dim < 2 || heights.size() < static_cast<std::size_t>(dim * dim)) return;
    const f4f23ma::f32 size = size_m > 0.01f ? size_m : 300.f;
    const f4f23ma::f32 step = size / static_cast<f4f23ma::f32>(dim - 1);
    const std::size_t n = static_cast<std::size_t>(dim) * static_cast<std::size_t>(dim);
    const std::vector<float> terrain_base = heights;
    std::vector<std::optional<f4f23ma::f32>> aligned(n);
    std::vector<f4f23ma::f32> apron_weight(n, 0.f);
    std::vector<f4f23ma::f32> apron_height(n, 0.f);

    // chain_end_z (same chained end elevation as OMSI's spline mesh) is precomputed per spline in
    // the plan, so no spline_id -> z map is rebuilt for every tile any more.

    // Terrain tiles are sampled on a coarse regular grid. Blend a modest apron at least
    // one grid interval wide so the triangles bordering the exact spline cut do not stay
    // at a completely unrelated height. Capping the apron keeps very coarse tiles sane.
    const f4f23ma::f32 apron = std::clamp(step * 1.75f, 1.0f, 10.0f);

    for (const auto& entry : plan.entries) {
        if (!terrain_align_entry_may_touch(entry, tile_rel_x, tile_rel_y, size)) continue;
        const auto& sp = *entry.sp;
        const int mode = static_cast<int>(sp.terrain_align);
        const auto& chain_end_z = entry.chain_end_z;

        for (const auto& compiled_ring : entry.comp->rings) {
            if (!compiled_ring.valid) continue;
            const auto& profile = compiled_ring.sorted;
            const f4f23ma::f32 xmin = compiled_ring.xmin;
            const f4f23ma::f32 xmax = compiled_ring.xmax;

            // Conservative broad-phase rejection: every point along a spline is at most
            // its arc length from the start point; the profile adds only its lateral extent.
            // This avoids testing all dim*dim terrain samples for splines nowhere near this tile.
            const f4f23ma::f32 lateral_extent = (std::max)(std::fabs(xmin), std::fabs(xmax));
            const f4f23ma::f32 dx_tile = (std::max)(0.f, (std::max)(tile_rel_x - sp.position.x,
                                                                      sp.position.x - (tile_rel_x + size)));
            const f4f23ma::f32 dy_tile = (std::max)(0.f, (std::max)(tile_rel_y - sp.position.y,
                                                                      sp.position.y - (tile_rel_y + size)));
            const f4f23ma::f32 reach = (std::max)(0.f, sp.length) + std::fabs(sp.local_length) +
                                       std::fabs(sp.skew_start) + std::fabs(sp.skew_end) + lateral_extent + apron;
            if (dx_tile * dx_tile + dy_tile * dy_tile > reach * reach) continue;

            for (int yi = 0; yi < dim; ++yi) {
                for (int xi = 0; xi < dim; ++xi) {
                    const f4f23ma::f32 wx = tile_rel_x + static_cast<f4f23ma::f32>(xi) * step;
                    const f4f23ma::f32 wy = tile_rel_y + static_cast<f4f23ma::f32>(yi) * step;
                    f4f23ma::f32 actual_along = 0.f, lat = 0.f;
                    if (!world_xy_to_spline_local_raw(sp, wx, wy, actual_along, lat)) continue;

                    const f4f23ma::f32 station = spline_station_from_actual_along(sp, actual_along, lat);
                    const f4f23ma::f32 edge_lat = std::clamp(lat, xmin, xmax);
                    // With skew, projecting the world point onto the profile edge changes
                    // its spline station too. Re-invert at the clamped profile lateral rather
                    // than reusing the station computed at an outside lateral coordinate.
                    const f4f23ma::f32 edge_station =
                        (edge_lat == lat) ? station : spline_station_from_actual_along(sp, actual_along, edge_lat);
                    f4f23ma::f32 height_off = 0.f, end_off = 0.f;
                    sample_hole_profile(profile, edge_lat, height_off, end_off);
                    f4f23ma::f32 s0 = hole_start_at(sp, mode, end_off);
                    f4f23ma::f32 s1 = hole_end_at(sp, mode, end_off);
                    if (s1 < s0) std::swap(s0, s1);

                    const bool inside_lateral = lat >= xmin && lat <= xmax;
                    const bool inside_longitudinal = station >= s0 && station <= s1;
                    const std::size_t k = static_cast<std::size_t>(yi) * static_cast<std::size_t>(dim) +
                                          static_cast<std::size_t>(xi);
                    if (inside_lateral && inside_longitudinal) {
                        // Match the same station + skew*lateral transform used when OMSI
                        // creates each spline mesh cross-section. evaluate_spline supplies
                        // radius, start/end gradients, delta_h and cant in Z.
                        const f4f23ma::f32 surface_along = spline_actual_along(sp, station, lat);
                        const auto sample = evaluate_spline(sp, surface_along, lat, height_off, chain_end_z);
                        if (std::isfinite(sample.z) && (!aligned[k] || sample.z > *aligned[k])) aligned[k] = sample.z;
                        continue;
                    }

                    // Closest point on this cross-section strip, including the per-point
                    // start/end offset. This adds a continuous height skirt at the sides
                    // and at both ends, rather than a grid-dependent nearest-neighbour ring.
                    const f4f23ma::f32 nearest_station = std::clamp(edge_station, s0, s1);
                    const f4f23ma::f32 d_lat = lat - edge_lat;
                    const f4f23ma::f32 d_station = edge_station - nearest_station;
                    const f4f23ma::f32 dist = std::sqrt(d_lat * d_lat + d_station * d_station);
                    if (dist >= apron) continue;
                    const f4f23ma::f32 u = std::clamp(1.f - dist / apron, 0.f, 1.f);
                    const f4f23ma::f32 weight = u * u * (3.f - 2.f * u);
                    if (weight <= apron_weight[k]) continue;
                    const f4f23ma::f32 surface_along = spline_actual_along(sp, nearest_station, edge_lat);
                    const auto sample = evaluate_spline(sp, surface_along, edge_lat, height_off, chain_end_z);
                    if (!std::isfinite(sample.z)) continue;
                    apron_weight[k] = weight;
                    apron_height[k] = sample.z;
                }
            }
        }
    }

    // Core of every aligned strip follows the spline profile exactly. Use the untouched
    // terrain values as the blend source so overlapping splines cannot progressively warp it.
    for (std::size_t k = 0; k < n; ++k) {
        if (aligned[k]) {
            heights[k] = *aligned[k];
        } else if (apron_weight[k] > 0.f) {
            heights[k] = terrain_base[k] + (apron_height[k] - terrain_base[k]) * apron_weight[k];
        }
    }
}

void mask_terrain_align_holes(std::vector<std::uint8_t>& mask, f4f23ma::f32 size_m, f4f23ma::f32 tile_rel_x,
                              f4f23ma::f32 tile_rel_y, const TerrainAlignPlan& plan, int dim) {
    if (dim < 2) return;
    mask.assign(static_cast<std::size_t>(dim * dim), 0);
    const f4f23ma::f32 size = size_m > 0.01f ? size_m : 300.f;
    const f4f23ma::f32 step = size / static_cast<f4f23ma::f32>(dim - 1);
    for (const auto& entry : plan.entries) {
        if (!terrain_align_entry_may_touch(entry, tile_rel_x, tile_rel_y, size)) continue;
        const auto& sp = *entry.sp;
        const int mode = static_cast<int>(sp.terrain_align);
        for (const auto& compiled_ring : entry.comp->rings) {
            if (!compiled_ring.valid) continue;
            const auto& profile = compiled_ring.sorted;
            const f4f23ma::f32 xmin = compiled_ring.xmin;
            const f4f23ma::f32 xmax = compiled_ring.xmax;

            // Same conservative broad phase as stamp_terrain_align, without the apron.
            const f4f23ma::f32 lateral_extent = (std::max)(std::fabs(xmin), std::fabs(xmax));
            const f4f23ma::f32 dx_tile = (std::max)(0.f, (std::max)(tile_rel_x - sp.position.x,
                                                                      sp.position.x - (tile_rel_x + size)));
            const f4f23ma::f32 dy_tile = (std::max)(0.f, (std::max)(tile_rel_y - sp.position.y,
                                                                      sp.position.y - (tile_rel_y + size)));
            const f4f23ma::f32 reach = (std::max)(0.f, sp.length) + std::fabs(sp.local_length) +
                                       std::fabs(sp.skew_start) + std::fabs(sp.skew_end) + lateral_extent;
            if (dx_tile * dx_tile + dy_tile * dy_tile > reach * reach) continue;
            for (int yi = 0; yi < dim; ++yi) {
                for (int xi = 0; xi < dim; ++xi) {
                    const f4f23ma::f32 wx = tile_rel_x + static_cast<f4f23ma::f32>(xi) * step;
                    const f4f23ma::f32 wy = tile_rel_y + static_cast<f4f23ma::f32>(yi) * step;
                    f4f23ma::f32 actual_along = 0.f, lat = 0.f;
                    if (!world_xy_to_spline_local_raw(sp, wx, wy, actual_along, lat)) continue;
                    if (lat < xmin || lat > xmax) continue;
                    const f4f23ma::f32 station = spline_station_from_actual_along(sp, actual_along, lat);
                    f4f23ma::f32 height_off = 0.f, end_off = 0.f;
                    sample_hole_profile(profile, lat, height_off, end_off);
                    (void)height_off;
                    const f4f23ma::f32 s0 = hole_start_at(sp, mode, end_off);
                    const f4f23ma::f32 s1 = hole_end_at(sp, mode, end_off);
                    if (station < std::min(s0, s1) || station > std::max(s0, s1)) continue;
                    mask[static_cast<std::size_t>(yi) * static_cast<std::size_t>(dim) +
                         static_cast<std::size_t>(xi)] = 1;
                }
            }
        }
    }
}

void mask_object_terrain_align_holes(std::vector<std::uint8_t>& mask, f4f23ma::f32 size_m,
                                     f4f23ma::f32 tile_rel_x, f4f23ma::f32 tile_rel_y, int dim,
                                     f4f23ma::f32 wx, f4f23ma::f32 wy, f4f23ma::f32 heading_deg,
                                     f4f23ma::f32 half_x, f4f23ma::f32 half_y) {
    if (dim < 2 || mask.size() < static_cast<std::size_t>(dim * dim)) return;
    const f4f23ma::f32 size = size_m > 0.01f ? size_m : 300.f;
    const f4f23ma::f32 step = size / static_cast<f4f23ma::f32>(dim - 1);
    const f4f23ma::f32 rad = heading_deg * (3.14159265f / 180.f);
    const f4f23ma::f32 c = std::cos(rad);
    const f4f23ma::f32 s = std::sin(rad);
    const f4f23ma::f32 hx = (std::max)(0.5f, half_x);
    const f4f23ma::f32 hy = (std::max)(0.5f, half_y);
    for (int yi = 0; yi < dim; ++yi) {
        for (int xi = 0; xi < dim; ++xi) {
            const f4f23ma::f32 px = tile_rel_x + static_cast<f4f23ma::f32>(xi) * step;
            const f4f23ma::f32 py = tile_rel_y + static_cast<f4f23ma::f32>(yi) * step;
            const f4f23ma::f32 dx = px - wx;
            const f4f23ma::f32 dy = py - wy;
            // OMSI yaw: local x = dx*cos + dy*sin? use atan2(x,y) basis: forward=(sin,cos)
            const f4f23ma::f32 local_x = dx * c + dy * s;
            const f4f23ma::f32 local_y = -dx * s + dy * c;
            if (std::fabs(local_x) <= hx && std::fabs(local_y) <= hy)
                mask[static_cast<std::size_t>(yi) * static_cast<std::size_t>(dim) +
                     static_cast<std::size_t>(xi)] = 1;
        }
    }
}

} // namespace omsi::spline
