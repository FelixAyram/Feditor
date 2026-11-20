#include "omsi/model_cfg.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace omsi::model_cfg {
namespace {

std::string trim(std::string_view s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

bool is_comment_or_separator(const std::string& line) {
    if (line.empty()) return true;
    if (line.size() >= 3 && line[0] == '*' && line.back() == '*') return true;
    return false;
}

bool is_section(const std::string& line) {
    return line.size() >= 3 && line.front() == '[' && line.back() == ']';
}

std::string section_name(std::string_view line) {
    const auto inner = trim(line.substr(1, line.size() - 2));
    std::string out(inner);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string texture_key(std::string_view path) {
    std::string out = trim(path);
    std::replace(out.begin(), out.end(), '\\', '/');
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool iequals_texture(std::string_view a, std::string_view b) {
    return texture_key(a) == texture_key(b);
}

bool parse_int(std::string_view text, int& out) {
    text = trim(text);
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{};
}

bool parse_float(std::string_view text, float& out) {
    text = trim(text);
    if (text.empty()) return false;
    try {
        out = std::stof(std::string(text));
        return true;
    } catch (...) {
        return false;
    }
}

struct LineReader {
    const std::vector<std::string>& lines;
    std::size_t index = 0;

    bool eof() const { return index >= lines.size(); }

    std::optional<std::string> peek() {
        while (index < lines.size()) {
            const auto line = trim(lines[index]);
            if (!line.empty() && !is_comment_or_separator(line)) return line;
            ++index;
        }
        return std::nullopt;
    }

    std::optional<std::string> next() {
        auto p = peek();
        if (p) ++index;
        return p;
    }

    std::optional<std::string> next_section() {
        while (true) {
            auto p = peek();
            if (!p) return std::nullopt;
            if (is_section(*p)) {
                ++index;
                return section_name(*p);
            }
            ++index; // skip orphan data lines before first section
        }
    }

    // A section's value line - never the next [section]: Omsi.exe reads the line after the keyword as it is, an
    // empty one (`[matl_transmap]` with no file: the texture's own alpha masks the envmap) gives an empty value.
    // Skipping blank lines to the next text took the next `[mesh]` as the file name: the AGG300 lost 45 sections
    // (aussen_1_weich.o3d - its roof -, the matrix displays, door animations), and what followed them was laid
    // on the mesh before.
    std::optional<std::string> read_line() {
        const auto p = peek();
        if (!p || is_section(*p)) return std::nullopt;
        ++index;
        return p;
    }

    bool skip_section_body() {
        while (!eof()) {
            const auto p = peek();
            if (!p) return true;
            if (is_section(*p)) return true;
            ++index;
        }
        return true;
    }
};

ModelCfgMaterial& current_material(ModelCfgMeshEntry& mesh, std::optional<std::size_t>& current_mat_index) {
    if (!current_mat_index || *current_mat_index >= mesh.materials.size())
        throw std::runtime_error("model.cfg: mat modifier without preceding [matl]");
    return mesh.materials[*current_mat_index];
}

void begin_material(ModelCfgMeshEntry& mesh, bool is_change, std::optional<std::size_t>& current_mat_index) {
    mesh.materials.push_back({});
    mesh.materials.back().is_matl_change = is_change;
    current_mat_index = mesh.materials.size() - 1;
}

void reset_mesh_material_context(std::optional<std::size_t>& current_mat_index) {
    current_mat_index = std::nullopt;
}

} // namespace

std::optional<ModelCfgDocument> load_model_cfg(const std::filesystem::path& path) {
    const auto raw_lines = cfg::read_cfg_lines(path);
    if (raw_lines.empty()) return std::nullopt;

    ModelCfgDocument doc{};
    doc.cfg_path = path;

    LineReader reader{raw_lines};
    ModelCfgLod* current_lod = nullptr;
    bool implicit_first_lod = false; // meshes came before any [LOD]
    ModelCfgMeshEntry* current_mesh = nullptr;
    std::optional<std::size_t> current_mat_index;
    bool item_open = false; // after a [matl_item]: the modifiers belong to that item of the [matl_change]

    while (!reader.eof()) {
        const auto section = reader.next_section();
        if (!section) break;

        if (*section == "texttexture" || *section == "texttexture_enh") {
            ModelCfgTextTexture tt{};
            tt.enh = *section == "texttexture_enh";
            if (auto v = reader.read_line()) tt.field = *v;
            if (auto v = reader.read_line()) tt.font_name = *v;
            if (auto v = reader.read_line()) parse_int(*v, tt.width);
            if (auto v = reader.read_line()) parse_int(*v, tt.height);
            if (auto v = reader.read_line()) parse_int(*v, tt.color_mode);
            if (auto v = reader.read_line()) parse_int(*v, tt.r);
            if (auto v = reader.read_line()) parse_int(*v, tt.g);
            if (auto v = reader.read_line()) parse_int(*v, tt.b);
            if (tt.enh) {
                if (auto v = reader.read_line()) parse_int(*v, tt.align);
                if (auto v = reader.read_line()) parse_int(*v, tt.letter_spacing);
            }
            if (!doc.text_texture) doc.text_texture = tt;
            doc.text_textures.push_back(std::move(tt));
            continue;
        }

        if (*section == "ctc") {
            ModelCfgCtcPack pack{};
            if (auto v = reader.read_line()) pack.name = *v;
            if (auto v = reader.read_line()) pack.folder = *v;
            if (auto v = reader.read_line()) parse_int(*v, pack.value);
            if (doc.ctc_packs.empty()) {
                doc.ctc_colorscheme = pack.name;
                doc.ctc_mode = pack.folder;
                doc.ctc_value = pack.value;
            }
            doc.ctc_packs.push_back(std::move(pack));
            continue;
        }

        if (*section == "ctctexture") {
            ModelCfgCtcTexture tex{};
            if (auto v = reader.read_line()) tex.var = *v;
            if (auto v = reader.read_line()) tex.file = *v;
            if (doc.ctc_texture_var.empty()) {
                doc.ctc_texture_var = tex.var;
                doc.ctc_texture_file = tex.file;
            }
            doc.ctc_textures.push_back(std::move(tex));
            continue;
        }

        if (*section == "tex_detail_factor") {
            if (auto v = reader.read_line()) parse_float(*v, doc.tex_detail_factor);
            continue;
        }

        if (*section == "lod") {
            ModelCfgLod lod{};
            if (auto v = reader.read_line()) parse_float(*v, lod.distance);
            doc.lods.push_back(std::move(lod));
            current_lod = &doc.lods.back();
            current_mesh = nullptr;
            reset_mesh_material_context(current_mat_index);
            continue;
        }

        if (*section == "mesh") {
            if (!current_lod) {
                doc.lods.push_back({});
                current_lod = &doc.lods.back();
                implicit_first_lod = true;
            }
            ModelCfgMeshEntry mesh{};
            if (auto v = reader.read_line()) mesh.mesh_file = *v;
            current_lod->meshes.push_back(std::move(mesh));
            current_mesh = &current_lod->meshes.back();
            item_open = false;
            reset_mesh_material_context(current_mat_index);
            continue;
        }

        if (*section == "shadow") {
            if (current_mesh) current_mesh->casts_shadow = true;
            continue;
        }

        if (*section == "isshadow") {
            if (current_mesh) current_mesh->is_shadow_mesh = true;
            if (current_mesh) current_mesh->pending_isshadow = true;
            continue;
        }

        if (*section == "mouseevent") {
            if (current_mesh)
                if (auto v = reader.read_line()) current_mesh->mouseevent_var = *v;
            continue;
        }

        if (*section == "mesh_ident") {
            if (current_mesh)
                if (auto v = reader.read_line()) current_mesh->mesh_ident = *v;
            continue;
        }

        if (*section == "animparent") {
            if (current_mesh)
                if (auto v = reader.read_line()) current_mesh->animparent = *v;
            continue;
        }

        if (*section == "visible") {
            if (current_mesh) {
                if (auto v = reader.read_line()) current_mesh->visible_var = *v;
                if (auto v = reader.read_line()) parse_int(*v, current_mesh->visible_default);
            }
            continue;
        }

        if (*section == "newanim") {
            MeshAnimLayer layer{};
            static const std::vector<std::string> kTags = {
                "origin_trans", "origin_rot_x", "origin_rot_y", "origin_rot_z", "origin_from_mesh",
                "anim_rot",     "anim_trans",   "maxspeed",     "delay",        "offset"};
            while (!reader.eof()) {
                const auto p = reader.peek();
                if (!p || is_section(*p)) break;
                const auto tag = *reader.next();
                const bool known = std::find(kTags.begin(), kTags.end(), tag) != kTags.end();
                if (!known) break;
                if (tag == "origin_trans") {
                    layer.has_origin_trans = true;
                    if (auto v = reader.read_line()) parse_float(*v, layer.origin_trans[0]);
                    if (auto v = reader.read_line()) parse_float(*v, layer.origin_trans[1]);
                    if (auto v = reader.read_line()) parse_float(*v, layer.origin_trans[2]);
                } else if (tag == "origin_rot_x") {
                    if (auto v = reader.read_line()) parse_float(*v, layer.origin_rot_deg[0]);
                } else if (tag == "origin_rot_y") {
                    if (auto v = reader.read_line()) parse_float(*v, layer.origin_rot_deg[1]);
                } else if (tag == "origin_rot_z") {
                    if (auto v = reader.read_line()) parse_float(*v, layer.origin_rot_deg[2]);
                } else if (tag == "origin_from_mesh") {
                    layer.origin_from_mesh = true;
                } else if (tag == "anim_rot") {
                    MeshAnimBinding binding{};
                    binding.type = MeshAnimBindingType::rotation;
                    binding.axis = MeshAnimAxis::x;
                    if (auto v = reader.read_line()) binding.var_name = *v;
                    if (auto v = reader.read_line()) parse_float(*v, binding.scale);
                    layer.bindings.push_back(std::move(binding));
                } else if (tag == "anim_trans") {
                    MeshAnimBinding binding{};
                    binding.type = MeshAnimBindingType::translation;
                    binding.axis = MeshAnimAxis::x; // OMSI: motion always on local X
                    if (auto v = reader.read_line()) binding.var_name = *v;
                    if (auto v = reader.read_line()) parse_float(*v, binding.scale);
                    layer.bindings.push_back(std::move(binding));
                } else if (tag == "maxspeed" || tag == "delay" || tag == "offset") {
                    float value = 0.f;
                    if (auto v = reader.read_line()) parse_float(*v, value);
                    if (layer.bindings.empty()) {
                        omsi::log::warn("model.cfg: " + tag + " before anim_rot/anim_trans (ignored)");
                    } else {
                        auto& b = layer.bindings.back();
                        if (tag == "maxspeed") b.maxspeed = value;
                        else if (tag == "delay") b.delay = value;
                        else b.offset = value;
                    }
                }
            }
            if (current_mesh) {
                current_mesh->animations.push_back(std::move(layer));
            } else {
                omsi::log::warn("model.cfg: [newanim] without preceding [mesh] (ignored, OMSI requires mesh first)");
            }
            continue;
        }

        if (*section == "smoothskin") {
            if (current_mesh) current_mesh->smooth_skin = true;
            continue;
        }

        if (*section == "viewpoint") {
            if (current_mesh)
                if (auto v = reader.read_line()) parse_int(*v, current_mesh->viewpoint);
            continue;
        }

        if (*section == "illumination_interior") {
            if (current_mesh) {
                for (int i = 0; i < 8; ++i) {
                    const auto p = reader.peek();
                    if (!p || is_section(*p)) break;
                    int value = 0;
                    if (!parse_int(*p, value)) break;
                    reader.next();
                    current_mesh->illumination_interior.push_back(value);
                }
            }
            continue;
        }

        if (!current_mesh) {
            reader.skip_section_body();
            continue;
        }

        if (*section == "matl_item") {
            // the next item of the [matl_change] before it: its modifiers change that item, not the slot's
            // own material (they were laid on the base: the AGG300's lit saloon night maps were lost)
            reader.skip_section_body();
            if (current_mat_index) {
                auto& mat = current_material(*current_mesh, current_mat_index);
                if (mat.is_matl_change) {
                    ++mat.item_count;
                    item_open = true;
                }
            }
            continue;
        }
        if (item_open && *section != "matl" && *section != "matl_change") {
            // modifiers of an item: only item 1's night / light map are kept (the variable on = item 1)
            auto& mat = current_material(*current_mesh, current_mat_index);
            if (mat.item_count == 1 && *section == "matl_nightmap") {
                if (auto v = reader.read_line()) mat.item_nightmap_texture = *v;
                continue;
            }
            if (mat.item_count == 1 && *section == "matl_lightmap") {
                if (auto v = reader.read_line()) mat.item_lightmap_texture = *v;
                reader.skip_section_body();
                continue;
            }
            if (*section != "mesh" && *section != "lod" && *section != "newanim" && *section != "viewpoint" &&
                *section != "visible" && *section != "mouseevent" && *section != "animparent" &&
                *section != "illumination_interior" && *section != "isshadow" && *section != "smoothskin" &&
                *section != "setbone" && *section != "light_enh" && *section != "light_enh_2" &&
                *section != "interiorlight" && *section != "spotlight" && *section != "texttexture" &&
                *section != "texttexture_enh") {
                reader.skip_section_body(); // (other item modifiers: not laid on the base material)
                continue;
            }
        }

        if (*section == "matl" || *section == "matl_change") {
            item_open = false;
            begin_material(*current_mesh, *section == "matl_change", current_mat_index);
            auto& mat = current_material(*current_mesh, current_mat_index);
            if (current_mesh->pending_isshadow) {
                mat.is_shadow = true;
                current_mesh->pending_isshadow = false;
            }
            if (auto v = reader.read_line()) {
                mat.diffuse_texture = *v;
                mat.texture_key = texture_key(*v);
            }
            if (auto v = reader.read_line()) parse_int(*v, mat.mat_id);
            if (mat.is_matl_change)
                if (auto v = reader.read_line()) mat.change_var = *v;
            continue;
        }

        const bool is_mat_modifier =
            *section == "matl_alpha" || *section == "matl_nozwrite" || *section == "matl_nozcheck" ||
            *section == "matl_nozread" || *section == "matl_freetex" || *section == "matl_zbias" ||
            *section == "matl_envmap" || *section == "matl_envmaprealtime" ||
            *section == "matl_bumpmap" || *section == "matl_envmap_mask" || *section == "matl_transmap" ||
            *section == "matl_nightmap" || *section == "matl_lightmap" || *section == "texcoordtransx" ||
            *section == "texcoordtransy" || *section == "usetexttexture" || *section == "alphascale";

        if (is_mat_modifier) {
            if (!current_mat_index) {
                omsi::log::warn("model.cfg: [" + *section + "] without preceding [matl] on mesh " +
                                current_mesh->mesh_file + " (ignored)");
                reader.skip_section_body();
                continue;
            }
            auto& mat = current_material(*current_mesh, current_mat_index);

            if (*section == "matl_alpha") {
                int mode = 0;
                if (auto v = reader.read_line()) parse_int(*v, mode);
                mat.alpha_mode = static_cast<MatlAlphaMode>((std::min)(2, (std::max)(0, mode)));
            } else if (*section == "matl_nozwrite") {
                mat.no_zwrite = true;
            } else if (*section == "matl_nozcheck" || *section == "matl_nozread") {
                mat.no_zcheck = true;
            } else if (*section == "matl_freetex") {
                (void)reader.read_line();
                if (auto v = reader.read_line()) mat.freetex_label = *v;
            } else if (*section == "matl_zbias") {
                if (auto v = reader.read_line()) parse_int(*v, mat.z_bias);
            } else if (*section == "matl_envmap" || *section == "matl_envmaprealtime") {
                if (auto v = reader.read_line()) mat.envmap_texture = *v;
                if (auto v = reader.read_line()) parse_float(*v, mat.envmap_strength);
                mat.envmap_realtime = (*section == "matl_envmaprealtime");
            } else if (*section == "matl_bumpmap") {
                if (auto v = reader.read_line()) mat.bumpmap_texture = *v;
                if (auto v = reader.read_line()) parse_float(*v, mat.bumpmap_strength);
            } else if (*section == "matl_envmap_mask") {
                if (auto v = reader.read_line()) mat.envmap_mask_texture = *v;
            } else if (*section == "matl_transmap") {
                mat.transmap_texture = reader.read_line().value_or(std::string{}); // (empty: own alpha masks the envmap)
            } else if (*section == "matl_nightmap") {
                if (auto v = reader.read_line()) mat.nightmap_texture = *v;
            } else if (*section == "matl_lightmap") {
                if (auto v = reader.read_line()) mat.lightmap_texture = *v;
                if (const auto p = reader.peek(); p && !is_section(*p)) mat.lightmap_var = *reader.read_line();
            } else if (*section == "texcoordtransx") {
                if (auto v = reader.read_line()) mat.texcoord_trans_x_var = *v;
            } else if (*section == "alphascale") {
                if (auto v = reader.read_line()) mat.alpha_scale_var = *v;
            } else if (*section == "texcoordtransy") {
                if (auto v = reader.read_line()) mat.texcoord_trans_y_var = *v;
            } else if (*section == "usetexttexture") {
                int idx = 0;
                if (auto v = reader.read_line()) parse_int(*v, idx);
                mat.use_text_texture = idx;
            }
            continue;
        }

        // illumination_interior, visible, smoke, etc.
        reader.skip_section_body();
    }

    if (doc.lods.empty()) doc.lods.push_back({});
    // [mesh]es before the first [LOD] belong to no band: OMSI draws them at every distance. Kept as a band
    // of their own, the first one, they were all a vehicle had for the primary LOD - the HH AGG300
    // (4 dummies there, its 406 body meshes under [LOD] 0.1) showed four meshes.
    if (implicit_first_lod && doc.lods.size() >= 2) {
        const auto always = std::move(doc.lods.front().meshes);
        doc.lods.erase(doc.lods.begin());
        for (auto& l : doc.lods) l.meshes.insert(l.meshes.begin(), always.begin(), always.end());
    }
    // [CTCTexture] name / default texture: model-wide (OMSI SDK "Colorscheme"); every material showing the default
    // texture takes the paint scheme's file. The renderer tags those slots per mesh (mesh.cpp "ctc:" key).
    if (!doc.ctc_textures.empty())
        for (auto& l : doc.lods)
            for (auto& m : l.meshes)
                for (const auto& t : doc.ctc_textures)
                    if (!t.file.empty()) m.ctc_defaults.push_back(t.file);

    // Unit87: CTC folder is relative to vehicle root (parent of model/), not model.cfg dir.
    const auto vehicle_root = path.parent_path().parent_path();
    const auto model_dir = path.parent_path();
    auto scan_cti = [&](ModelCfgCtcPack& pack) {
        if (pack.folder.empty()) return;
        std::error_code ec;
        std::filesystem::path ctc_dir = vehicle_root / pack.folder;
        if (!std::filesystem::is_directory(ctc_dir, ec)) ctc_dir = model_dir / pack.folder;
        if (!std::filesystem::is_directory(ctc_dir, ec)) return;
        for (const auto& entry : std::filesystem::directory_iterator(ctc_dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            auto ext = entry.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".cti") {
                pack.cti_files.push_back(entry.path());
                doc.cti_files.push_back(entry.path());
            }
        }
        std::sort(pack.cti_files.begin(), pack.cti_files.end());
    };
    for (auto& pack : doc.ctc_packs) scan_cti(pack);
    std::sort(doc.cti_files.begin(), doc.cti_files.end());
    doc.cti_files.erase(std::unique(doc.cti_files.begin(), doc.cti_files.end()), doc.cti_files.end());

    std::size_t anim_layers = 0;
    std::size_t anim_meshes = 0;
    for (const auto& lod : doc.lods) {
        for (const auto& mesh : lod.meshes) {
            if (!mesh.animations.empty()) {
                ++anim_meshes;
                anim_layers += mesh.animations.size();
            }
        }
    }

    omsi::log::info("model.cfg: " + path.string() + " lods=" + std::to_string(doc.lods.size()) +
                    " meshes=" + std::to_string(doc.lods[0].meshes.size()) + " newanim_meshes=" +
                    std::to_string(anim_meshes) + " newanim_layers=" + std::to_string(anim_layers) +
                    " cti=" + std::to_string(doc.cti_files.size()) + " ctc_packs=" +
                    std::to_string(doc.ctc_packs.size()));
    return doc;
}

const ModelCfgLod* primary_lod(const ModelCfgDocument& doc) {
    if (doc.lods.empty()) return nullptr;
    return &doc.lods[0];
}

float compute_screensize(float visual_radius, float distance_m, float vertical_fov_rad) {
    const float dist = (std::max)(distance_m, 1.e-3f);
    const float fov = (std::max)(vertical_fov_rad, 1.e-3f);
    return 2.f * (std::max)(visual_radius, 0.f) / (dist * fov);
}

const ModelCfgLod* select_lod(const ModelCfgDocument& doc, float screensize) {
    const int i = select_lod_index(doc, screensize, -1, 0.f);
    if (i < 0) return nullptr;
    return &doc.lods[static_cast<std::size_t>(i)];
}

int select_lod_index(const ModelCfgDocument& doc, float screensize, int prior_index, float hysteresis_eps) {
    if (doc.lods.empty()) return -1;
    const int n = static_cast<int>(doc.lods.size());
    // OMSI: first matching band where screensize >= threshold (thresholds typically descend).
    int pick = n - 1;
    for (int i = 0; i < n; ++i) {
        if (screensize + 1.e-6f >= doc.lods[static_cast<std::size_t>(i)].distance) {
            pick = i;
            break;
        }
    }
    if (prior_index >= 0 && prior_index < n && hysteresis_eps > 0.f && prior_index != pick) {
        const float th = doc.lods[static_cast<std::size_t>(pick)].distance;
        const float th_prev = doc.lods[static_cast<std::size_t>(prior_index)].distance;
        // Stay on previous until clearly past the boundary by eps.
        if (prior_index < pick) {
            // Switching to coarser (smaller threshold) — wait until screensize drops below th_prev - eps.
            if (screensize > th_prev - hysteresis_eps) return prior_index;
        } else {
            // Switching to finer — wait until screensize rises above th + eps.
            if (screensize < th + hysteresis_eps) return prior_index;
        }
    }
    return pick;
}

const ModelCfgMaterial* resolve_material_for_o3d_slot(const ModelCfgMeshEntry& mesh, int o3d_mat_id,
                                                        std::string_view o3d_texture_name) {
    const ModelCfgMaterial* by_id_fallback = nullptr;
    for (const auto& mat : mesh.materials) {
        if (mat.mat_id != o3d_mat_id) continue;
        if (mat.diffuse_texture.empty() && mat.mat_id < 0) continue;
        if (!by_id_fallback) by_id_fallback = &mat;
        if (!o3d_texture_name.empty() && iequals_texture(mat.diffuse_texture, o3d_texture_name)) return &mat;
    }
    // Do not apply a mismatched cfg diffuse by mat_id alone (Taxi_2: mat_id=0 glass must not
    // replace Taxi2_atlas.dds on the body). Placeholders (xxxxxxxxxx) still return so alpha sticks.
    if (!by_id_fallback) return nullptr;
    const auto& d = by_id_fallback->diffuse_texture;
    if (d.empty() || d.find("xxxxxxxxxx") != std::string::npos) return by_id_fallback;
    if (o3d_texture_name.empty()) return by_id_fallback;
    return nullptr;
}

const ModelCfgMaterial* resolve_cfg_material_by_o3d_texture(const ModelCfgMeshEntry& mesh,
                                                              std::string_view o3d_texture_name) {
    if (o3d_texture_name.empty()) return nullptr;
    const auto key = texture_key(o3d_texture_name);
    for (const auto& mat : mesh.materials) {
        if (!mat.diffuse_texture.empty() && mat.texture_key == key) return &mat;
    }
    return nullptr;
}

void apply_omsi_material_state(MatlAlphaMode& alpha_mode, bool& no_zwrite, bool& no_zcheck) {
    // Omsi.exe material setter @ 0x007fd9b0: D3DRS_ZWRITEENABLE (14) = !noZwrite (0x7fe2d4, material byte
    // +0x5d) whatever the alpha mode; [matl_alpha] 2 (byte +0x5c, 0x800099) only turns ALPHATESTENABLE (15)
    // off and blending on (SRCALPHA / INVSRCALPHA). State 15 was misread as the z-write: every alpha-2
    // material lost its depth and solid bodies (S315UL_Static, the AI cars) showed what lay behind them.
    (void)alpha_mode;
    (void)no_zwrite;
    (void)no_zcheck;
}

std::vector<const ModelCfgMaterial*> materials_for_o3d_slot(const ModelCfgMeshEntry& mesh, int o3d_mat_id) {
    std::vector<const ModelCfgMaterial*> out;
    for (const auto& mat : mesh.materials)
        if (mat.mat_id == o3d_mat_id) out.push_back(&mat);
    return out;
}

} // namespace omsi::model_cfg
