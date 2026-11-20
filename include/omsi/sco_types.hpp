#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "f4f23ma/math.hpp"
#include "omsi/model_cfg_types.hpp"

namespace omsi::sco {

struct SceneryMeshRef {
    std::filesystem::path mesh_file; // .o3d or .x under sco folder / model/
    bool is_o3d = false;
    std::vector<model_cfg::MeshAnimLayer> animations;
    // OMSI [LOD] screensize threshold for this mesh (1 = always / highest detail band).
    float lod_screensize = 1.f;
    int lod_index = 0;
};

// .sco [texttexture]: string var (name or stringvarlist index), OFT font, size, RGB.
struct ScoTextTexture {
    int source_index = 0; // used when string_var is empty (index into stringvarnamelist / map extra-lines)
    std::string string_var; // named (S.$x) when the first line is not an integer
    std::string font_name;  // [newfont] name in OMSI 2\Fonts\*.oft
    int width = 128;
    int height = 128;
    int color_mode = 0; // 0 = einfarbig (RGB below), 1 = atlas colors
    int r = 255;
    int g = 255;
    int b = 255;
    // [texttexture_enh] extras (OMSI TOFTTex): 0=left, 1=center, 2=right.
    // Plain [texttexture] keeps center (legacy F4 / typical signs).
    int align = 1;
    int letter_spacing = 0;
};

struct TextTextureImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; // RGBA8, transparent outside glyphs
};

// OMSI [tree] billboard (MergeMapTrees / editor) — game draws this, not treehelper.x.
struct TreeBillboard {
    bool valid = false;
    std::string texture; // file name relative to sco folder (often overridden by .map)
    float height = 12.f;
    float width = 0.f;  // 0 → height * ratio
    float ratio = 0.85f;
};

