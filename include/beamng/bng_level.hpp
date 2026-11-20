#pragma once
// BeamNG level model: info.json, the SimGroup scene tree (main/**/items.level.json), terrain block, forest,
// materials. Every scene object keeps its original JSON so fields the editor doesn't model are written back as-is.
#include "beamng/bng_json.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bng {

class Vfs;

struct SpawnPointInfo {
    std::string object_name;   // SpawnSphere name in the scene
    std::string translation_id;
    std::string preview;
};

struct LevelInfo {
    std::string name;          // folder name ("gridmap_v2")
    std::string title;         // display title (may be a translation id)
    std::string dir;           // "/levels/gridmap_v2"
    std::vector<std::string> previews;
    std::vector<SpawnPointInfo> spawn_points;
    std::string default_spawn;
    float size[2]{0.f, 0.f};
    std::string source;        // zip or folder it came from
};

std::vector<LevelInfo> discover_levels(const Vfs& vfs);

// Spawn object names found by streaming the scene files (main/**/items.level.json, or items.level.json at the
// level root when there is no main/): SpawnSphere, PlayerSpawn, BeamNGWorldSpawn and TSStatic whose name contains
// "spawn" / "playerstart". Used when info.json has no spawnPoints. Safe to call from a worker thread.
std::vector<std::string> scan_scene_spawns(const Vfs& vfs, const LevelInfo& info);

// 3x3 rotation, row-major (r[row*3+col]) and applied to column vectors. BeamNG/Torque writes
// "rotationMatrix" COLUMN-major (array[col*3+row]), so the reader stores the transpose: local X is the
// world direction (r[0], r[3]) and local Y is (r[1], r[4]) — see mat3_from_rotation_matrix().
using Mat3 = std::array<float, 9>;
inline Mat3 mat3_identity() { return {1, 0, 0, 0, 1, 0, 0, 0, 1}; }

struct SceneObject {
    std::uint32_t id = 0;                // editor id (stable for the session)
    std::string cls;                     // "TSStatic", "SimGroup", ...
    std::string name;
    std::string persistent_id;
    std::string parent;                  // __parent group name
    std::string file;                    // VFS path of the items.level.json this row was read from (save target)
    int group_index = -1;                // index into Level::groups
    float pos[3]{0.f, 0.f, 0.f};
    Mat3 rot = mat3_identity();
    float scale[3]{1.f, 1.f, 1.f};
    bool has_pos = false, has_rot = false, has_scale = false;
    JsonValue raw;                       // full original object (updated on save)
    std::string shape;                   // resolved shapeName for TSStatic / prefab child (normalized VFS path)
    bool dirty = false;
    bool deleted = false;
    bool hidden = false;                 // editor-only visibility
    bool locked = false;                 // editor-only lock
};

struct SceneGroup {
    std::string name;
    std::string parent;                  // parent group name ("" for MissionGroup)
    std::string file;                    // items.level.json this group's objects live in
    std::vector<std::uint32_t> objects;  // object ids, in file order
    std::vector<int> children;           // child group indices
    int self_object = -1;                // the SimGroup object id that declares this group (in its parent's file)
};

// Terrain (.ter). Heights are raw u16; world height = h / 65535 * max_height + origin.z.
struct TerrainData {
    std::string file;                    // VFS path of the .ter
    std::uint8_t version = 9;
    std::uint32_t size = 0;
    std::vector<std::uint16_t> heights;  // size*size, row-major, row 0 = y min
    std::vector<std::uint8_t> layers;    // size*size material index (255 = hole)
    std::vector<std::string> material_names;
    float origin[3]{0.f, 0.f, 0.f};      // TerrainBlock.position
    float square_size = 1.f;             // metres per sample
    float max_height = 0.f;
    std::uint32_t object_id = 0;
    bool dirty = false;

    float height_at_sample(std::uint32_t x, std::uint32_t y) const {
        return static_cast<float>(heights[static_cast<std::size_t>(y) * size + x]) / 65535.f * max_height + origin[2];
    }
    float sample_world(float wx, float wy) const; // bilinear, clamped
    bool contains(float wx, float wy) const;
};

