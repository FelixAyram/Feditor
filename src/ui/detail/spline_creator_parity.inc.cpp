// Included from spline_creator_dialog.cpp — wireframe sidebar layout.

void ensure_texture_parallel(spline::SplineDefinition& def) {
    while (def.texture_alpha.size() < def.textures.size()) def.texture_alpha.push_back(0);
    while (def.texture_extra.size() < def.textures.size()) def.texture_extra.push_back({});
}

void ensure_profile_labels(CreatorState* st) {
    if (!st) return;
    while (st->def.profile_comments.size() < st->def.profiles.size()) st->def.profile_comments.push_back({});
    if (st->def.profile_comments.size() > st->def.profiles.size())
        st->def.profile_comments.resize(st->def.profiles.size());
    while (st->profile_labels.size() < st->def.profiles.size()) {
        const auto n = st->profile_labels.size();
        if (!st->def.profile_comments[n].empty())
            st->profile_labels.push_back(utf8_to_wide(st->def.profile_comments[n]));
        else
            st->profile_labels.push_back(L"Perfil " + std::to_wstring(n + 1));
    }
    if (st->profile_labels.size() > st->def.profiles.size()) st->profile_labels.resize(st->def.profiles.size());
}

std::wstring format_profile_slot_label(CreatorState* st, std::size_t pi) {
    ensure_profile_labels(st);
    const auto& strip = st->def.profiles[pi];
    std::wstring label = st->profile_labels[pi];
    if (label.empty()) label = L"Perfil " + std::to_wstring(pi + 1);
    label += L": ";
    const int mat = strip.empty() ? 0 : strip.front().material_id;
    if (mat >= 0 && static_cast<std::size_t>(mat) < st->def.textures.size())
        label += utf8_to_wide(st->def.textures[static_cast<std::size_t>(mat)]);
    else
        label += L"(sin textura)";
    if (strip.size() >= 2) {
        wchar_t span[96];
        swprintf_s(span, L"  X %.2f..%.2f", static_cast<double>(strip.front().x),
                   static_cast<double>(strip.back().x));
        label += span;
    } else if (strip.size() == 1) {
        wchar_t span[64];
        swprintf_s(span, L"  X %.2f", static_cast<double>(strip.front().x));
        label += span;
    } else {
        label += L"  (vacio)";
    }
    return label;
}

void add_profile_strip(CreatorState* st, float x0, float z0, float x1, float z1) {
    const int mat =
        st->def.textures.empty() ? 0 : (std::clamp)(st->sel_tex, 0, static_cast<int>(st->def.textures.size()) - 1);
    if (x0 > x1) {
        std::swap(x0, x1);
        std::swap(z0, z1);
    }
    auto& np = st->def.profiles.emplace_back();
    np.push_back({x0, z0, 0.f, 0.167f, mat});
    np.push_back({x1, z1, 1.f, 0.167f, mat});
    st->def.profile_comments.push_back({});
    ensure_profile_labels(st);
    st->profile_labels.back() = L"Perfil " + std::to_wstring(st->def.profiles.size());
    st->sel_profile = static_cast<int>(st->def.profiles.size()) - 1;
}

void add_empty_profile(CreatorState* st) {
    push_undo(st);
    float x0 = 0.f, z0 = 0.1f, x1 = 3.f;
    if (!st->def.profiles.empty()) {
        const auto& last = st->def.profiles.back();
        if (!last.empty()) {
            x0 = last.back().x;
            z0 = last.back().z;
            x1 = x0 + 3.f;
        }
    }
    add_profile_strip(st, x0, z0, x1, z0);
    select_created_endpoint(st, x1, z0);
    refill_wrap_list(st);
    refill_pt_list(st);
    sync_uv_edits(st);
    invalidate_all(st);
}

void get_pencil_anchor(CreatorState* st, float& x0, float& z0) {
    x0 = 0.f;
    z0 = 0.1f;
    if (!st->sel_pts.empty()) {
        const int pi = st->sel_profile;
        if (pi >= 0 && static_cast<std::size_t>(pi) < st->def.profiles.size()) {
            const auto& cur = st->def.profiles[static_cast<std::size_t>(pi)];
            const int ri = st->sel_pts.back();
            if (ri >= 0 && static_cast<std::size_t>(ri) < cur.size()) {
                x0 = cur[static_cast<std::size_t>(ri)].x;
                z0 = cur[static_cast<std::size_t>(ri)].z;
            }
        }
    } else if (!st->sel_multi.empty()) {
        const auto& r = st->sel_multi.back();
        if (r.profile >= 0 && static_cast<std::size_t>(r.profile) < st->def.profiles.size() && r.pt >= 0 &&
            static_cast<std::size_t>(r.pt) < st->def.profiles[static_cast<std::size_t>(r.profile)].size()) {
            const auto& p = st->def.profiles[static_cast<std::size_t>(r.profile)][static_cast<std::size_t>(r.pt)];
            x0 = p.x;
            z0 = p.z;
        }
    } else {
        auto& cur = active_strip(st);
        if (!cur.empty()) {
            x0 = cur.back().x;
            z0 = cur.back().z;
        }
    }
}

