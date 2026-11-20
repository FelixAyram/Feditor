                const auto dist_to = [&](float x, float y, float z) {
                    const auto p = draw.transform_point({x, y, z, 1.f});
                    float best = 1e9f;
                    for (const auto& r : rest_gpu) {
                        const float dx = r.x - p.x, dy = r.y - p.y, dz = r.z - p.z;
                        best = (std::min)(best, dx * dx + dy * dy + dz * dz);
                    }
                    return std::sqrt(best);
                };
                const float d_raw = dist_to(origin_tx, origin_ty, origin_tz);
                const float d_yz = dist_to(origin_tx, origin_tz, origin_ty);
                if (d_yz + 0.05f < d_raw) {
                    fx = origin_tx;
                    fy = origin_tz;
                    fz = origin_ty;
                } else {
                    fx = origin_tx;
                    fy = origin_ty;
                    fz = origin_tz;
                }
                if (rendered_frames == 0) {
                    log::info("pivot_fit d_raw=" + std::to_string(d_raw) + " d_yz=" + std::to_string(d_yz) +
                              " map=" + std::string(d_yz + 0.05f < d_raw ? "yz_swap" : "identity"));
                }
            }
            f4f23ma::ma4 R = f4f23ma::ma4::identity();
            const float rx = origin_rx * kDeg, ry = origin_ry * kDeg, rz = origin_rz * kDeg;
            if (axis_mode == 0) {
                if (origin_rx != 0.f) R = R * rot_ax(0, rx);
                if (origin_ry != 0.f) R = R * rot_ax(2, ry); // cfgY→Z
                if (origin_rz != 0.f) R = R * rot_ax(1, rz);
            } else if (axis_mode == 1) {
                if (origin_rx != 0.f) R = rot_ax(0, rx) * R;
                if (origin_ry != 0.f) R = rot_ax(2, ry) * R;
                if (origin_rz != 0.f) R = rot_ax(1, rz) * R;
            } else if (axis_mode == 2) {
                if (origin_rx != 0.f) R = R * rot_ax(0, rx);
                if (origin_ry != 0.f) R = R * rot_ax(1, ry);
                if (origin_rz != 0.f) R = R * rot_ax(2, rz);
            }
            f4f23ma::f4 axis_f = R.transform_direction3({1.f, 0.f, 0.f, 0.f}).normalized3();
            f4f23ma::f4 pivot_f{fx, fy, fz, 1.f};
            f4f23ma::f4 P = pivot_f;
            f4f23ma::f4 A = axis_f;
            if (has_origin_trans && axis_mode <= 2) {
                P = draw.transform_point(pivot_f);
                A = draw.transform_direction3(axis_f).normalized3();
            }
            if (axis_mode == 3) {
                P = {hinge_x, hinge_y, hinge_z, 1.f};
                A = {1.f, 0.f, 0.f, 0.f};
            }
            if (axis_mode == 4) {
                P = {hinge_x, hinge_y, hinge_z, 1.f};
                A = {0.f, 1.f, 0.f, 0.f};
            }
            if (axis_mode == 5) {
                P = {hinge_x, hinge_y, hinge_z, 1.f};
                A = {0.f, 0.f, 1.f, 0.f};
            }
            const float rad = rot_deg_fixed * kDeg;
            const DirectX::XMVECTOR axis_v = DirectX::XMVector3Normalize(DirectX::XMVectorSet(A.x, A.y, A.z, 0.f));
            const DirectX::XMMATRIX world = DirectX::XMMatrixTranslation(-P.x, -P.y, -P.z) *
                                           DirectX::XMMatrixRotationAxis(axis_v, rad) *
                                           DirectX::XMMatrixTranslation(P.x, P.y, P.z);
            std::vector<std::array<float, 16>> mats(1);
            pack_world_t(world, mats[0]);
            renderer.set_chunk_world_matrices(mats);
            if (rendered_frames == 0) {
                log::info("rot_test mode=" + std::to_string(axis_mode) + " angle=" + std::to_string(rot_deg_fixed) +
                          " pivot=(" + std::to_string(P.x) + "," + std::to_string(P.y) + "," + std::to_string(P.z) +
                          ") axis=(" + std::to_string(A.x) + "," + std::to_string(A.y) + "," + std::to_string(A.z) +
                          ")");
                log_pivot_radii(world, rot_deg_fixed, P.x, P.y, P.z);
                // Tip motion vs axis (= dial normal for mode0): sink = |Δheight along A|.
                if (!rest_gpu.empty() && has_origin_trans) {
                    const auto tip0 = *std::max_element(rest_gpu.begin(), rest_gpu.end(), [&](const auto& a, const auto& b) {
                        const float da = (a.x - P.x) * (a.x - P.x) + (a.y - P.y) * (a.y - P.y) + (a.z - P.z) * (a.z - P.z);
                        const float db = (b.x - P.x) * (b.x - P.x) + (b.y - P.y) * (b.y - P.y) + (b.z - P.z) * (b.z - P.z);
                        return da < db;
                    });
                    DirectX::XMFLOAT3 tip1{};
                    DirectX::XMStoreFloat3(
                        &tip1, DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(tip0.x, tip0.y, tip0.z, 1.f), world));
                    const float h0 = (tip0.x - P.x) * A.x + (tip0.y - P.y) * A.y + (tip0.z - P.z) * A.z;
                    const float h1 = (tip1.x - P.x) * A.x + (tip1.y - P.y) * A.y + (tip1.z - P.z) * A.z;
                    log::info("tip_plane h0=" + std::to_string(h0) + " h1=" + std::to_string(h1) +
                              " sink=" + std::to_string(std::fabs(h1 - h0)) + " tip0=(" + std::to_string(tip0.x) + "," +
                              std::to_string(tip0.y) + "," + std::to_string(tip0.z) + ") tip1=(" +
                              std::to_string(tip1.x) + "," + std::to_string(tip1.y) + "," + std::to_string(tip1.z) + ")");
                }
            }
        }

        float axis_wvp_t[16]{};
        camera.build_axis_gizmo_wvp(axis_wvp_t);
        renderer.set_axis_gizmo_matrices(axis_wvp_t);

        if (!screenshot_path.empty() && !screenshot_done && rendered_frames + 1 >= 3)
            renderer.set_pending_screenshot(screenshot_path);

        renderer.render();

        if (!screenshot_path.empty() && !screenshot_done && rendered_frames + 1 >= 3) {
            screenshot_done = true;
            log::info("captured screenshot after frame " + std::to_string(rendered_frames + 1));
        }

        if (fps_accum >= 1.f) {
            const float fps_value = static_cast<float>(frame_count) / fps_accum;
            frame_count = 0;
            fps_accum = 0.f;
            std::ostringstream title;
            title << "OMSI Hello O3D | " << static_cast<int>(fps_value)
                  << " FPS | Z arriba | LMB orbitar | RMB zoom | ESC salir";
            glfwSetWindowTitle(window, title.str().c_str());
            log::info("hello o3d fps=" + std::to_string(static_cast<int>(fps_value)));
        }

        if (max_frames > 0 && ++rendered_frames >= max_frames) glfwSetWindowShouldClose(window, GLFW_TRUE);
    }

    renderer.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    log::info("hello o3d exit ok");
    return 0;
}

