#include "ui/editor_side_panel.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace omsi::ui {
namespace {

void merge_flag(bool& dst, const bool src) {
    if (src) dst = true;
}

} // namespace

void EditorSidePanel::set_imgui_primary(bool on) {
    if (imgui_primary_ == on) return;
    if (on) {
        imgui_object_ = last_object_;
        imgui_spline_ = last_spline_;
        imgui_path_rule_ = last_path_rule_;
        read_tile_page_edits(imgui_state_.tile_geo);
        read_tile_page_flags(imgui_state_.tile_aerial_visible, imgui_state_.aerial_resolution_pos);
        imgui_state_.show_spline_ends =
            !s_chk_ends_ || SendMessageW(s_chk_ends_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        imgui_state_.show_path_ends =
            !s_chk_path_ends_ || SendMessageW(s_chk_path_ends_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        imgui_state_.show_path_rails =
            (tool_ == EditorTool::TrafficRules) ||
            (s_chk_path_rails_ && SendMessageW(s_chk_path_rails_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        imgui_state_.autofill_spline_ends =
            s_chk_autofill_ && SendMessageW(s_chk_autofill_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        imgui_state_.spline_max_radius = get_edit_float(s_max_radius_, 200.f);
        imgui_state_.parent_id_field = static_cast<std::int32_t>(get_edit_float(o_parent_, 0.f));
        imgui_state_.attach_point_field = static_cast<int>(get_edit_float(o_att_pnt_, 0.f));
        TerrainBrushUi brush{};
        brush.brush_size_m = get_edit_float(terr_brush_size_, 20.f);
        brush.speed = get_edit_float(terr_speed_, 3.f);
        brush.set_height = get_edit_float(terr_set_h_, 0.f);
        brush.ground_layer = ground_layer_sel_;
        brush.mode = TerrainBrushModeUi::Higher;
        if (terr_radio_lower_ && SendMessageW(terr_radio_lower_, BM_GETCHECK, 0, 0) == BST_CHECKED)
            brush.mode = TerrainBrushModeUi::Lower;
        else if (terr_radio_set_ && SendMessageW(terr_radio_set_, BM_GETCHECK, 0, 0) == BST_CHECKED)
            brush.mode = TerrainBrushModeUi::SetHeight;
        else if (terr_radio_paint_ && SendMessageW(terr_radio_paint_, BM_GETCHECK, 0, 0) == BST_CHECKED)
            brush.mode = TerrainBrushModeUi::PaintLayer;
        else if (terr_radio_rubber_ && SendMessageW(terr_radio_rubber_, BM_GETCHECK, 0, 0) == BST_CHECKED)
            brush.mode = TerrainBrushModeUi::RubberLayer;
        // FitMesh is ImGui-only (combo in draw_terrain_tab); keep Rubber as last Win32 radio.
        imgui_state_.terrain_brush = brush;
    }
    const bool was_primary = imgui_primary_;
    imgui_primary_ = on;
    const int show = on ? SW_HIDE : SW_SHOW;
    if (hwnd_) ShowWindow(hwnd_, show);
    if (toolbar_) ShowWindow(toolbar_, show);
    if (was_primary && !on) {
        // (its lists were left empty while ImGui was the panel)
        refill_catalog();
        refill_instances();
    }
}

void EditorSidePanel::imgui_set_tool(EditorTool tool) {
    set_tool(tool);
    if (tool == EditorTool::TrafficRules) imgui_state_.show_path_rails = true;
}

void EditorSidePanel::imgui_set_catalog_sel(int object_sel, int spline_sel) {
    if (object_sel >= 0) catalog_obj_sel_ = object_sel;
    if (spline_sel >= 0) catalog_sli_sel_ = spline_sel;
}

void EditorSidePanel::imgui_merge_pending(const EditorSidePanelState& ev) {
    if (ev.teleport_entry >= 0) pending_.teleport_entry = ev.teleport_entry;
    if (ev.select_mesh >= 0) pending_.select_mesh = ev.select_mesh;
    if (ev.select_object_index >= 0) pending_.select_object_index = ev.select_object_index;
    if (ev.select_spline_index >= 0) pending_.select_spline_index = ev.select_spline_index;
    if (ev.select_object_id > 0) pending_.select_object_id = ev.select_object_id;
    if (ev.select_spline_id > 0) pending_.select_spline_id = ev.select_spline_id;
    if (ev.select_catalog_object >= 0) pending_.select_catalog_object = ev.select_catalog_object;
    if (ev.select_catalog_spline >= 0) pending_.select_catalog_spline = ev.select_catalog_spline;

    merge_flag(pending_.apply_object, ev.apply_object);
    merge_flag(pending_.apply_spline, ev.apply_spline);
    merge_flag(pending_.apply_spline_mirror_only, ev.apply_spline_mirror_only);
    merge_flag(pending_.spline_grad_preview, ev.spline_grad_preview);
    merge_flag(pending_.spline_grad_preview_end, ev.spline_grad_preview_end);
    merge_flag(pending_.undo_before_spline, ev.undo_before_spline);
    merge_flag(pending_.undo_before_object, ev.undo_before_object);
    merge_flag(pending_.apply_path_rule, ev.apply_path_rule);
    merge_flag(pending_.open_path_rule_dialog, ev.open_path_rule_dialog);
    merge_flag(pending_.open_extra_lines_dialog, ev.open_extra_lines_dialog);
    merge_flag(pending_.closed, ev.closed);
    merge_flag(pending_.switch_map, ev.switch_map);
    merge_flag(pending_.toggled_markers, ev.toggled_markers);
    merge_flag(pending_.load_catalog_file, ev.load_catalog_file);
    merge_flag(pending_.load_catalog_directory, ev.load_catalog_directory);
    merge_flag(pending_.spawn_random_scan_folder, ev.spawn_random_scan_folder);
    merge_flag(pending_.open_catalog_folder, ev.open_catalog_folder);
    merge_flag(pending_.edit_catalog_file, ev.edit_catalog_file);
    merge_flag(pending_.reload_catalog_file, ev.reload_catalog_file);
    merge_flag(pending_.open_spline_creator, ev.open_spline_creator);
    merge_flag(pending_.open_instance_map, ev.open_instance_map);
    merge_flag(pending_.spline_place_commit, ev.spline_place_commit);
    merge_flag(pending_.request_load_map, ev.request_load_map);
    merge_flag(pending_.request_open_tile_map, ev.request_open_tile_map);
    merge_flag(pending_.show_shortcuts_dialog, ev.show_shortcuts_dialog);
    merge_flag(pending_.open_entrypoint_picker, ev.open_entrypoint_picker);
    merge_flag(pending_.open_options_dialog, ev.open_options_dialog);
    merge_flag(pending_.apply_editor_options, ev.apply_editor_options);
    if (ev.apply_editor_options) {
        pending_.options_tile_radius = ev.options_tile_radius;
        pending_.options_visual_radius_m = ev.options_visual_radius_m;
        pending_.options_union_pct = ev.options_union_pct;
        pending_.options_grad_pct = ev.options_grad_pct;
    }
    merge_flag(pending_.spawn_omsi_bus, ev.spawn_omsi_bus);
    if (ev.spawn_omsi_bus) pending_.omsi_bus_path = ev.omsi_bus_path;
    merge_flag(pending_.omsi_bus_despawn, ev.omsi_bus_despawn);
    merge_flag(pending_.omsi_bus_reset, ev.omsi_bus_reset);
    merge_flag(pending_.save_ui_settings, ev.save_ui_settings);
    merge_flag(pending_.save_geo_settings, ev.save_geo_settings);
    merge_flag(pending_.reload_file_textures, ev.reload_file_textures);
    merge_flag(pending_.convert_world_coords, ev.convert_world_coords);
    merge_flag(pending_.download_tile_aerial, ev.download_tile_aerial);
    merge_flag(pending_.delete_tile_aerial, ev.delete_tile_aerial);
    merge_flag(pending_.apply_tile_dtm, ev.apply_tile_dtm);
    merge_flag(pending_.import_tile_dem, ev.import_tile_dem);
    if (ev.import_tile_dem) pending_.import_tile_dem_path = ev.import_tile_dem_path;
    merge_flag(pending_.create_tile, ev.create_tile);
    if (ev.create_tile) {
        pending_.create_tile_dx = ev.create_tile_dx;
        pending_.create_tile_dy = ev.create_tile_dy;
    }
    merge_flag(pending_.delete_tile, ev.delete_tile);
    merge_flag(pending_.attach_to_object, ev.attach_to_object);
    merge_flag(pending_.attach_to_spline, ev.attach_to_spline);
    merge_flag(pending_.parent_to_object, ev.parent_to_object);
    merge_flag(pending_.spline_split, ev.spline_split);
    if (ev.spline_split && ev.spline_split_cuts > 0) pending_.spline_split_cuts = ev.spline_split_cuts;
    merge_flag(pending_.toggle_editor_hide, ev.toggle_editor_hide);
    merge_flag(pending_.ground_apply, ev.ground_apply);
    if (ev.ground_apply) {
        pending_.ground_apply_index = ev.ground_apply_index;
        pending_.ground_apply_main = ev.ground_apply_main;
        pending_.ground_apply_detail = ev.ground_apply_detail;
        pending_.ground_apply_main_rep = ev.ground_apply_main_rep;
        pending_.ground_apply_det_rep = ev.ground_apply_det_rep;
    }
    merge_flag(pending_.water_create, ev.water_create);
    merge_flag(pending_.water_delete, ev.water_delete);
    merge_flag(pending_.water_set, ev.water_set);
    if (ev.water_set || ev.water_create) for (int i = 0; i < 4; ++i) pending_.water_h[i] = ev.water_h[i];
    merge_flag(pending_.group_select_same, ev.group_select_same);
    if (ev.group_select_same) pending_.group_select_attach = ev.group_select_attach;
    merge_flag(pending_.group_new_gesture, ev.group_new_gesture);
    merge_flag(pending_.group_apply_delta, ev.group_apply_delta);
    merge_flag(pending_.group_set_z, ev.group_set_z);
    merge_flag(pending_.group_sco_flags, ev.group_sco_flags);
    merge_flag(pending_.group_spline_align, ev.group_spline_align);
    merge_flag(pending_.group_split, ev.group_split);
    merge_flag(pending_.group_replace_apply, ev.group_replace_apply);
    merge_flag(pending_.group_list_apply, ev.group_list_apply);
    if (ev.group_apply_delta || ev.group_set_z) pending_.group_about_center = ev.group_about_center;
    if (ev.group_apply_delta) {
        for (int i = 0; i < 4; ++i) pending_.group_delta[i] += ev.group_delta[i];
        for (int i = 0; i < 2; ++i) pending_.group_delta_pb[i] += ev.group_delta_pb[i];
    }
    if (ev.group_set_z) pending_.group_z = ev.group_z;
    if (ev.group_sco_flags) { pending_.group_sco_align = ev.group_sco_align; pending_.group_sco_tangential = ev.group_sco_tangential; }
    if (ev.group_spline_align) pending_.group_spline_align_value = ev.group_spline_align_value;
    if (ev.group_replace_apply) { pending_.group_replace_path = ev.group_replace_path; pending_.group_replace_spline = ev.group_replace_spline; }
    if (ev.group_list_apply) pending_.group_rows = ev.group_rows;
    merge_flag(pending_.unhide_all_editor, ev.unhide_all_editor);
    merge_flag(pending_.spline_complete, ev.spline_complete);
    merge_flag(pending_.spline_align_start, ev.spline_align_start);
    merge_flag(pending_.spline_align_end, ev.spline_align_end);
    merge_flag(pending_.adapt_spline_gradient_terrain, ev.adapt_spline_gradient_terrain);
    merge_flag(pending_.adapt_spline_clearance, ev.adapt_spline_clearance);
    if (ev.adapt_spline_clearance && ev.spline_clearance_quality > 0)
        pending_.spline_clearance_quality = ev.spline_clearance_quality;
    merge_flag(pending_.toggle_spline_multi_pick, ev.toggle_spline_multi_pick);
    merge_flag(pending_.toggle_object_multi_pick, ev.toggle_object_multi_pick);
    merge_flag(pending_.start_group_marquee, ev.start_group_marquee);
    merge_flag(pending_.sel_use_circle, ev.sel_use_circle);
    merge_flag(pending_.sel_use_lasso, ev.sel_use_lasso);
    merge_flag(pending_.cancel_group_marquee, ev.cancel_group_marquee);
    merge_flag(pending_.save_selection_preset, ev.save_selection_preset);
    if (ev.spawn_preset_index >= 0) pending_.spawn_preset_index = ev.spawn_preset_index;
    merge_flag(pending_.sli_bake_pick, ev.sli_bake_pick);
    merge_flag(pending_.enter_mesh_edit, ev.enter_mesh_edit);
    merge_flag(pending_.edit_o3d_file, ev.edit_o3d_file);
    merge_flag(pending_.enter_sco_path_edit, ev.enter_sco_path_edit);
    merge_flag(pending_.sco_path_pick_accept, ev.sco_path_pick_accept);
    merge_flag(pending_.enter_sco_traffic, ev.enter_sco_traffic);
    merge_flag(pending_.sli_bake_run, ev.sli_bake_run);
    merge_flag(pending_.sli_bake_replace, ev.sli_bake_replace);
    merge_flag(pending_.tile_bake_run, ev.tile_bake_run);
    merge_flag(pending_.tile_bake_replace, ev.tile_bake_replace);
    merge_flag(pending_.tile_bake_fbx, ev.tile_bake_fbx);
    merge_flag(pending_.tile_bake_clean_o3d, ev.tile_bake_clean_o3d);
    merge_flag(pending_.tile_bake_clean_fbx, ev.tile_bake_clean_fbx);
    merge_flag(pending_.tile_bake_open_folder, ev.tile_bake_open_folder);
    if (ev.tile_bake_parts >= 1 && ev.tile_bake_parts <= 5)
        pending_.tile_bake_parts = ev.tile_bake_parts;
    merge_flag(pending_.create_lightmap, ev.create_lightmap);
    merge_flag(pending_.do_whole_map, ev.do_whole_map);
    merge_flag(pending_.aerial_missing, ev.aerial_missing);
    merge_flag(pending_.aerial_all, ev.aerial_all);
    merge_flag(pending_.chrono_plus, ev.chrono_plus);
    merge_flag(pending_.chrono_minus, ev.chrono_minus);
    merge_flag(pending_.chrono_remove, ev.chrono_remove);
    merge_flag(pending_.ground_create_layer, ev.ground_create_layer);
    merge_flag(pending_.ground_delete_layer, ev.ground_delete_layer);
    merge_flag(pending_.ground_edit_layer, ev.ground_edit_layer);
    merge_flag(pending_.refresh_tt, ev.refresh_tt);
    merge_flag(pending_.open_tt_file, ev.open_tt_file);
    merge_flag(pending_.tt_create_track, ev.tt_create_track);
    merge_flag(pending_.tt_delete_track_entry, ev.tt_delete_track_entry);
    merge_flag(pending_.tt_create_trip_t1, ev.tt_create_trip_t1);
    merge_flag(pending_.tt_create_trip_t2, ev.tt_create_trip_t2);
    merge_flag(pending_.tt_delete_station, ev.tt_delete_station);
    merge_flag(pending_.tt_edit_profiles, ev.tt_edit_profiles);
    merge_flag(pending_.tt_assistent, ev.tt_assistent);
    merge_flag(pending_.tt_add_from_list, ev.tt_add_from_list);
    merge_flag(pending_.tt_change_type2, ev.tt_change_type2);
    merge_flag(pending_.tt_copy_track, ev.tt_copy_track);
    merge_flag(pending_.tt_copy_trip, ev.tt_copy_trip);
    merge_flag(pending_.tt_copy_both, ev.tt_copy_both);
    merge_flag(pending_.tt_edit_timetable, ev.tt_edit_timetable);
    merge_flag(pending_.tt_find_errors, ev.tt_find_errors);
    merge_flag(pending_.tt_track_entry_sel_changed, ev.tt_track_entry_sel_changed);
    merge_flag(pending_.tt_station_sel_changed, ev.tt_station_sel_changed);
    merge_flag(pending_.tt_track_combo_changed, ev.tt_track_combo_changed);
    merge_flag(pending_.tt_trip_combo_changed, ev.tt_trip_combo_changed);
    merge_flag(pending_.tt_request_camera_jump, ev.tt_request_camera_jump);
    if (ev.tt_track_entry_list_index >= 0) pending_.tt_track_entry_list_index = ev.tt_track_entry_list_index;
    if (ev.tt_station_list_index >= 0) pending_.tt_station_list_index = ev.tt_station_list_index;
    if (ev.tt_track_combo_index >= 0) pending_.tt_track_combo_index = ev.tt_track_combo_index;
    if (ev.tt_trip_combo_index >= 0) pending_.tt_trip_combo_index = ev.tt_trip_combo_index;
    if (ev.tt_request_camera_jump && (ev.tt_camera_target[0] != 0.f || ev.tt_camera_target[1] != 0.f || ev.tt_camera_target[2] != 0.f)) {
        pending_.tt_camera_target[0] = ev.tt_camera_target[0];
        pending_.tt_camera_target[1] = ev.tt_camera_target[1];
        pending_.tt_camera_target[2] = ev.tt_camera_target[2];
    }
    merge_flag(pending_.sl_filter, ev.sl_filter);
    merge_flag(pending_.sl_new, ev.sl_new);
    merge_flag(pending_.sl_search_err, ev.sl_search_err);
    merge_flag(pending_.sl_delete, ev.sl_delete);
    if (ev.sl_mode >= 0) pending_.sl_mode = ev.sl_mode;
    merge_flag(pending_.tt_train_reversed, ev.tt_train_reversed);

    if (ev.options_tile_radius >= 0) pending_.options_tile_radius = ev.options_tile_radius;
    if (ev.attach_point >= 0) pending_.attach_point = ev.attach_point;
    if (ev.parent_id != 0) pending_.parent_id = ev.parent_id;
    if (ev.ground_layer_sel >= 0) pending_.ground_layer_sel = ev.ground_layer_sel;
    if (ev.chrono_sel >= 0) pending_.chrono_sel = ev.chrono_sel;
    if (ev.tt_mode >= 0) pending_.tt_mode = ev.tt_mode;
    if (ev.options_vsync_set) { pending_.options_vsync = ev.options_vsync; pending_.options_vsync_set = true; }

    if (ev.apply_object) pending_.object_edit = imgui_object_;
    if (ev.apply_spline) pending_.spline_edit = imgui_spline_;
    if (ev.apply_path_rule) pending_.path_rule_edit = imgui_path_rule_;
    if (ev.object_edit.valid) pending_.object_edit = ev.object_edit;
    if (ev.spline_edit.valid) pending_.spline_edit = ev.spline_edit;
    if (ev.path_rule_edit.valid) pending_.path_rule_edit = ev.path_rule_edit;

    if (!ev.switch_map_path.empty()) pending_.switch_map_path = ev.switch_map_path;
    if (!ev.tt_file_to_open.empty()) pending_.tt_file_to_open = ev.tt_file_to_open;
    if (!ev.tt_terminus.empty()) pending_.tt_terminus = ev.tt_terminus;
    if (!ev.tt_line.empty()) pending_.tt_line = ev.tt_line;

    if (ev.convert_world_coords || ev.download_tile_aerial || ev.apply_tile_dtm || ev.create_tile ||
        ev.delete_tile) {
        pending_.tile_geo_edit = imgui_state_.tile_geo;
        pending_.tile_aerial_visible = imgui_state_.tile_aerial_visible;
        pending_.aerial_resolution_pos = imgui_state_.aerial_resolution_pos;
    }
}

} // namespace omsi::ui
