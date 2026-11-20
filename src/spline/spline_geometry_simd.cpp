#include "omsi/math_simd.hpp"
#include <vector>

namespace omsi::spline {

// Batch evaluate 8 sample points t_0 .. t_7 along cubic spline coefficients (c, a, b, d) using AVX2 FMA3.
void evaluate_cubic_spline_batch_simd(const float* t_values8, float c, float a, float b, float d, float* out_results8) {
    omsi::math::SimdFloat8 t = omsi::math::SimdFloat8::load(t_values8);
    omsi::math::SimdFloat8 res = omsi::math::evaluate_cubic_spline_simd(t, c, a, b, d);
    res.store(out_results8);
}

} // namespace omsi::spline
