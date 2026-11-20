#pragma once
// Cascaded-shadow-map cache helpers (pure CPU math, no D3D types so it can be unit-tested anywhere).
//
// Problem solved: the old cache hashed the light matrices, which were re-derived from the camera pose every
// frame, plus EVERY mesh chunk in the scene. Any camera move/rotation > 1 shadow texel changed the matrix,
// and any chunk change anywhere (streaming) invalidated all three 2048^2 cascades.
//
// Now each cascade keeps a stable "light volume" (center + radius). It is only refitted when the camera
// slice no longer fits inside it. Validity is tracked per cascade, and the scene hash only covers casters
// that can actually land inside that cascade's volume.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace omsi::gfx::dx12 {

struct CsmCascadeState {
    bool volume_valid = false;  // center/radius describe the volume csm_light_vp[i] was built from
    bool shadow_valid = false;  // depth slice holds a render of cached_matrix_hash / cached_scene_hash
    float center[3]{};
    float radius = 0.f;
    std::uint64_t cached_matrix_hash = 0;
    std::uint64_t cached_scene_hash = 0;
};

// ---- FNV-1a mixing -------------------------------------------------------------------------------------
inline void csm_mix(std::uint64_t& h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xffu;
        h *= 1099511628211ull;
    }
}
inline void csm_mix_float(std::uint64_t& h, float f) {
    if (f == 0.f) f = 0.f; // fold -0.0 into +0.0
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    csm_mix(h, u);
}
inline void csm_mix_floats(std::uint64_t& h, const float* p, int n) {
    for (int i = 0; i < n; ++i) csm_mix_float(h, p[i]);
}
inline void csm_mix_ptr(std::uint64_t& h, const void* p) {
    csm_mix(h, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)));
}

// Extra radius applied when a cascade is refitted, so following frames of camera motion hit the cache.
// Near cascade needs the least slack (it is the sharpest), far cascade the most.
inline float csm_volume_margin(int cascade) {
    static const float kMargin[3] = {1.20f, 1.25f, 1.30f};
    return kMargin[cascade < 0 ? 0 : (cascade > 2 ? 2 : cascade)];
}

// True when the cached volume still fully contains a sphere (needed_c, needed_r) that bounds the camera
// slice, with `slack_texels` of safety for texel snapping. Also refits when the cached volume has become
// much larger than needed (e.g. camera moved from a wide view to a tight one) to avoid wasting resolution.
inline bool csm_volume_reusable(const CsmCascadeState& cs, const float needed_c[3], float needed_r,
                                float slack_texels, float cached_texel) {
    if (!cs.volume_valid || cs.radius <= 0.f) return false;
    const float dx = needed_c[0] - cs.center[0];
    const float dy = needed_c[1] - cs.center[1];
    const float dz = needed_c[2] - cs.center[2];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist + needed_r + slack_texels * cached_texel > cs.radius) return false;
    if (cs.radius > needed_r * 1.75f) return false;
    return true;
}

// World-space bounding sphere of a chunk from its local bbox and transposed world matrix (16 floats,
// row-major storage of W^T, i.e. W[i][j] = m[j*4+i]). Unset/degenerate bbox -> "infinite" sphere, so the
// chunk is conservatively always treated as a caster.
inline void csm_chunk_world_sphere(const float* m, const float bmin[3], const float bmax[3], float out_c[3],
                                   float& out_r) {
    const float ex = bmax[0] - bmin[0], ey = bmax[1] - bmin[1], ez = bmax[2] - bmin[2];
    if (!(ex > 0.f || ey > 0.f || ez > 0.f) || !std::isfinite(ex + ey + ez)) {
        out_c[0] = out_c[1] = out_c[2] = 0.f;
        out_r = 1e30f;
        return;
    }
    const float lc[3] = {(bmin[0] + bmax[0]) * 0.5f, (bmin[1] + bmax[1]) * 0.5f, (bmin[2] + bmax[2]) * 0.5f};
    const float lr = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
    for (int j = 0; j < 3; ++j)
        out_c[j] = lc[0] * m[j * 4 + 0] + lc[1] * m[j * 4 + 1] + lc[2] * m[j * 4 + 2] + m[j * 4 + 3];
    float s2 = 0.f; // largest squared row scale of W's 3x3 (handles non-uniform scale conservatively)
    for (int i = 0; i < 3; ++i) {
        const float a = m[0 * 4 + i], b = m[1 * 4 + i], c = m[2 * 4 + i];
        const float v = a * a + b * b + c * c;
        if (v > s2) s2 = v;
    }
    out_r = lr * std::sqrt(s2);
}

// Can a caster sphere (c, r) contribute to a cascade whose ortho volume is a box of half-size vr around vc,
// in the light basis (right, up, dir)? Depth range matches the projection: eye at vr+extra along dir.
inline bool csm_sphere_in_light_volume(const float c[3], float r, const float vc[3], float vr,
                                       const float right[3], const float up[3], const float dir[3],
                                       float extra_depth) {
    if (r >= 1e29f) return true;
    const float d[3] = {c[0] - vc[0], c[1] - vc[1], c[2] - vc[2]};
    const float x = d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
    const float y = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
    const float z = d[0] * dir[0] + d[1] * dir[1] + d[2] * dir[2];
    const float lim = vr + r;
    return std::fabs(x) <= lim && std::fabs(y) <= lim && std::fabs(z) <= lim + extra_depth;
}

} // namespace omsi::gfx::dx12
