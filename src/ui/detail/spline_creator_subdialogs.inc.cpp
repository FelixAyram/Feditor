// Included from spline_creator_dialog.cpp — Texture-Wrapper and Path sub-dialogs (SplineCreator parity).

constexpr int kWrapDlgOk = 9201;
constexpr int kWrapDlgCancel = 9202;
constexpr int kWrapDlgTex = 9203;
constexpr int kWrapDlgName = 9204;
constexpr int kWrapDlgUv0 = 9205;
constexpr int kWrapDlgUv1 = 9206;
constexpr int kWrapDlgPick0 = 9207;
constexpr int kWrapDlgPick1 = 9208;
constexpr int kWrapDlgHp = 9209;
constexpr int kWrapDlgX0 = 9210;
constexpr int kWrapDlgX1 = 9211;
constexpr int kWrapDlgZ0 = 9212;
constexpr int kWrapDlgZ1 = 9213;
constexpr int kWrapDlgTiling = 9214;
constexpr int kWrapDlgFlip = 9215;
constexpr int kWrapDlgAlpha = 9216;
constexpr int kWrapDlgTexList = 9217;
constexpr int kWrapDlgTexPrev = 9218;

constexpr int kPathDlgOk = 9251;
constexpr int kPathDlgCancel = 9252;
constexpr int kPathDlgTyp = 9253;
constexpr int kPathDlgLat = 9254;
constexpr int kPathDlgHt = 9255;
constexpr int kPathDlgW = 9256;
constexpr int kPathDlgDir = 9257;
constexpr int kPathDlg2 = 9258;
constexpr int kPathDlgLen = 9259;

struct SubDlgState {
    CreatorState* parent = nullptr;
    bool create_new = false;
    int index = 0;
    bool done = false;
    bool ok = false;
    int uv_pick = 0; // 0=none 1=start 2=end
    wchar_t wrap_name[128]{};
    HWND tex_preview = nullptr;
    HBITMAP tex_bmp = nullptr;
    int tex_w = 0;
    int tex_h = 0;
    RECT tex_rect{};
    float draft_u0 = 0.f;
    float draft_u1 = 1.f;
    float draft_x0 = 0.f;
    float draft_z0 = 0.1f;
    float draft_x1 = 3.f;
    float draft_z1 = 0.1f;
    float draft_tiling = 0.2f;
    int draft_mat = 0;
    int draft_alpha = 0;
};

float sub_getf(HWND dlg, int id, float fb);
void sub_setf(HWND dlg, int id, float v);

void wrap_calc_tex_rect(SubDlgState* sd, const RECT& client) {
    if (!sd) return;
    sd->tex_rect = client;
    if (!sd->tex_bmp || sd->tex_w <= 0 || sd->tex_h <= 0) return;
    const int cw = client.right - client.left;
    const int ch = client.bottom - client.top;
    if (cw <= 0 || ch <= 0) return;
    const float aspect = static_cast<float>(sd->tex_w) / static_cast<float>(sd->tex_h);
    int dw = cw;
    int dh = static_cast<int>(static_cast<float>(dw) / aspect);
    if (dh > ch) {
        dh = ch;
        dw = static_cast<int>(static_cast<float>(dh) * aspect);
    }
    const int ox = client.left + (cw - dw) / 2;
    const int oy = client.top + (ch - dh) / 2;
    sd->tex_rect = {ox, oy, ox + dw, oy + dh};
}

void wrap_load_preview_tex(SubDlgState* sd, HWND dlg) {
    if (!sd || !sd->parent) return;
    if (sd->tex_bmp) {
        DeleteObject(sd->tex_bmp);
        sd->tex_bmp = nullptr;
    }
    sd->tex_w = 0;
    sd->tex_h = 0;
    const int mat = static_cast<int>(SendMessageW(GetDlgItem(dlg, kWrapDlgTex), CB_GETCURSEL, 0, 0));
    if (mat < 0 || static_cast<std::size_t>(mat) >= sd->parent->def.textures.size()) return;
    const auto& name = sd->parent->def.textures[static_cast<std::size_t>(mat)];
    const auto path = resolve_tex_file(sd->parent, name);
    if (!path) return;
    const auto img = gfx::dx12::load_image_cpu_rgba(*path);
    if (!img) return;
    sd->tex_bmp = rgba_to_hbitmap(*img);
    sd->tex_w = static_cast<int>(img->width);
    sd->tex_h = static_cast<int>(img->height);
}