void add_profile_from_click(CreatorState* st, float x, float z) {
    float x0 = 0.f, z0 = 0.1f;
    get_pencil_anchor(st, x0, z0);
    add_profile_strip(st, x0, z0, x, z);
    refill_wrap_list(st);
}

void free_texture_previews(CreatorState* st) {
    if (!st) return;
    for (auto& c : st->tex_cells) {
        if (c.bmp) DeleteObject(c.bmp);
        c = {};
    }
}

void load_texture_previews(CreatorState* st) {
    free_texture_previews(st);
    if (!st || st->def.textures.empty()) return;
    const int n = static_cast<int>(st->def.textures.size());
    int base = st->sel_tex - 1;
    if (n <= kTexPreviewCols) base = 0;
    else base = (std::clamp)(base, 0, n - kTexPreviewCols);
    for (int col = 0; col < kTexPreviewCols; ++col) {
        const int ti = (n <= kTexPreviewCols) ? (col < n ? col : -1) : base + col;
        if (ti < 0 || ti >= n) continue;
        auto& cell = st->tex_cells[static_cast<std::size_t>(col)];
        cell.tex_index = ti;
        const auto& name = st->def.textures[static_cast<std::size_t>(ti)];
        const auto path = resolve_tex_file(st, name);
        if (!path) continue;
        cell.bmp = load_file_hbitmap(st, *path);
        if (!cell.bmp) continue;
        BITMAP bm{};
        GetObjectW(cell.bmp, sizeof(bm), &bm);
        cell.w = bm.bmWidth;
        cell.h = std::abs(bm.bmHeight);
    }
}

void remap_materials_after_tex_delete(CreatorState* st, int deleted) {
    if (!st || deleted < 0) return;
    for (auto& strip : st->def.profiles) {
        for (auto& pt : strip) {
            if (pt.material_id > deleted) --pt.material_id;
            else if (pt.material_id == deleted) pt.material_id = deleted == 0 ? 0 : deleted - 1;
        }
    }
}

bool texture_index_in_use(const CreatorState* st, int tex_idx) {
    if (tex_idx < 0) return false;
    for (const auto& strip : st->def.profiles) {
        if (strip.empty()) continue;
        if (strip.front().material_id == tex_idx) return true;
    }
    return false;
}

