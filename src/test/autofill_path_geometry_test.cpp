// Standalone geometry test for path autofill scoring and in-memory path snap.
#include "omsi/map_types.hpp"
#include "omsi/spline_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.f;

float wrap180(float a) {
    while (a > 180.f) a -= 360.f;
    while (a < -180.f) a += 360.f;
    return a;
}

bool compute_biarc_omsi(float x0, float y0, float h0_deg, float x1, float y1, float h1_deg, float soft, float& L0,
                        float& R0, float& L1, float& R1) {
    float t0x = std::sin(h0_deg * kDeg), t0y = std::cos(h0_deg * kDeg);
    float t1x = std::sin(h1_deg * kDeg), t1y = std::cos(h1_deg * kDeg);
    const float vx = x1 - x0, vy = y1 - y0;
    const float dist = std::sqrt(vx * vx + vy * vy);
    if (dist < 0.5f) return false;

    auto solve_ab = [&](float ax, float ay, float bx, float by, float& a, float& b) -> bool {
        const float det = ax * by - ay * bx;
        if (std::fabs(det) < 1.e-6f) return false;
        a = (vx * by - vy * bx) / det;
        b = (ax * vy - ay * vx) / det;
        return true;
    };

    float a = 0.f, b = 0.f;
    float jx = 0.f, jy = 0.f, hj_deg = 0.f;
    bool ok_ab = solve_ab(t0x, t0y, t1x, t1y, a, b) && a > 0.05f && b > 0.05f;
    if (!ok_ab) {
        t1x = -t1x;
        t1y = -t1y;
        h1_deg = wrap180(h1_deg + 180.f);
        ok_ab = solve_ab(t0x, t0y, t1x, t1y, a, b) && a > 0.05f && b > 0.05f;
    }
    if (!ok_ab) {
        jx = 0.5f * (x0 + x1);
        jy = 0.5f * (y0 + y1);
        const float mx = -vy / dist, my = vx / dist;
        const float chord_h = std::atan2(vx, vy) / kDeg;
        const float alpha = wrap180(chord_h - h0_deg);
        const float beta = wrap180(h1_deg - chord_h);
        const float offs = 0.38f * dist * std::tan(0.5f * (alpha + beta) * kDeg);
        jx += mx * offs;
        jy += my * offs;
    } else {
        a *= soft;
        b *= soft;
        const float Qx = x0 + a * t0x;
        const float Qy = y0 + a * t0y;
        const float d0 = std::sqrt((Qx - x1) * (Qx - x1) + (Qy - y1) * (Qy - y1));
        const float d1 = std::sqrt((Qx - x0) * (Qx - x0) + (Qy - y0) * (Qy - y0));
        const float d2 = dist;
        const float s = d0 + d1 + d2;
        if (s < 1.e-3f) return false;
        jx = (d0 * x0 + d1 * x1 + d2 * Qx) / s;
        jy = (d0 * y0 + d1 * y1 + d2 * Qy) / s;
    }

    omsi::spline::fit_spline_to_xy(x0, y0, h0_deg, jx, jy, L0, R0);
    if (!(L0 > 0.25f) || !std::isfinite(L0) || !std::isfinite(R0)) return false;
    omsi::map::SplineInst tmp{};
    tmp.position.x = x0;
    tmp.position.y = y0;
    tmp.heading_deg = h0_deg;
    tmp.length = L0;
    tmp.radius = R0;
    const auto mid = omsi::spline::evaluate_spline(tmp, L0);
    jx = mid.x;
    jy = mid.y;
    hj_deg = mid.heading_rad / kDeg;

    omsi::spline::fit_spline_to_xy(jx, jy, hj_deg, x1, y1, L1, R1);
    if (!(L1 > 0.25f) || !std::isfinite(L1) || !std::isfinite(R1)) return false;
    tmp.position.x = jx;
    tmp.position.y = jy;
    tmp.heading_deg = hj_deg;
    tmp.length = L1;
    tmp.radius = R1;
    const auto end = omsi::spline::evaluate_spline(tmp, L1);
    const float err_p = std::sqrt((end.x - x1) * (end.x - x1) + (end.y - y1) * (end.y - y1));
    const float err_h = std::fabs(wrap180(end.heading_rad / kDeg - h1_deg));
    return err_p < 0.35f && err_h < 45.f;
}

bool make_biarc(float x0, float y0, float h0, float tx, float ty, float h1, float soft, float lateral,
                std::vector<omsi::map::SplineInst>& out) {
    out.clear();
    float L0 = 0.f, R0 = 0.f, L1 = 0.f, R1 = 0.f;
    if (!compute_biarc_omsi(x0, y0, h0, tx, ty, h1, soft, L0, R0, L1, R1)) return false;
    omsi::map::SplineInst a{};
    a.position.x = x0;
    a.position.y = y0;
    a.heading_deg = h0;
    a.length = L0;
    a.radius = R0;
    const auto mid = omsi::spline::evaluate_spline(a, a.length);
    omsi::map::SplineInst b{};
    b.position.x = mid.x;
    b.position.y = mid.y;
    b.heading_deg = mid.heading_rad / kDeg;
    b.length = L1;
    b.radius = R1;
    out = {a, b};
    (void)lateral;
    return true;
}