void wrap_invalidate_preview(SubDlgState* sd) {
    if (sd && sd->tex_preview) InvalidateRect(sd->tex_preview, nullptr, FALSE);
}

LRESULT CALLBACK wrap_tex_preview_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HWND dlg = GetParent(hwnd);
    auto* sd = dlg ? reinterpret_cast<SubDlgState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA)) : nullptr;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (sd) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            wrap_calc_tex_rect(sd, rc);
            if (sd->tex_bmp && sd->tex_w > 0 && sd->tex_h > 0) {
                HDC mem = CreateCompatibleDC(dc);
                SelectObject(mem, sd->tex_bmp);
                SetStretchBltMode(dc, HALFTONE);
                StretchBlt(dc, sd->tex_rect.left, sd->tex_rect.top, sd->tex_rect.right - sd->tex_rect.left,
                         sd->tex_rect.bottom - sd->tex_rect.top, mem, 0, 0, sd->tex_w, sd->tex_h, SRCCOPY);
                DeleteDC(mem);
            } else {
                SetBkMode(dc, TRANSPARENT);
                TextOutW(dc, rc.left + 16, rc.top + 16, L"(sin textura)", 13);
            }
            if (dlg && sd->tex_rect.right > sd->tex_rect.left && sd->tex_rect.bottom > sd->tex_rect.top) {
                const float u0 = sd->draft_u0;
                const float u1 = sd->draft_u1;
                const int tw = sd->tex_rect.right - sd->tex_rect.left;
                const int x0 = sd->tex_rect.left + static_cast<int>((std::clamp)(u0, 0.f, 1.f) * tw);
                const int x1 = sd->tex_rect.left + static_cast<int>((std::clamp)(u1, 0.f, 1.f) * tw);
                auto draw_line = [&](int x, COLORREF c, int w) {
                    HPEN pen = CreatePen(PS_SOLID, w, c);
                    HGDIOBJ op = SelectObject(dc, pen);
                    MoveToEx(dc, x, sd->tex_rect.top, nullptr);
                    LineTo(dc, x, sd->tex_rect.bottom);
                    SelectObject(dc, op);
                    DeleteObject(pen);
                };
                draw_line(x0, RGB(255, 255, 255), sd->uv_pick == 1 ? 3 : 2);
                draw_line(x1, RGB(255, 255, 255), sd->uv_pick == 2 ? 3 : 2);
                if (sd->uv_pick == 1) draw_line(x0, RGB(60, 120, 255), 1);
                if (sd->uv_pick == 2) draw_line(x1, RGB(60, 120, 255), 1);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (sd && dlg && sd->uv_pick != 0) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            wrap_calc_tex_rect(sd, rc);
            const int sx = GET_X_LPARAM(lp);
            if (sx >= sd->tex_rect.left && sx < sd->tex_rect.right && sd->tex_rect.bottom > sd->tex_rect.top) {
                const float u = static_cast<float>(sx - sd->tex_rect.left) /
                                static_cast<float>(sd->tex_rect.right - sd->tex_rect.left);
                const float uc = (std::clamp)(u, 0.f, 1.f);
                if (sd->uv_pick == 1) sd->draft_u0 = uc;
                else sd->draft_u1 = uc;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_SETCURSOR:
        if (sd && sd->uv_pick != 0) {
            SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_CROSS)));
            return TRUE;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool pump_sub_modal(HWND owner, HWND dlg, bool* done) {
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    SetForegroundWindow(dlg);
    if (owner) EnableWindow(owner, FALSE);
    MSG msg{};
    while (!*done) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(msg.wParam));
                continue;
            }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                *done = true;
                break;
            }
            if (!IsDialogMessageW(dlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (*done) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }
    if (owner) EnableWindow(owner, TRUE);
    return *done;
}

float sub_getf(HWND dlg, int id, float fb) {
    wchar_t buf[32]{};
    GetWindowTextW(GetDlgItem(dlg, id), buf, 32);
    try {
        return std::stof(std::wstring(buf));
    } catch (...) {
        return fb;
    }
}

int sub_geti(HWND dlg, int id, int fb) {
    wchar_t buf[16]{};
    GetWindowTextW(GetDlgItem(dlg, id), buf, 16);
    try {
        return std::stoi(std::wstring(buf));
    } catch (...) {
        return fb;
    }
}

