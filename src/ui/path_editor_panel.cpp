// Path editor tab (model of the web "OMSI Path Editor 3D", implemented on the engine).
// One tab with sub-tabs Paths / Traffic lights / Props / Textures. It edits the single
// PathEditorState owned by the run loop (no mirror copy): field edits set dirty + rebuild_requested,
// while actions that need an undo snapshot (new/duplicate/delete/paste/save/exit) set req_* flags.
// All labels go through i18n::tr so the panel follows the editor language.

#include "ui/map_editor_imgui_panel.hpp"

#include "omsi/sco_types.hpp"
#include "ui/editor_side_panel.hpp"
#include "ui/i18n.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace omsi::ui {

namespace {

using omsi::app::map_editor::PathEditorState;

// Translated label with a stable ImGui id suffix ("text##id").
std::string L(const char* key, const char* id = nullptr) {
    std::string s = i18n::tr(key);
    if (id) {
        s += "##";
        s += id;
    }
    return s;
}

void draw_paths_subtab(EditorSidePanel& panel, PathEditorState& pe) {
    auto& st = panel.imgui_state();
    ImGui::Checkbox(L("paths.hide_model", "hide_model").c_str(), &st.sco_path_hide_model);
    ImGui::SameLine();
    ImGui::Checkbox(L("paths.only_active", "only_active").c_str(), &st.sco_path_hide_visual);
    {
        const char* styles[2] = {i18n::tr("paths.style.original"), i18n::tr("paths.style.modern")};
        ImGui::SetNextItemWidth(200.f);
        ImGui::Combo(L("paths.style", "pstyle").c_str(), &st.path_style, styles, 2);
    }
    ImGui::Separator();

    const int count = static_cast<int>(pe.paths.size());
    const bool ctrl = ImGui::GetIO().KeyCtrl;

    const char* kinds[4] = {i18n::tr("kind.street"), i18n::tr("kind.sidewalk"), i18n::tr("kind.rail"),
                            i18n::tr("kind.air")};
    const char* dirs[3] = {i18n::tr("dir.forward"), i18n::tr("dir.reverse"), i18n::tr("dir.both")};
    const char* blinks[4] = {i18n::tr("turn.none"), i18n::tr("turn.straight"), i18n::tr("turn.left"),
                             i18n::tr("turn.right")};

    // Colores por tipo de path (igual que el overlay 2D/3D): calle, vereda, riel, aire.
    static const ImU32 kKindCol[4] = {IM_COL32(96, 168, 255, 255), IM_COL32(190, 200, 210, 255),
                                      IM_COL32(232, 150, 66, 255), IM_COL32(120, 220, 255, 255)};
    if (ImGui::BeginChild("##path_list", ImVec2(0.f, 130.f), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < count; ++i) {
            const auto& p = pe.paths[static_cast<std::size_t>(i)];
            const int k = (p.type >= 0 && p.type < 4) ? p.type : 0;
            char label[160]{};
            std::snprintf(label, sizeof(label), "P%d%s · %s · r=%.1f  (%.1fm)", i,
                           pe.is_selected(i) ? "*" : "", kinds[k], static_cast<double>(p.radius),
                           static_cast<double>(p.length));
            const ImVec4 col = ImGui::ColorConvertU32ToFloat4(kKindCol[k]);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (ImGui::Selectable(label, pe.is_selected(i))) {
                ImGui::PopStyleColor();
                if (ctrl)
                    pe.toggle_select(i);
                else
                    pe.select_only(i);
            } else {
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::EndChild();

    const bool has_sel = pe.has_index(pe.sel);
    auto* ap = has_sel ? &pe.paths[static_cast<std::size_t>(pe.sel)] : nullptr;

    if (ImGui::Button(L("paths.new", "pnew").c_str())) pe.req_new = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(pe.selected.empty() && !has_sel);
    if (ImGui::Button(L("common.delete", "pdel").c_str())) pe.req_delete = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("<##pprev") && count > 0) pe.select_only((pe.sel <= 0) ? count - 1 : pe.sel - 1);
    ImGui::SameLine();
    if (ImGui::Button(">##pnext") && count > 0) pe.select_only((pe.sel + 1) % count);
    ImGui::SameLine();
    ImGui::TextDisabled("%d%s", count, pe.dirty ? " *" : "");

    auto touch = [&]() {
        pe.dirty = true;
        pe.rebuild_requested = true;
    };

    ImGui::Separator();
    if (ap) {
        ImGui::PushItemWidth(150.f);
        bool ch = false;
        ch |= ImGui::DragFloat("X##px", &ap->x, 0.05f);
        ch |= ImGui::DragFloat("Y##py", &ap->y, 0.05f);
        ch |= ImGui::DragFloat("Z##pz", &ap->z, 0.05f);
        ch |= ImGui::DragFloat(L("paths.heading", "phdg").c_str(), &ap->heading_deg, 0.5f);
        ch |= ImGui::DragFloat(L("paths.radius", "prad").c_str(), &ap->radius, 0.1f);
        ch |= ImGui::DragFloat(L("paths.length", "plen").c_str(), &ap->length, 0.1f, 0.0f, 100000.f);
        ch |= ImGui::DragFloat(L("paths.width", "pwid").c_str(), &ap->width, 0.1f, 0.05f, 100.f);
        ch |= ImGui::DragFloat(L("paths.grad_start", "pgs").c_str(), &ap->grad_start, 0.01f);
        ch |= ImGui::DragFloat(L("paths.grad_end", "pge").c_str(), &ap->grad_end, 0.01f);
        int kind = (ap->type >= 0 && ap->type < 4) ? ap->type : 0;
        if (ImGui::Combo(L("paths.kind", "pkind").c_str(), &kind, kinds, 4)) {
            ap->type = kind;
            ch = true;
        }
        int dir = (ap->direction >= 0 && ap->direction < 3) ? ap->direction : 0;
        if (ImGui::Combo(L("paths.dir", "pdir").c_str(), &dir, dirs, 3)) {
            ap->direction = dir;
            ch = true;
        }
        int blink = (ap->blinker >= 0 && ap->blinker < 4) ? ap->blinker : 0;
        if (ImGui::Combo(L("paths.turn", "pturn").c_str(), &blink, blinks, 4)) {
            ap->blinker = blink;
            ch = true;
        }
        int tl_idx = ap->has_use_traffic_light ? ap->use_traffic_light + 1 : 0;
        std::vector<std::string> names{i18n::tr("common.none")};
        for (int i = 0; i < static_cast<int>(pe.tl.lights.size()); ++i)
            names.push_back(std::to_string(i) + " " + pe.tl.lights[static_cast<std::size_t>(i)].name);
        std::vector<const char*> cnames;
        cnames.reserve(names.size());
        for (const auto& s : names) cnames.push_back(s.c_str());
        if (ImGui::Combo(L("paths.traffic_light", "ptl").c_str(), &tl_idx, cnames.data(),
                         static_cast<int>(cnames.size()))) {
            if (tl_idx <= 0) {
                ap->has_use_traffic_light = false;
                ap->use_traffic_light = -1;
            } else {
                ap->has_use_traffic_light = true;
                ap->use_traffic_light = tl_idx - 1;
            }
            ch = true;
        }
        ch |= ImGui::Checkbox(L("paths.crossing", "pcross").c_str(), &ap->crossing_problem);
        ch |= ImGui::Checkbox(L("paths.block", "pblock").c_str(), &ap->block_path);
        ImGui::PopItemWidth();
        if (ch) touch();
    } else {
        ImGui::TextDisabled("%s", i18n::tr("paths.no_selection"));
    }

    ImGui::Separator();
    if (ImGui::Button(L("common.undo", "pundo").c_str(), ImVec2(84.f, 0.f))) pe.req_undo = true;
    ImGui::SameLine();
    if (ImGui::Button(L("common.redo", "predo").c_str(), ImVec2(84.f, 0.f))) pe.req_redo = true;
    ImGui::SameLine();
    if (ImGui::Button(L("common.close", "pclose").c_str(), ImVec2(84.f, 0.f))) pe.req_exit = true;
    if (ImGui::Button(L("common.save_sco", "psave").c_str(), ImVec2(-FLT_MIN, 0.f))) pe.req_save = true;
}

void draw_tl_subtab(EditorSidePanel& panel, PathEditorState& pe) {
    bool has_tl = pe.has_tl;
    if (ImGui::Checkbox(L("tl.group", "tlgrp").c_str(), &has_tl)) {
        pe.has_tl = has_tl;
        pe.dirty = true;
    }
    ImGui::DragFloat(L("tl.cycle", "tlcyc").c_str(), &pe.tl.cycle, 0.1f, 0.f, 600.f);

    if (ImGui::Button(L("tl.add_light", "tladd").c_str())) {
        sco::ScoTrafficLight L2{};
        L2.name = "Light" + std::to_string(static_cast<int>(pe.tl.lights.size()));
        L2.phases.push_back(sco::ScoTrafficPhase{6, 10.f});
        L2.phases.push_back(sco::ScoTrafficPhase{0, 10.f});
        pe.tl.lights.push_back(std::move(L2));
        pe.has_tl = true;
        pe.tl_sel = static_cast<int>(pe.tl.lights.size()) - 1;
        pe.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(L("tl.del_light", "tldel").c_str()) && !pe.tl.lights.empty() && pe.tl_sel >= 0 &&
        static_cast<std::size_t>(pe.tl_sel) < pe.tl.lights.size()) {
        const int rem = pe.tl_sel;
        pe.tl.lights.erase(pe.tl.lights.begin() + rem);
        for (auto& p : pe.paths) {
            if (!p.has_use_traffic_light) continue;
            if (p.use_traffic_light == rem) {
                p.has_use_traffic_light = false;
                p.use_traffic_light = -1;
            } else if (p.use_traffic_light > rem) {
                --p.use_traffic_light;
            }
        }
        pe.tl_sel = (std::min)(pe.tl_sel, static_cast<int>(pe.tl.lights.size()) - 1);
        pe.has_tl = !pe.tl.lights.empty();
        pe.dirty = true;
    }

    if (ImGui::BeginChild("##tl_list", ImVec2(0.f, 96.f), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < static_cast<int>(pe.tl.lights.size()); ++i) {
            char lab[128]{};
            std::snprintf(lab, sizeof(lab), "%d %s", i, pe.tl.lights[static_cast<std::size_t>(i)].name.c_str());
            if (ImGui::Selectable(lab, i == pe.tl_sel)) pe.tl_sel = i;
        }
    }
    ImGui::EndChild();

    if (pe.tl_sel >= 0 && static_cast<std::size_t>(pe.tl_sel) < pe.tl.lights.size()) {
        auto& light = pe.tl.lights[static_cast<std::size_t>(pe.tl_sel)];
        char name[64]{};
        std::snprintf(name, sizeof(name), "%s", light.name.c_str());
        if (ImGui::InputText(L("tl.name", "tlnm").c_str(), name, sizeof(name))) {
            light.name = name;
            pe.dirty = true;
        }
        if (ImGui::DragFloat(L("tl.approach", "tlapp").c_str(), &light.approach_dist, 0.1f, 0.f, 200.f))
            pe.dirty = true;

        float sum = 0.f;
        for (const auto& ph : light.phases) sum += ph.duration;
        ImGui::Text("%s: %.1f s  (%.1f)", i18n::tr("tl.time"), static_cast<double>(sum),
                    static_cast<double>(pe.tl.cycle));

        // Colored cycle timeline (phase codes 0/3/6/8/9 like the page).
        if (!light.phases.empty()) {
            const float total = (sum > 0.01f) ? sum : 1.f;
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            const float h = 20.f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float x = p0.x;
            static const ImU32 kCol[12] = {
                IM_COL32(198, 40, 40, 255),  IM_COL32(198, 40, 40, 255),  IM_COL32(198, 40, 40, 255),
                IM_COL32(214, 130, 27, 255), IM_COL32(214, 130, 27, 255), IM_COL32(214, 130, 27, 255),
                IM_COL32(46, 125, 50, 255),  IM_COL32(46, 125, 50, 255),  IM_COL32(46, 125, 50, 255),
                IM_COL32(249, 168, 37, 255), IM_COL32(249, 168, 37, 255), IM_COL32(249, 168, 37, 255)};
            for (const auto& ph : light.phases) {
                const float frac = (std::max)(ph.duration, 0.1f) / total;
                const float x1 = x + w * frac;
                const int code = (std::clamp)(ph.phase, 0, 11);
                dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x1, p0.y + h), kCol[code]);
                x = x1;
            }
            dl->AddRect(ImVec2(p0.x, p0.y), ImVec2(p0.x + w, p0.y + h), IM_COL32(90, 90, 90, 255));
            ImGui::Dummy(ImVec2(w, h));
        }

        if (ImGui::Button(L("tl.add_phase", "tlph").c_str())) {
            light.phases.push_back(sco::ScoTrafficPhase{0, 5.f});
            pe.dirty = true;
        }
        static const char* kPh[] = {"0 rojo",      "1 rojo",     "2 rojo",   "3 rojo-ambar",
                                    "4 rojo-ambar", "5 rojo-ambar", "6 verde", "7 verde",
                                    "8 verde",    "9 ambar",   "10 ambar", "11 ambar"};
        int del_ph = -1;
        for (int pi = 0; pi < static_cast<int>(light.phases.size()); ++pi) {
            auto& ph = light.phases[static_cast<std::size_t>(pi)];
            ImGui::PushID(pi);
            int code = (std::clamp)(ph.phase, 0, 11);
            ImGui::SetNextItemWidth(150.f);
            if (ImGui::Combo("##ph", &code, kPh, 12)) {
                ph.phase = code;
                pe.dirty = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.f);
            if (ImGui::DragFloat("##dur", &ph.duration, 0.1f, 0.1f, 120.f)) pe.dirty = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) del_ph = pi;
            ImGui::PopID();
        }
        if (del_ph >= 0) {
            light.phases.erase(light.phases.begin() + del_ph);
            pe.dirty = true;
        }
    }
    (void)panel;
}