// [new_attachment] attach point on a parent SCO (attachObj / spline attachments).
struct AttachmentPoint {
    f4f23ma::f4 local_pos{}; // OMSI object space (Z-up)
    float rot_x = 0.f;
    float rot_y = 0.f;
    float rot_z = 0.f;
    // attach_trans / attach_rot_* applied in file order (openOMSI attachment_matrix): row-vector
    // (Direct3D) layout, row-major - the same memory as a column-major column-vector matrix
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

// [path] in .sco — 12 floats (Unity OMSIScoParser / Omsi.exe TObjectPathInfo).
struct ScoPath {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float heading_deg = 0.f;
    float radius = 0.f;
    float length = 0.f; // meters (or degrees-of-arc in rare .sco — treat as meters)
    float grad_start = 0.f;
    float grad_end = 0.f;
    int type = 0;
    float width = 1.f;
    int direction = 0;
    int blinker = 0;
    // Runtime after [crossing_heightdeformation] (OMSI path +0x28 / +0x2c).
    float height_delta = 0.f;
    bool height_deformed = false;
    // Optional [use_traffic_light] N after the path block (traffic light group index).
    bool has_use_traffic_light = false;
    int use_traffic_light = -1;
    bool crossing_problem = false; // [crossingproblem]
    bool block_path = false;       // [blockpath]
};

// Unit128 FOS_* special object kinds (helparrow=8, trafficlight=4, …).
// Matches Unit128 TComplMapObj +0x1c4 (TFileObjectSpecial / FOS_*).
enum class ScoSpecial : std::uint8_t {
    None = 0,
    Busstop = 1,      // [busstop] — 7 string slots
    EntryPoint = 3,   // [entrypoint] — 1 string (name); also listed in global.cfg
    TrafficLight = 4, // [trafficlight] — 1 string (nr_traffic_light)
    Signal = 5,       // [signal] — 1 string
    Tree = 6,         // [tree] — texture/height/ratio (map may write type 4 + 3 lines)
    Carpark = 7,      // [carpark_p]
    HelpArrow = 8,    // [helparrow]
    Switch = 9,       // [switch]
};

// Default string-slot count from special type (before stringvarnamelist expansion).
[[nodiscard]] inline int special_default_string_slots(ScoSpecial s) {
    switch (s) {
    case ScoSpecial::Busstop: return 7;
    case ScoSpecial::EntryPoint: return 1;
    case ScoSpecial::TrafficLight: return 1;
    case ScoSpecial::Signal: return 1;
    case ScoSpecial::Tree: return 3;
    default: return 0;
    }
}

// Editor labels for the fixed slots of each special (order = map string index).
[[nodiscard]] inline std::vector<std::string> special_default_string_labels(ScoSpecial s) {
    switch (s) {
    case ScoSpecial::Busstop:
        return {"name", "pass_enter_max", "pass_enter_min", "pass_exit", "dockdist", "side", "subname"};
    case ScoSpecial::EntryPoint:
        return {"name"};
    case ScoSpecial::TrafficLight:
        return {"nr_traffic_light"};
    case ScoSpecial::Signal:
        return {"name"};
    case ScoSpecial::Tree:
        return {"texture", "height", "ratio"};
    default:
        return {};
    }
}

// Unit128 TComplMapObj.priority / [rendertype] (TMapRenderPriority).
enum class ScoRenderPriority : std::uint8_t {
    Presurface = 0, // TMRP_PreSurfaces — TMap.Render pass 0 (before terrain)
    Surface = 1,    // with terrain
    OnSurface = 2,  // road markings / decals after terrain
    Normal = 4,     // default 3D (unknown tag also lands here)
    Layer1 = 3,
    Layer3 = 5,
    Layer4 = 6,
};

// OpenOMSI RenderPhase (omsi-render DRAW_ORDER). Values match gfx::dx12::WorldRenderPhase.
inline std::uint8_t world_render_phase_from_priority(ScoRenderPriority p) {
    switch (p) {
    case ScoRenderPriority::Presurface:
        return 0;
    case ScoRenderPriority::Surface:
        return 2;
    case ScoRenderPriority::OnSurface:
        return 4;
    case ScoRenderPriority::Layer1:
        return 5;
    case ScoRenderPriority::Layer3:
        return 7;
    case ScoRenderPriority::Layer4:
        return 8;
    default:
        return 6;
    }
}
inline bool sco_is_ground_layer(ScoRenderPriority p) {
    return p == ScoRenderPriority::Presurface || p == ScoRenderPriority::Surface ||
           p == ScoRenderPriority::OnSurface;
}

// OMSI [maplight]: bakes into tile .LM.bmp (full intensity to radius, then 1/d^2).
struct ScoMapLight {
    f4f23ma::f4 position{};
    f4f23ma::f4 color{1.f, 1.f, 1.f, 1.f}; // 0..1
    float radius = 10.f;
};

struct ScoLightEnh {
    f4f23ma::f4 position{};
    f4f23ma::f4 direction{0.f, 0.f, -1.f, 0.f};
    f4f23ma::f4 color{1.f, 1.f, 1.f, 1.f};
    float range = 10.f;
    float inner_cone = 0.f;
    float outer_cone = 0.f;
    bool omni = true;
    bool enh2 = false;
    std::string bitmap;
    float size = 0.f;       // corona size (m)
    std::string variable;   // switch variable (traffic light phase, lamp on/off); empty = always
};

struct ScoTrafficPhase {
    int phase = 0;
    float duration = 1.f;
};

struct ScoTrafficStop {
    int path_index = 0;
    float distance = 0.f;
};

struct ScoTrafficLight {
    std::string name;
    std::vector<ScoTrafficPhase> phases;
    float approach_dist = 0.f;
    std::vector<ScoTrafficStop> stops;
};

struct ScoTrafficLightsGroup {
    float cycle = 0.f;
    std::vector<ScoTrafficLight> lights;
};

struct ScoVisibleLink {
    std::string var_name;
    float value = 1.f;
    int mesh_index = -1; // mesh that precedes [visible]
};

// [CTC] on a scenery object (openOMSI DynamicTextureGroup): a script variable picks one of the
// schemes (the [item]s of the .cti files in the folder, items of the same name forming one scheme);
// each scheme replaces the textures that [CTCTexture] names - the advert of a street clock.
struct ScoCtcScheme {
    std::string name;
    std::filesystem::path dir;
    std::vector<std::pair<std::string, std::string>> textures; // ([CTCTexture] name, file)
};
struct ScoCtc {
    std::string var;
    std::vector<ScoCtcScheme> schemes;
};

struct SceneryObject {
    std::filesystem::path sco_path;
    std::string friendly_name;
    // [collision_mesh] / [collisionmesh] — OMSI display mesh when set (Unity GetDisplayMesh).
    std::string collision_mesh;
    // [crossing_heightdeformation] — relative .o3d height plane; not rendered.
    std::string crossing_height_deformation_mesh;
    // [terrainhole] — .o3d volume that cuts the terrain (OMSI editor caches its outline as <mesh>.hole).
    std::string terrain_hole_mesh;
    std::vector<SceneryMeshRef> meshes;
    std::vector<std::string> material_textures;
    // [matl] / [matl_alpha] / … same tags as model.cfg (decals, glass, etc.).
    std::vector<model_cfg::ModelCfgMaterial> materials;
    bool only_editor_mesh = false; // [onlyeditor] — mesh is editor helper (.x), use [tree] instead
    bool no_collision = false;     // [nocollision] / [onlyeditor] — Unit128 +0x155
    bool surface = false;          // [surface] — drive-on ground (Unit128 +0x157)
    bool fixed = false;            // [fixed] / [surface] / [crashmode_pole]
    // [crashmode_pole] mass (t) and break load (kN): a post that breaks off when a vehicle hits it hard
    bool crash_pole = false;
    float crash_pole_mass_t = 0.f;
    float crash_pole_load_kn = 0.f;
    bool active = true;            // default 1; [surface] sets 0
    bool has_boundingbox = false;
    float bbox_w = 0.f;
    float bbox_l = 0.f;
    float bbox_h = 0.f;
    float bbox_off_x = 0.f;
    float bbox_off_y = 0.f;
    float bbox_off_z = 0.f;
    TreeBillboard tree{};
    // [absheight]: map Z is world height. [path]: same (Unity/F4: crossings sit on authored Z, no terrain add).
    bool abs_height = false;
    bool has_path = false;
    int complexity = 0;       // [complexity] — TComplMapObj.complexity (0..3)
    bool lightmap = false;    // [LightMapMapping]
    ScoRenderPriority render_priority = ScoRenderPriority::Normal;
    ScoSpecial special = ScoSpecial::None;
    std::vector<AttachmentPoint> attachment_points;
    std::vector<ScoCtc> ctc;
    std::vector<std::pair<std::string, std::string>> ctc_textures; // [CTCTexture] name, default texture
    std::vector<ScoPath> paths;
    // Names from [stringvarnamelist] .txt (OMSI Svarstrings) — editor "extra lines".
    std::vector<std::string> string_var_names;
    std::vector<std::filesystem::path> script_files; // [script] N / rel.osc
    std::vector<std::filesystem::path> sound_cfgs;   // [sound] rel.cfg (resolved against the .sco folder)
    std::vector<std::filesystem::path> varnamelist_files; // [varnamelist]
    std::vector<std::filesystem::path> stringvarnamelist_files;
    std::vector<ScoMapLight> maplights;
    std::vector<ScoLightEnh> lights;
    std::vector<ScoVisibleLink> visible_links;
    std::optional<ScoTrafficLightsGroup> traffic_lights_group;
    std::vector<ScoTextTexture> text_textures;

