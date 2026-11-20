// BeamNG World Editor object actions (Edit / Camera / Object menus of lua/ge/extensions/editor/mainMenu.lua and
// the editor.json input actions) on the editor selection. Included at the end of mod_beamng_setup.inc.cpp; the
// UI is drawn by bng_ui_object_actions at the top of the "Escena" tab. Every action that changes objects takes a
// push_undo snapshot first, so Ctrl+Z / Ctrl+Y work as for any other edit, and Ctrl+S (bng_save_hook) writes the
// result to items.level.json.
//
// Lock (editor-only, like BeamNG's Lock Selection): a locked object can still be selected, but any pose change is
// reverted by bng_lock_enforce (called every frame from mod_beamng_frame).

struct BngLockState {
    struct Pose {
        float p[3], r[3];
    };
    std::unordered_map<std::int32_t, Pose> locked;
};
BngLockState bng_lock;

// selection -> edit_objects indices (BeamNG objects only, no duplicates, primary first)
auto bng_sel_indices = [&]() {
    std::vector<int> out;
    auto add = [&](int i) {
        if (i < 0 || static_cast<std::size_t>(i) >= edit_objects.size()) return;
        if (!bng_is_obj(edit_objects[static_cast<std::size_t>(i)])) return;
        if (std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    };
    add(selected_object);
    for (int gi : group_sel_obj) add(gi);
    return out;
};

auto bng_set_selection = [&](const std::vector<int>& idx) {
    group_sel_obj.clear();
    group_sel_obj_ids.clear();
    selected_object = idx.empty() ? -1 : idx.front();
    selected_spline = -1;
    if (idx.size() > 1)
        for (int i : idx) {
            group_sel_obj.push_back(i);
            group_sel_obj_ids.push_back(edit_objects[static_cast<std::size_t>(i)].id);
        }
};

auto bng_resync = [&](map::MapObjectInst& o) {
    if (bng_sync_hook) bng_sync_hook(o);
};

auto bng_ground_z = [&](float x, float y, float fallback) {
    if (bng_terrain_sample)
        if (auto z = bng_terrain_sample(x, y)) return *z;
    return fallback;
};

auto bng_obj_extent = [&](const map::MapObjectInst& o) {
    return (std::max)(0.25f, bng_obj_radius(o) * bng_obj_scale_max(o.id));
};

// copy of an editor object as a NEW scene row (keeps scale; level_index = new -> appended on save)
auto bng_clone_object = [&](const map::MapObjectInst& src) -> int {
    std::int32_t next_id = 1;
    for (const auto& o : edit_objects) next_id = (std::max)(next_id, o.id + 1);
    map::MapObjectInst o = src;
    o.id = next_id;
    BngObjExtra ex;
    if (auto it = bng_rt->extra.find(src.id); it != bng_rt->extra.end()) std::memcpy(ex.scale, it->second.scale, sizeof(ex.scale));
    ex.level_index = UINT32_MAX;
    ex.orig_yaw = o.rotation_deg.x;
    ex.orig_pitch = o.rotation_deg.y;
    ex.orig_bank = o.rotation_deg.z;
    bng_rt->extra[o.id] = ex;
    edit_objects.push_back(std::move(o));
    spawn_editor_object_gpu(edit_objects.back());
    return static_cast<int>(edit_objects.size() - 1);
};

auto bng_centroid = [&](const std::vector<int>& idx, float c[3]) {
    c[0] = c[1] = c[2] = 0.f;
    for (int i : idx) {
        const auto& o = edit_objects[static_cast<std::size_t>(i)];
        c[0] += o.position.x;
        c[1] += o.position.y;
        c[2] += o.position.z;
    }
    if (!idx.empty())
        for (int k = 0; k < 3; ++k) c[k] /= static_cast<float>(idx.size());
};

// camera heading in editor yaw degrees: heading = -theta - pi/2 (see "Ir al spawn"), editor yaw = -heading
auto bng_camera_yaw_deg = [&]() { return (camera.theta + 1.5707963f) * 57.29577951f; };

auto bng_lock_enforce = [&]() {
    if (bng_lock.locked.empty()) return;
    bool reverted = false;
    for (auto& o : edit_objects) {
        auto it = bng_lock.locked.find(o.id);
        if (it == bng_lock.locked.end()) continue;
        const auto& P = it->second;
        if (o.position.x == P.p[0] && o.position.y == P.p[1] && o.position.z == P.p[2] && o.rotation_deg.x == P.r[0] &&
            o.rotation_deg.y == P.r[1] && o.rotation_deg.z == P.r[2])
            continue;
        o.position.x = P.p[0];
        o.position.y = P.p[1];
        o.position.z = P.p[2];
        o.rotation_deg.x = P.r[0];
        o.rotation_deg.y = P.r[1];
        o.rotation_deg.z = P.r[2];
        bng_resync(o);
        reverted = true;
    }
    if (reverted) panel.set_status("Objeto bloqueado: desbloquealo para moverlo (Objeto > Desbloquear)");
};

bng_ui_object_actions = [&]() {
    if (!bng_rt) return;
    if (!ImGui::CollapsingHeader("Acciones de objeto (menu del World Editor)", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const auto sel = bng_sel_indices();
    ImGui::TextDisabled("Seleccion: %d objeto(s)", static_cast<int>(sel.size()));
    const float w3 = (ImGui::GetContentRegionAvail().x - 16.f) / 3.f;
    const bool none = sel.empty() || view_only;

    // ── Edit ──────────────────────────────────────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Edicion");
    if (ImGui::Button("Seleccionar todo", ImVec2(w3, 0.f))) {
        const auto& hid = panel.imgui_state().hidden_object_ids;
        std::vector<int> all;
        for (std::size_t i = 0; i < edit_objects.size(); ++i)
            if (bng_is_obj(edit_objects[i]) && !hid.count(edit_objects[i].id)) all.push_back(static_cast<int>(i));
        bng_set_selection(all);
        panel.set_status("Seleccionados " + std::to_string(all.size()) + " objetos");
    }
    ImGui::SameLine(0.f, 8.f);
    if (ImGui::Button("Deseleccionar", ImVec2(w3, 0.f))) bng_set_selection({});
    ImGui::SameLine(0.f, 8.f);
    ImGui::BeginDisabled(none);
    if (ImGui::Button("Duplicar", ImVec2(w3, 0.f))) {
        push_undo("bng_duplicate");
        std::vector<int> copies;
        for (int i : sel) copies.push_back(bng_clone_object(edit_objects[static_cast<std::size_t>(i)]));
        bng_set_selection(copies);
        panel.set_status("Duplicados " + std::to_string(copies.size()) + " objetos (en el mismo sitio, G para moverlos)");
    }
    if (ImGui::Button("Duplicar en la camara", ImVec2(w3, 0.f))) {
        push_undo("bng_duplicate_at_camera");
        float c[3];
        bng_centroid(sel, c);
        std::vector<int> copies;
        for (int i : sel) {
            const int k = bng_clone_object(edit_objects[static_cast<std::size_t>(i)]);
            auto& o = edit_objects[static_cast<std::size_t>(k)];
            o.position.x += camera.target_x - c[0];
            o.position.y += camera.target_y - c[1];
            o.position.z = bng_ground_z(o.position.x, o.position.y, o.position.z + camera.target_z - c[2]);
            bng_resync(o);
            copies.push_back(k);
        }
        bng_set_selection(copies);
    }
    ImGui::SameLine(0.f, 8.f);
    if (ImGui::Button("Copiar a seleccionados", ImVec2(w3 * 2.f + 8.f, 0.f)) && sel.size() > 1) {
        // copyToSelected.lua: the first selected object is copied onto the transform of every other one
        push_undo("bng_copy_to_selected");
        const auto src = edit_objects[static_cast<std::size_t>(sel.front())];
        std::vector<int> made;
        for (std::size_t s = 1; s < sel.size(); ++s) {
            const auto dst = edit_objects[static_cast<std::size_t>(sel[s])];
            const int k = bng_clone_object(src);
            auto& o = edit_objects[static_cast<std::size_t>(k)];
            o.position = dst.position;
            o.rotation_deg = dst.rotation_deg;
            bng_resync(o);
            made.push_back(k);
        }
        panel.set_status("Copiado a " + std::to_string(made.size()) + " transformaciones");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("El PRIMER seleccionado se copia en la posicion/rotacion de cada uno de los demas.");
    ImGui::EndDisabled();

    // ── Camera ────────────────────────────────────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Camara");
    ImGui::BeginDisabled(sel.empty());
    if (ImGui::Button("Camara a la seleccion", ImVec2(w3, 0.f))) {
        float c[3];
        bng_centroid(sel, c);
        float ext = 1.f;
        for (int i : sel) {
            const auto& o = edit_objects[static_cast<std::size_t>(i)];
            ext = (std::max)(ext, std::hypot(o.position.x - c[0], o.position.y - c[1]) + bng_obj_extent(o));
        }
        camera.target_x = c[0];
        camera.target_y = c[1];
        camera.target_z = c[2];
        camera.ground_z = c[2];
        camera.radius = (std::clamp)(ext * 1.6f, 8.f, 2000.f);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Place Camera at Selection / Fit View to Selection");
    ImGui::EndDisabled();
    ImGui::SameLine(0.f, 8.f);
    ImGui::BeginDisabled(none);
    if (ImGui::Button("Mover frente a la camara", ImVec2(w3, 0.f))) {
        push_undo("bng_move_to_camera");
        float c[3];
        bng_centroid(sel, c);
        for (int i : sel) {
            auto& o = edit_objects[static_cast<std::size_t>(i)];
            o.position.x += camera.target_x - c[0];
            o.position.y += camera.target_y - c[1];
            o.position.z = bng_ground_z(o.position.x, o.position.y, o.position.z + camera.target_z - c[2]);
            bng_resync(o);
        }
    }
    ImGui::SameLine(0.f, 8.f);
    if (ImGui::Button("Transform desde camara", ImVec2(w3, 0.f))) {
        push_undo("bng_transform_from_camera");
        float eye[3];
        camera.eye_position(eye[0], eye[1], eye[2]);
        for (int i : sel) {
            auto& o = edit_objects[static_cast<std::size_t>(i)];
            o.position.x = eye[0];
            o.position.y = eye[1];
            o.position.z = eye[2];
            o.rotation_deg.x = bng_camera_yaw_deg();
            o.rotation_deg.y = 0.f;
            o.rotation_deg.z = 0.f;
            bng_resync(o);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set Selection Transform from Camera: posicion del ojo + rumbo de la camara.");

    // ── Object ────────────────────────────────────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Objeto");
    auto& hid = panel.imgui_state().hidden_object_ids;
    if (ImGui::Button("Bloquear", ImVec2(w3, 0.f))) {
        for (int i : sel) {
            const auto& o = edit_objects[static_cast<std::size_t>(i)];
            bng_lock.locked[o.id] = {{o.position.x, o.position.y, o.position.z},
                                     {o.rotation_deg.x, o.rotation_deg.y, o.rotation_deg.z}};
        }
        panel.set_status("Bloqueados " + std::to_string(sel.size()) + " objetos");
    }
    ImGui::SameLine(0.f, 8.f);
    if (ImGui::Button("Desbloquear", ImVec2(w3, 0.f)))
        for (int i : sel) bng_lock.locked.erase(edit_objects[static_cast<std::size_t>(i)].id);
    ImGui::SameLine(0.f, 8.f);
    if (ImGui::Button("Ocultar", ImVec2(w3, 0.f))) {
        for (int i : sel) {
            const auto id = edit_objects[static_cast<std::size_t>(i)].id;
            hid.insert(id);
            bng_hide_object(id);
        }
        bng_set_selection({});
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(hid.empty());
    if (ImGui::Button("Mostrar todo lo oculto", ImVec2(-FLT_MIN, 0.f))) {
        hid.clear(); // the streaming pass respawns them next frames
        panel.set_status("Objetos ocultos: visibles de nuevo");
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Bloqueados: %d  |  Ocultos: %d", static_cast<int>(bng_lock.locked.size()), static_cast<int>(hid.size()));

    ImGui::BeginDisabled(none);
    auto reset = [&](bool rot, bool scl, const char* why) {
        push_undo(why);
        for (int i : sel) {
            auto& o = edit_objects[static_cast<std::size_t>(i)];
            if (rot) o.rotation_deg = {};
            if (scl) {
                auto& ex = bng_rt->extra[o.id];
                ex.scale[0] = ex.scale[1] = ex.scale[2] = 1.f;
            }
            bng_resync(o);
        }
    };
    if (ImGui::Button("Reset transform", ImVec2(w3, 0.f))) reset(true, true, "bng_reset_transform");
    ImGui::SameLine(0.f, 8.f);
    if (ImGui::Button("Reset rotacion", ImVec2(w3, 0.f))) reset(true, false, "bng_reset_rotation");
    ImGui::SameLine(0.f, 8.f);
    if (ImGui::Button("Reset escala", ImVec2(w3, 0.f))) reset(false, true, "bng_reset_scale");

    if (ImGui::Button("Apoyar en el terreno", ImVec2(-FLT_MIN, 0.f))) {
        push_undo("bng_drop_to_terrain");
        int n = 0;
        for (int i : sel) {
            auto& o = edit_objects[static_cast<std::size_t>(i)];
            if (!bng_terrain_sample) break;
            if (auto z = bng_terrain_sample(o.position.x, o.position.y)) {
                o.position.z = *z;
                bng_resync(o);
                ++n;
            }
        }
        panel.set_status("Apoyados en el terreno: " + std::to_string(n));
    }

    // Align Center / Align Bounds (mainMenu.lua): reference = the FIRST selected object.
    ImGui::TextUnformatted("Alinear centro a la 1a seleccion:");
    auto align_center = [&](int axis) {
        if (sel.size() < 2) return;
        push_undo("bng_align_center");
        const auto& ref = edit_objects[static_cast<std::size_t>(sel.front())].position;
        const float v = axis == 0 ? ref.x : axis == 1 ? ref.y : ref.z;
        for (std::size_t s = 1; s < sel.size(); ++s) {
            auto& o = edit_objects[static_cast<std::size_t>(sel[s])];
            (axis == 0 ? o.position.x : axis == 1 ? o.position.y : o.position.z) = v;
            bng_resync(o);
        }
    };
    const float w6 = (ImGui::GetContentRegionAvail().x - 40.f) / 6.f;
    if (ImGui::Button("X##ac", ImVec2(w6, 0.f))) align_center(0);
    ImGui::SameLine(0.f, 8.f);
    if (ImGui::Button("Y##ac", ImVec2(w6, 0.f))) align_center(1);
    ImGui::SameLine(0.f, 8.f);
    if (ImGui::Button("Z##ac", ImVec2(w6, 0.f))) align_center(2);
    ImGui::TextUnformatted("Alinear bordes (por radio del modelo):");
    auto align_bounds = [&](int axis, float sign) {
        if (sel.size() < 2) return;
        push_undo("bng_align_bounds");
        const auto& r = edit_objects[static_cast<std::size_t>(sel.front())];
        const float rv = (axis == 0 ? r.position.x : axis == 1 ? r.position.y : r.position.z) + sign * bng_obj_extent(r);
        for (std::size_t s = 1; s < sel.size(); ++s) {
            auto& o = edit_objects[static_cast<std::size_t>(sel[s])];
            (axis == 0 ? o.position.x : axis == 1 ? o.position.y : o.position.z) = rv - sign * bng_obj_extent(o);
            bng_resync(o);
        }
    };
    const char* lab[6] = {"-X", "+X", "-Y", "+Y", "-Z", "+Z"};
    for (int k = 0; k < 6; ++k) {
        if (k) ImGui::SameLine(0.f, 8.f);
        if (ImGui::Button((std::string(lab[k]) + "##ab").c_str(), ImVec2(w6, 0.f))) align_bounds(k / 2, (k % 2) ? 1.f : -1.f);
    }

    // SetObjectScale: scale of the primary selection, applied to every selected object
    if (!sel.empty()) {
        auto& ex0 = bng_rt->extra[edit_objects[static_cast<std::size_t>(sel.front())].id];
        float s3[3]{ex0.scale[0], ex0.scale[1], ex0.scale[2]};
        static bool uniform = true;
        ImGui::Checkbox("Escala uniforme", &uniform);
        bool edited = false;
        if (uniform) {
            float u = s3[0];
            if (ImGui::DragFloat("Escala##bngs", &u, 0.01f, 0.01f, 100.f, "%.3f")) {
                s3[0] = s3[1] = s3[2] = u;
                edited = true;
            }
        } else {
            edited = ImGui::DragFloat3("Escala XYZ##bngs", s3, 0.01f, 0.01f, 100.f, "%.3f");
        }
        if (ImGui::IsItemActivated()) push_undo("bng_set_scale");
        if (edited)
            for (int i : sel) {
                auto& o = edit_objects[static_cast<std::size_t>(i)];
                std::memcpy(bng_rt->extra[o.id].scale, s3, sizeof(s3));
                bng_resync(o);
            }
    }
    ImGui::EndDisabled();
};
