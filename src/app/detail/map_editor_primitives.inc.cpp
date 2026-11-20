const map::MapObjectInst* find_object(const world::World& world, std::int32_t object_id) {
    for (const auto& tile : world.tiles()) {
        for (const auto& obj : tile.objects) {
            if (obj.id == object_id) return &obj;
        }
    }
    return nullptr;
}

model_cfg::ModelCfgMeshEntry sco_matl_cfg(const sco::SceneryObject& sco, const std::filesystem::path& mesh) {
    model_cfg::ModelCfgMeshEntry entry{};
    entry.mesh_file = mesh.filename().string();
    entry.on_surface = sco.render_priority == sco::ScoRenderPriority::OnSurface;
    entry.presurface = sco.render_priority == sco::ScoRenderPriority::Presurface;
    int mesh_i = -1;
    const auto want = mesh.filename();
    for (std::size_t i = 0; i < sco.meshes.size(); ++i) {
        if (sco.meshes[i].mesh_file.filename() == want) {
            mesh_i = static_cast<int>(i);
            break;
        }
    }
    for (const auto& m : sco.materials) {
        if (mesh_i >= 0 && m.mesh_index >= 0 && m.mesh_index != mesh_i) continue;
        entry.materials.push_back(m);
    }
    if (!sco.ctc.empty())
        for (const auto& t : sco.ctc_textures) entry.ctc_defaults.push_back(t.second);
    return entry;
}

struct XPartBind {
    int text_texture_index = -1;
    std::uint8_t alpha_mode = 0;
};

std::string x_part_tex_key(std::string_view name) {
    std::string k(name);
    for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k;
}

// Bind SCO [matl] to one .x Mesh by texture filename + occurrence ([matl] line 2).
// Skip [matl_change]. Never apply another part's texture or texttexture.
XPartBind bind_x_part(const sco::SceneryObject& sco, int mesh_idx, const std::vector<xfile::XMesh>& parts,
                      std::size_t part_i) {
    XPartBind out{};
    if (part_i >= parts.size()) return out;
    const auto key = x_part_tex_key(parts[part_i].texture_filename);
    int occ = 0;
    if (!key.empty()) {
        for (std::size_t i = 0; i < part_i; ++i) {
            if (x_part_tex_key(parts[i].texture_filename) == key) ++occ;
        }
    }
    const model_cfg::ModelCfgMaterial* exact = nullptr;
    for (const auto& m : sco.materials) {
        if (m.is_matl_change) continue;
        if (mesh_idx >= 0 && m.mesh_index >= 0 && m.mesh_index != mesh_idx) continue;
        if (key.empty() || m.texture_key != key) continue;
        if (m.mat_id == occ) {
            exact = &m;
            break;
        }
    }
    if (!exact) {
        if (sco.only_editor_mesh) out.alpha_mode = 1;
        return out;
    }
    out.alpha_mode = static_cast<std::uint8_t>(exact->alpha_mode);
    if (exact->use_text_texture) out.text_texture_index = *exact->use_text_texture;
    return out;
}

float dist_point_ray(float px, float py, float pz, const f4f23ma::f4& o, const f4f23ma::f4& d) {
    const float vx = px - o.x, vy = py - o.y, vz = pz - o.z;
    const float t = vx * d.x + vy * d.y + vz * d.z;
    const float cx = o.x + d.x * t - px;
    const float cy = o.y + d.y * t - py;
    const float cz = o.z + d.z * t - pz;
    return std::sqrt(cx * cx + cy * cy + cz * cz);
}
