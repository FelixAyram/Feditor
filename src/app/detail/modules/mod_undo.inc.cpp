// MOD: undo — aislado (push/pop/clear snapshots).

    std::vector<EditorSnapshot> undo_stack;
    std::vector<EditorSnapshot> redo_stack;
    struct CameraSnapshot {
        float target_x = 0.f;
        float target_y = 0.f;
        float target_z = 0.f;
        float radius = 0.f;
    };
    std::deque<CameraSnapshot> camera_undo_stack;
    std::deque<CameraSnapshot> camera_redo_stack;
    CameraSnapshot camera_track_last{};
    bool camera_track_init = false;
    bool camera_apply_silent = false;
    std::chrono::steady_clock::time_point camera_undo_last_push{};
    bool camera_undo_last_push_valid = false;
    // Unlimited undo/redo (user request). Snapshots are shifted on tile stream.
    constexpr std::size_t kMaxUndo = 32; // cap: large maps make unlimited undo very slow
    auto snap_brief = [](const EditorSnapshot& s) -> std::string {
        return "obj=" + std::to_string(s.objects.size()) + " spl=" + std::to_string(s.splines.size()) +
               " tr_obj=" + std::to_string(s.transient_objects.size()) +
               " tr_spl=" + std::to_string(s.transient_splines.size());
    };
    auto edit_mode_name = [](EditMode m) -> const char* {
        switch (m) {
        case EditMode::None: return "None";
        case EditMode::Grab: return "Grab";
        case EditMode::GrabLength: return "GrabLength";
        case EditMode::GrabHeight: return "GrabHeight";
        case EditMode::Radius: return "Radius";
        case EditMode::Rotate: return "Rotate";
        case EditMode::SplineFollow: return "SplineFollow";
        case EditMode::NewGrab: return "NewGrab";
        case EditMode::NewFollow: return "NewFollow";
        case EditMode::NewScoFollow: return "NewScoFollow";
        case EditMode::TrimSpline: return "TrimSpline";
        case EditMode::AttachPickParent: return "AttachPickParent";
        case EditMode::AttachPickSpline: return "AttachPickSpline";
        case EditMode::CompleteSpline: return "CompleteSpline";
        case EditMode::PickSplineGrad: return "PickSplineGrad";
        case EditMode::EditSplineGrad: return "EditSplineGrad";
        case EditMode::Mesh: return "Mesh";
        default: return "?";
        }
    };
    auto marquee_phase_name = [](MarqueePhase p) -> const char* {
        switch (p) {
        case MarqueePhase::None: return "None";
        case MarqueePhase::Select: return "Select";
        case MarqueePhase::Placing: return "Placing";
        default: return "?";
        }
    };
    auto session_state = [&]() -> std::string {
        std::string s = "mode=";
        s += edit_mode_name(edit_mode);
        s += " marquee=";
        s += marquee_phase_name(marquee.phase);
        s += " undo=" + std::to_string(undo_stack.size());
        s += " redo=" + std::to_string(redo_stack.size());
        s += " sel_obj=" + std::to_string(selected_object);
        s += " sel_spl=" + std::to_string(selected_spline);
        s += " grp=" + std::to_string(group_sel_obj.size()) + "+" + std::to_string(group_sel_spl.size());
        if (shift_s_move.active)
            s += " ss_move=" + std::to_string(shift_s_move.items.size());
        return s;
    };
    auto fill_snapshot_focus = [&](EditorSnapshot& snap) {
        // Record what the user is editing NOW — the mutation that follows this push
        // will touch these IDs. Undo/redo then remeshes only them (not the whole map).
        snap.focus_spline_ids.clear();
        snap.focus_object_ids.clear();
        auto push_spl_id = [&](std::int32_t id) {
            if (id == 0) return;
            if (std::find(snap.focus_spline_ids.begin(), snap.focus_spline_ids.end(), id) ==
                snap.focus_spline_ids.end())
                snap.focus_spline_ids.push_back(id);
        };
        auto push_obj_id = [&](std::int32_t id) {
            if (id == 0) return;
            if (std::find(snap.focus_object_ids.begin(), snap.focus_object_ids.end(), id) ==
                snap.focus_object_ids.end())
                snap.focus_object_ids.push_back(id);
        };
        if (selected_spline >= 0 && static_cast<std::size_t>(selected_spline) < edit_splines.size())
            push_spl_id(edit_splines[static_cast<std::size_t>(selected_spline)].id);
        for (int gi : group_sel_spl) {
            if (gi >= 0 && static_cast<std::size_t>(gi) < edit_splines.size())
                push_spl_id(edit_splines[static_cast<std::size_t>(gi)].id);
        }
        // Stick-chain / fill-chain: walk next_id AND prev_id so undo remeshes
        // every neighbor adapted during grab (L/R/pos).
        {
            std::unordered_set<std::int32_t> seen;
            std::vector<std::int32_t> seeds = snap.focus_spline_ids;
            auto lookup = [&](std::int32_t sid) -> const map::SplineInst* {
                for (const auto& x : edit_splines)
                    if (x.id == sid) return &x;
                return nullptr;
            };
            for (std::int32_t seed : seeds) {
                std::int32_t sid = seed;
                for (int hop = 0; hop < 256; ++hop) {
                    if (sid == 0 || !seen.insert(sid).second) break;
                    push_spl_id(sid);
                    const map::SplineInst* cur = lookup(sid);
                    if (!cur) break;
                    sid = cur->next_id;
                }
                sid = seed;
                for (int hop = 0; hop < 256; ++hop) {
                    const map::SplineInst* cur = lookup(sid);
                    if (!cur || cur->prev_id == 0) break;
                    sid = cur->prev_id;
                    if (!seen.insert(sid).second) break;
                    push_spl_id(sid);
                }
            }
        }
        if (selected_object >= 0 && static_cast<std::size_t>(selected_object) < edit_objects.size())
            push_obj_id(edit_objects[static_cast<std::size_t>(selected_object)].id);
        for (int gi : group_sel_obj) {
            if (gi >= 0 && static_cast<std::size_t>(gi) < edit_objects.size())
                push_obj_id(edit_objects[static_cast<std::size_t>(gi)].id);
        }
    };
    auto push_undo = [&](const char* reason) {
        omsi::editor::CommandManager::Get().Note(reason ? reason : "push_undo", session_state());
        undo_stack.push_back(capture_editor_snapshot());
        undo_stack.back().action = reason ? reason : "";
        fill_snapshot_focus(undo_stack.back());
        if (kMaxUndo > 0 && undo_stack.size() > kMaxUndo) undo_stack.erase(undo_stack.begin());
        redo_stack.clear();
        editor_log::event("UNDO_PUSH", std::string(reason) + " | " + session_state() +
                                             " snap=" + snap_brief(undo_stack.back()) +
                                             " focus_s=" + std::to_string(undo_stack.back().focus_spline_ids.size()));
    };
    auto pop_undo = [&](const char* reason) {
        const std::size_t before = undo_stack.size();
        if (undo_stack.empty()) {
            editor_log::event("UNDO_POP_SKIP", std::string(reason) + " empty | " + session_state());
            return;
        }
        undo_stack.pop_back();
        editor_log::event("UNDO_POP",
                          std::string(reason) + " " + std::to_string(before) + "->" +
                              std::to_string(undo_stack.size()) + " | " + session_state());
    };
    auto clear_undo = [&](const char* reason) {
        editor_log::event("UNDO_CLEAR", std::string(reason) + " was undo=" + std::to_string(undo_stack.size()) +
                                             " redo=" + std::to_string(redo_stack.size()) + " | " + session_state());
        undo_stack.clear();
        redo_stack.clear();
    };