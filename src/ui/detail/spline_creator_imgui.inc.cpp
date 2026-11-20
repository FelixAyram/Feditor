// Included from spline_creator_dialog.cpp — Texture-Wrapper modal (Dear ImGui + Win32 + DX12).

#include "gfx/dx12/hello_o3d_renderer.hpp"
#include "gfx/dx12/dx12_texture.hpp"

struct WrapImGuiHost {
    CreatorState* parent = nullptr;
    SubDlgState sd{};
    omsi::ui::ImGuiOverlay overlay{};
    HWND hwnd = nullptr;
    HWND tex_preview = nullptr;
    std::unique_ptr<gfx::dx12::HelloO3dRenderer> rhi;
    bool done = false;
    int cw = 900;
    int ch = 620;
};

WrapImGuiHost* g_wrap_host = nullptr;

void wrap_imgui_load_tex(WrapImGuiHost* host) {
    if (!host) return;
    auto& sd = host->sd;
    if (sd.tex_bmp) {
        DeleteObject(sd.tex_bmp);
        sd.tex_bmp = nullptr;
    }
    sd.tex_w = 0;
    sd.tex_h = 0;
    if (!sd.parent) return;
    const int pi = sd.index;
    if (pi < 0 || static_cast<std::size_t>(pi) >= sd.parent->def.profiles.size()) return;
    const auto& strip = sd.parent->def.profiles[static_cast<std::size_t>(pi)];
    if (strip.empty()) return;
    const int mat = strip.front().material_id;
    if (mat < 0 || static_cast<std::size_t>(mat) >= sd.parent->def.textures.size()) return;
    const auto path = resolve_tex_file(sd.parent, sd.parent->def.textures[static_cast<std::size_t>(mat)]);
    if (!path) return;
    const auto img = gfx::dx12::load_image_cpu_rgba(*path);
    if (!img) return;
    sd.tex_bmp = rgba_to_hbitmap(*img);
    sd.tex_w = static_cast<int>(img->width);
    sd.tex_h = static_cast<int>(img->height);
    if (host->tex_preview) InvalidateRect(host->tex_preview, nullptr, FALSE);
}

void wrap_imgui_fill_from_profile(WrapImGuiHost* host) {
    if (!host || !host->parent) return;
    auto& sd = host->sd;
    auto& def = sd.parent->def;
    if (sd.index < 0 || static_cast<std::size_t>(sd.index) >= def.profiles.size()) return;
    const auto& strip = def.profiles[static_cast<std::size_t>(sd.index)];
    if (strip.empty()) return;
    ensure_profile_labels(sd.parent);
    if (static_cast<std::size_t>(sd.index) < sd.parent->profile_labels.size())
        wcscpy_s(sd.wrap_name, sd.parent->profile_labels[static_cast<std::size_t>(sd.index)].c_str());
    else
        swprintf_s(sd.wrap_name, L"Perfil %d", sd.index + 1);
    sd.draft_x0 = strip.front().x;
    sd.draft_z0 = strip.front().z;
    sd.draft_u0 = strip.front().uv_x;
    sd.draft_x1 = strip.back().x;
    sd.draft_z1 = strip.back().z;
    sd.draft_u1 = strip.back().uv_x;
    sd.draft_tiling = strip.front().y_tiling;
    sd.draft_mat = strip.front().material_id;
    if (sd.draft_mat >= 0 && static_cast<std::size_t>(sd.draft_mat) < def.texture_alpha.size())
        sd.draft_alpha = def.texture_alpha[static_cast<std::size_t>(sd.draft_mat)];
    wrap_imgui_load_tex(host);
}