    [[nodiscard]] int extra_text_slots() const {
        int n = special_default_string_slots(special);
        const int from_names = static_cast<int>(string_var_names.size());
        if (from_names > n) n = from_names;
        for (const auto& tt : text_textures)
            if (tt.source_index + 1 > n) n = tt.source_index + 1;
        return n;
    }

    // Labels for editor inputs: fixed special labels, then string_var_names for any extra.
    [[nodiscard]] std::vector<std::string> editor_string_labels() const {
        auto labels = special_default_string_labels(special);
        if (!string_var_names.empty()) {
            // Prefer namelist when present (covers generic + extends specials).
            if (labels.empty() || string_var_names.size() >= labels.size())
                return string_var_names;
            // Keep fixed labels, append any extra namelist names beyond fixed count.
            for (std::size_t i = labels.size(); i < string_var_names.size(); ++i)
                labels.push_back(string_var_names[i]);
        }
        return labels;
    }
    [[nodiscard]] bool has_mesh_anims() const {
        for (const auto& m : meshes)
            if (!m.animations.empty()) return true;
        return false;
    }
};

// Raster [texttexture] with the named .oft atlas under omsi_root/Fonts.
// nullopt = atlas BMP not ready yet (do not block main; retry next frame).
[[nodiscard]] std::optional<TextTextureImage> rasterize_text_texture(const ScoTextTexture& tt,
                                                                     std::string_view text,
                                                                     const std::filesystem::path& omsi_root);

// Decode .oft color/alpha BMPs off the render thread (SA worker or font job).
void prefetch_oft_images(std::string_view font_name, const std::filesystem::path& omsi_root);

// .sco is cfg-format; [mesh] lists model files under model/ or same folder.
// Pointer into the process-wide cache (no per-call copy). Null if missing/unreadable.
[[nodiscard]] const SceneryObject* load_sco(const std::filesystem::path& sco_path);
// Cache lookup only — never reads disk. Null if not loaded yet.
[[nodiscard]] const SceneryObject* peek_sco(const std::filesystem::path& sco_path);
void invalidate_sco_cache(const std::filesystem::path& sco_path = {});
[[nodiscard]] std::size_t sco_cache_size();
// Replace [path]/[path_2] blocks; keeps the rest of the .sco. Invalidates cache.
[[nodiscard]] bool rewrite_sco_paths(const std::filesystem::path& sco_path, const std::vector<ScoPath>& paths);
// Paths + optional [traffic_lights_group] rewrite (same file, rest kept).
[[nodiscard]] bool rewrite_sco_paths_and_traffic(const std::filesystem::path& sco_path,
                                                 const std::vector<ScoPath>& paths,
                                                 const std::optional<ScoTrafficLightsGroup>& group);
// Header props rewrite ([friendlyname]/[complexity]/[rendertype]/flags), rest of the file kept.
[[nodiscard]] bool rewrite_sco_header(const std::filesystem::path& sco_path, const std::string& friendly_name,
                                      int complexity, ScoRenderPriority rendertype, bool absheight,
                                      bool onlyeditor, bool nocollision, bool fixed, bool lightmap);
// OMSI [rendertype] tag for a render priority (empty string = omit the tag).
[[nodiscard]] const char* sco_rendertype_tag(ScoRenderPriority p);
// HDD-friendly: unique paths, sorted (sequential seeks), then load once into cache.
// hdd_mode=true → single-threaded sequential; false → parallel after sort.
void preload_scos(const std::vector<std::filesystem::path>& paths, bool hdd_mode = false);

// Skip terrain sample: [absheight] or any [path] (Kreuzung / traffic SCO authored Z).
[[nodiscard]] inline bool skip_terrain_add(const SceneryObject& sco) {
    return sco.abs_height || sco.has_path;
}

// World Z = map Z when skip_terrain_add; else map Z + terrain sample at XY.
[[nodiscard]] inline float world_height(const SceneryObject& sco, float map_z, float terrain_h) {
    if (skip_terrain_add(sco)) return map_z;
    return map_z + terrain_h;
}

} // namespace omsi::sco