void sub_setf(HWND dlg, int id, float v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.4f", static_cast<double>(v));
    SetWindowTextW(GetDlgItem(dlg, id), buf);
}

void sub_seti(HWND dlg, int id, int v) {
    wchar_t buf[16];
    swprintf_s(buf, L"%d", v);
    SetWindowTextW(GetDlgItem(dlg, id), buf);
}

void wrap_fill_from_profile(HWND dlg, SubDlgState* sd) {
    auto& def = sd->parent->def;
    if (sd->index < 0 || static_cast<std::size_t>(sd->index) >= def.profiles.size()) return;
    const auto& strip = def.profiles[static_cast<std::size_t>(sd->index)];
    if (strip.empty()) return;
    const auto& a = strip.front();
    const auto& b = strip.back();
    sub_setf(dlg, kWrapDlgX0, a.x);
    sub_setf(dlg, kWrapDlgZ0, a.z);
    sub_setf(dlg, kWrapDlgX1, b.x);
    sub_setf(dlg, kWrapDlgZ1, b.z);
    sub_setf(dlg, kWrapDlgUv0, a.uv_x);
    sub_setf(dlg, kWrapDlgUv1, b.uv_x);
    sub_setf(dlg, kWrapDlgTiling, a.y_tiling);
    HWND tex = GetDlgItem(dlg, kWrapDlgTex);
    if (tex) SendMessageW(tex, CB_SETCURSEL, (std::clamp)(a.material_id, 0, static_cast<int>(def.textures.size()) - 1), 0);
}

void wrap_apply_to_profile(SubDlgState* sd, HWND dlg) {
    auto& def = sd->parent->def;
    if (sd->index < 0 || static_cast<std::size_t>(sd->index) >= def.profiles.size()) return;
    auto& strip = def.profiles[static_cast<std::size_t>(sd->index)];
    const int mat = static_cast<int>(SendMessageW(GetDlgItem(dlg, kWrapDlgTex), CB_GETCURSEL, 0, 0));
    const float x0 = sub_getf(dlg, kWrapDlgX0, 0.f);
    const float z0 = sub_getf(dlg, kWrapDlgZ0, 0.1f);
    const float x1 = sub_getf(dlg, kWrapDlgX1, 3.f);
    const float z1 = sub_getf(dlg, kWrapDlgZ1, 0.1f);
    const float u0 = sub_getf(dlg, kWrapDlgUv0, 0.f);
    const float u1 = sub_getf(dlg, kWrapDlgUv1, 1.f);
    const float til = sub_getf(dlg, kWrapDlgTiling, 0.2f);
    if (strip.size() < 2) {
        strip.clear();
        strip.push_back({x0, z0, u0, til, mat});
        strip.push_back({x1, z1, u1, til, mat});
    } else {
        strip.front() = {x0, z0, u0, til, mat};
        strip.back() = {x1, z1, u1, til, mat};
        for (auto& pt : strip) pt.material_id = mat;
    }
    if (mat >= 0 && static_cast<std::size_t>(mat) < def.textures.size()) {
        ensure_texture_parallel(def);
        const int a = static_cast<int>(SendMessageW(GetDlgItem(dlg, kWrapDlgAlpha), CB_GETCURSEL, 0, 0));
        def.texture_alpha[static_cast<std::size_t>(mat)] = static_cast<std::uint8_t>((std::clamp)(a, 0, 2));
    }
}