void snap_path_end_in_memory(omsi::map::SplineInst& s, float lateral, float px, float py, float pz, float ph_deg) {
    const auto samp = omsi::spline::evaluate_spline(s, s.length, lateral, 0.f);
    s.position.x += px - samp.x;
    s.position.y += py - samp.y;
    s.position.z += pz - samp.z;
    float dh = ph_deg - samp.heading_rad * (180.f / kPi);
    while (dh > 180.f) dh -= 360.f;
    while (dh < -180.f) dh += 360.f;
    s.heading_deg += dh;
}

float path_end_err(const omsi::map::SplineInst& s, float lateral, float px, float py) {
    const auto e = omsi::spline::evaluate_spline(s, s.length, lateral, 0.f);
    return std::sqrt((e.x - px) * (e.x - px) + (e.y - py) * (e.y - py));
}

float min_abs_radius(const std::vector<omsi::map::SplineInst>& segs) {
    float min_r = 1.e9f;
    for (const auto& s : segs) min_r = (std::min)(min_r, std::fabs(s.radius));
    return min_r;
}

float score_solution(const std::vector<omsi::map::SplineInst>& segs, float path_err, float dist) {
    const float min_smooth_r = (std::max)(25.f, dist * 0.32f);
    float len = 0.f, smooth = 0.f, inv_r = 0.f;
    for (const auto& s : segs) {
        len += s.length;
        const float ar = std::fabs(s.radius);
        if (ar > 0.5f) {
            if (ar < min_smooth_r) smooth += (min_smooth_r - ar) * (min_smooth_r - ar);
            inv_r += 1.f / ar;
        }
    }
    return 400.f * path_err + 0.12f * len + 6.f * smooth + 450.f * inv_r;
}

} // namespace

int main() {
    // 90° junction: start heading 0 (+Y), end heading 90 (+X), ~42 m chord.
    const float x0 = 0.f, y0 = 0.f, h0 = 0.f;
    const float tx = 30.f, ty = 30.f, h1 = 90.f;
    const float lateral = 1.5f;
    const float dist = std::sqrt((tx - x0) * (tx - x0) + (ty - y0) * (ty - y0));

    // Target path endpoint at B (lateral offset from centerline anchor).
    omsi::map::SplineInst probe{};
    probe.position.x = tx;
    probe.position.y = ty;
    probe.heading_deg = h1;
    probe.length = 0.01f;
    probe.radius = 500.f;
    const auto path_at_b = omsi::spline::evaluate_spline(probe, probe.length, lateral, 0.f);
    const float pick_bx = path_at_b.x;
    const float pick_by = path_at_b.y;

    struct Cand {
        std::vector<omsi::map::SplineInst> segs;
        float score = 1.e9f;
        float path_err = 1.e9f;
        float min_r = 0.f;
    };
    Cand best{};

    static constexpr float kSofts[] = {1.0f, 1.25f, 1.5f, 1.85f, 2.2f, 2.6f, 3.1f, 3.7f, 4.4f, 5.2f, 6.2f, 7.5f};
    for (int flip = 0; flip < 2; ++flip) {
        const float htry = wrap180(h1 + (flip ? 180.f : 0.f));
        for (float soft : kSofts) {
            Cand c{};
            if (!make_biarc(x0, y0, h0, tx, ty, htry, soft, lateral, c.segs)) continue;
            auto trial = c.segs;
            snap_path_end_in_memory(trial.back(), lateral, pick_bx, pick_by, 0.f, h1);
            c.path_err = path_end_err(trial.back(), lateral, pick_bx, pick_by);
            c.min_r = min_abs_radius(trial);
            c.score = score_solution(trial, c.path_err, dist);
            if (c.score < best.score) best = std::move(c);
        }
    }

    if (best.segs.empty()) {
        std::cout << "FAIL: no biarc solution\n";
        return 1;
    }

    auto final_segs = best.segs;
    snap_path_end_in_memory(final_segs.back(), lateral, pick_bx, pick_by, 0.f, h1);
    const float final_path_err = path_end_err(final_segs.back(), lateral, pick_bx, pick_by);
    const float final_min_r = min_abs_radius(final_segs);

    std::cout << "autofill_path_geometry_test\n";
    std::cout << "  dist=" << dist << " lateral=" << lateral << "\n";
    std::cout << "  segs=" << best.segs.size() << " score=" << best.score << "\n";
    std::cout << "  min_|R|=" << final_min_r << " path_err=" << final_path_err << " m\n";
    for (std::size_t i = 0; i < final_segs.size(); ++i) {
        std::cout << "  seg" << i << " L=" << final_segs[i].length << " R=" << final_segs[i].radius << "\n";
    }

    bool pass = true;
    if (final_min_r < 20.f) {
        std::cout << "FAIL: min |radius|=" << final_min_r << " < 20\n";
        pass = false;
    }
    if (final_path_err >= 0.2f) {
        std::cout << "FAIL: path_err=" << final_path_err << " >= 0.2\n";
        pass = false;
    }
    if (pass) std::cout << "PASS\n";
    return pass ? 0 : 1;
}
