#pragma once

#include "omsi/bus_catalog.hpp"
#include "ui/editor_side_panel.hpp"
#include "ui/ultralight_overlay.hpp"

#include <cstddef>
#include <cstdint>

namespace omsi::ui {

struct MapEditorUltralightArgs {
    EditorSidePanel* panel = nullptr;
    UltralightOverlay* overlay = nullptr;
    EditorTilePageInfo tile_info{};
    const char* map_name = nullptr;
    std::size_t tile_count = 0;
    std::size_t object_count = 0;
    std::size_t spline_count = 0;
    // Currently selected instance ids (0 = none) — drives list highlight + scroll.
    std::int32_t selected_object_id = 0;
    std::int32_t selected_spline_id = 0;
    const std::vector<omsi::bus_catalog::BusEntry>* bus_catalog = nullptr;
    std::string last_omsi_bus;
};

void handle_ultralight_panel_event(EditorSidePanel& panel, const std::string& json);
void sync_map_editor_ultralight_state(const MapEditorUltralightArgs& args);
void draw_map_editor_ultralight_panel(const MapEditorUltralightArgs& args);

} // namespace omsi::ui