MeshBounds compute_model_cfg_bounds(const omsi::model_cfg::ModelCfgDocument& cfg) {
    MeshBounds combined{};
    const auto* lod = omsi::model_cfg::primary_lod(cfg);
    if (!lod) return combined;
    const auto model_dir = cfg.cfg_path.parent_path();
    for (const auto& mesh_entry : lod->meshes) {
        if (mesh_entry.mesh_file.empty()) continue;
        const auto o3d_path = model_dir / mesh_entry.mesh_file;
        std::error_code ec;
        if (!std::filesystem::exists(o3d_path, ec)) continue;
        if (auto loaded = o3d::load_mapped(o3d_path)) {
            const auto b = compute_mesh_bounds(*loaded);
            if (!b.valid) continue;
            if (!combined.valid) {
                combined = b;
            } else {
                combined.min_x = (std::min)(combined.min_x, b.min_x);
                combined.min_y = (std::min)(combined.min_y, b.min_y);
                combined.min_z = (std::min)(combined.min_z, b.min_z);
                combined.max_x = (std::max)(combined.max_x, b.max_x);
                combined.max_y = (std::max)(combined.max_y, b.max_y);
                combined.max_z = (std::max)(combined.max_z, b.max_z);
            }
        }
    }
    return combined;
}

void log_model_cfg_summary(const omsi::model_cfg::ModelCfgDocument& cfg) {
    const auto* lod = omsi::model_cfg::primary_lod(cfg);
    if (!lod) return;
    log::info("=== model.cfg dump ===");
    log::info("path: " + cfg.cfg_path.string());
    log::info("tex_detail_factor=" + std::to_string(cfg.tex_detail_factor));
    log::info("ctc_packs=" + std::to_string(cfg.ctc_packs.size()) + " cti_files=" +
              std::to_string(cfg.cti_files.size()) + " ctc_textures=" + std::to_string(cfg.ctc_textures.size()));
    for (const auto& pack : cfg.ctc_packs) {
        log::info("  CTC " + pack.name + " folder=" + pack.folder + " cti=" + std::to_string(pack.cti_files.size()));
    }
    log::info("lod0 distance=" + std::to_string(lod->distance) + " meshes=" + std::to_string(lod->meshes.size()));
    for (std::size_t mi = 0; mi < lod->meshes.size(); ++mi) {
        const auto& mesh = lod->meshes[mi];
        std::string mesh_flags;
        if (mesh.visible_var)
            mesh_flags += " visible=" + *mesh.visible_var + "(" + std::to_string(mesh.visible_default) + ")";
        if (mesh.mouseevent_var) mesh_flags += " mouseevent=" + *mesh.mouseevent_var;
        log::info("  mesh[" + std::to_string(mi) + "] " + mesh.mesh_file +
                  (mesh.is_shadow_mesh ? " [shadow]" : "") + " materials=" + std::to_string(mesh.materials.size()) +
                  mesh_flags);
        for (std::size_t m = 0; m < mesh.materials.size(); ++m) {
            const auto& mat = mesh.materials[m];
            std::string flags;
            if (mat.alpha_mode != omsi::model_cfg::MatlAlphaMode::opaque)
                flags += " alpha=" + std::to_string(static_cast<int>(mat.alpha_mode));
            if (mat.no_zwrite) flags += " noZwrite";
            if (mat.no_zcheck) flags += " noZcheck";
            if (mat.envmap_texture) flags += " envmap";
            if (mat.transmap_texture) flags += " transmap";
            log::info("    mat[" + std::to_string(m) + "] id=" + std::to_string(mat.mat_id) + " tex=\"" +
                      mat.diffuse_texture + "\"" + flags);
        }
    }
    log::info("=== fin model.cfg ===");
}