void build_wrap_imgui_ui(WrapImGuiHost* host) {
    if (!host || !host->parent) return;
    auto& sd = host->sd;
    auto& def = host->parent->def;
    char name_utf8[128]{};
    WideCharToMultiByte(CP_UTF8, 0, sd.wrap_name, -1, name_utf8, static_cast<int>(sizeof(name_utf8)), nullptr,
                        nullptr);

    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(host->cw), static_cast<float>(host->ch)));
    ImGui::Begin("Textur konfigurieren", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextDisabled("Perfil %d", sd.index + 1);
    ImGui::Separator();

    if (ImGui::BeginChild("wrap_scroll", ImVec2(static_cast<float>(host->cw) * 0.46f, -36.f),
                          ImGuiChildFlags_Borders)) {
        ImGui::Text("Textura");
        if (ImGui::BeginListBox("##texlist", ImVec2(-1.f, 110.f))) {
            for (int i = 0; i < static_cast<int>(def.textures.size()); ++i) {
                const bool sel = (i == sd.draft_mat);
                if (ImGui::Selectable(def.textures[static_cast<std::size_t>(i)].c_str(), sel)) {
                    sd.draft_mat = i;
                    wrap_imgui_load_tex(host);
                }
            }
            ImGui::EndListBox();
        }
        ImGui::InputText("Bezeichnung", name_utf8, sizeof(name_utf8));
        ImGui::Separator();
        ImGui::Text("Texturbereich (UV)");
        ImGui::InputFloat("Von U", &sd.draft_u0, 0.01f, 0.1f, "%.4f");
        ImGui::InputFloat("bis U", &sd.draft_u1, 0.01f, 0.1f, "%.4f");
        if (ImGui::Button("Start waehlen")) sd.uv_pick = 1;
        ImGui::SameLine();
        if (ImGui::Button("Ende waehlen")) sd.uv_pick = 2;
        if (sd.uv_pick == 1) ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.f, 1.f), "Clic en la imagen: inicio UV");
        if (sd.uv_pick == 2) ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.f, 1.f), "Clic en la imagen: fin UV");
        ImGui::Separator();
        ImGui::InputFloat("Start X", &sd.draft_x0, 0.05f, 0.5f, "%.3f");
        ImGui::InputFloat("Start Z", &sd.draft_z0, 0.01f, 0.1f, "%.3f");
        ImGui::InputFloat("End X", &sd.draft_x1, 0.05f, 0.5f, "%.3f");
        ImGui::InputFloat("End Z", &sd.draft_z1, 0.01f, 0.1f, "%.3f");
        ImGui::InputFloat("V-Steigung", &sd.draft_tiling, 0.01f, 0.1f, "%.4f");
        ImGui::Combo("Alpha", &sd.draft_alpha, "0 opaco\0""1 test\0""2 blend\0");
    }
    ImGui::EndChild();

    ImGui::SameLine();
    if (ImGui::BeginChild("wrap_hint", ImVec2(0.f, -36.f), ImGuiChildFlags_Borders)) {
        ImGui::TextWrapped("Vista previa a la derecha.\nRueda del mouse: scroll en listas.");
    }
    ImGui::EndChild();

    if (ImGui::Button("Abbrechen", ImVec2(120.f, 0.f))) {
        sd.ok = false;
        host->done = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Ok", ImVec2(120.f, 0.f))) {
        MultiByteToWideChar(CP_UTF8, 0, name_utf8, -1, sd.wrap_name, static_cast<int>(std::size(sd.wrap_name)));
        auto& strip = def.profiles[static_cast<std::size_t>(sd.index)];
        if (strip.size() < 2) {
            strip.clear();
            strip.push_back({sd.draft_x0, sd.draft_z0, sd.draft_u0, sd.draft_tiling, sd.draft_mat});
            strip.push_back({sd.draft_x1, sd.draft_z1, sd.draft_u1, sd.draft_tiling, sd.draft_mat});
        } else {
            strip.front() = {sd.draft_x0, sd.draft_z0, sd.draft_u0, sd.draft_tiling, sd.draft_mat};
            strip.back() = {sd.draft_x1, sd.draft_z1, sd.draft_u1, sd.draft_tiling, sd.draft_mat};
            for (auto& pt : strip) pt.material_id = sd.draft_mat;
        }
        if (sd.draft_mat >= 0 && static_cast<std::size_t>(sd.draft_mat) < def.textures.size()) {
            ensure_texture_parallel(def);
            def.texture_alpha[static_cast<std::size_t>(sd.draft_mat)] =
                static_cast<std::uint8_t>((std::clamp)(sd.draft_alpha, 0, 2));
        }
        sd.ok = true;
        host->done = true;
    }
    ImGui::End();
}

