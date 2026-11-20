#pragma once

#include <cstdint>
#include <vector>

#include "omsi/map_types.hpp"

namespace omsi::app::map_editor {

struct EndMarker {
    std::size_t chunk = 0;
    bool is_path = false;
    std::int32_t owner_id = 0;
    bool is_start = true;
    bool from_object = false;
    int path_index = 0;
    float scale = 0.85f;
    std::int32_t tile_gx = 0;
    std::int32_t tile_gy = 0;
    bool alive = true;
};

struct PathRail {
    std::size_t chunk = 0;
    std::int32_t owner_id = 0;
    bool from_object = false;
    int path_index = 0;
    int path_typ = 0;
    int path_dir = 0;
    std::int32_t tile_gx = 0;
    std::int32_t tile_gy = 0;
    bool alive = true;
};

struct PathEndpointPick {
    bool valid = false;
    std::int32_t owner_id = 0;
    bool from_object = false;
    int path_index = 0;
    bool is_start = true;
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float heading_deg = 0.f;
};

enum class EditMode {
    None,
    Grab,
    GrabLength,
    GrabHeight,
    Radius,
    Rotate,
    SplineFollow,
    NewGrab,
    NewFollow,
    NewScoFollow,
    TrimSpline,
    AttachPickParent,
    AttachPickSpline,
    CompleteSpline,
    PickSplineGrad,
    EditSplineGrad,
    Mesh
};

enum class AutofillAttach { ReplaceStart, AppendEnd, PrependStart };

enum class MarqueePhase { None, Select, Placing };
enum class MarqueeIntent { Clone, SelectOnly };

} // namespace omsi::app::map_editor
