#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace omsi::model_cfg {

// OMSI [matl_alpha] (SDK: Alphakanal): 0 keiner, 1 = 1 Bit ausstechen, 2 = Transparenzen.
enum class MatlAlphaMode : std::uint8_t {
    opaque = 0,
    alpha_test = 1,
    alpha_blend = 2,
};

// One [matl] / [matl_change] block plus following modifier sections until the next [matl].
struct ModelCfgMaterial {
    std::string diffuse_texture; // line 1 of [matl]
    std::string texture_key;     // lowercase path for lookup (OMSI _LStrEqual on texture name)
    int mat_id = -1; // [matl] line 2: 0-based occurrence of this texture name in the O3D, not the global slot.
    int mesh_index = -1; // SCO: [matl] belongs to this [mesh] (0-based). -1 = unknown.

    bool is_matl_change = false;
    std::string change_var;

    MatlAlphaMode alpha_mode = MatlAlphaMode::opaque;
    bool no_zwrite = false;
    bool no_zcheck = false;
    int z_bias = 0;
    bool is_shadow = false;

    std::optional<std::string> transmap_texture;
    std::optional<std::string> envmap_texture;
    float envmap_strength = 0.f;
    bool envmap_realtime = false;
    std::optional<std::string> envmap_mask_texture;
    std::optional<std::string> bumpmap_texture;
    float bumpmap_strength = 0.f;
    std::optional<std::string> nightmap_texture;
    // [matl_change] item 1 ([matl_item] and the modifiers after it): openOMSI 0x5efae8 - a copy of the slot's
    // material with those changes, shown while the change variable is on. Its night map glows whenever the
    // variable is on, by day as well (warning lamps, lit displays, the saloon lights).
    int item_count = 0;
    std::optional<std::string> item_nightmap_texture;
    std::optional<std::string> item_lightmap_texture;
    std::optional<std::string> lightmap_texture;
    // [matl_lightmap] line 2 (vehicles): the variable whose value lights it (cockpit lamps, interior light)
    std::optional<std::string> lightmap_var;
    // [texcoordtransX]/[texcoordtransY]: UV offset = script var (0 = none, 1 = full texture).
    std::optional<std::string> texcoord_trans_x_var;
    std::optional<std::string> texcoord_trans_y_var;
    // [alphascale] var: material alpha multiplied by this script variable (0..1).
    std::optional<std::string> alpha_scale_var;
    // [useTextTexture] N — index into SCO/model [texttexture] list.
    std::optional<int> use_text_texture;
    // [matl_freetex] texture name + friendly name (stringvarnamelist / map label).
    std::optional<std::string> freetex_label;
};

enum class MeshAnimBindingType : std::uint8_t { rotation, translation };
enum class MeshAnimAxis : std::uint8_t { x, y, z };

struct MeshAnimBinding {
    MeshAnimBindingType type = MeshAnimBindingType::rotation;
    // OMSI TAnimSubMesh: both anim_rot and anim_trans act on local X; origin_rot_* reorients.
    MeshAnimAxis axis = MeshAnimAxis::x;
    std::string var_name;
    float scale = 1.f;
    float offset = 0.f;
    std::optional<float> maxspeed; // units/s of (var*scale+offset); unset = snap
    std::optional<float> delay;    // approach rate toward target (1/s style)
};

// One [newanim] block in model.cfg (a mesh may have several).
struct MeshAnimLayer {
    float origin_trans[3]{};
    float origin_rot_deg[3]{}; // origin_rot_x/y/z
    bool origin_from_mesh = false;
    bool has_origin_trans = false;
    std::vector<MeshAnimBinding> bindings;
};

struct ModelCfgMeshEntry {
    std::string mesh_file; // .o3d filename from line after [mesh]
    // [isshadow]: the mesh IS a shadow (a blob drawn darkening the ground). ([shadow] is not this: it only says
    // the mesh casts a shadow - read as a shadow blob, the bodies of the AI cars and parts of buses were never drawn.)
    bool is_shadow_mesh = false;
    bool casts_shadow = false; // [shadow]
    bool pending_isshadow = false;
    std::optional<std::string> mouseevent_var;
    std::optional<std::string> visible_var;
    int visible_default = 1; // [visible] line 2: 0 hidden, 1 shown at load
    std::optional<std::string> mesh_ident;  // [mesh_ident] animation group name
    std::optional<std::string> animparent;  // [animparent] parent mesh_ident
    std::vector<int> illumination_interior;
    // [viewpoint] (OMSI SDK; openOMSI picking.rs / lods.rs): bit 1 = drawn seen from outside, 2 = from inside (the
    // cab, the saloon), 4 = while the vehicle is AI-driven; 0 = always. The AGG300's `_AI` meshes (4) are the
    // simplified saloon boxes an AI bus shows - drawn on the player's bus they covered whole sections.
    int viewpoint = 0;
    // [smoothskin]: the mesh is skinned to the meshes its [setbone]s name (an articulated bus's bellows)
    bool smooth_skin = false;
    std::vector<MeshAnimLayer> animations;
    std::vector<ModelCfgMaterial> materials; // ordered as in cfg
    // SCO [CTCTexture] default textures: their slots take the scheme the object's script picks
    std::vector<std::string> ctc_defaults;
    // SCO [rendertype] on_surface → terrain decal (depth bias, no Z write).
    bool on_surface = false;
    // SCO [rendertype] presurface → TMap pass 0 (before terrain+splines). Normal Z.
    bool presurface = false;
};

struct ModelCfgLod {
    // OMSI [LOD] value = screensize threshold (NOT meters). Shown when screensize >= this.
    // Screensize = 2 * visualRadius / (distance * verticalFovRadians).
    float distance = 0.f;
    std::vector<ModelCfgMeshEntry> meshes;
};

// [texttexture] / [texttexture_enh] (OMSI SDK): string field (a script string variable, or the number of a
// built-in string), font, size, colour mode + RGB; _enh adds alignment and letter spacing. [useTextTexture] N
// indexes the model's list of both kinds in the order they appear.
struct ModelCfgTextTexture {
    std::string field;
    std::string font_name;
    int width = 128;
    int height = 128;
    int color_mode = 0;
    int r = 255, g = 255, b = 255;
    int align = 1;
    int letter_spacing = 0;
    bool enh = false;
};

struct ModelCfgCtcPack {
    std::string name;   // Colorscheme / script var (line 1 of [CTC])
    std::string folder; // relative to vehicle root (line 2)
    int value = 0;
    std::vector<std::filesystem::path> cti_files;
};

struct ModelCfgCtcTexture {
    std::string var;
    std::string file;
};

struct ModelCfgDocument {
    std::filesystem::path cfg_path;
    std::optional<ModelCfgTextTexture> text_texture; // the first one (legacy)
    std::vector<ModelCfgTextTexture> text_textures;
    std::string ctc_colorscheme; // legacy: first pack name
    std::string ctc_mode;        // legacy: first pack folder
    int ctc_value = 0;
    std::string ctc_texture_var;
    std::string ctc_texture_file;
    std::vector<ModelCfgCtcPack> ctc_packs;
    std::vector<ModelCfgCtcTexture> ctc_textures;
    float tex_detail_factor = 0.5f;
    std::vector<std::filesystem::path> cti_files; // flat union of all packs
    std::vector<ModelCfgLod> lods;
};

} // namespace omsi::model_cfg
