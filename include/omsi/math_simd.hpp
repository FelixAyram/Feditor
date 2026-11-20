#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define OMSI_HAS_X86_SIMD 1
#endif

namespace omsi::math {

// Runtime CPU capability check for AVX2 & FMA3
inline bool cpu_supports_avx2() {
#if defined(OMSI_HAS_X86_SIMD) && defined(_MSC_VER)
    int cpuInfo[4];
    __cpuid(cpuInfo, 0);
    if (cpuInfo[0] >= 7) {
        int cpuInfo7[4];
        __cpuidex(cpuInfo7, 7, 0);
        // EBX bit 5 = AVX2, bit 12 = FMA
        bool avx2 = (cpuInfo7[1] & (1 << 5)) != 0;
        bool fma  = (cpuInfo7[1] & (1 << 12)) != 0;
        return avx2 && fma;
    }
    return false;
#elif defined(OMSI_HAS_X86_SIMD) && (defined(__GNUC__) || defined(__clang__))
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
    return false;
#endif
}

// 8-wide Single Precision Vector wrapper for AVX2 (__m256)
struct SimdFloat8 {
#ifdef OMSI_HAS_X86_SIMD
    __m256 v;

    SimdFloat8() : v(_mm256_setzero_ps()) {}
    explicit SimdFloat8(__m256 val) : v(val) {}
    SimdFloat8(float f) : v(_mm256_set1_ps(f)) {}
    SimdFloat8(float f0, float f1, float f2, float f3, float f4, float f5, float f6, float f7)
        : v(_mm256_set_ps(f7, f6, f5, f4, f3, f2, f1, f0)) {}

    static SimdFloat8 load(const float* ptr) {
        return SimdFloat8(_mm256_loadu_ps(ptr));
    }

    static SimdFloat8 load_aligned(const float* ptr) {
        return SimdFloat8(_mm256_load_ps(ptr));
    }

    void store(float* ptr) const {
        _mm256_storeu_ps(ptr, v);
    }

    void store_aligned(float* ptr) const {
        _mm256_store_ps(ptr, v);
    }

    SimdFloat8 operator+(const SimdFloat8& o) const {
        return SimdFloat8(_mm256_add_ps(v, o.v));
    }

    SimdFloat8 operator-(const SimdFloat8& o) const {
        return SimdFloat8(_mm256_sub_ps(v, o.v));
    }

    SimdFloat8 operator*(const SimdFloat8& o) const {
        return SimdFloat8(_mm256_mul_ps(v, o.v));
    }

    SimdFloat8 operator/(const SimdFloat8& o) const {
        return SimdFloat8(_mm256_div_ps(v, o.v));
    }

    // Fused Multiply-Add: (this * a) + b
    SimdFloat8 fmadd(const SimdFloat8& a, const SimdFloat8& b) const {
#ifdef __FMA__
        return SimdFloat8(_mm256_fmadd_ps(v, a.v, b.v));
#else
        return SimdFloat8(_mm256_add_ps(_mm256_mul_ps(v, a.v), b.v));
#endif
    }
#else
    // Fallback scalar implementation for non-x86 or non-SIMD platforms
    float f[8]{};

    SimdFloat8() = default;
    SimdFloat8(float val) { for(int i=0;i<8;++i) f[i] = val; }
    SimdFloat8(float f0, float f1, float f2, float f3, float f4, float f5, float f6, float f7)
        : f{f0, f1, f2, f3, f4, f5, f6, f7} {}

    static SimdFloat8 load(const float* ptr) {
        SimdFloat8 r;
        for(int i=0;i<8;++i) r.f[i] = ptr[i];
        return r;
    }
    static SimdFloat8 load_aligned(const float* ptr) { return load(ptr); }
    void store(float* ptr) const { for(int i=0;i<8;++i) ptr[i] = f[i]; }
    void store_aligned(float* ptr) const { store(ptr); }

    SimdFloat8 operator+(const SimdFloat8& o) const {
        SimdFloat8 r;
        for(int i=0;i<8;++i) r.f[i] = f[i] + o.f[i];
        return r;
    }
    SimdFloat8 operator-(const SimdFloat8& o) const {
        SimdFloat8 r;
        for(int i=0;i<8;++i) r.f[i] = f[i] - o.f[i];
        return r;
    }
    SimdFloat8 operator*(const SimdFloat8& o) const {
        SimdFloat8 r;
        for(int i=0;i<8;++i) r.f[i] = f[i] * o.f[i];
        return r;
    }
    SimdFloat8 operator/(const SimdFloat8& o) const {
        SimdFloat8 r;
        for(int i=0;i<8;++i) r.f[i] = f[i] / o.f[i];
        return r;
    }
    SimdFloat8 fmadd(const SimdFloat8& a, const SimdFloat8& b) const {
        SimdFloat8 r;
        for(int i=0;i<8;++i) r.f[i] = (f[i] * a.f[i]) + b.f[i];
        return r;
    }
#endif
};

// Vectorized Cubic Spline evaluation using Horner's method with FMA3:
// z(t) = ((c * t + a) * t + b) * t + d
inline SimdFloat8 evaluate_cubic_spline_simd(const SimdFloat8& t, float c, float a, float b, float d) {
    SimdFloat8 vc(c);
    SimdFloat8 va(a);
    SimdFloat8 vb(b);
    SimdFloat8 vd(d);

    // ((c * t + a) * t + b) * t + d
    SimdFloat8 res = vc.fmadd(t, va);
    res = res.fmadd(t, vb);
    res = res.fmadd(t, vd);
    return res;
}

} // namespace omsi::math