void draw_props_subtab(EditorSidePanel& panel, PathEditorState& pe) {
    static std::filesystem::path s_file;
    static char s_name[128]{};
    static int s_complexity = 0;
    static int s_rt = 1;
    static bool s_abs = false, s_only = false, s_nocol = false, s_fixed = false, s_lm = false;
    static const char* kRt[] = {"presurface", "surface", "on_surface", "(normal)"};

    if (s_file != pe.sco_path) {
        s_file = pe.sco_path;
        s_name[0] = '\0';
        s_complexity = 0;
        s_rt = 1;
        s_abs = s_only = s_nocol = s_fixed = s_lm = false;
        if (const sco::SceneryObject* so = sco::load_sco(pe.sco_path)) {
            std::snprintf(s_name, sizeof(s_name), "%s", so->friendly_name.c_str());
            s_complexity = so->complexity;
            switch (so->render_priority) {
            case sco::ScoRenderPriority::Presurface: s_rt = 0; break;
            case sco::ScoRenderPriority::Surface: s_rt = 1; break;
            case sco::ScoRenderPriority::OnSurface: s_rt = 2; break;
            default: s_rt = 3; break;
            }
            s_abs = so->abs_height;
            s_only = so->only_editor_mesh;
            s_nocol = so->no_collision;
            s_fixed = so->fixed;
            s_lm = so->lightmap;
        }
    }

    ImGui::TextDisabled("%s", pe.sco_path.filename().string().c_str());
    ImGui::InputText(L("props.friendly", "pf").c_str(), s_name, sizeof(s_name));
    ImGui::DragInt(L("props.complexity", "pcx").c_str(), &s_complexity, 1.f, 0, 3);
    ImGui::Combo(L("props.rendertype", "prt").c_str(), &s_rt, kRt, 4);
    ImGui::Checkbox(L("props.absheight", "pab").c_str(), &s_abs);
    ImGui::Checkbox(L("props.onlyeditor", "poe").c_str(), &s_only);
    ImGui::Checkbox(L("props.nocollision", "pnc").c_str(), &s_nocol);
    ImGui::Checkbox(L("props.fixed", "pfx").c_str(), &s_fixed);
    ImGui::Checkbox(L("props.lightmap", "plm").c_str(), &s_lm);
    if (ImGui::Button(L("common.apply", "papply").c_str(), ImVec2(-FLT_MIN, 0.f))) {
        sco::ScoRenderPriority rp = sco::ScoRenderPriority::Normal;
        if (s_rt == 0) rp = sco::ScoRenderPriority::Presurface;
        else if (s_rt == 1) rp = sco::ScoRenderPriority::Surface;
        else if (s_rt == 2) rp = sco::ScoRenderPriority::OnSurface;
        if (sco::rewrite_sco_header(pe.sco_path, s_name, s_complexity, rp, s_abs, s_only, s_nocol, s_fixed, s_lm)) {
            panel.set_status(i18n::tr("props.saved", "Props .sco guardadas"));
        } else {
            panel.set_status(i18n::tr("props.save_failed", "No se pudo guardar props .sco"));
        }
    }
}