void refill_wrap_list(CreatorState* st) {
    if (!st->wrap_list) return;
    SendMessageW(st->wrap_list, LB_RESETCONTENT, 0, 0);
    for (std::size_t pi = 0; pi < st->def.profiles.size(); ++pi) {
        const auto label = format_profile_slot_label(st, pi);
        SendMessageW(st->wrap_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    if (!st->def.profiles.empty()) {
        st->sel_profile = (std::clamp)(st->sel_profile, 0, static_cast<int>(st->def.profiles.size()) - 1);
        st->suppress_commands = true;
        SendMessageW(st->wrap_list, LB_SETCURSEL, st->sel_profile, 0);
        st->suppress_commands = false;
    }
}

void refill_hp_list(CreatorState* st) {
    (void)st;
}

void sync_profile_fields_ui(CreatorState* st) {
    (void)st;
}

void sync_tex_slot_ui(CreatorState* st) {
    sync_profile_fields_ui(st);
}

void sync_path_field_ui(CreatorState* st) {
    (void)st;
}

void sync_texture_flags_ui(CreatorState* st) {
    if (!st->dlg) return;
    const bool ok = st->sel_tex >= 0 && static_cast<std::size_t>(st->sel_tex) < st->def.textures.size();
    st->suppress_commands = true;
    if (ok && static_cast<std::size_t>(st->sel_tex) < st->def.texture_extra.size()) {
        const auto& ex = st->def.texture_extra[static_cast<std::size_t>(st->sel_tex)];
        if (HWND h = GetDlgItem(st->dlg, kTexDamage))
            SendMessageW(h, BM_SETCHECK, ex.damage_surf ? BST_CHECKED : BST_UNCHECKED, 0);
        if (HWND h = GetDlgItem(st->dlg, kTexPud))
            SendMessageW(h, BM_SETCHECK, ex.puddles ? BST_CHECKED : BST_UNCHECKED, 0);
        if (HWND h = GetDlgItem(st->dlg, kTexMoist))
            SendMessageW(h, BM_SETCHECK, ex.moisture ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    st->suppress_commands = false;
}

void read_texture_flags_ui(CreatorState* st) {
    if (!st->dlg || st->sel_tex < 0 || static_cast<std::size_t>(st->sel_tex) >= st->def.textures.size()) return;
    ensure_texture_parallel(st->def);
    auto& ex = st->def.texture_extra[static_cast<std::size_t>(st->sel_tex)];
    if (HWND h = GetDlgItem(st->dlg, kTexDamage))
        ex.damage_surf = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (HWND h = GetDlgItem(st->dlg, kTexPud))
        ex.puddles = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (HWND h = GetDlgItem(st->dlg, kTexMoist))
        ex.moisture = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void import_sli_into(CreatorState* st, const std::filesystem::path& path) {
    const auto loaded = spline::load_sli(path);
    if (!loaded) {
        MessageBoxW(st->dlg, L"No se pudo leer el .sli.", L"Abrir spline", MB_OK | MB_ICONERROR);
        return;
    }
    push_undo(st);
    st->def = *loaded;
    ensure_texture_parallel(st->def);
    st->save_path = path;
    st->def.sli_path = path;
    st->preview_tex_cache.clear();
    st->tex_thumb_cache.clear();
    st->preview_first_frame = true;
    st->preview_mesh_dirty = true;
    st->sel_tex = 0;
    st->sel_profile = 0;
    st->profile_labels.clear();
    for (const auto& c : st->def.profile_comments) {
        st->profile_labels.push_back(c.empty() ? std::wstring{} : utf8_to_wide(c));
    }
    ensure_profile_labels(st);
    st->sel_hp = 0;
    st->sel_pts.clear();
    st->sel_multi.clear();
    st->sel_profiles.clear();
    st->marquee_drag = false;
    if (st->canvas && GetCapture() == st->canvas) ReleaseCapture();
    if (st->dlg) {
        sync_hp_edits(st->dlg, st);
        center_view(st);
    }
    refill_textures(st);
    load_texture_previews(st);
    select_profile(st, 0);
    refill_pt_list(st);
    refill_wrap_list(st);
    refresh_phase2_ui(st);
    sync_path_field_ui(st);
    sync_texture_flags_ui(st);
    sync_extra_edits(st);
    invalidate_all(st);
}

void open_sli_dialog(CreatorState* st, HWND owner) {
    wchar_t file[MAX_PATH]{};
    if (!st->save_path.empty()) wcscpy_s(file, st->save_path.c_str());
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Splines (*.sli)\0*.sli\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!st->omsi_root.empty()) {
        static std::wstring init_dir = (st->omsi_root / L"Splines").wstring();
        ofn.lpstrInitialDir = init_dir.c_str();
    }
    if (!GetOpenFileNameW(&ofn)) return;
    import_sli_into(st, file);
}

void draw_tex_preview(HDC dc, CreatorState* st) {
    if (!st->tex_preview) return;
    RECT rc{};
    GetClientRect(st->tex_preview, &rc);
    FillRect(dc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    const int total_w = rc.right - rc.left;
    if (total_w <= 0) return;
    SetBkMode(dc, TRANSPARENT);
    const int gap = 2;
    const int col_w = (total_w - gap * (kTexPreviewCols - 1)) / kTexPreviewCols;
    for (int col = 0; col < kTexPreviewCols; ++col) {
        const int left = rc.left + col * (col_w + gap);
        const RECT cell{left, rc.top, left + col_w, rc.bottom};
        const auto& tc = st->tex_cells[static_cast<std::size_t>(col)];
        const bool sel = tc.tex_index == st->sel_tex;
        FrameRect(dc, &cell, reinterpret_cast<HBRUSH>(GetStockObject(sel ? GRAY_BRUSH : LTGRAY_BRUSH)));
        if (tc.tex_index >= 0) {
            TextOutW(dc, cell.left + 4, cell.top + 2, L"imagen", 6);
        }
        if (!tc.bmp || tc.w <= 0) continue;
        const int pad = 4;
        const int top = cell.top + 16;
        HDC mem = CreateCompatibleDC(dc);
        SelectObject(mem, tc.bmp);
        SetStretchBltMode(dc, HALFTONE);
        StretchBlt(dc, cell.left + pad, top, cell.right - cell.left - pad * 2, cell.bottom - top - pad, mem, 0, 0,
                 tc.w, tc.h, SRCCOPY);
        DeleteDC(mem);
    }
}

void set_uv_from_texture_click(CreatorState* st, int sx, int w) {
    if (w <= 1 || !st) return;
    const int gap = 2;
    const int col_w = (w - gap * (kTexPreviewCols - 1)) / kTexPreviewCols;
    if (col_w <= 0) return;
    int col = 0;
    for (; col < kTexPreviewCols; ++col) {
        const int x0 = col * (col_w + gap);
        if (sx < x0 + col_w + gap) break;
    }
    col = (std::clamp)(col, 0, kTexPreviewCols - 1);
    const int ti = st->tex_cells[static_cast<std::size_t>(col)].tex_index;
    if (ti >= 0) {
        st->sel_tex = ti;
        sync_tex_slot_ui(st);
        sync_texture_flags_ui(st);
        invalidate_all(st);
    }
}

void build_parity_ui(HWND dlg, CreatorState* st) {
    const int rx = kRightX;
    const int rw = kDesignW - rx - 8;
    const int bw = (rw - 8) / 3;

    st->profile_combo = nullptr;
    st->wrap_list = CreateWindowW(WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL, rx, 10,
                                  rw, 100, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kWrapList)),
                                  GetModuleHandleW(nullptr), nullptr);

    mk_static(dlg, L"texture wrapping", rx, 116, 120, 16);
    mk_btn(dlg, kWrapDel, L"Borrar", rx, 134, bw, 26);
    mk_btn(dlg, kWrapEdit, L"Renombrar", rx + bw + 4, 134, bw, 26);
    mk_btn(dlg, kWrapAdd, L"Agregar", rx + (bw + 4) * 2, 134, bw, 26);
    {
        const int hw = (rw - 4) / 2;
        mk_btn(dlg, kWrapOpen, L"Textur-Wrapper", rx, 164, hw, 28);
        mk_btn(dlg, kApplyTex, L"Aplicar textura", rx + hw + 4, 164, hw, 28);
    }

    mk_static(dlg, L"Texturas", rx, 198, 80, 16);
    st->tex_list = CreateWindowW(
        WC_LISTBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_VSCROLL, rx, 216, rw,
        kTexSlotH * 3 + 6, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTexList)), GetModuleHandleW(nullptr),
        nullptr);
    mk_btn(dlg, kTexDel, L"Borrar", rx, 216 + kTexSlotH * 3 + 14, bw, 26);
    mk_btn(dlg, kTexPaste, L"Pegar", rx + bw + 4, 216 + kTexSlotH * 3 + 14, bw, 26);
    mk_btn(dlg, kTexAdd, L"Agregar", rx + (bw + 4) * 2, 216 + kTexSlotH * 3 + 14, bw, 26);

    const int py = 216 + kTexSlotH * 3 + 48;
    mk_static(dlg, L"Path", rx, py, 40, 16);
    st->path_list = CreateWindowW(WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL, rx,
                                  py + 18, rw, 88, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPathList)),
                                  GetModuleHandleW(nullptr), nullptr);
    SetWindowSubclass(st->path_list, path_list_subclass, 0, reinterpret_cast<DWORD_PTR>(st));

    const int pby = py + 114;
    mk_btn(dlg, kPathDel, L"Borrar", rx, pby, bw, 26);
    mk_btn(dlg, kPathCreate, L"Crear en 2D", rx + bw + 4, pby, bw + 12, 26);
    mk_btn(dlg, kPathAdd, L"Agregar", rx + (bw + 4) * 2 + 12, pby, bw - 12, 26);

    const int pwy = pby + 34;
    mk_static(dlg, L"Patchwork chain", rx, pwy, rw, 16);
    if (HWND pw = GetDlgItem(dlg, kPwEn)) {
        SetWindowPos(pw, nullptr, rx, pwy + 18, rw, 20, SWP_NOZORDER | SWP_SHOWWINDOW);
        SetWindowTextW(pw, L"[patchwork_chain]");
    }
    mk_static(dlg, L"seglen:", rx, pwy + 42, 44, 16);
    if (HWND seg = GetDlgItem(dlg, kPwSeg))
        SetWindowPos(seg, nullptr, rx + 48, pwy + 38, 56, 22, SWP_NOZORDER | SWP_SHOWWINDOW);
    mk_static(dlg, L"trans:", rx + 112, pwy + 42, 36, 16);
    if (HWND tr = GetDlgItem(dlg, kPwTrans))
        SetWindowPos(tr, nullptr, rx + 152, pwy + 38, rw - 164, 22, SWP_NOZORDER | SWP_SHOWWINDOW);
    mk_static(dlg, L"fact:", rx, pwy + 68, 32, 16);
    if (HWND fa = GetDlgItem(dlg, kPwFact))
        SetWindowPos(fa, nullptr, rx + 36, pwy + 64, rw - 48, 22, SWP_NOZORDER | SWP_SHOWWINDOW);
    mk_static(dlg, L"inv:", rx, pwy + 94, 24, 16);
    if (HWND inv = GetDlgItem(dlg, kPwInv))
        SetWindowPos(inv, nullptr, rx + 28, pwy + 90, rw - 40, 22, SWP_NOZORDER | SWP_SHOWWINDOW);

    const int fy = pwy + 120;
    mk_btn(dlg, kOpenSli, L"abrir .sli", rx, fy, rw, 36);
    mk_btn(dlg, kSave, L"guardar .sli", rx, fy + 42, rw, 36);
    mk_btn(dlg, kCancel, L"cancelar", rx, fy + 84, rw, 36);

    CreateWindowW(L"BUTTON", L"Copiar texturas al exportar", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, rx, fy + 126, rw,
                  20, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kExportCopy)), GetModuleHandleW(nullptr),
                  nullptr);
    SendMessageW(GetDlgItem(dlg, kExportCopy), BM_SETCHECK, BST_CHECKED, 0);

    CreateWindowExW(0, L"OmsiF4SplineTexPrev", L"", WS_CHILD, -400, -400, 1, 1, dlg,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTexPreview)), GetModuleHandleW(nullptr), st);
    CreateWindowW(L"BUTTON", L"", WS_CHILD | BS_AUTOCHECKBOX, -200, -200, 1, 1, dlg,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTexDamage)), GetModuleHandleW(nullptr), nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD | BS_AUTOCHECKBOX, -200, -200, 1, 1, dlg,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTexPud)), GetModuleHandleW(nullptr), nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD | BS_AUTOCHECKBOX, -200, -200, 1, 1, dlg,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTexMoist)), GetModuleHandleW(nullptr), nullptr);
    mk_edit(dlg, kTexEdit, -200, -200, 1, 1);
    st->hp_list = nullptr;
}

