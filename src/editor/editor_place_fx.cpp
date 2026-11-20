#include "omsi/editor_place_fx.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace omsi::editor {
namespace {

float ease_out_quad(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return 1.f - (1.f - t) * (1.f - t);
}

float default_duration(PlaceFxKind k) {
    switch (k) {
    case PlaceFxKind::PlaceBounce:
        return 0.8f;
    case PlaceFxKind::SoftPuff:
        return 0.45f;
    case PlaceFxKind::RotateChips:
        return 0.35f;
    case PlaceFxKind::DeleteShrink:
        return 0.4f;
    case PlaceFxKind::SplineStripe:
        return 0.55f;
    }
    return 0.5f;
}

// Deterministic soft hash → [0,1)
float hash01(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return static_cast<float>(x & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

int dust_count(PlaceFxKind k) {
    // v165 O: denser / more opaque place juice (still dots, not rings).
    switch (k) {
    case PlaceFxKind::PlaceBounce:
        return 64;
    case PlaceFxKind::SoftPuff:
        return 44;
    case PlaceFxKind::RotateChips:
        return 52;
    case PlaceFxKind::DeleteShrink:
        return 48;
    case PlaceFxKind::SplineStripe:
        return 36;
    }
    return 44;
}

} // namespace

void PlaceFxSystem::spawn(PlaceFxKind kind, float x, float y, float z, float radius,
                          std::int32_t object_id) {
    if (!enabled) return;
    PlaceFxEvent e{};
    e.kind = kind;
    e.x = x;
    e.y = y;
    e.z = z;
    e.radius = (std::max)(0.35f, radius);
    e.object_id = object_id;
    e.age = 0.f;
    e.duration = default_duration(kind);
    events_.push_back(e);
}

void PlaceFxSystem::tick(float dt) {
    if (dt <= 0.f) return;
    for (auto& e : events_) e.age += dt;
    events_.erase(std::remove_if(events_.begin(), events_.end(),
                                 [](const PlaceFxEvent& e) { return e.age >= e.duration; }),
                  events_.end());
}

void PlaceFxSystem::clear() { events_.clear(); }

float PlaceFxSystem::bounce_scale(std::int32_t object_id) const {
    if (object_id == 0) return 1.f;
    float scale = 1.f;
    for (const auto& e : events_) {
        if (e.object_id != object_id || e.kind != PlaceFxKind::PlaceBounce) continue;
        const float t = std::clamp(e.age / 0.4f, 0.f, 1.f);
        // Overshoot then settle (Tiny Glade-ish pop) + slight snap 2–4 cm feel via scale.
        const float s = 1.f + 0.12f * std::sin(t * 3.14159265f) * (1.f - t);
        scale = (std::max)(scale, s);
    }
    return scale;
}

float PlaceFxSystem::delete_opacity(std::int32_t object_id) const {
    if (object_id == 0) return 1.f;
    for (const auto& e : events_) {
        if (e.object_id != object_id || e.kind != PlaceFxKind::DeleteShrink) continue;
        const float t = ease_out_quad(e.age / e.duration);
        return 1.f - t;
    }
    return 1.f;
}

void PlaceFxSystem::draw_imgui(const ProjectFn& project) const {
    if (!enabled || events_.empty() || !project) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;
    for (const auto& e : events_) {
        float sx = 0.f, sy = 0.f;
        if (!project(e.x, e.y, e.z + 0.05f * e.radius, sx, sy)) continue;
        const float t = std::clamp(e.age / e.duration, 0.f, 1.f);
        const float eo = ease_out_quad(t);
        const int n = dust_count(e.kind);
        const std::uint32_t seed =
            static_cast<std::uint32_t>(e.object_id) * 2654435761u +
            static_cast<std::uint32_t>(e.kind) * 97u + 0xA17Fu;

        // Soft dust dots only — no rings / connecting lines.
        for (int i = 0; i < n; ++i) {
            const float u = hash01(seed + static_cast<std::uint32_t>(i) * 17u);
            const float v = hash01(seed + static_cast<std::uint32_t>(i) * 31u + 3u);
            const float w = hash01(seed + static_cast<std::uint32_t>(i) * 53u + 7u);
            const float pix = std::clamp(e.radius * 6.5f, 10.f, 120.f);
            float cx = 0.f, cy = 0.f, size = 1.6f;
            if (e.kind == PlaceFxKind::SplineStripe) {
                // Horizontal ribbon pulse — distinct from SCO radial puff.
                const float along = (u - 0.5f) * pix * 2.2f * (1.f + 0.3f * eo);
                const float side = (v - 0.5f) * pix * 0.35f;
                cx = sx + along;
                cy = sy + side - eo * 4.f * w;
                size = 1.4f + 1.8f * (1.f - eo);
            } else {
                const float ang = u * 6.2831853f;
                const float radial = (0.15f + 0.85f * v) * e.radius * (0.55f + 0.9f * eo);
                const float lift = (0.2f + w * 0.9f) * e.radius * eo;
                cx = sx + std::cos(ang) * radial * (pix / (std::max)(0.35f, e.radius));
                cy = sy - std::sin(ang) * radial * 0.55f * (pix / (std::max)(0.35f, e.radius)) -
                     lift * 7.f - eo * 6.f * w;
                size = (1.6f + 2.4f * (1.f - eo) * (0.4f + v)) *
                       (e.kind == PlaceFxKind::RotateChips ? 1.15f : 1.f);
            }
            float alpha = (1.f - eo) * (e.kind == PlaceFxKind::SoftPuff ? 0.58f : 0.72f);
            if (e.kind == PlaceFxKind::DeleteShrink) alpha *= 0.85f;
            if (e.kind == PlaceFxKind::SplineStripe) alpha *= 0.65f;
            const float r = e.kind == PlaceFxKind::SplineStripe   ? 0.45f
                            : e.kind == PlaceFxKind::RotateChips ? 0.90f
                                                                : 0.82f;
            const float g = e.kind == PlaceFxKind::SplineStripe   ? 0.78f
                            : e.kind == PlaceFxKind::RotateChips ? 0.78f
                                                                : 0.78f;
            const float b = e.kind == PlaceFxKind::SplineStripe   ? 0.88f
                            : e.kind == PlaceFxKind::RotateChips ? 0.55f
                                                                : 0.70f;
            dl->AddCircleFilled(ImVec2(cx, cy), size, ImGui::GetColorU32(ImVec4(r, g, b, alpha)), 8);
        }

        if (e.kind == PlaceFxKind::DeleteShrink) {
            // Soft fade core only (still dots, not a line ring).
            const float s = 1.f - eo;
            dl->AddCircleFilled(ImVec2(sx, sy), 4.f * s,
                                ImGui::GetColorU32(ImVec4(0.92f, 0.70f, 0.70f, 0.25f * s)), 10);
        }
    }
}

} // namespace omsi::editor