LRESULT CALLBACK wrap_imgui_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return TRUE;
    auto* host = g_wrap_host;
    switch (msg) {
    case WM_SIZE:
        if (host) {
            host->cw = LOWORD(lp);
            host->ch = HIWORD(lp);
            if (host->rhi) host->rhi->resize(static_cast<std::uint32_t>((std::max)(host->cw, 1)),
                                               static_cast<std::uint32_t>((std::max)(host->ch, 1)));
            if (host->tex_preview) {
                const int left_w = host->cw / 2;
                SetWindowPos(host->tex_preview, HWND_TOP, left_w, 0, host->cw - left_w, host->ch,
                             SWP_NOACTIVATE);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool pump_wrap_imgui_host(WrapImGuiHost* host, HWND owner) {
    ShowWindow(host->hwnd, SW_SHOW);
    UpdateWindow(host->hwnd);
    SetForegroundWindow(host->hwnd);
    if (owner) EnableWindow(owner, FALSE);

    MSG msg{};
    while (!host->done) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(msg.wParam));
                continue;
            }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                host->sd.ok = false;
                host->done = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (host->done) break;
        host->overlay.begin_frame();
        build_wrap_imgui_ui(host);
        host->overlay.end_frame_and_build();
        if (host->rhi) host->rhi->render();
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
    if (owner) EnableWindow(owner, TRUE);
    return host->sd.ok;
}

bool run_texture_wrap_dialog_imgui(HWND owner, CreatorState* st, int profile_idx, bool create_new) {
    if (!st) return false;
    ensure_subclass(L"OmsiF4WrapImGuiDlg", wrap_imgui_wnd_proc);
    ensure_subclass(L"OmsiF4WrapTexPrev", wrap_tex_preview_proc, reinterpret_cast<LPCWSTR>(IDC_CROSS));

    WrapImGuiHost host{};
    host.parent = st;
    host.sd.parent = st;
    host.sd.create_new = create_new;
    if (create_new) {
        push_undo(st);
        auto& np = st->def.profiles.emplace_back();
        const int mat = st->def.textures.empty()
                            ? 0
                            : (std::clamp)(st->sel_tex, 0, static_cast<int>(st->def.textures.size()) - 1);
        np.push_back({0.f, 0.1f, 0.f, 0.2f, mat});
        np.push_back({3.f, 0.1f, 1.f, 0.2f, mat});
        profile_idx = static_cast<int>(st->def.profiles.size()) - 1;
    }
    host.sd.index = profile_idx;
    st->sel_profile = profile_idx;
    g_wrap_host = &host;

    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) work = {0, 0, 1280, 800};
    host.cw = (work.right - work.left) * 85 / 100;
    host.ch = (work.bottom - work.top) * 85 / 100;
    RECT wr{0, 0, host.cw, host.ch};
    AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);

    host.hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"OmsiF4WrapImGuiDlg", L"Textur konfigurieren",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                wr.right - wr.left, wr.bottom - wr.top, owner, nullptr, GetModuleHandleW(nullptr),
                                nullptr);
    if (!host.hwnd) {
        g_wrap_host = nullptr;
        return false;
    }
    SetWindowLongPtrW(host.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&host.sd));

    const int left_w = host.cw / 2;
    host.tex_preview =
        CreateWindowExW(WS_EX_CLIENTEDGE, L"OmsiF4WrapTexPrev", L"", WS_CHILD | WS_VISIBLE, left_w, 0,
                        host.cw - left_w, host.ch, host.hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWrapDlgTexPrev)), GetModuleHandleW(nullptr),
                        nullptr);
    host.sd.tex_preview = host.tex_preview;

    host.rhi = std::make_unique<gfx::dx12::HelloO3dRenderer>();
    if (!host.rhi->initialize(host.hwnd, static_cast<std::uint32_t>(host.cw), static_cast<std::uint32_t>(host.ch),
                              true)) {
        DestroyWindow(host.hwnd);
        g_wrap_host = nullptr;
        return false;
    }
    host.rhi->set_draw_shadow_decals(false);
    if (!host.overlay.initialize_win32(reinterpret_cast<HWND__*>(host.hwnd), *host.rhi)) {
        host.rhi->shutdown();
        DestroyWindow(host.hwnd);
        g_wrap_host = nullptr;
        return false;
    }

    wrap_imgui_fill_from_profile(&host);
    const bool ok = pump_wrap_imgui_host(&host, owner);

    host.rhi->set_imgui_overlay(nullptr);
    host.overlay.shutdown();
    host.rhi->shutdown();
    if (host.sd.tex_bmp) DeleteObject(host.sd.tex_bmp);
    DestroyWindow(host.hwnd);
    g_wrap_host = nullptr;

    if (ok) {
        ensure_profile_labels(st);
        if (host.sd.wrap_name[0]) {
            st->profile_labels[static_cast<std::size_t>(profile_idx)] = host.sd.wrap_name;
            if (static_cast<std::size_t>(profile_idx) < st->def.profile_comments.size())
                st->def.profile_comments[static_cast<std::size_t>(profile_idx)] = wide_to_utf8(host.sd.wrap_name);
        }
        st->sel_profile = profile_idx;
        refill_wrap_list(st);
        refill_profiles(st);
        refill_pt_list(st);
        invalidate_all(st);
    } else if (create_new && !st->def.profiles.empty()) {
        do_undo(st);
    }
    return ok;
}
