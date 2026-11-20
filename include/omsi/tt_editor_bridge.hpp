#pragma once
// Bridge: EditorSidePanel pending_  <->  TimeTableMan  <->  camera / path highlight
// Call these from the map-editor frame after drain of side-panel pending events.

#include "omsi/tt_man.hpp"

#include <cstdint>

namespace omsi::tt {

// Resolve world position of a path segment (owner_id + path_index) or busstop.
// Implement with your map's spline/object lookup (TTrafficPathManager_PathSegment equivalent).
using PathWorldPosFn = bool (*)(std::int32_t owner_id, std::int32_t path_index,
                                std::int32_t tile_index, float out_xyz[3], void* user);
using BusstopWorldPosFn = bool (*)(std::int32_t object_id, std::int32_t tile_index,
                                   float out_xyz[3], void* user);

struct CameraJumpRequest {
    bool valid = false;
    float xyz[3] = {0.f, 0.f, 0.f};
};

// After side panel sets pending_.tt_request_camera_jump + selection:
//   CameraJumpRequest j = compute_tt_camera_jump(resolve_path, resolve_busstop, user);
//   if (j.valid) camera.set_target(j.xyz);
inline CameraJumpRequest compute_tt_camera_jump(PathWorldPosFn path_fn,
                                                BusstopWorldPosFn bus_fn,
                                                void* user) {
    CameraJumpRequest r{};
    auto& man = global_tt_man();
    float xyz[3]{};
    // Prefer station jump if station selection changed last; else track entry.
    if (man.selected_trip_station >= 0 && bus_fn &&
        man.selected_station_world_pos(
            [](std::int32_t oid, std::int32_t, std::int32_t tile, float* out, void* u) -> bool {
                auto* pack = static_cast<std::pair<BusstopWorldPosFn, void*>*>(u);
                return pack->first(oid, tile, out, pack->second);
            },
            // adapter via static local is messy — call bus_fn directly:
            nullptr, xyz)) {
        // fallback: call bus_fn directly below
    }
    if (man.selected_trip_station >= 0 && bus_fn) {
        const auto& tp = man.data.trips[static_cast<std::size_t>(man.selected_trip)];
        if (man.selected_trip_station < static_cast<int>(tp.stations.size())) {
            const auto& st = tp.stations[static_cast<std::size_t>(man.selected_trip_station)];
            if (bus_fn(st.object_id, st.tile_index, xyz, user)) {
                r.valid = true;
                r.xyz[0] = xyz[0];
                r.xyz[1] = xyz[1];
                r.xyz[2] = xyz[2];
                return r;
            }
        }
    }
    if (man.selected_track_entry >= 0 && path_fn &&
        man.selected_track_entry_world_pos(path_fn, user, xyz)) {
        r.valid = true;
        r.xyz[0] = xyz[0];
        r.xyz[1] = xyz[1];
        r.xyz[2] = xyz[2];
    }
    return r;
}

// Path highlight colors (caller supplies RGB). Returns which style to use.
enum class PathHighlight : std::uint8_t { None = 0, InRoute = 1, SelectedEntry = 2 };

inline PathHighlight path_highlight_for(std::int32_t owner_id, std::int32_t path_index) {
    auto& man = global_tt_man();
    if (man.is_path_selected_entry(owner_id, path_index)) return PathHighlight::SelectedEntry;
    if (man.is_path_in_selected_track(owner_id, path_index)) return PathHighlight::InRoute;
    return PathHighlight::None;
}

} // namespace omsi::tt
