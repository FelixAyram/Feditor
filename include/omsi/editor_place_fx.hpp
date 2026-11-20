#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace omsi::editor {

enum class PlaceFxKind {
    PlaceBounce = 0, // place / dup / paste SCO — bounce + smoke
    SoftPuff,        // release G / SA first along
    RotateChips,     // release R — short chip arc + puff
    DeleteShrink,    // delete SCO — shrink + dust
    SplineStripe,    // place / edit .sli — distinct ribbon pulse (not SCO bounce)
};

struct PlaceFxEvent {
    PlaceFxKind kind = PlaceFxKind::SoftPuff;
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float radius = 1.f; // half-diagonal XY of AABB
    std::int32_t object_id = 0;
    float age = 0.f;
    float duration = 0.8f;
};

// One-shot juice on place / release / delete (never per-frame G/R).
class PlaceFxSystem {
public:
    bool enabled = true;

    void spawn(PlaceFxKind kind, float x, float y, float z, float radius, std::int32_t object_id = 0);
    void tick(float dt);
    void clear();

    [[nodiscard]] float bounce_scale(std::int32_t object_id) const;
    [[nodiscard]] float delete_opacity(std::int32_t object_id) const;

    // Project world → screen; return false if behind camera.
    using ProjectFn = std::function<bool(float wx, float wy, float wz, float& sx, float& sy)>;
    void draw_imgui(const ProjectFn& project) const;

    [[nodiscard]] const std::vector<PlaceFxEvent>& events() const { return events_; }

private:
    std::vector<PlaceFxEvent> events_;
};

} // namespace omsi::editor