std::optional<TerrainData> read_ter(const std::vector<std::uint8_t>& bytes, std::string* err = nullptr);
std::vector<std::uint8_t> write_ter(const TerrainData& t);

// Forest instances (forest/*.forest4.json) + item data (art/forest/managedItemData.json).
struct ForestItemType {
    std::string name;                    // key used by forest4 "type"
    std::string shape_file;              // resolved .dae
    float radius = 1.f;
    JsonValue raw;
};
struct ForestInstance {
    std::uint32_t id = 0;
    int type = -1;                       // index into Level::forest_types
    float pos[3]{0.f, 0.f, 0.f};
    Mat3 rot = mat3_identity();
    float scale = 1.f;
    int ctxid = 0;
    bool deleted = false;
};

// Decals: main.decals.json (instances) + art/decals/managedDecalData.json (DecalData types).
struct DecalType {
    std::string name;                    // key used by main.decals.json "instances"
    std::string material;                // "material" of the DecalData (= Material name / mapTo)
    int tex_rows = 1, tex_cols = 1;
    float size = 1.f;                    // default size (instances usually override it)
    float render_priority = 1.f;
    std::vector<std::array<float, 4>> texture_coords; // u, v, w, h (rectIdx = index)
    JsonValue raw;
};
struct DecalInstance {
    std::uint32_t id = 0;
    int type = -1;                       // index into Level::decal_types
    int rect = 0;                        // rectIdx into DecalType::texture_coords
    float size = 1.f;
    float render_priority = 0.f;
    float pos[3]{0.f, 0.f, 0.f};
    float normal[3]{0.f, 0.f, 1.f};
    float tangent[3]{1.f, 0.f, 0.f};
    std::uint32_t uid = 0;
};

// Materials (*.materials.json "Material" + "TerrainMaterial").
struct MaterialStage {
    std::string color_map, normal_map, roughness_map, metallic_map, ao_map, opacity_map, emissive_map, detail_map;
    std::string detail_normal_map;
    float base_color[4]{1.f, 1.f, 1.f, 1.f};
    float roughness = 1.f, metallic = 0.f;
    float opacity_factor = 1.f; // opacityFactor (tyre marks / decals with only an opacityMap)
    float detail_scale[2]{1.f, 1.f};
    float detail_far = 0.f; // detailDistances far edge in metres (0 = shader default 80 m)
    bool vert_color = false;
};
struct Material {
    std::string name;
    std::string map_to;
    std::string file;                    // the materials.json it was read from
    MaterialStage stage0;
    bool alpha_test = false, double_sided = false, translucent = false;
    int alpha_ref = 127;
    // "translucentBlendOp" (Add/SrcAlpha/…), lower-cased. Non-empty implies translucent.
    std::string translucent_blend_op;
    JsonValue raw;
};
struct TerrainMaterialDef {
    std::string key;                     // object key in main.materials.json
    std::string internal_name;           // matches TerrainData::material_names
    std::string base_color, base_normal, macro_color, macro_normal, detail_color, detail_normal, roughness_base,
        ao_base, height_base;
    float detail_size = 2.f, macro_size = 60.f, base_size = 512.f;
    float detail_strength = 1.f, macro_strength = 0.5f;
    float detail_distances[4]{0.f, 10.f, 100.f, 150.f};
    float macro_distances[4]{0.f, 10.f, 100.f, 1000.f};
    JsonValue raw;
};

struct LoadProgress {
    std::string stage;
    float fraction = 0.f;
};

struct Level {
    LevelInfo info;
    std::vector<SceneObject> objects;    // id = index
    std::vector<SceneGroup> groups;
    std::unordered_map<std::string, int> group_by_name;
    std::vector<std::string> warnings;

    std::optional<TerrainData> terrain;
    std::vector<ForestItemType> forest_types;
    std::unordered_map<std::string, int> forest_type_by_name;
    std::vector<ForestInstance> forest;
    std::vector<std::string> forest_files_dirty;

    std::vector<DecalType> decal_types;
    std::unordered_map<std::string, int> decal_type_by_name;
    std::vector<DecalInstance> decals;

