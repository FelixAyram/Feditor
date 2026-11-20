#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace f4f23ma {

using f32 = float;

struct f4 {
    f32 x{}, y{}, z{}, w{};

    constexpr f4() = default;
    constexpr f4(f32 x_, f32 y_, f32 z_, f32 w_ = 0.f) : x(x_), y(y_), z(z_), w(w_) {}

    static constexpr f4 zero() { return {}; }
    static constexpr f4 one() { return {1.f, 1.f, 1.f, 1.f}; }

    f4 operator+(const f4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    f4 operator-(const f4& o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    f4 operator*(f32 s) const { return {x * s, y * s, z * s, w * s}; }

    f32 dot3(const f4& o) const { return x * o.x + y * o.y + z * o.z; }
    f4 cross3(const f4& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x, 0.f};
    }

    f4 normalized3() const {
        const f32 len = std::sqrt(dot3(*this));
        if (len <= 1e-8f) return {0.f, 0.f, 1.f, 0.f};
        return *this * (1.f / len);
    }
};

// Column-major 4x4, DirectX / OMSI convention.
struct ma4 {
    f32 m[4][4]{};

    static constexpr ma4 identity() {
        ma4 r{};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.f;
        return r;
    }

    static ma4 from_o3d_rows(const f32 rows[16]) {
        // O3D 0x79 chunk stores 4 rows (xyz + scale per row) in file order.
        ma4 out{};
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                out.m[c][r] = rows[r * 4 + c];
        return out;
    }

    f4 transform_point(const f4& p) const {
        return {
            m[0][0] * p.x + m[1][0] * p.y + m[2][0] * p.z + m[3][0] * p.w,
            m[0][1] * p.x + m[1][1] * p.y + m[2][1] * p.z + m[3][1] * p.w,
            m[0][2] * p.x + m[1][2] * p.y + m[2][2] * p.z + m[3][2] * p.w,
            m[0][3] * p.x + m[1][3] * p.y + m[2][3] * p.z + m[3][3] * p.w,
        };
    }

    // Upper 3x3 for directions/normals (orthogonal OMSI object matrices).
    f4 transform_direction3(const f4& d) const {
        return {
            m[0][0] * d.x + m[1][0] * d.y + m[2][0] * d.z,
            m[0][1] * d.x + m[1][1] * d.y + m[2][1] * d.z,
            m[0][2] * d.x + m[1][2] * d.y + m[2][2] * d.z,
            0.f,
        };
    }

    ma4 operator*(const ma4& o) const {
        ma4 r{};
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row)
                r.m[c][row] = m[0][row] * o.m[c][0] + m[1][row] * o.m[c][1] +
                              m[2][row] * o.m[c][2] + m[3][row] * o.m[c][3];
        return r;
    }

    // HLSL expects column-major float4x4 in memory; transpose row-major C++ layouts.
    ma4 transposed() const {
        ma4 r{};
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row)
                r.m[c][row] = m[row][c];
        return r;
    }
};

struct ma3x4 {
    f32 m[3][4]{};

    static ma3x4 from_view_proj(const ma4& view, const ma4& proj) {
        const ma4 vp = proj * view;
        ma3x4 out{};
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 3; ++r)
                out.m[r][c] = vp.m[c][r];
        return out;
    }
};

inline ma4 perspective_dx(f32 fov_y, f32 aspect, f32 zn, f32 zf) {
    const f32 y = 1.f / std::tan(fov_y * 0.5f);
    const f32 x = y / aspect;
    ma4 r{};
    r.m[0][0] = x;
    r.m[1][1] = y;
    r.m[2][2] = zf / (zf - zn);
    r.m[2][3] = 1.f;
    r.m[3][2] = -zn * zf / (zf - zn);
    return r;
}

inline ma4 look_at(const f4& eye, const f4& target, const f4& up) {
    // DirectX LH view matrix (same basis as D3DXMatrixLookAtLH).
    const f4 zaxis = (target - eye).normalized3();
    const f4 xaxis = up.cross3(zaxis).normalized3();
    const f4 yaxis = zaxis.cross3(xaxis);

    ma4 r = ma4::identity();
    r.m[0][0] = xaxis.x;
    r.m[1][0] = xaxis.y;
    r.m[2][0] = xaxis.z;
    r.m[0][1] = yaxis.x;
    r.m[1][1] = yaxis.y;
    r.m[2][1] = yaxis.z;
    r.m[0][2] = zaxis.x;
    r.m[1][2] = zaxis.y;
    r.m[2][2] = zaxis.z;
    r.m[3][0] = -xaxis.dot3(eye);
    r.m[3][1] = -yaxis.dot3(eye);
    r.m[3][2] = -zaxis.dot3(eye);
    return r;
}

} // namespace f4f23ma