LRESULT CALLBACK wrap_subdlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* sd = reinterpret_cast<SubDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE:
        sd = reinterpret_cast<SubDlgState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(sd));
        return 0;
    case WM_COMMAND: {
        if (!sd) break;
        const int id = LOWORD(wp);
        HWND dlg = hwnd;
        if (id == kWrapDlgCancel) {
            sd->ok = false;
            sd->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == kWrapDlgOk) {
            wrap_apply_to_profile(sd, dlg);
            if (HWND ed = GetDlgItem(dlg, kWrapDlgName)) GetWindowTextW(ed, sd->wrap_name, 128);
            sd->ok = true;
            sd->done = true;
            if (sd->tex_bmp) DeleteObject(sd->tex_bmp);
            sd->tex_bmp = nullptr;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == kWrapDlgFlip) {
            const float u0 = sub_getf(dlg, kWrapDlgUv0, 0.f);
            const float u1 = sub_getf(dlg, kWrapDlgUv1, 1.f);
            sub_setf(dlg, kWrapDlgUv0, u1);
            sub_setf(dlg, kWrapDlgUv1, u0);
            const float x0 = sub_getf(dlg, kWrapDlgX0, 0.f);
            const float x1 = sub_getf(dlg, kWrapDlgX1, 3.f);
            sub_setf(dlg, kWrapDlgX0, -x1);
            sub_setf(dlg, kWrapDlgX1, -x0);
            return 0;
        }
        if (id == kWrapDlgPick0) {
            sd->uv_pick = 1;
            wrap_invalidate_preview(sd);
            return 0;
        }
        if (id == kWrapDlgPick1) {
            sd->uv_pick = 2;
            wrap_invalidate_preview(sd);
            return 0;
        }
        if (id == kWrapDlgUv0 || id == kWrapDlgUv1) {
            if (HIWORD(wp) == EN_CHANGE) wrap_invalidate_preview(sd);
            return 0;
        }
        if (id == kWrapDlgTex && HIWORD(wp) == CBN_SELCHANGE) {
            wrap_load_preview_tex(sd, dlg);
            wrap_invalidate_preview(sd);
            return 0;
        }
        if (id == kWrapDlgHp && HIWORD(wp) == CBN_SELCHANGE) {
            const int hi = static_cast<int>(SendMessageW(GetDlgItem(dlg, kWrapDlgHp), CB_GETCURSEL, 0, 0));
            if (hi >= 0 && static_cast<std::size_t>(hi) < sd->parent->def.height_profiles.size()) {
                const auto& hp = sd->parent->def.height_profiles[static_cast<std::size_t>(hi)];
                sub_setf(dlg, kWrapDlgX0, hp.left);
                sub_setf(dlg, kWrapDlgX1, hp.right);
                sub_setf(dlg, kWrapDlgZ0, hp.height);
                sub_setf(dlg, kWrapDlgZ1, hp.height);
            }
            return 0;
        }
        if (id == kWrapDlgTexList && HIWORD(wp) == LBN_SELCHANGE) {
            const int ti = static_cast<int>(SendMessageW(GetDlgItem(dlg, kWrapDlgTexList), LB_GETCURSEL, 0, 0));
            if (ti >= 0) {
                SendMessageW(GetDlgItem(dlg, kWrapDlgTex), CB_SETCURSEL, ti, 0);
                wrap_load_preview_tex(sd, dlg);
                wrap_invalidate_preview(sd);
            }
            return 0;
        }
        break;
    }
    case WM_MEASUREITEM: {
        if (wp == kWrapDlgTexList) {
            reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = kTexSlotH;
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis->CtlID != kWrapDlgTexList || !sd) break;
        FillRect(dis->hDC, &dis->rcItem, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        const int idx = dis->itemID;
        if (idx < 0) return TRUE;
        const int thumb = kThumbPx;
        if (sd->parent && static_cast<std::size_t>(idx) < sd->parent->tex_thumbs.size() &&
            sd->parent->tex_thumbs[static_cast<std::size_t>(idx)]) {
            HDC mem = CreateCompatibleDC(dis->hDC);
            SelectObject(mem, sd->parent->tex_thumbs[static_cast<std::size_t>(idx)]);
            BitBlt(dis->hDC, dis->rcItem.left + 4, dis->rcItem.top + 2, thumb, thumb, mem, 0, 0, SRCCOPY);
            DeleteDC(mem);
        }
        wchar_t text[260]{};
        SendMessageW(dis->hwndItem, LB_GETTEXT, idx, reinterpret_cast<LPARAM>(text));
        SetBkMode(dis->hDC, TRANSPARENT);
        TextOutW(dis->hDC, dis->rcItem.left + thumb + 12, dis->rcItem.top + 16, text, static_cast<int>(wcslen(text)));
        if (dis->itemState & ODS_SELECTED) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(60, 120, 200));
            SelectObject(dis->hDC, pen);
            SelectObject(dis->hDC, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right - 1, dis->rcItem.bottom - 1);
            DeleteObject(pen);
        }
        return TRUE;
    }
    case WM_CLOSE:
        if (sd) {
            if (sd->tex_bmp) DeleteObject(sd->tex_bmp);
            sd->tex_bmp = nullptr;
            sd->ok = false;
            sd->done = true;
        }
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