bool handle_parity_command(HWND hwnd, CreatorState* st, int id, int code) {
    if (!st || st->suppress_commands) return false;
    if (id == kOpenSli) {
        open_sli_dialog(st, hwnd);
        return true;
    }
    if (id == kWrapList && code == LBN_SELCHANGE) {
        const int pi = static_cast<int>(SendMessageW(st->wrap_list, LB_GETCURSEL, 0, 0));
        if (pi >= 0) set_active_profile_keep_selection(st, pi);
        return true;
    }
    if (id == kWrapList && code == LBN_DBLCLK) {
        if (!st->def.profiles.empty()) run_texture_wrap_dialog(hwnd, st, st->sel_profile, false);
        return true;
    }
    if (id == kWrapAdd) {
        add_empty_profile(st);
        return true;
    }
    if (id == kWrapOpen) {
        if (!st->def.profiles.empty()) run_texture_wrap_dialog(hwnd, st, st->sel_profile, false);
        return true;
    }
    if (id == kWrapEdit) {
        if (!st->def.profiles.empty()) run_rename_profile_dialog(hwnd, st, st->sel_profile);
        return true;
    }
    if (id == kPathList && code == LBN_SELCHANGE) {
        read_path_edits(st);
        st->sel_path = static_cast<int>(SendMessageW(st->path_list, LB_GETCURSEL, 0, 0));
        sync_path_edits(st);
        sync_path_field_ui(st);
        invalidate_all(st);
        return true;
    }
    if (id == kPathList && code == LBN_DBLCLK) {
        if (!st->def.paths.empty()) run_path_config_dialog(hwnd, st, st->sel_path, false);
        return true;
    }
    if (id == kDrawTex && code == BN_CLICKED) {
        st->draw_textures = !st->draw_textures;
        if (HWND h = GetDlgItem(hwnd, kDrawTex))
            SendMessageW(h, BM_SETCHECK, st->draw_textures ? BST_CHECKED : BST_UNCHECKED, 0);
        invalidate_tool_btn(st, kDrawTex);
        InvalidateRect(st->canvas, nullptr, FALSE);
        return true;
    }
    if (id == kDrawPath && code == BN_CLICKED) {
        st->draw_paths = !st->draw_paths;
        if (HWND h = GetDlgItem(hwnd, kDrawPath))
            SendMessageW(h, BM_SETCHECK, st->draw_paths ? BST_CHECKED : BST_UNCHECKED, 0);
        invalidate_tool_btn(st, kDrawPath);
        InvalidateRect(st->canvas, nullptr, FALSE);
        return true;
    }
    return false;
}