int run_hello_model_cfg_app(const std::filesystem::path& model_cfg_path, const std::filesystem::path& omsi_root,
                            int max_frames, const std::filesystem::path& screenshot_path) {
    auto resolved = normalize_cli_path(model_cfg_path);
    std::error_code ec;
    if (!std::filesystem::exists(resolved, ec) && !omsi_root.empty() && resolved.is_relative())
        resolved = omsi_root / resolved;
    if (!std::filesystem::exists(resolved, ec)) {
        log::error("model.cfg not found: " + model_cfg_path.string());
        return 2;
    }

    auto cfg = omsi::model_cfg::load_model_cfg(resolved);
    if (!cfg) {
        log::error("failed to parse model.cfg: " + resolved.string());
        return 2;
    }

    log_model_cfg_summary(*cfg);
    const auto bounds = compute_model_cfg_bounds(*cfg);

    if (!glfwInit()) {
        log::error("glfwInit failed");
        return 5;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "OMSI model.cfg", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        log::error("glfwCreateWindow failed");
        return 5;
    }
    glfwShowWindow(window);

    int fbw = 0;
    int fbh = 0;
    glfwGetFramebufferSize(window, &fbw, &fbh);

    gfx::dx12::HelloO3dRenderer renderer{};
    if (!renderer.initialize(glfwGetWin32Window(window), static_cast<std::uint32_t>(fbw),
                             static_cast<std::uint32_t>(fbh))) {
        glfwDestroyWindow(window);
        glfwTerminate();
        log::error("hello model.cfg renderer init failed");
        return 7;
    }

    if (!renderer.load_model_cfg(*cfg)) {
        renderer.shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        log::error("model.cfg mesh upload failed");
        return 7;
    }

    input::LunaCamera camera{};
    if (bounds.valid) {
        camera.frame_omsi_bounds(bounds.min_x, bounds.min_y, bounds.min_z, bounds.max_x, bounds.max_y,
                                 bounds.max_z);
    } else {
        camera.z_up = true;
    }

    auto last = std::chrono::steady_clock::now();
    int rendered_frames = 0;
    float fps_accum = 0.f;
    int frame_count = 0;
    int last_fbw = fbw;
    int last_fbh = fbh;
    bool screenshot_done = false;

    while (!glfwWindowShouldClose(window)) {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        fps_accum += dt;
        ++frame_count;

        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, GLFW_TRUE);

        camera.update(window);

        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (fbw > 0 && fbh > 0 && (fbw != last_fbw || fbh != last_fbh)) {
            renderer.resize(static_cast<std::uint32_t>(fbw), static_cast<std::uint32_t>(fbh));
            last_fbw = fbw;
            last_fbh = fbh;
        }

        const float aspect = fbw > 0 && fbh > 0 ? static_cast<float>(fbw) / static_cast<float>(fbh) : 16.f / 9.f;
        float wvp_t[16]{};
        camera.build_world_view_proj(aspect, wvp_t);
        renderer.set_world_view_proj(wvp_t);

        float axis_wvp_t[16]{};
        camera.build_axis_gizmo_wvp(axis_wvp_t);
        renderer.set_axis_gizmo_matrices(axis_wvp_t);

        if (!screenshot_path.empty() && !screenshot_done && rendered_frames + 1 >= 3)
            renderer.set_pending_screenshot(screenshot_path);

        renderer.render();

        if (!screenshot_path.empty() && !screenshot_done && rendered_frames + 1 >= 3) {
            screenshot_done = true;
            log::info("captured screenshot after frame " + std::to_string(rendered_frames + 1));
        }

        if (fps_accum >= 1.f) {
            const float fps_value = static_cast<float>(frame_count) / fps_accum;
            frame_count = 0;
            fps_accum = 0.f;
            std::ostringstream title;
            title << "OMSI model.cfg | " << static_cast<int>(fps_value) << " FPS | ESC salir";
            glfwSetWindowTitle(window, title.str().c_str());
        }

        if (max_frames > 0 && ++rendered_frames >= max_frames) glfwSetWindowShouldClose(window, GLFW_TRUE);
    }

    renderer.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    log::info("hello model.cfg exit ok");
    return 0;
}

