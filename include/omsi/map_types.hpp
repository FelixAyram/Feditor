#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "f4f23ma/math.hpp"
#include "omsi/cfg_document.hpp"

namespace omsi::map {

// TEntrypoint record from TMap.loadGlobalFile — 0x30 bytes in memory layout fields.
struct EntryPoint {
    std::int32_t type = 0;
    std::int32_t id = 0;
    std::int32_t link_id = 0;
    f4f23ma::f4 position_local{};
    f4f23ma::f4 position{}; // same as position_local (OMSI stores world coords in global.cfg)
    f4f23ma::f4 rotation_quat{}; // x,y,z,w from file
    std::int32_t category = 0;
    std::string name;
};

// Used when global.cfg has no [entrypoint] blocks (editor still needs a spawn pose).
[[nodiscard]] inline EntryPoint default_spawn_entrypoint() {
    EntryPoint ep{};
    ep.name = "(origin)";
    ep.position = {0.f, 0.f, 0.f, 1.f};
    ep.position_local = ep.position;
    ep.rotation_quat = {0.f, 0.f, 0.f, 1.f};
    return ep;
}

struct MapTileRef {
    std::int32_t grid_x = 0;
    std::int32_t grid_y = 0;
    std::filesystem::path tile_path;
};

// [mapcam] from TMap.loadGlobalFile / OMSI Editor view restore.
struct MapCam {
    bool valid = false;
    std::int32_t grid_x = 0;
    std::int32_t grid_y = 0;
    f4f23ma::f4 position{}; // tile-local X,Y,Z (Y up in OMSI cfg)
    f4f23ma::f32 yaw_deg = 0.f;
    f4f23ma::f32 pitch_deg = 0.f;
    f4f23ma::f32 distance = 50.f;
};

// [groundtex] diffuse / detail / … / tile meters (Blender-O3D-IO).
struct GroundTex {
    std::filesystem::path diffuse;
    std::filesystem::path detail;
    f4f23ma::f32 scale = 60.f; // detail repeats
    int flag = 0;
    int main_rep = 1;
};

struct GlobalMap {
    std::filesystem::path root;
    std::filesystem::path global_cfg_path;
    std::string name;
    std::string friendly_name;
    bool world_coordinates = false;
    MapCam mapcam{};
    std::vector<GroundTex> ground_textures;
    std::vector<MapTileRef> tiles;
    std::vector<EntryPoint> entrypoints;
    struct UnschedVehGroup {
        std::string name;
        int path_mode = 1; // 0 = locked until rules, 1 = free
    };
    std::vector<UnschedVehGroup> unsched_vehgroups;
    struct UnschedTrafficDens {
        std::string group_name;
        float factor = 1.f;
        int day_mask = 0;
        std::vector<float> hour_values;
    };
    std::vector<UnschedTrafficDens> unsched_trafficdens;
};

// Tile .map [rule] / [kill_rule] after a spline/object (Unit126 / Unit84 TPathRule_Road).
struct PathRuleEntry {
    std::int32_t path_index = 0;
    bool kill = false; // [kill_rule] → clear / deactivate
    std::string keyword; // speedlimit|trafficdensity|priority|no_cars|buses|trucks|overtaking_prohib
    f4f23ma::f32 value = 0.f;
    std::int32_t extra = 0; // trafficdensity: UVG index
};

struct MapObjectInst {
    std::int32_t flags = 0;
    std::filesystem::path sco_path;
    std::int32_t id = 0;
    f4f23ma::f4 position{};
    f4f23ma::f4 rotation_deg{}; // ZYX euler degrees (OMSI convention)
    std::int32_t obj_type = 0;
    std::string tree_texture;
    f4f23ma::f32 tree_height = 0.f;
    f4f23ma::f32 tree_aspect = 1.f;
    // TMapKachel.loadMapFile Part5: count + N strings (Svarstrings / texttexture lines).
    std::int32_t string_line_count = 0;
    std::vector<std::string> strings;
    std::vector<PathRuleEntry> path_rules;
    // [varparent] parent object id code (TFileObject.myvarparent_IDCode, map version >= 6).
    std::int32_t varparent_id = 0;
    // [spline_terrain_align] after [object] — AlignTerrain for this SCO (Z on heightfield).
    bool align_terrain = false;
};

// [splineAttachement] / [splineAttachement_repeater] — Unity MapSplineAttachmentEntry.
struct MapSplineAttachmentInst {
    std::filesystem::path sco_path;
    std::int32_t instance_id = 0;
    std::int32_t spline_index = 0;
    float offset_x = 0.f;
    float offset_y = 0.f;
    float offset_along = 0.f;
    float rotation_y = 0.f;
    float rotation_x = 0.f; // pitch (adelante) — .map line after rotation_y
    float rotation_z = 0.f; // bank (lateral)
    float repeat_step = 0.f;
    float repeat_range = 0.f;
    // Body line after repeat_range: 1 = Tangential to spline, 0 = fixed rotation.
    bool tangential = false;
    // AlignTerrain for SA is runtime/editor (Z on heightfield); default off for SA.
    bool align_terrain = false;
    bool is_repeater = false;
    // Count line in .map (e.g. 7) — may include empty slots; 0 => use object_text_lines.size().
    std::int32_t object_text_line_count = 0;
    std::vector<std::string> object_text_lines;
    // [varparent] on spline attachment instance (TFileObject.myvarparent_IDCode).
    std::int32_t varparent_id = 0;
};

// [attachObj] — child SCO parented to another instance (Unity MapAttachObjectEntry).
struct MapAttachObjectInst {
    std::filesystem::path sco_path;
    std::int32_t child_id = 0;
    std::int32_t parent_id = 0;
    // .map after parent id: extra_rot_y, attach_point, extra_rot_x, extra_rot_y2, extra_rot_z.
    // Same euler slots as [object] inst[6..8] for the last three: heading, pitch, bank.
    float extra_rot_y = 0.f; // inst[4] extra heading (often 0)
    int attach_point_index = 0;
    float extra_rot_x = 0.f; // inst[6] extra heading (e.g. 90 / 270 on LED_Ped)
    float extra_rot_y2 = 0.f; // inst[7] extra pitch
    float extra_rot_z = 0.f; // inst[8] extra bank
    int options_flag = 0;
    std::string label;
    // Count + lines after options_flag (same as [splineAttachement] / [object] extra strings).
    std::int32_t object_text_line_count = 0;
    std::vector<std::string> object_text_lines;
    // [varparent] after attachObj block (TFileObject.myvarparent_IDCode on child).
    std::int32_t varparent_id = 0;
    // [spline_terrain_align] after [attachObj]
    bool align_terrain = false;
};

// [spline] / [spline_h] — OMSI .map after path: id, prev_id, next_id (next = successor at end).
// File stores engine Y-up (x,height,y); we remap to Z-up position (x,y,height).
struct SplineInst {
    std::int32_t flags = 0;
    std::filesystem::path sli_path;
    std::int32_t id = 0;
    std::int32_t next_id = 0;
    std::int32_t prev_id = 0;
    f4f23ma::f4 position{}; // Z-up tile-local (x, y, height)
    f4f23ma::f32 heading_deg = 0.f;
    f4f23ma::f32 length = 0.f;
    f4f23ma::f32 radius = 0.f;
    f4f23ma::f32 start_grad = 0.f; // percent
    f4f23ma::f32 end_grad = 0.f;
    bool is_spline_h = false;
    bool use_delta_height = false;
    f4f23ma::f32 delta_height = 0.f;
    f4f23ma::f32 cant_start = 0.f;
    f4f23ma::f32 cant_end = 0.f;
    f4f23ma::f32 skew_start = 0.f;
    f4f23ma::f32 skew_end = 0.f;
    // Trailing optional localLength on [spline]/[spline_h]; absent or ≈ length → 0. along = offsetAlong - local_length.
    f4f23ma::f32 local_length = 0.f;
    f4f23ma::f32 length_accum = 0.f;
    bool mirror = false;
    // TFileSpline.alignTerrain: [spline_terrain_align] => 1, [spline_terrain_align_2] + 0..255.
    // 0 = off; 1..4 = terrain hole (both / start / end / no THs). Does not drape the spline mesh.
    std::uint8_t terrain_align = 0;
    std::vector<PathRuleEntry> path_rules;
};

// TMapKachel.objectsFromFile — "Object Nr. N" labels index this interleaved list.
// Real .map: [object] | [attachObj] | [splineAttachement(_repeater) only (never [spline]).
enum class MapObjectsFromFileKind { Object, Attach, SplineAttach };

struct MapObjectsFromFileRef {
    MapObjectsFromFileKind kind = MapObjectsFromFileKind::Object;
    std::size_t index = 0;
};

struct MapTileData {
    std::filesystem::path path;
    std::int32_t grid_x = 0;
    std::int32_t grid_y = 0;
    std::vector<MapObjectInst> objects;
    std::vector<MapAttachObjectInst> attachments;
    std::vector<MapSplineAttachmentInst> spline_attachments;
    std::vector<SplineInst> splines;
    // File order of objectsFromFile entries (preserved on load; rebuilt on editor save).
    std::vector<MapObjectsFromFileRef> objects_from_file_order;
    // OMSI: [splineAttachement] index → splinesFromFile[index] pointer at load time.
    // Ids in pure .map parse order; never cleared/rebuilt by sync_tile_lists_only.
    std::vector<std::int32_t> splines_from_file_order;
    bool has_terrain = false;
};

// Fill length_accum when missing: sum of previous chain segment lengths via prev_id.
void compute_spline_length_accum(std::vector<SplineInst>& splines);

// Patch [spline_terrain_align]/[_2] for one instance id in a .map file.
[[nodiscard]] bool update_spline_terrain_align_in_map(const std::filesystem::path& map_path,
                                                     std::int32_t spline_id, std::uint8_t terrain_align);
[[nodiscard]] bool update_object_align_terrain_in_map(const std::filesystem::path& map_path,
                                                     std::int32_t object_id, bool align_terrain);
// Write/remove [spline_terrain_align] under [object]|[splineAttachement]|[attachObj] by instance id.
[[nodiscard]] bool update_sco_align_terrain_in_map(const std::filesystem::path& map_path,
                                                  std::string_view block_tag,
                                                  std::int32_t instance_id, bool align_terrain);
[[nodiscard]] bool update_sa_tangential_in_map(const std::filesystem::path& map_path,
                                              std::int32_t instance_id, bool tangential);

[[nodiscard]] std::optional<GlobalMap> load_global_map(const std::filesystem::path& global_cfg);

// Write [entrypoints] section back to global.cfg (preserves UTF-16 LE if present).
[[nodiscard]] bool save_global_entrypoints(const std::filesystem::path& global_cfg,
                                           const std::vector<EntryPoint>& entrypoints);

// category = index of tile (grid_x, grid_y) in global.cfg [map] order (same as OMSI TEntrypoint).
[[nodiscard]] std::int32_t entrypoint_category_for_tile(const GlobalMap& global,
                                                        std::int32_t grid_x, std::int32_t grid_y);

[[nodiscard]] bool upsert_global_entrypoint(GlobalMap& global, const EntryPoint& ep);
// Remove entrypoint record(s) that reference this map object id; rewrite global.cfg.
[[nodiscard]] bool remove_global_entrypoint_by_object_id(GlobalMap& global, std::int32_t object_id);

[[nodiscard]] std::optional<MapTileData> load_map_tile(const std::filesystem::path& tile_map,
                                                       const std::filesystem::path& omsi_root);
// Drop cached .map parse (after editor write). Empty path clears all.
void invalidate_map_tile_cache(const std::filesystem::path& tile_map = {});

// Chrono scenarios (<map>/Chrono/**/Chrono.cfg, Omsi.exe / openOMSI): a scenario is in force from its
// [startdate] to its [enddate] (YYYYMMDD, either may be missing; one with neither never is). While some are
// active, load_map_tile lays each one's tile_x_y.map over the base tile, in the game's order: its objects /
// splines / attachments are added and its [selobject] / [selspline] records change what carries that id
// ([delete], [typ], [relabel], [rule] / [kill_rule]).
struct ChronoScenario {
    std::filesystem::path dir;
    std::string name;
    std::int32_t start_date = 0;
    std::int32_t end_date = 0;
    [[nodiscard]] bool active_on(std::int32_t date) const {
        return (start_date != 0 || end_date != 0) && (start_date == 0 || date >= start_date) &&
               (end_date == 0 || date <= end_date);
    }
};
[[nodiscard]] std::vector<ChronoScenario> list_chrono_scenarios(const std::filesystem::path& map_root);
// Sets the scenarios load_map_tile applies (empty = the plain map) and drops the tile parse cache.
void set_active_chrono_dirs(std::vector<std::filesystem::path> dirs);
[[nodiscard]] std::vector<std::filesystem::path> active_chrono_dirs();
[[nodiscard]] std::vector<std::filesystem::path> discover_maps(const std::filesystem::path& maps_dir);

} // namespace omsi::map
