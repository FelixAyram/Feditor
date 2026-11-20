#pragma once

#include "omsi/load_governor.hpp"

#include <cstdlib>

namespace omsi {

/// Visual-only path (sampling / reload). Does not change mesh, SLI, or stream logic.
/// Default ON with OMSI_LOAD_TURBO=1; set OMSI_VISUAL_QUALITY=0 to disable.
///
/// DO NOT TOUCH without a clear bug: AF16 + mip_lod_bias=+0.5 (distance shimmer),
/// MSAA/SMAA defaults, or pairing with OMSI_LOAD_TURBO Idle/Loading slice semantics.
inline bool visual_quality_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("OMSI_VISUAL_QUALITY");
        if (e && e[0]) return e[0] != '0';
        return load_turbo_enabled();
    }();
    return on;
}

/// Prefer full-resolution albedo + CPU mip chain (see Vista → desmarcar "Límite 256px").
inline bool visual_quality_native_textures() {
    return visual_quality_enabled();
}

/// Requested MSAA sample count for the 3D scene (flip swapchain stays 1×; offscreen MSAA + resolve).
/// Default 8 when visual quality is on. OMSI_MSAA=1|2|4|8 (1 = off).
inline unsigned visual_quality_msaa_samples() {
    if (!visual_quality_enabled()) return 1u;
    static const unsigned n = [] {
        const char* e = std::getenv("OMSI_MSAA");
        if (e && e[0]) {
            const int v = std::atoi(e);
            if (v <= 1) return 1u;
            if (v <= 2) return 2u;
            if (v <= 4) return 4u;
            return 8u;
        }
        return 8u;
    }();
    return n;
}

/// Aniso 16× handles near sharpness on ground planes (MS Learn / Bgolus).
/// Small positive bias picks a slightly blurrier mip far away → less shimmer/noise on roads.
/// Negative bias sharpens but moirés lane markings. Override: OMSI_MIP_LOD_BIAS.
inline float visual_quality_mip_lod_bias() {
    if (!visual_quality_enabled()) return 0.f;
    const char* e = std::getenv("OMSI_MIP_LOD_BIAS");
    if (e && e[0]) {
        const double v = std::atof(e);
        if (v >= -2.0 && v <= 2.0) return static_cast<float>(v);
    }
    // +0.5 ≈ half mip toward minification: cleaner distant ground without soft near (AF keeps edges).
    return 0.5f;
}

/// Fullscreen SMAA 1x (iryoku HIGH) after MSAA resolve. Env keeps OMSI_FXAA name for compat.
/// Force with OMSI_FXAA=1 / OMSI_SMAA=1; disable with OMSI_FXAA=0.
inline bool visual_quality_fxaa_enabled() {
    if (!visual_quality_enabled()) return false;
    static const bool on = [] {
        if (const char* e = std::getenv("OMSI_SMAA"); e && e[0]) return e[0] != '0';
        if (const char* e = std::getenv("OMSI_FXAA"); e && e[0]) return e[0] != '0';
        return visual_quality_msaa_samples() <= 1u;
    }();
    return on;
}

inline bool visual_quality_smaa_enabled() { return visual_quality_fxaa_enabled(); }

} // namespace omsi