    std::unordered_map<std::string, Material> materials;          // by name
    std::unordered_map<std::string, std::string> material_by_map_to; // mapTo -> name
    std::unordered_map<std::string, std::string> material_by_base;   // base name (LOD suffix stripped) -> name
    std::vector<TerrainMaterialDef> terrain_materials;

    SceneObject* find_object(std::uint32_t id) { return id < objects.size() ? &objects[id] : nullptr; }
    const Material* material_for(const std::string& mat_name_or_map_to) const;
    const SceneObject* find_named(std::string_view name) const;
};

// Loads the level's scene tree, terrain, forest and materials (materials of the whole level folder plus the
// shared ones referenced later are resolved lazily by the renderer through Vfs).
// Safe to call from a worker thread.
std::optional<Level> load_level(const Vfs& vfs, const LevelInfo& info, LoadProgress* progress = nullptr);

// Writes back every items.level.json that holds a dirty / deleted / new object, as NDJSON in file order, under
// workspace_root mirroring the path after "/levels/<level>/" (the game install is never written). Untouched rows
// are written byte-for-byte from their raw JSON; dirty rows get position / rotationMatrix (column-major) / scale
// refreshed. An existing workspace file is kept as <file>.bak. Clears `dirty` on success.
struct SceneSaveResult {
    bool ok = false;
    std::vector<std::string> written;    // absolute paths
    std::size_t rows = 0, changed = 0, removed = 0;
    std::string error;
};
SceneSaveResult save_scene(Level& lvl, const std::filesystem::path& workspace_root);

// New scene row (TSStatic / Prefab / any class) appended to the group `parent` (created file = that group's file).
// Returns the new object id. persistentId is a fresh UUID v4.
std::uint32_t add_scene_object(Level& lvl, std::string cls, std::string parent, JsonValue raw);
std::string make_persistent_id();
// Serialised forms used for edited rows (rotationMatrix is written column-major, like the game).
JsonValue json_vec3(const float v[3]);
JsonValue json_rotation_matrix(const Mat3& row_major);

// Reads main.decals.json (instances) + art/decals/managedDecalData.json (types/materials/UV rects).
void load_decals(const Vfs& vfs, const LevelInfo& info, Level& lvl);

// Reads every *materials.json under dir into the level's tables (later files override earlier ones).
// Base material name: lower-cased, with the per-LOD decorations removed ("_lod", "lod_", "_far", trailing
// digits/_/-). Used to resolve a LOD material name ("tree_leaves_01_lod") against the real one and to pair the
// submeshes of different LODs of the same shape.
std::string material_base_name(const std::string& n);

void load_materials_under(const Vfs& vfs, std::string_view dir, Level& lvl);

// Prefab contents flattened to shapes in prefab space (nested prefabs / SimGroups resolved).
struct PrefabChild {
    std::string shape;   // resolved .dae
    Mat3 rot = mat3_identity();
    float pos[3]{0.f, 0.f, 0.f};
    float scale[3]{1.f, 1.f, 1.f};
};
std::vector<PrefabChild> load_prefab(const Vfs& vfs, const std::string& file, const std::string& level_dir, int depth = 0);

// Spawn transform: SpawnSphere named spawn (or default / first one), falls back to terrain centre.
bool find_spawn(const Level& lvl, std::string_view spawn_name, float out_pos[3], float* out_heading_rad);

// Pose helpers: forward = rot * (0,1,0) etc. BeamNG writes every rotationMatrix (items.level.json scene
// objects, prefab.json children and forest4 lines) COLUMN-major; all of them are normalised to row-major
// (r[row*3+col]) on load. Exposed so the parser test can lock the convention down.
Mat3 mat3_from_rotation_matrix(const JsonValue& v, bool& has);
void mat3_to_world4x4(const Mat3& r, const float scale[3], const float pos[3], float out16[16]);
float mat3_heading(const Mat3& r); // rotation about Z (radians)
Mat3 mat3_from_euler_zyx(float rz, float ry, float rx);

} // namespace bng