int run_hello_bus_app(const std::filesystem::path& bus_path, const std::filesystem::path& omsi_root, int max_frames,
                      const std::filesystem::path& screenshot_path) {
    auto resolved_bus = normalize_cli_path(bus_path);
    std::error_code ec;
    if (!std::filesystem::exists(resolved_bus, ec) && !omsi_root.empty() && resolved_bus.is_relative())
        resolved_bus = omsi_root / resolved_bus;
    if (!std::filesystem::exists(resolved_bus, ec)) {
        log::error("bus file not found: " + bus_path.string());
        return 2;
    }

    const auto bus_doc = omsi::bus::load_bus(resolved_bus);
    if (!bus_doc) {
        log::error("failed to parse bus: " + resolved_bus.string());
        return 2;
    }

    auto model_cfg_path = omsi::bus::resolve_model_cfg_path(*bus_doc);
    if (!std::filesystem::exists(model_cfg_path, ec)) {
        log::error("model.cfg not found for bus: " + model_cfg_path.string());
        return 2;
    }

    auto cfg = omsi::model_cfg::load_model_cfg(model_cfg_path);
    if (!cfg) {
        log::error("failed to parse model.cfg: " + model_cfg_path.string());
        return 2;
    }

    log_model_cfg_summary(*cfg);
    const auto bounds = compute_model_cfg_bounds(*cfg);

    omsi::script::VehicleRuntime vehicle_rt{};
    if (!vehicle_rt.load(*bus_doc, omsi_root)) omsi::log::warn("vehicle runtime load incomplete");

    // Unit87: apply first Colorscheme .cti setvars (prefer ASEAG / first pack with CTIs).
    {
        std::filesystem::path cti_pick;
        for (const auto& pack : cfg->ctc_packs) {
            if (pack.cti_files.empty()) continue;
            for (const auto& p : pack.cti_files) {
                const auto stem = p.stem().string();
                if (stem.find("ASEAG") != std::string::npos || stem.find("aseag") != std::string::npos) {
                    cti_pick = p;
                    break;
                }
            }
            if (cti_pick.empty()) cti_pick = pack.cti_files.front();
            if (!cti_pick.empty()) break;
        }
        if (cti_pick.empty() && !cfg->cti_files.empty()) cti_pick = cfg->cti_files.front();
        if (!cti_pick.empty()) {
            if (const auto cti = omsi::cti::load_cti(cti_pick)) vehicle_rt.apply_cti(*cti);
        }
    }

    vehicle_rt.start_async_sim();

    omsi::script::VehicleStateLogger state_logger{};
    state_logger.set_interval(2.f);

    omsi::visible::VisibleRuntime visible_rt{};
    visible_rt.init_from_cfg(*cfg);

    omsi::model_cfg::MeshAnimator mesh_animator{};
    mesh_animator.init(*cfg);

    if (!glfwInit()) {
        log::error("glfwInit failed");
        return 5;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1280, 720, "OMSI bus", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        log::error("glfwCreateWindow failed");
        return 5;
    }
    glfwShowWindow(window);

    int fbw = 0;
    int fbh = 0;
    glfwGetFramebufferSize(window, &fbw, &fbh);

    gfx::dx12::HelloO3dRenderer renderer{};
    if (!renderer.initialize(glfwGetWin32Window(window), static_cast<std::uint32_t>(fbw),
                             static_cast<std::uint32_t>(fbh))) {
        glfwDestroyWindow(window);
        glfwTerminate();
        log::error("hello bus renderer init failed");
        return 7;
    }

    if (!renderer.load_model_cfg(*cfg)) {
        renderer.shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        log::error("bus model.cfg mesh upload failed");
        return 7;
    }

    renderer.apply_visible_runtime(*cfg, visible_rt);

    if (bounds.valid) {
        const float ground_z = -0.02f; // tire contact ≈ z=0 (OA101 axles), not whole-mesh min_z
        const float half_x = (std::max)(60.f, (bounds.max_x - bounds.min_x) * 4.f);
        const float half_y = (std::max)(60.f, (bounds.max_y - bounds.min_y) * 4.f);
        renderer.upload_ground_plane((bounds.min_x + bounds.max_x) * 0.5f, (bounds.min_y + bounds.max_y) * 0.5f,
                                     ground_z, half_x, half_y);
    }

    input::LunaCamera exterior_camera{};
    if (bounds.valid) {
        exterior_camera.frame_omsi_bounds(bounds.min_x, bounds.min_y, bounds.min_z, bounds.max_x, bounds.max_y,
                                          bounds.max_z);
    } else {
        exterior_camera.z_up = true;
    }
    exterior_camera.target_x = bus_doc->outside_center_x;
    exterior_camera.target_y = bus_doc->outside_center_y;
    exterior_camera.target_z = bus_doc->outside_center_z;

    input::OmsiBusCamera interior_camera{};
    input::BusCameraController cam_ctrl{};
    cam_ctrl.apply_bus_defaults(bus_doc->std_driver_camera_index);
    if (!bus_doc->driver_cameras.empty()) {
        cam_ctrl.driver_index =
            (std::min)(cam_ctrl.driver_index, static_cast<int>(bus_doc->driver_cameras.size()) - 1);
        cam_ctrl.saved_driver_index = cam_ctrl.driver_index;
    }
    // Screenshot harness: side profile exterior so wheel/ground contact is visible.
    if (!screenshot_path.empty()) {
        cam_ctrl.on_key_f3();
        exterior_camera.theta = 0.f; // side-on (Y-forward bus)
        exterior_camera.phi = 1.35f;
        exterior_camera.radius = (std::max)(exterior_camera.radius, 12.f);
    }

    omsi::inputs::ViewInputBindings view_inputs{};
    view_inputs.load_from_omsi_root(omsi_root);

    omsi::inputs::VehicleInputBindings vehicle_inputs{};
    vehicle_inputs.load_from_omsi_root(omsi_root);

    auto last = std::chrono::steady_clock::now();
    int rendered_frames = 0;
    float fps_accum = 0.f;
    int frame_count = 0;
    int last_fbw = fbw;
    int last_fbh = fbh;
    bool screenshot_done = false;
    input::BusViewMode prev_cam_mode = cam_ctrl.mode;
    double lmb_down_x = 0.0;
    double lmb_down_y = 0.0;
    bool lmb_was = false;
    bool mouse_drive_active = false;
    std::optional<std::string> cockpit_mouseevent;
    bool cockpit_mouse_dragging = false;
    double cockpit_prev_mx = 0.0;
    double cockpit_prev_my = 0.0;
    bool schedule_view_active = false;
    float anim_log_accum = 0.f;

    while (!glfwWindowShouldClose(window)) {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        fps_accum += dt;
        ++frame_count;

        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, GLFW_TRUE);

        view_inputs.update(window);
        vehicle_inputs.update(window, dt);

        if (view_inputs.pressed(omsi::inputs::ViewAction::toggle_mouse_ctrl)) {
            mouse_drive_active = !mouse_drive_active;
            vehicle_rt.set_mouse_drive_mode(mouse_drive_active);
            omsi::log::info(std::string("OMSI mouse drive (O): ") + (mouse_drive_active ? "ON" : "OFF"));
        }

        float throttle = 0.f;
        float brake = 0.f;
        const bool interior_cam = cam_ctrl.mode != input::BusViewMode::exterior;
        if (mouse_drive_active && interior_cam) {
            int win_w = 0;
            int win_h = 0;
            glfwGetWindowSize(window, &win_w, &win_h);
            double mx = 0.0;
            double my = 0.0;
            glfwGetCursorPos(window, &mx, &my);
            const float nx = win_w > 0 ? static_cast<float>((mx / static_cast<double>(win_w) - 0.5) * 2.0) : 0.f;
            const float ny = win_h > 0 ? static_cast<float>((my / static_cast<double>(win_h) - 0.5) * 2.0) : 0.f;
            throttle = (std::max)(0.f, -ny);
            brake = (std::max)(0.f, ny);
            const float inv_r = bus_doc->inv_min_turnradius > 0.01f ? bus_doc->inv_min_turnradius : 0.13f;
            vehicle_rt.set_mouse_drive_input(throttle, brake, nx * inv_r);
            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
                mouse_drive_active = false;
                vehicle_rt.set_mouse_drive_mode(false);
            }
        } else {
            vehicle_rt.set_mouse_drive_mode(false);
            // OMSI 1:1 — only keyboard.cfg bindings (not WASD; those are not throttle/brake).
            if (vehicle_inputs.held("throttle")) throttle = 1.f;
            if (vehicle_inputs.held("brake")) brake = 1.f;
            vehicle_rt.set_driver_input(throttle, brake);
            // Unit129 KeyCommand: KP4/5/6 continuous → inv_lenkradius → Axle_Steering_*.
            vehicle_rt.set_steering_keys(vehicle_inputs.held("steering_left"), vehicle_inputs.held("steering_right"),
                                         vehicle_inputs.held("steering_neutral"));
        }

        vehicle_rt.apply_triggers(vehicle_inputs.pressed_triggers(), vehicle_inputs.released_triggers());

        vehicle_rt.post_tick(dt);
        vehicle_rt.sync_sim();
        state_logger.maybe_log(*bus_doc, vehicle_rt, vehicle_rt.sim_time());

        mesh_animator.update(*cfg, dt, [&](const std::string& name) {
            // Flat-ground: OA101 rear tire meshes sit higher in the wells than front at the
            // same Federweg; bias rear visuals down so both contact z≈0. Scripts still get
            // the real Axle_Suspension values from the vehicle runtime.
            if (name.rfind("Axle_Suspension_", 0) == 0) {
                const bool rear = name.find("Suspension_1_") != std::string::npos;
                // Calibrated side shots: front 0.05 ≈ road; rear 0.35 + heck-down pitch.
                return rear ? 0.35f : 0.05f;
            }
            return vehicle_rt.public_float(name, 0.f);
        });
        mesh_animator.xform_log(*cfg, vehicle_rt.sim_time(),
                                [&](const std::string& name) { return vehicle_rt.public_float(name, 0.f); });
        mesh_animator.door_vert_log(*cfg, vehicle_rt.sim_time());

        if (const auto* lod = omsi::model_cfg::primary_lod(*cfg)) {
            // OMSI Federung around Schwerpunkt. Wheels share the same body pose so
            // body_z(axle)+Axle_Suspension keeps tires near z=0 (see update_suspension).
            const float cog_z = bus_doc->schwerpunkt_m > 0.1f ? bus_doc->schwerpunkt_m : 1.2f;
            const float ride_z = vehicle_rt.ride_height_m();
            const DirectX::XMMATRIX body =
                DirectX::XMMatrixTranslation(0.f, 0.f, -cog_z) *
                DirectX::XMMatrixRotationX(vehicle_rt.body_pitch_rad()) *
                DirectX::XMMatrixRotationY(vehicle_rt.body_roll_rad()) *
                DirectX::XMMatrixTranslation(0.f, 0.f, cog_z + vehicle_rt.body_heave_m() + ride_z);
            auto mats = mesh_animator.matrices_dx_transposed();
            for (std::size_t mi = 0; mi < mats.size(); ++mi) {
                auto& m = mats[mi];
                DirectX::XMFLOAT4X4 stored{};
                std::memcpy(&stored, m.data(), sizeof(stored));
                const DirectX::XMMATRIX mesh = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&stored));
                DirectX::XMMATRIX world = mesh * body;
                if (mi < lod->meshes.size() && lod->meshes[mi].is_shadow_mesh) {
                    DirectX::XMFLOAT4X4 wf{};
                    DirectX::XMStoreFloat4x4(&wf, world);
                    // Flatten onto ground plane under the bus (keep translation XY from body).
                    const float tx = wf._41;
                    const float ty = wf._42;
                    world = DirectX::XMMatrixScaling(1.f, 1.f, 1.f) *
                            DirectX::XMMatrixTranslation(tx, ty, 0.02f);
                }
                DirectX::XMStoreFloat4x4(&stored, DirectX::XMMatrixTranspose(world));
                std::memcpy(m.data(), &stored, sizeof(stored));
            }
            renderer.set_chunk_world_matrices(mats);
            (void)lod;
        }

        anim_log_accum += dt;
        if (anim_log_accum >= 2.f) {
            anim_log_accum = 0.f;
            mesh_animator.debug_log(
                *cfg, [&](const std::string& name) { return vehicle_rt.public_float(name, 0.f); },
                [&](const std::string& name) { return vehicle_rt.vars().float_index(name).has_value(); });
        }

        if (view_inputs.pressed(omsi::inputs::ViewAction::set_driver)) cam_ctrl.on_key_f1();
        if (view_inputs.pressed(omsi::inputs::ViewAction::set_passenger)) cam_ctrl.on_key_f2();
        if (view_inputs.pressed(omsi::inputs::ViewAction::set_outside)) cam_ctrl.on_key_f3();
        if (view_inputs.pressed(omsi::inputs::ViewAction::reset_direction)) interior_camera.reset_look();
        if (view_inputs.pressed(omsi::inputs::ViewAction::reset_all_directions)) interior_camera.reset_look();

        if (view_inputs.pressed(omsi::inputs::ViewAction::set_schedule)) {
            if (const auto sched = omsi::bus::schedule_camera_index(*bus_doc)) {
                cam_ctrl.mode = input::BusViewMode::driver;
                cam_ctrl.driver_index = static_cast<int>(*sched);
                schedule_view_active = true;
            }
        } else if (schedule_view_active && !view_inputs.held(omsi::inputs::ViewAction::set_schedule)) {
            schedule_view_active = false;
            cam_ctrl.driver_index = cam_ctrl.saved_driver_index;
        }

        if (cam_ctrl.mode == input::BusViewMode::driver && !bus_doc->driver_cameras.empty()) {
            if (view_inputs.pressed(omsi::inputs::ViewAction::interiorcam_minus))
                cam_ctrl.cycle_driver(-1, static_cast<int>(bus_doc->driver_cameras.size()));
            if (view_inputs.pressed(omsi::inputs::ViewAction::interiorcam_plus))
                cam_ctrl.cycle_driver(1, static_cast<int>(bus_doc->driver_cameras.size()));
            if (!schedule_view_active) cam_ctrl.saved_driver_index = cam_ctrl.driver_index;
        } else if (cam_ctrl.mode == input::BusViewMode::passenger && !bus_doc->passenger_cameras.empty()) {
            if (view_inputs.pressed(omsi::inputs::ViewAction::interiorcam_minus))
                cam_ctrl.cycle_passenger(-1, static_cast<int>(bus_doc->passenger_cameras.size()));
            if (view_inputs.pressed(omsi::inputs::ViewAction::interiorcam_plus))
                cam_ctrl.cycle_passenger(1, static_cast<int>(bus_doc->passenger_cameras.size()));
        }

        if (cam_ctrl.mode != prev_cam_mode) interior_camera.reset_look();
        prev_cam_mode = cam_ctrl.mode;

        if (cam_ctrl.mode == input::BusViewMode::exterior) {
            exterior_camera.target_z =
                bus_doc->outside_center_z + vehicle_rt.body_heave_m() + vehicle_rt.ride_height_m();
            exterior_camera.update(window);
        } else if (!mouse_drive_active) {
            interior_camera.update_mouse(window);
        }

        glfwGetFramebufferSize(window, &fbw, &fbh);
        if (fbw > 0 && fbh > 0 && (fbw != last_fbw || fbh != last_fbh)) {
            renderer.resize(static_cast<std::uint32_t>(fbw), static_cast<std::uint32_t>(fbh));
            last_fbw = fbw;
            last_fbh = fbh;
        }

        const float aspect = fbw > 0 && fbh > 0 ? static_cast<float>(fbw) / static_cast<float>(fbh) : 16.f / 9.f;
        float wvp_t[16]{};
        if (cam_ctrl.mode == input::BusViewMode::exterior) {
            exterior_camera.build_world_view_proj(aspect, wvp_t);
        } else if (cam_ctrl.mode == input::BusViewMode::driver && !bus_doc->driver_cameras.empty()) {
            interior_camera.set_definition(bus_doc->driver_cameras[static_cast<std::size_t>(cam_ctrl.driver_index)],
                                           cam_ctrl.driver_index);
            interior_camera.set_body_pose(vehicle_rt.body_heave_m(), vehicle_rt.body_pitch_rad(),
                                          vehicle_rt.body_roll_rad(), vehicle_rt.ride_height_m());
            interior_camera.build_world_view_proj(aspect, wvp_t);
        } else if (!bus_doc->passenger_cameras.empty()) {
            interior_camera.set_definition(bus_doc->passenger_cameras[static_cast<std::size_t>(cam_ctrl.passenger_index)],
                                           cam_ctrl.passenger_index);
            interior_camera.set_body_pose(vehicle_rt.body_heave_m(), vehicle_rt.body_pitch_rad(),
                                          vehicle_rt.body_roll_rad(), vehicle_rt.ride_height_m());
            interior_camera.build_world_view_proj(aspect, wvp_t);
        } else {
            exterior_camera.build_world_view_proj(aspect, wvp_t);
        }
        renderer.set_world_view_proj(wvp_t);

        float axis_wvp_t[16]{};
        exterior_camera.build_axis_gizmo_wvp(axis_wvp_t);
        renderer.set_axis_gizmo_matrices(axis_wvp_t);

        const bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        const bool mmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
        if (lmb && !lmb_was) glfwGetCursorPos(window, &lmb_down_x, &lmb_down_y);

        // Cockpit [mouseevent] → TriggerXML (Unit128); disabled while O mouse-drive is active.
        if (!mouse_drive_active && cam_ctrl.mode != input::BusViewMode::exterior && !mmb && fbw > 0 && fbh > 0) {
            double mx = 0.0;
            double my = 0.0;
            glfwGetCursorPos(window, &mx, &my);
            int win_w = 0;
            int win_h = 0;
            glfwGetWindowSize(window, &win_w, &win_h);
            const float sx = win_w > 0 ? static_cast<float>(fbw) / static_cast<float>(win_w) : 1.f;
            const float sy = win_h > 0 ? static_cast<float>(fbh) / static_cast<float>(win_h) : 1.f;
            const float pick_x = static_cast<float>(mx) * sx;
            const float pick_y = static_cast<float>(my) * sy;

            if (lmb && !lmb_was) {
                if (const auto picked = renderer.pick_chunk(pick_x, pick_y, static_cast<std::uint32_t>(fbw),
                                                            static_cast<std::uint32_t>(fbh), wvp_t)) {
                    const auto chunks = renderer.chunk_info();
                    if (*picked < chunks.size() && chunks[*picked].mouseevent_var) {
                        cockpit_mouseevent = *chunks[*picked].mouseevent_var;
                        cockpit_mouse_dragging = false;
                        cockpit_prev_mx = mx;
                        cockpit_prev_my = my;
                        vehicle_rt.trigger_xml(*cockpit_mouseevent, 1);
                    }
                }
            } else if (lmb && lmb_was && cockpit_mouseevent) {
                const double drag = std::hypot(mx - lmb_down_x, my - lmb_down_y);
                if (drag >= 5.0 && !cockpit_mouse_dragging) {
                    cockpit_mouse_dragging = true;
                    vehicle_rt.trigger_xml(*cockpit_mouseevent, 2);
                }
                if (cockpit_mouse_dragging) {
                    vehicle_rt.set_mouse(static_cast<float>(mx - cockpit_prev_mx),
                                         static_cast<float>(my - cockpit_prev_my));
                    cockpit_prev_mx = mx;
                    cockpit_prev_my = my;
                }
            } else if (!lmb && lmb_was && cockpit_mouseevent) {
                vehicle_rt.trigger_xml(*cockpit_mouseevent, 0);
                if (visible_rt.trigger_mouseevent(*cfg, *cockpit_mouseevent))
                    renderer.apply_visible_runtime(*cfg, visible_rt);
                cockpit_mouseevent.reset();
                cockpit_mouse_dragging = false;
            }
        } else if (!lmb && lmb_was && cockpit_mouseevent) {
            vehicle_rt.trigger_xml(*cockpit_mouseevent, 0);
            cockpit_mouseevent.reset();
            cockpit_mouse_dragging = false;
        }
        lmb_was = lmb;

        const int shot_at = (max_frames > 3) ? max_frames : 3;
        if (!screenshot_path.empty() && !screenshot_done && rendered_frames + 1 >= shot_at)
            renderer.set_pending_screenshot(screenshot_path);

        renderer.render();

        if (!screenshot_path.empty() && !screenshot_done && rendered_frames + 1 >= shot_at) {
            screenshot_done = true;
            log::info("captured screenshot after frame " + std::to_string(rendered_frames + 1));
        }

        if (fps_accum >= 1.f) {
            const float fps_value = static_cast<float>(frame_count) / fps_accum;
            frame_count = 0;
            fps_accum = 0.f;
            const char* mode =
                cam_ctrl.mode == input::BusViewMode::driver
                    ? "F1 conductor"
                    : (cam_ctrl.mode == input::BusViewMode::passenger ? "F2 pasajero" : "F3 exterior");
            std::ostringstream title;
            title << "OMSI bus | " << static_cast<int>(fps_value) << " FPS | " << mode
                  << " | v=" << static_cast<int>(vehicle_rt.public_float("Velocity"))
                  << (mouse_drive_active ? " | O=mouse drive" : " | O=mouse drive off")
                  << " | N=motor L=luz 1/2/3=puertas | W/↑ gas S/↓ freno";
            glfwSetWindowTitle(window, title.str().c_str());
        }

        if (max_frames > 0 && ++rendered_frames >= max_frames) glfwSetWindowShouldClose(window, GLFW_TRUE);
    }

    renderer.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    log::info("hello bus exit ok");
    return 0;
}

} // namespace omsi::app