#if 0 // legacy Win32 texture-wrapper dialog
bool run_texture_wrap_dialog_legacy(HWND owner, CreatorState* st, int profile_idx, bool create_new) {
    if (!st) return false;
    ensure_subclass(L"OmsiF4WrapDlg", wrap_subdlg_proc);
    ensure_subclass(L"OmsiF4WrapTexPrev", wrap_tex_preview_proc, reinterpret_cast<LPCWSTR>(IDC_CROSS));

    SubDlgState sd{};
    sd.parent = st;
    sd.create_new = create_new;
    if (create_new) {
        push_undo(st);
        auto& np = st->def.profiles.emplace_back();
        const int mat = st->def.textures.empty() ? 0 : (std::clamp)(st->sel_tex, 0, static_cast<int>(st->def.textures.size()) - 1);
        np.push_back({0.f, 0.1f, 0.f, 0.2f, mat});
        np.push_back({3.f, 0.1f, 1.f, 0.2f, mat});
        profile_idx = static_cast<int>(st->def.profiles.size()) - 1;
    }
    sd.index = profile_idx;
    st->sel_profile = profile_idx;

    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) work = {0, 0, 1280, 800};
    const int client_w = (work.right - work.left) * 85 / 100;
    const int client_h = (work.bottom - work.top) * 85 / 100;
    const int left_w = client_w / 2;
    const int right_w = client_w - left_w;
    const int LW = left_w - 24;
    RECT wr{0, 0, client_w, client_h};
    AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"OmsiF4WrapDlg", L"Textur konfigurieren",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                               wr.right - wr.left, wr.bottom - wr.top, owner, nullptr, GetModuleHandleW(nullptr), &sd);
    if (!dlg) return false;

    sd.tex_preview =
        CreateWindowExW(WS_EX_CLIENTEDGE, L"OmsiF4WrapTexPrev", L"", WS_CHILD | WS_VISIBLE, left_w, 0, right_w,
                        client_h, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWrapDlgTexPrev)),
                        GetModuleHandleW(nullptr), nullptr);

    int y = 12;
    mk_static(dlg, L"Textur:", 12, y, 60, 16);
    HWND tex = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 80, y - 2, LW - 68, 200, dlg,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWrapDlgTex)), GetModuleHandleW(nullptr),
                             nullptr);
    for (const auto& t : st->def.textures)
        SendMessageW(tex, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(utf8_to_wide(t).c_str()));
    if (st->def.textures.empty()) SendMessageW(tex, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(sin textura)"));
    y += 32;
    mk_static(dlg, L"Texturas (clic):", 12, y, 120, 16);
    y += 18;
    HWND texlist = CreateWindowW(
        WC_LISTBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_VSCROLL, 12, y, LW,
        kTexSlotH * 2 + 6, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWrapDlgTexList)), GetModuleHandleW(nullptr),
        nullptr);
    for (const auto& t : st->def.textures)
        SendMessageW(texlist, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(utf8_to_wide(t).c_str()));
    if (!st->def.textures.empty()) {
        const int mat = profile_idx >= 0 && static_cast<std::size_t>(profile_idx) < st->def.profiles.size() &&
                                !st->def.profiles[static_cast<std::size_t>(profile_idx)].empty()
                            ? st->def.profiles[static_cast<std::size_t>(profile_idx)].front().material_id
                            : 0;
        const int ti = (std::clamp)(mat, 0, static_cast<int>(st->def.textures.size()) - 1);
        SendMessageW(texlist, LB_SETCURSEL, ti, 0);
    }
    y += kTexSlotH * 2 + 14;
    mk_static(dlg, L"Bezeichnung:", 12, y, 80, 16);
    mk_edit(dlg, kWrapDlgName, 96, y - 2, LW - 84, 22);
    ensure_profile_labels(st);
    if (profile_idx >= 0 && static_cast<std::size_t>(profile_idx) < st->profile_labels.size())
        SetWindowTextW(GetDlgItem(dlg, kWrapDlgName), st->profile_labels[static_cast<std::size_t>(profile_idx)].c_str());
    else
        SetWindowTextW(GetDlgItem(dlg, kWrapDlgName), (L"Perfil " + std::to_wstring(profile_idx + 1)).c_str());
    y += 36;
    mk_static(dlg, L"--- Texturbereich ---", 12, y, 200, 16);
    y += 22;
    mk_static(dlg, L"Von:", 12, y, 28, 16);
    mk_edit(dlg, kWrapDlgUv0, 44, y - 2, 72, 22);
    mk_static(dlg, L"bis:", 124, y, 24, 16);
    mk_edit(dlg, kWrapDlgUv1, 152, y - 2, 72, 22);
    y += 28;
    mk_btn(dlg, kWrapDlgPick0, L"Start w\u00E4hlen", 12, y - 2, (LW - 8) / 2, 26);
    mk_btn(dlg, kWrapDlgPick1, L"Ende w\u00E4hlen", 16 + (LW - 8) / 2, y - 2, (LW - 8) / 2, 26);
    y += 36;
    mk_static(dlg, L"Positionen aus H-Profil:", 12, y, 150, 16);
    HWND hp = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 168, y - 2, LW - 176, 160, dlg,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWrapDlgHp)), GetModuleHandleW(nullptr),
                            nullptr);
    for (std::size_t i = 0; i < st->def.height_profiles.size(); ++i) {
        const auto& h = st->def.height_profiles[i];
        wchar_t line[96];
        swprintf_s(line, L"HP%zu L=%.2f R=%.2f", i + 1, static_cast<double>(h.left), static_cast<double>(h.right));
        SendMessageW(hp, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line));
    }
    if (!st->def.height_profiles.empty()) SendMessageW(hp, CB_SETCURSEL, 0, 0);
    y += 36;
    mk_static(dlg, L"--- Position ---", 12, y, 120, 16);
    y += 22;
    mk_static(dlg, L"Startpunkt (X):", 12, y, 90, 16);
    mk_edit(dlg, kWrapDlgX0, 108, y - 2, 72, 22);
    mk_static(dlg, L"Endpunkt (X):", 200, y, 80, 16);
    mk_edit(dlg, kWrapDlgX1, 286, y - 2, 72, 22);
    y += 28;
    mk_static(dlg, L"Starth\u00F6he (Z):", 12, y, 90, 16);
    mk_edit(dlg, kWrapDlgZ0, 108, y - 2, 72, 22);
    mk_static(dlg, L"Endh\u00F6he (Z):", 200, y, 80, 16);
    mk_edit(dlg, kWrapDlgZ1, 286, y - 2, 72, 22);
    y += 28;
    mk_static(dlg, L"V-Steigung (1/m):", 12, y, 100, 16);
    mk_edit(dlg, kWrapDlgTiling, 116, y - 2, 72, 22);
    mk_static(dlg, L"[matl_alpha]", 200, y, 72, 16);
    HWND alpha = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 276, y - 2, 82, 120, dlg,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWrapDlgAlpha)), GetModuleHandleW(nullptr),
                               nullptr);
    SendMessageW(alpha, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"0 opaco"));
    SendMessageW(alpha, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1 test"));
    SendMessageW(alpha, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2 blend"));
    y += 36;
    mk_btn(dlg, kWrapDlgFlip, L"Flip", LW - 88, y - 2, 80, 26);
    mk_btn(dlg, kWrapDlgCancel, L"Abbrechen", 12, client_h - 52, 100, 28);
    mk_btn(dlg, kWrapDlgOk, L"Ok", left_w - 112, client_h - 52, 100, 28);

    wrap_fill_from_profile(dlg, &sd);
    if (!st->def.textures.empty() && tex) {
        const int want = (std::clamp)(st->sel_tex, 0, static_cast<int>(st->def.textures.size()) - 1);
        SendMessageW(tex, CB_SETCURSEL, want, 0);
        if (texlist) SendMessageW(texlist, LB_SETCURSEL, want, 0);
    }
    wrap_load_preview_tex(&sd, dlg);
    wrap_invalidate_preview(&sd);
    if (!st->def.textures.empty() && profile_idx >= 0) {
        const int mat = st->def.profiles[static_cast<std::size_t>(profile_idx)].empty()
                            ? 0
                            : st->def.profiles[static_cast<std::size_t>(profile_idx)].front().material_id;
        ensure_texture_parallel(st->def);
        if (mat >= 0 && static_cast<std::size_t>(mat) < st->def.texture_alpha.size())
            SendMessageW(alpha, CB_SETCURSEL, st->def.texture_alpha[static_cast<std::size_t>(mat)], 0);
    }
    RECT orc{};
    if (GetWindowRect(owner, &orc))
        SetWindowPos(dlg, nullptr, orc.left + 40, orc.top + 40, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    sd.done = false;
    pump_sub_modal(owner, dlg, &sd.done);
    if (sd.ok) {
        ensure_profile_labels(st);
        if (sd.wrap_name[0]) {
            st->profile_labels[static_cast<std::size_t>(profile_idx)] = sd.wrap_name;
            if (static_cast<std::size_t>(profile_idx) < st->def.profile_comments.size())
                st->def.profile_comments[static_cast<std::size_t>(profile_idx)] = wide_to_utf8(sd.wrap_name);
        }
        st->sel_profile = profile_idx;
        refill_wrap_list(st);
        refill_profiles(st);
        refill_pt_list(st);
        invalidate_all(st);
    } else if (create_new && !st->def.profiles.empty()) {
        do_undo(st);
    }
    return sd.ok;
}
#endif

LRESULT CALLBACK path_subdlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* sd = reinterpret_cast<SubDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE:
        sd = reinterpret_cast<SubDlgState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(sd));
        return 0;
    case WM_COMMAND:
        if (!sd) break;
        if (LOWORD(wp) == kPathDlgCancel) {
            sd->ok = false;
            sd->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == kPathDlgOk) {
            auto& def = sd->parent->def;
            if (sd->create_new) {
                def.paths.push_back({});
                sd->index = static_cast<int>(def.paths.size()) - 1;
            }
            if (sd->index >= 0 && static_cast<std::size_t>(sd->index) < def.paths.size()) {
                auto& p = def.paths[static_cast<std::size_t>(sd->index)];
                p.typ = sub_geti(hwnd, kPathDlgTyp, p.typ);
                p.lateral = sub_getf(hwnd, kPathDlgLat, p.lateral);
                p.height = sub_getf(hwnd, kPathDlgHt, p.height);
                p.width = sub_getf(hwnd, kPathDlgW, p.width);
                p.direction = sub_geti(hwnd, kPathDlgDir, p.direction);
                p.secondary = SendMessageW(GetDlgItem(hwnd, kPathDlg2), BM_GETCHECK, 0, 0) == BST_CHECKED;
                def.path_length = sub_getf(hwnd, kPathDlgLen, def.path_length);
            }
            sd->ok = true;
            sd->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (sd) {
            sd->ok = false;
            sd->done = true;
        }
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool run_path_config_dialog(HWND owner, CreatorState* st, int path_idx, bool create_new) {
    if (!st) return false;
    ensure_subclass(L"OmsiF4PathDlg", path_subdlg_proc);

    SubDlgState sd{};
    sd.parent = st;
    sd.create_new = create_new;
    sd.index = path_idx;

    spline::SliPath draft{};
    if (!create_new && path_idx >= 0 && static_cast<std::size_t>(path_idx) < st->def.paths.size())
        draft = st->def.paths[static_cast<std::size_t>(path_idx)];
    else {
        draft = {1, 0.f, 0.1f, 3.f, 2};
    }

    const int W = 360;
    const int H = 340;
    RECT wr{0, 0, W, H};
    AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"OmsiF4PathDlg", L"Path konfigurieren",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                               wr.right - wr.left, wr.bottom - wr.top, owner, nullptr, GetModuleHandleW(nullptr), &sd);
    if (!dlg) return false;

    int y = 16;
    mk_static(dlg, L"typ:", 12, y, 40, 16);
    mk_edit(dlg, kPathDlgTyp, 56, y - 2, 48, 22);
    sub_seti(dlg, kPathDlgTyp, draft.typ);
    mk_static(dlg, L"lateral:", 120, y, 50, 16);
    mk_edit(dlg, kPathDlgLat, 176, y - 2, 72, 22);
    sub_setf(dlg, kPathDlgLat, draft.lateral);
    y += 32;
    mk_static(dlg, L"height:", 12, y, 48, 16);
    mk_edit(dlg, kPathDlgHt, 64, y - 2, 72, 22);
    sub_setf(dlg, kPathDlgHt, draft.height);
    mk_static(dlg, L"width:", 150, y, 44, 16);
    mk_edit(dlg, kPathDlgW, 198, y - 2, 72, 22);
    sub_setf(dlg, kPathDlgW, draft.width);
    y += 32;
    mk_static(dlg, L"direction:", 12, y, 60, 16);
    mk_edit(dlg, kPathDlgDir, 76, y - 2, 48, 22);
    sub_seti(dlg, kPathDlgDir, draft.direction);
    CreateWindowW(L"BUTTON", L"[path_2]", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 140, y - 2, 80, 22, dlg,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPathDlg2)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(GetDlgItem(dlg, kPathDlg2), BM_SETCHECK, draft.secondary ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 36;
    mk_static(dlg, L"[length] (spline):", 12, y, 110, 16);
    mk_edit(dlg, kPathDlgLen, 128, y - 2, 72, 22);
    sub_setf(dlg, kPathDlgLen, st->def.path_length);
    y += 40;
    mk_static(dlg, L"0=carril 1=peaton 2=riel", 12, y, 220, 16);

    mk_btn(dlg, kPathDlgCancel, L"Abbrechen", 12, H - 52, 100, 28);
    mk_btn(dlg, kPathDlgOk, L"Ok", W - 112, H - 52, 100, 28);

    RECT orc{};
    if (GetWindowRect(owner, &orc))
        SetWindowPos(dlg, nullptr, orc.left + 60, orc.top + 60, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    if (create_new) push_undo(st);
    sd.done = false;
    pump_sub_modal(owner, dlg, &sd.done);
    if (sd.ok) {
        st->sel_path = sd.index;
        refill_paths(st);
        invalidate_all(st);
    } else if (create_new) {
        do_undo(st);
    }
    return sd.ok;
}

constexpr int kRenOk = 9270;
constexpr int kRenCancel = 9271;
constexpr int kRenEdit = 9272;

struct RenDlgState {
    CreatorState* st = nullptr;
    int idx = 0;
    bool done = false;
    bool ok = false;
    wchar_t result[128]{};
};

LRESULT CALLBACK rename_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* sd = reinterpret_cast<RenDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE:
        sd = reinterpret_cast<RenDlgState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(sd));
        return 0;
    case WM_COMMAND:
        if (!sd) break;
        if (LOWORD(wp) == kRenCancel) {
            sd->ok = false;
            sd->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == kRenOk) {
            if (HWND ed = GetDlgItem(hwnd, kRenEdit)) GetWindowTextW(ed, sd->result, 128);
            sd->ok = true;
            sd->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (sd) {
            sd->ok = false;
            sd->done = true;
        }
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool run_rename_profile_dialog(HWND owner, CreatorState* st, int idx) {
    if (!st || idx < 0 || static_cast<std::size_t>(idx) >= st->def.profiles.size()) return false;
    ensure_profile_labels(st);
    ensure_subclass(L"OmsiF4ProfileRen", rename_dlg_proc);

    RenDlgState sd{};
    sd.st = st;
    sd.idx = idx;
    const int W = 360;
    const int H = 140;
    RECT wr{0, 0, W, H};
    AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"OmsiF4ProfileRen", L"Perfil umbenennen",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                               wr.right - wr.left, wr.bottom - wr.top, owner, nullptr, GetModuleHandleW(nullptr), &sd);
    if (!dlg) return false;

    mk_static(dlg, L"Bezeichnung:", 12, 16, 90, 16);
    mk_edit(dlg, kRenEdit, 108, 12, 232, 22);
    SetWindowTextW(GetDlgItem(dlg, kRenEdit), st->profile_labels[static_cast<std::size_t>(idx)].c_str());
    mk_btn(dlg, kRenCancel, L"Abbrechen", 12, H - 52, 100, 28);
    mk_btn(dlg, kRenOk, L"Ok", W - 112, H - 52, 100, 28);

    RECT orc{};
    if (GetWindowRect(owner, &orc))
        SetWindowPos(dlg, nullptr, orc.left + 60, orc.top + 80, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    sd.done = false;
    pump_sub_modal(owner, dlg, &sd.done);
    if (sd.ok && sd.result[0]) {
        st->profile_labels[static_cast<std::size_t>(idx)] = sd.result;
        if (static_cast<std::size_t>(idx) < st->def.profile_comments.size())
            st->def.profile_comments[static_cast<std::size_t>(idx)] = wide_to_utf8(sd.result);
        refill_wrap_list(st);
        refill_pt_list(st);
    }
    return sd.ok;
}
