#pragma once

#include <DirectXMath.h>

#include <cmath>
#include <cstdint>

namespace omsi {

// Gribb–Hartmann from a DirectX ViewProj (XMMATRIX rows, NDC z ∈ [0,1]).
// Near is row3 only — NOT OpenGL row4+row3 (that puts near behind the eye).
struct FrustumPlane {
    float a = 0.f, b = 0.f, c = 0.f, d = 0.f;
};

struct Frustum {
    FrustumPlane planes[6]{}; // L R B T N F
};

inline void normalize_plane(FrustumPlane& p) {
    const float len = std::sqrt(p.a * p.a + p.b * p.b + p.c * p.c);
    if (len < 1.e-8f) return;
    const float inv = 1.f / len;
    p.a *= inv;
    p.b *= inv;
    p.c *= inv;
    p.d *= inv;
}

inline Frustum extract_frustum_planes_dx(DirectX::FXMMATRIX view_proj) {
    DirectX::XMFLOAT4X4 m{};
    DirectX::XMStoreFloat4x4(&m, view_proj);
    // DirectXMath is row-major: _11 is row0.col0. row4 = translation/homogeneous row (_41.._44).
    const float r1[4] = {m._11, m._12, m._13, m._14};
    const float r2[4] = {m._21, m._22, m._23, m._24};
    const float r3[4] = {m._31, m._32, m._33, m._34};
    const float r4[4] = {m._41, m._42, m._43, m._44};
    Frustum f{};
    auto set = [](FrustumPlane& p, const float* a, const float* b, float sb) {
        p.a = a[0] + sb * b[0];
        p.b = a[1] + sb * b[1];
        p.c = a[2] + sb * b[2];
        p.d = a[3] + sb * b[3];
        normalize_plane(p);
    };
    set(f.planes[0], r4, r1, 1.f);  // left  row4+row1
    set(f.planes[1], r4, r1, -1.f); // right row4-row1
    set(f.planes[2], r4, r2, 1.f);  // bottom
    set(f.planes[3], r4, r2, -1.f); // top
    f.planes[4].a = r3[0];
    f.planes[4].b = r3[1];
    f.planes[4].c = r3[2];
    f.planes[4].d = r3[3];
    normalize_plane(f.planes[4]); // near = row3 only (D3D)
    set(f.planes[5], r4, r3, -1.f); // far row4-row3
    return f;
}

inline bool aabb_in_frustum(const Frustum& f, float minx, float miny, float minz, float maxx, float maxy,
                            float maxz) {
    constexpr float kPad = 0.75f;
    minx -= kPad;
    miny -= kPad;
    minz -= kPad;
    maxx += kPad;
    maxy += kPad;
    maxz += kPad;
    for (int i = 0; i < 6; ++i) {
        const auto& p = f.planes[i];
        const float px = p.a >= 0.f ? maxx : minx;
        const float py = p.b >= 0.f ? maxy : miny;
        const float pz = p.c >= 0.f ? maxz : minz;
        const float dist_p = p.a * px + p.b * py + p.c * pz + p.d;
        if (dist_p >= 0.f) continue;
        if (i == 4) {
            const float nx = p.a >= 0.f ? minx : maxx;
            const float ny = p.b >= 0.f ? miny : maxy;
            const float nz = p.c >= 0.f ? minz : maxz;
            if (p.a * nx + p.b * ny + p.c * nz + p.d >= 0.f) continue;
        }
        return false;
    }
    return true;
}

inline bool point_in_frustum(const Frustum& f, float x, float y, float z) {
    for (int i = 0; i < 6; ++i) {
        const auto& p = f.planes[i];
        if (p.a * x + p.b * y + p.c * z + p.d < 0.f) return false;
    }
    return true;
}

} // namespace omsi
