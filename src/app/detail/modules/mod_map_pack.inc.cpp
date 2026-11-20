// ---- Archivo > Empaquetar mapa: options dialog, worker thread, progress, open the folder ----
// Included inside run_map_editor_app (setup_06). Uses omsi::pack (src/editor/map_packager.cpp).
    struct MapPackUi {
        bool open = false;
        bool running = false;
        omsi::pack::PackOptions opt;
        std::shared_ptr<omsi::pack::PackProgress> progress;
        std::thread worker;
        char out_buf[1024]{};
        int level_idx = 2; // 0 store, 1 fast, 2 normal, 3 max
        bool opened_folder = false;
        ~MapPackUi() {
            if (worker.joinable()) {
                if (progress) progress->cancel = true;
                worker.join();
            }
        }
    };
    auto map_pack = std::make_shared<MapPackUi>();
    auto map_pack_live_cfg = [&]() -> std::filesystem::path {
        std::filesystem::path live = cfg.workspace && !cfg.workspace->actual_dir.empty()
                                         ? map::workspace_live_map_dir(*cfg.workspace)
                                         : map::live_map_dir_from_folder(cfg.global.root);
        if (live.empty()) live = cfg.global.root;
        // Pack the edited copy when the live map has no tiles of its own (fresh map made in the editor)
        std::error_code ec;
        if (!std::filesystem::exists(live / "global.cfg", ec) && cfg.workspace) live = cfg.workspace->actual_dir;
        return live / "global.cfg";
    };
    auto draw_map_pack_dialog = [&]() {
        auto& st = panel.imgui_state();
        auto& mp = *map_pack;
        if (st.pack_dialog_open && !mp.open) {
            st.pack_dialog_open = false;
            mp.open = true;
            mp.opened_folder = false;
            mp.opt = omsi::pack::PackOptions{};
            mp.opt.omsi_root = cfg.omsi_root;
            mp.opt.global_cfg = map_pack_live_cfg();
            {
                // tools\map_dependency_scanner next to the exe (copied by the build), else the source tree
                wchar_t exe[MAX_PATH]{};
                GetModuleFileNameW(nullptr, exe, MAX_PATH);
                const std::filesystem::path near_exe =
                    std::filesystem::path(exe).parent_path() / "tools" / "map_dependency_scanner";
                std::error_code ec;
                mp.opt.scanner_dir = std::filesystem::exists(near_exe / "f4_pack_scan.py", ec)
                                         ? near_exe
                                         : std::filesystem::path(OMSI_F4_SOURCE_DIR_W) / "tools" / "map_dependency_scanner";
            }
            const auto def = omsi::pack::default_pack_path(cfg.omsi_root, mp.opt.global_cfg);
            std::snprintf(mp.out_buf, sizeof(mp.out_buf), "%s", omsi::wide_to_utf8(def.native()).c_str());
            ImGui::OpenPopup("Empaquetar mapa");
        }
        if (!mp.open) return;
        if (!ImGui::IsPopupOpen("Empaquetar mapa")) ImGui::OpenPopup("Empaquetar mapa");
        ImGui::SetNextWindowSize(ImVec2(560.f, 0.f), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("Empaquetar mapa", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        const bool busy = mp.running && mp.progress && !mp.progress->done;
        ImGui::TextDisabled("Mapa: %s", omsi::path_text(mp.opt.global_cfg.parent_path().filename()).c_str());
        ImGui::Separator();
        if (busy) ImGui::BeginDisabled();
        ImGui::TextUnformatted("Incluir:");
        ImGui::Checkbox("Mapa (tiles, terreno, cfg, texturas del mapa)", &mp.opt.map_files);
        ImGui::Checkbox("Objetos (.sco + mallas, texturas, scripts, sonidos)", &mp.opt.objects);
        ImGui::Checkbox("Splines (.sli + texturas)", &mp.opt.splines);
        ImGui::Checkbox("Vehiculos AI (ailists, humanos, choferes)", &mp.opt.ai_vehicles);
        ImGui::Checkbox("Vehiculos depot ([standarddepot])", &mp.opt.depot_vehicles);
        static const char* kLevels[] = {"Sin compresion (rapido, zip grande)", "Rapida", "Normal", "Maxima (lento)"};
        ImGui::SetNextItemWidth(300.f);
        ImGui::Combo("Compresion", &mp.level_idx, kLevels, IM_ARRAYSIZE(kLevels));
        ImGui::Checkbox("Excluir contenido original y DLC de Steam", &mp.opt.exclude_steam);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Lee los manifiestos de depot de Steam (los mismos datos que SteamDB) de OMSI 2 y\n"
                              "sus DLC y deja afuera todo archivo que venga con el juego o un DLC comprado.\n"
                              "Asi el zip solo lleva contenido propio o de terceros.");
        ImGui::SetNextItemWidth(470.f);
        ImGui::InputText("##pack_out", mp.out_buf, sizeof(mp.out_buf));
        ImGui::SameLine();
        ImGui::TextUnformatted("salida .zip");
        if (busy) ImGui::EndDisabled();

        if (mp.progress) {
            auto& pr = *mp.progress;
            std::string stage, cur, res;
            {
                std::lock_guard<std::mutex> lk(pr.mu);
                stage = pr.stage;
                cur = pr.current;
                res = pr.result;
            }
            ImGui::Separator();
            char ov[96];
            std::snprintf(ov, sizeof(ov), "%.0f%%  (%d / %d)", pr.frac.load() * 100.f, pr.added, pr.total);
            ImGui::ProgressBar(pr.frac.load(), ImVec2(-FLT_MIN, 0.f), ov);
            ImGui::TextUnformatted(stage.c_str());
            if (!pr.done && !cur.empty()) ImGui::TextDisabled("%s", cur.c_str());
            if (pr.done) {
                ImGui::TextColored(pr.ok ? ImVec4(0.4f, 0.9f, 0.4f, 1.f) : ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s",
                                   res.c_str());
                if (pr.ok && !mp.opened_folder) {
                    mp.opened_folder = true;
                    const std::wstring args = L"/select,\"" + mp.opt.out_zip.wstring() + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                    panel.set_status("Paquete listo: " + omsi::path_text(mp.opt.out_zip.filename()));
                }
            }
        }
        ImGui::Separator();
        if (!busy) {
            const bool any = mp.opt.map_files || mp.opt.objects || mp.opt.splines || mp.opt.ai_vehicles ||
                             mp.opt.depot_vehicles;
            if (!any) ImGui::BeginDisabled();
            if (ImGui::Button(mp.progress && mp.progress->done ? "Empaquetar de nuevo" : "Empaquetar", ImVec2(160.f, 0.f))) {
                if (mp.worker.joinable()) mp.worker.join();
                static const int kLv[] = {0, 1, 6, 9};
                mp.opt.compression = kLv[std::clamp(mp.level_idx, 0, 3)];
                mp.opt.out_zip = std::filesystem::path(omsi::utf8_to_wide(mp.out_buf));
                mp.opened_folder = false;
                mp.progress = std::make_shared<omsi::pack::PackProgress>();
                mp.running = true;
                auto prog = mp.progress;
                const auto opt = mp.opt;
                mp.worker = std::thread([opt, prog]() { omsi::pack::run_pack(opt, *prog); });
            }
            if (!any) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cerrar", ImVec2(120.f, 0.f))) {
                if (mp.worker.joinable()) mp.worker.join();
                mp.open = false;
                mp.running = false;
                mp.progress.reset();
                ImGui::CloseCurrentPopup();
            }
        } else if (ImGui::Button("Cancelar", ImVec2(120.f, 0.f))) {
            mp.progress->cancel = true;
        }
        ImGui::EndPopup();
    };