void draw_tex_subtab(EditorSidePanel& panel, PathEditorState& pe) {
    auto& st = panel.imgui_state();
    ImGui::Checkbox(L("tex.arrows", "txa").c_str(), &pe.show_arrows);
    ImGui::Checkbox(L("tex.width", "txw").c_str(), &pe.show_width);
    ImGui::SliderFloat(L("tex.opacity", "txo").c_str(), &pe.path_opacity, 0.2f, 0.85f, "%.2f");
    ImGui::SeparatorText(i18n::tr("tex.view"));
    ImGui::Checkbox(L("paths.hide_model", "thm").c_str(), &st.sco_path_hide_model);
    ImGui::Checkbox(L("paths.only_active", "toa").c_str(), &st.sco_path_hide_visual);
    ImGui::Checkbox(L("paths.hide_nodes", "thn").c_str(), &st.sco_path_hide_nodes);
}

} // namespace

void draw_path_editor_tab(EditorSidePanel& panel) {
    PathEditorState* pe = panel.path_editor();
    if (!pe || !pe->active) {
        ImGui::TextDisabled("%s", i18n::tr("paths.no_editor", "Sin editor de paths activo"));
        return;
    }
    ImGui::Text("%s  %s%s", i18n::tr("paths.title"), pe->sco_path.filename().string().c_str(),
                pe->dirty ? "  *" : "");
    ImGui::Separator();

    if (ImGui::BeginTabBar("##path_subtabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem(i18n::tr("paths.tab.paths"))) {
            draw_paths_subtab(panel, *pe);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(i18n::tr("paths.tab.traffic"))) {
            draw_tl_subtab(panel, *pe);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(i18n::tr("paths.tab.props"))) {
            draw_props_subtab(panel, *pe);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(i18n::tr("paths.tab.textures"))) {
            draw_tex_subtab(panel, *pe);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace omsi::ui
