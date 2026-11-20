// Included from spline_creator_dialog.cpp (anonymous namespace).

constexpr int kProfileCombo = 9131;
constexpr int kProfileAdd = 9132;
constexpr int kProfileDel = 9133;
constexpr int kMatId = 9134;
constexpr int kPathTyp = 9136;
constexpr int kPathLat = 9137;
constexpr int kPathHt = 9138;
constexpr int kPathW = 9139;
constexpr int kPathDir = 9140;
constexpr int kPath2Chk = 9141;
constexpr int kTexAlpha = 9142;
constexpr int kTexDel = 9143;
constexpr int kLength = 9144;
constexpr int kPwEn = 9145;
constexpr int kPwSeg = 9146;
constexpr int kPwTrans = 9147;
constexpr int kPwFact = 9148;
constexpr int kPwInv = 9149;
constexpr int kOnlyEditor = 9150;
constexpr int kRightScroll = 9151;

COLORREF sample_tex(const gfx::dx12::CpuRgbaImage& img, float u, float v) {
    if (img.width == 0 || img.height == 0 || img.rgba.empty()) return RGB(180, 180, 180);
    u = (std::clamp)(u, 0.f, 1.f);
    v = (std::clamp)(v, 0.f, 1.f);
    const int x = static_cast<int>(u * static_cast<float>(img.width - 1));
    const int y = static_cast<int>(v * static_cast<float>(img.height - 1));
    const std::uint8_t* p = img.rgba.data() + (static_cast<std::size_t>(y) * img.width + x) * 4u;
    return RGB(p[0], p[1], p[2]);
}

HBITMAP rgba_to_hbitmap(const gfx::dx12::CpuRgbaImage& img) {
    if (img.width == 0 || img.height == 0 || img.rgba.empty()) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(img.width);
    bi.bmiHeader.biHeight = -static_cast<LONG>(img.height);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bmp || !bits) return nullptr;
    auto* dst = static_cast<std::uint8_t*>(bits);
    for (std::size_t i = 0; i + 3 < img.rgba.size(); i += 4) {
        dst[i + 0] = img.rgba[i + 2];
        dst[i + 1] = img.rgba[i + 1];
        dst[i + 2] = img.rgba[i + 0];
        dst[i + 3] = img.rgba[i + 3];
    }
    return bmp;
}

HBITMAP load_file_hbitmap(CreatorState* st, const std::filesystem::path& path) {
    if (const auto img = gfx::dx12::load_image_cpu_rgba(path)) return rgba_to_hbitmap(*img);
    return nullptr;
}

const gfx::dx12::CpuRgbaImage* preview_tex_pixels(CreatorState* st, const std::string& name) {
    if (const auto it = st->preview_tex_cache.find(name); it != st->preview_tex_cache.end()) return &it->second;
    const auto path = resolve_tex_file(st, name);
    if (!path) return nullptr;
    auto img = gfx::dx12::load_image_cpu_rgba(*path);
    if (!img) return nullptr;
    const auto [ins, ok] = st->preview_tex_cache.emplace(name, std::move(*img));
    return ok ? &ins->second : nullptr;
}

void project3d_depth(float x, float y, float z, float yaw, float pitch, float pan_x, float pan_y, float cx, float cy,
                     float scale, int& sx, int& sy, float& depth) {
    project3d(x, y, z, yaw, pitch, pan_x, pan_y, cx, cy, scale, sx, sy);
    const float cyaw = std::cos(yaw);
    const float syaw = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float rx = x * cyaw - y * syaw;
    const float ry = x * syaw + y * cyaw;
    depth = z * cp + ry * sp;
}

void draw_preview3d_mesh(HDC dc, CreatorState* st) {
    RECT rc{};
    GetClientRect(st->preview3d, &rc);
    FillRect(dc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    if (!st || !st->preview3d) return;
    try {
        const float cx = (rc.left + rc.right) * 0.5f;
        const float cy = (rc.top + rc.bottom) * 0.55f;

        map::SplineInst sp{};
        sp.length = kPreviewLenM;
        sp.heading_deg = 0.f;
        const auto meshes = spline::build_spline_meshes(sp, st->def, st->omsi_root);
        if (meshes.empty()) {
            draw_preview3d_wire(dc, st);
            return;
        }

        struct TriDraw {
            POINT pt[3];
            COLORREF col;
            float depth;
        };
        std::vector<TriDraw> tris;

        for (const auto& mesh : meshes) {
            const gfx::dx12::CpuRgbaImage* tex = nullptr;
            if (!mesh.texture_path.empty()) {
                const std::string tex_name = mesh.texture_path.generic_string();
                tex = preview_tex_pixels(st, tex_name);
                if (!tex) tex = preview_tex_pixels(st, mesh.texture_path.filename().string());
            }
            for (std::size_t ti = 0; ti + 2 < mesh.indices.size(); ti += 3) {
                const std::uint32_t ia = mesh.indices[ti];
                const std::uint32_t ib = mesh.indices[ti + 1];
                const std::uint32_t ic = mesh.indices[ti + 2];
                if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() || ic >= mesh.vertices.size()) continue;
                const auto& v0 = mesh.vertices[ia];
                const auto& v1 = mesh.vertices[ib];
                const auto& v2 = mesh.vertices[ic];

                int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0, sx2 = 0, sy2 = 0;
                float d0 = 0.f, d1 = 0.f, d2 = 0.f;
                project3d_depth(v0.position[0], v0.position[1], v0.position[2], st->preview_yaw,
                                st->preview_pitch, st->preview_pan_x, st->preview_pan_y, cx, cy, st->preview_scale,
                                sx0, sy0, d0);
                project3d_depth(v1.position[0], v1.position[1], v1.position[2], st->preview_yaw,
                                st->preview_pitch, st->preview_pan_x, st->preview_pan_y, cx, cy, st->preview_scale,
                                sx1, sy1, d1);
                project3d_depth(v2.position[0], v2.position[1], v2.position[2], st->preview_yaw,
                                st->preview_pitch, st->preview_pan_x, st->preview_pan_y, cx, cy, st->preview_scale,
                                sx2, sy2, d2);

                const long long cross =
                    static_cast<long long>(sx1 - sx0) * (sy2 - sy0) - static_cast<long long>(sy1 - sy0) * (sx2 - sx0);

                TriDraw t{};
                t.pt[0] = {sx0, sy0};
                if (cross < 0) {
                    t.pt[1] = {sx2, sy2};
                    t.pt[2] = {sx1, sy1};
                } else {
                    t.pt[1] = {sx1, sy1};
                    t.pt[2] = {sx2, sy2};
                }
                if (cross == 0) continue;
                t.depth = (d0 + d1 + d2) / 3.f;
                const float u = (v0.uv[0] + v1.uv[0] + v2.uv[0]) / 3.f;
                const float v = (v0.uv[1] + v1.uv[1] + v2.uv[1]) / 3.f;
                if (tex)
                    t.col = sample_tex(*tex, u - std::floor(u), v - std::floor(v));
                else
                    t.col = RGB(140, 160, 200);
                tris.push_back(t);
            }
        }

        if (tris.empty()) {
            draw_preview3d_wire(dc, st);
            return;
        }

        std::sort(tris.begin(), tris.end(), [](const TriDraw& a, const TriDraw& b) { return a.depth < b.depth; });
        const int old_pf = SetPolyFillMode(dc, WINDING);
        for (const auto& t : tris) {
            HBRUSH br = CreateSolidBrush(t.col);
            HGDIOBJ old = SelectObject(dc, br);
            Polygon(dc, t.pt, 3);
            SelectObject(dc, old);
            DeleteObject(br);
        }
        SetPolyFillMode(dc, old_pf);
        SetBkMode(dc, TRANSPARENT);
        TextOutW(dc, 8, 4, L"Preview 3D texturizado  |  rueda=zoom  |  MMB=pan  |  LMB=rotar", 58);
    } catch (...) {
        draw_preview3d_wire(dc, st);
    }
}

void refill_profiles(CreatorState* st) {
    if (!st->profile_combo) return;
    SendMessageW(st->profile_combo, CB_RESETCONTENT, 0, 0);
    for (std::size_t i = 0; i < st->def.profiles.size(); ++i) {
        wchar_t line[64];
        const int mat = st->def.profiles[i].empty() ? 0 : st->def.profiles[i].front().material_id;
        swprintf_s(line, L"Perfil %zu  mat=%d  pts=%zu", i + 1, mat, st->def.profiles[i].size());
        SendMessageW(st->profile_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line));
    }
    st->sel_profile = (std::clamp)(st->sel_profile, 0, static_cast<int>(std::max<std::size_t>(1, st->def.profiles.size()) - 1));
    SendMessageW(st->profile_combo, CB_SETCURSEL, st->sel_profile, 0);
    if (st->dlg) {
        st->suppress_commands = true;
        wchar_t buf[16];
        swprintf_s(buf, L"%d", active_strip(st).empty() ? 0 : active_strip(st).front().material_id);
        SetWindowTextW(GetDlgItem(st->dlg, kMatId), buf);
        st->suppress_commands = false;
    }
}

void refill_paths(CreatorState* st) {
    if (!st->path_list) return;
    SendMessageW(st->path_list, LB_RESETCONTENT, 0, 0);
    for (std::size_t i = 0; i < st->def.paths.size(); ++i) {
        const auto& p = st->def.paths[i];
        wchar_t line[128];
        swprintf_s(line, L"#%zu %s typ=%d lat=%.2f h=%.2f w=%.2f dir=%d", i + 1,
                   p.secondary ? L"[path_2]" : L"[path]", p.typ, static_cast<double>(p.lateral),
                   static_cast<double>(p.height), static_cast<double>(p.width), p.direction);
        SendMessageW(st->path_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line));
    }
    if (!st->def.paths.empty()) {
        st->sel_path = (std::clamp)(st->sel_path, 0, static_cast<int>(st->def.paths.size()) - 1);
        SendMessageW(st->path_list, LB_SETCURSEL, st->sel_path, 0);
    }
    sync_path_edits(st);
    sync_path_field_ui(st);
}

void sync_path_edits(CreatorState* st) {
    if (!st->dlg) return;
    st->suppress_commands = true;
    auto setf = [&](int id, float v) {
        wchar_t b[32];
        swprintf_s(b, L"%.3f", static_cast<double>(v));
        SetWindowTextW(GetDlgItem(st->dlg, id), b);
    };
    auto seti = [&](int id, int v) {
        wchar_t b[16];
        swprintf_s(b, L"%d", v);
        SetWindowTextW(GetDlgItem(st->dlg, id), b);
    };
    if (st->def.paths.empty() || st->sel_path < 0 ||
        static_cast<std::size_t>(st->sel_path) >= st->def.paths.size()) {
        seti(kPathTyp, 0);
        setf(kPathLat, 0.f);
        setf(kPathHt, 0.f);
        setf(kPathW, 3.f);
        seti(kPathDir, 2);
        SendMessageW(GetDlgItem(st->dlg, kPath2Chk), BM_SETCHECK, BST_UNCHECKED, 0);
        st->suppress_commands = false;
        return;
    }
    const auto& p = st->def.paths[static_cast<std::size_t>(st->sel_path)];
    seti(kPathTyp, p.typ);
    setf(kPathLat, p.lateral);
    setf(kPathHt, p.height);
    setf(kPathW, p.width);
    seti(kPathDir, p.direction);
    SendMessageW(GetDlgItem(st->dlg, kPath2Chk), BM_SETCHECK, p.secondary ? BST_CHECKED : BST_UNCHECKED, 0);
    st->suppress_commands = false;
}

void read_path_edits(CreatorState* st) {
    if (!st->dlg || st->def.paths.empty() || st->sel_path < 0 ||
        static_cast<std::size_t>(st->sel_path) >= st->def.paths.size())
        return;
    auto& p = st->def.paths[static_cast<std::size_t>(st->sel_path)];
    auto getf = [&](int id, float fb) {
        wchar_t buf[32]{};
        GetWindowTextW(GetDlgItem(st->dlg, id), buf, 32);
        try {
            return std::stof(std::wstring(buf));
        } catch (...) {
            return fb;
        }
    };
    auto geti = [&](int id, int fb) {
        wchar_t buf[16]{};
        GetWindowTextW(GetDlgItem(st->dlg, id), buf, 16);
        try {
            return std::stoi(std::wstring(buf));
        } catch (...) {
            return fb;
        }
    };
    p.typ = geti(kPathTyp, p.typ);
    p.lateral = getf(kPathLat, p.lateral);
    p.height = getf(kPathHt, p.height);
    p.width = getf(kPathW, p.width);
    p.direction = geti(kPathDir, p.direction);
    p.secondary = SendMessageW(GetDlgItem(st->dlg, kPath2Chk), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void sync_extra_edits(CreatorState* st) {
    if (!st->dlg) return;
    st->suppress_commands = true;
    wchar_t buf[32];
    swprintf_s(buf, L"%.3f", static_cast<double>(st->def.path_length));
    SetWindowTextW(GetDlgItem(st->dlg, kLength), buf);
    SendMessageW(GetDlgItem(st->dlg, kOnlyEditor), BM_SETCHECK, st->def.only_editor ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(st->dlg, kPwEn), BM_SETCHECK, st->def.patchwork.valid ? BST_CHECKED : BST_UNCHECKED, 0);
    swprintf_s(buf, L"%.3f", static_cast<double>(st->def.patchwork.seg_length));
    SetWindowTextW(GetDlgItem(st->dlg, kPwSeg), buf);
    SetWindowTextW(GetDlgItem(st->dlg, kPwTrans), utf8_to_wide(st->def.patchwork.transitions).c_str());
    SetWindowTextW(GetDlgItem(st->dlg, kPwFact), utf8_to_wide(st->def.patchwork.factors).c_str());
    SetWindowTextW(GetDlgItem(st->dlg, kPwInv), utf8_to_wide(st->def.patchwork.inverse).c_str());
    if (!st->def.textures.empty() && st->sel_tex >= 0 &&
        static_cast<std::size_t>(st->sel_tex) < st->def.texture_alpha.size()) {
        SendMessageW(GetDlgItem(st->dlg, kTexAlpha), CB_SETCURSEL,
                     static_cast<int>(st->def.texture_alpha[static_cast<std::size_t>(st->sel_tex)]), 0);
    }
}

void read_extra_edits(CreatorState* st) {
    if (!st->dlg) return;
    wchar_t buf[256]{};
    GetWindowTextW(GetDlgItem(st->dlg, kLength), buf, 32);
    try {
        st->def.path_length = std::stof(std::wstring(buf));
    } catch (...) {
    }
    st->def.only_editor = SendMessageW(GetDlgItem(st->dlg, kOnlyEditor), BM_GETCHECK, 0, 0) == BST_CHECKED;
    st->def.patchwork.valid = SendMessageW(GetDlgItem(st->dlg, kPwEn), BM_GETCHECK, 0, 0) == BST_CHECKED;
    GetWindowTextW(GetDlgItem(st->dlg, kPwSeg), buf, 32);
    try {
        st->def.patchwork.seg_length = std::stof(std::wstring(buf));
    } catch (...) {
    }
    GetWindowTextW(GetDlgItem(st->dlg, kPwTrans), buf, 256);
    st->def.patchwork.transitions = wide_to_utf8(buf);
    GetWindowTextW(GetDlgItem(st->dlg, kPwFact), buf, 256);
    st->def.patchwork.factors = wide_to_utf8(buf);
    GetWindowTextW(GetDlgItem(st->dlg, kPwInv), buf, 256);
    st->def.patchwork.inverse = wide_to_utf8(buf);
    if (!st->def.textures.empty() && st->sel_tex >= 0 &&
        static_cast<std::size_t>(st->sel_tex) < st->def.texture_alpha.size()) {
        const int a = static_cast<int>(SendMessageW(GetDlgItem(st->dlg, kTexAlpha), CB_GETCURSEL, 0, 0));
        st->def.texture_alpha[static_cast<std::size_t>(st->sel_tex)] = static_cast<std::uint8_t>((std::clamp)(a, 0, 2));
    }
    st->suppress_commands = false;
}

void draw_ghost_profiles(HDC dc, CreatorState* st, const RECT& rc) {
    HPEN ghost = CreatePen(PS_DOT, 1, RGB(180, 190, 210));
    HGDIOBJ old = SelectObject(dc, ghost);
    for (std::size_t pi = 0; pi < st->def.profiles.size(); ++pi) {
        if (static_cast<int>(pi) == st->sel_profile) continue;
        const auto& strip = st->def.profiles[pi];
        for (std::size_t i = 1; i < strip.size(); ++i) {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            world_to_screen(rc, st->view, strip[i - 1].x, strip[i - 1].z, x0, y0);
            world_to_screen(rc, st->view, strip[i].x, strip[i].z, x1, y1);
            MoveToEx(dc, x0, y0, nullptr);
            LineTo(dc, x1, y1);
        }
    }
    SelectObject(dc, old);
    DeleteObject(ghost);
}

void draw_path_lanes(HDC dc, CreatorState* st, const RECT& rc) {
    for (std::size_t i = 0; i < st->def.paths.size(); ++i) {
        const auto& p = st->def.paths[i];
        const float half = p.width * 0.5f;
        const float x0 = p.lateral - half;
        const float x1 = p.lateral + half;
        const float z = p.height;
        int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0, scx = 0, scy = 0;
        world_to_screen(rc, st->view, x0, z, sx0, sy0);
        world_to_screen(rc, st->view, x1, z, sx1, sy1);
        world_to_screen(rc, st->view, p.lateral, z, scx, scy);

        const bool sel = static_cast<int>(i) == st->sel_path ||
                         std::find(st->sel_paths.begin(), st->sel_paths.end(), static_cast<int>(i)) != st->sel_paths.end();
        const COLORREF col = sel ? RGB(255, 120, 30) : RGB(220, 120, 40);
        HPEN pen = CreatePen(PS_SOLID, sel ? 4 : 3, col);
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, sx0, sy0, nullptr);
        LineTo(dc, sx1, sy1);
        SelectObject(dc, old);
        DeleteObject(pen);

        HBRUSH ctr = CreateSolidBrush(col);
        old = SelectObject(dc, ctr);
        Ellipse(dc, scx - 4, scy - 4, scx + 4, scy + 4);
        SelectObject(dc, old);
        DeleteObject(ctr);

        if (sel) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, col);
            wchar_t lbl[64];
            swprintf_s(lbl, L"path %.1fm @ lat=%.2f h=%.2f", static_cast<double>(p.width),
                       static_cast<double>(p.lateral), static_cast<double>(p.height));
            TextOutW(dc, scx + 6, scy - 10, lbl, static_cast<int>(wcslen(lbl)));
        }
    }
}

void build_phase2_ui(HWND dlg, CreatorState* st) {
    (void)st;
    // Hidden edits — synced from sub-dialogs / defaults; used on save and HP helpers.
    mk_edit(dlg, kHpL, -300, -300, 1, 1);
    mk_edit(dlg, kHpR, -300, -300, 1, 1);
    mk_edit(dlg, kHpH, -300, -300, 1, 1);
    mk_edit(dlg, kHpS, -300, -300, 1, 1);
    mk_edit(dlg, kPathTyp, -300, -300, 1, 1);
    mk_edit(dlg, kPathLat, -300, -300, 1, 1);
    mk_edit(dlg, kPathHt, -300, -300, 1, 1);
    mk_edit(dlg, kPathW, -300, -300, 1, 1);
    mk_edit(dlg, kPathDir, -300, -300, 1, 1);
    mk_edit(dlg, kLength, -300, -300, 1, 1);
    mk_edit(dlg, kPwSeg, -300, -300, 1, 1);
    mk_edit(dlg, kPwTrans, -300, -300, 1, 1);
    mk_edit(dlg, kPwFact, -300, -300, 1, 1);
    mk_edit(dlg, kPwInv, -300, -300, 1, 1);
    CreateWindowW(L"BUTTON", L"", WS_CHILD | BS_AUTOCHECKBOX, -300, -300, 1, 1, dlg,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPath2Chk)), GetModuleHandleW(nullptr), nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD | BS_AUTOCHECKBOX, -300, -300, 1, 1, dlg,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPwEn)), GetModuleHandleW(nullptr), nullptr);
    CreateWindowW(L"BUTTON", L"", WS_CHILD | BS_AUTOCHECKBOX, -300, -300, 1, 1, dlg,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOnlyEditor)), GetModuleHandleW(nullptr), nullptr);
    HWND alpha = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | CBS_DROPDOWNLIST, -300, -300, 1, 1, dlg,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTexAlpha)), GetModuleHandleW(nullptr),
                               nullptr);
    SendMessageW(alpha, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"0"));
    SendMessageW(alpha, CB_SETCURSEL, 0, 0);
    st->hp_list = nullptr;
}

bool handle_phase2_command(HWND hwnd, CreatorState* st, int id, int code) {
    if (st->suppress_commands) return false;
    if (id == kProfileAdd) {
        push_undo(st);
        auto& np = st->def.profiles.emplace_back();
        np.push_back({0.f, 0.1f, 0.f, 0.167f, st->sel_tex});
        np.push_back({3.f, 0.1f, 1.f, 0.167f, st->sel_tex});
        st->sel_profile = static_cast<int>(st->def.profiles.size()) - 1;
        st->sel_pts.clear();
        refill_profiles(st);
        refill_pt_list(st);
        refill_wrap_list(st);
        invalidate_all(st);
        return true;
    }
    if (id == kProfileDel && st->def.profiles.size() > 1) {
        push_undo(st);
        st->def.profiles.erase(st->def.profiles.begin() + st->sel_profile);
        st->sel_profile = (std::clamp)(st->sel_profile, 0, static_cast<int>(st->def.profiles.size()) - 1);
        st->sel_pts.clear();
        refill_profiles(st);
        refill_pt_list(st);
        refill_wrap_list(st);
        invalidate_all(st);
        return true;
    }
    if (id == kProfileCombo && code == CBN_SELCHANGE) {
        set_active_profile_keep_selection(st,
                                          static_cast<int>(SendMessageW(st->profile_combo, CB_GETCURSEL, 0, 0)));
        return true;
    }
    if (id == kMatId && code == EN_CHANGE) {
        wchar_t buf[16]{};
        GetWindowTextW(GetDlgItem(hwnd, kMatId), buf, 16);
        try {
            const int mat = std::stoi(std::wstring(buf));
            auto& strip = active_strip(st);
            for (auto& pt : strip) pt.material_id = mat;
            refill_profiles(st);
            invalidate_all(st);
        } catch (...) {
        }
        return true;
    }
    if (id == kPathList && code == LBN_SELCHANGE) {
        read_path_edits(st);
        st->sel_path = static_cast<int>(SendMessageW(st->path_list, LB_GETCURSEL, 0, 0));
        if (st->sel_path >= 0) st->sel_paths = {st->sel_path};
        else st->sel_paths.clear();
        sync_path_edits(st);
        invalidate_all(st);
        return true;
    }
    if ((id == kPathTyp || id == kPathLat || id == kPathHt || id == kPathW || id == kPathDir || id == kPath2Chk) &&
        (code == EN_CHANGE || code == BN_CLICKED)) {
        read_path_edits(st);
        invalidate_all(st);
        return true;
    }
    if (id == kTexDel && !st->def.textures.empty()) {
        const int i = st->sel_tex;
        push_undo(st);
        remap_materials_after_tex_delete(st, i);
        st->def.textures.erase(st->def.textures.begin() + i);
        if (static_cast<std::size_t>(i) < st->def.texture_alpha.size())
            st->def.texture_alpha.erase(st->def.texture_alpha.begin() + i);
        if (static_cast<std::size_t>(i) < st->def.texture_extra.size())
            st->def.texture_extra.erase(st->def.texture_extra.begin() + i);
        st->sel_tex = (std::clamp)(i - 1, 0, static_cast<int>(st->def.textures.size()) - 1);
        refill_textures(st);
        refill_wrap_list(st);
        load_texture_previews(st);
        sync_texture_flags_ui(st);
        invalidate_all(st);
        return true;
    }
    if (id == kTexAlpha && code == CBN_SELCHANGE) {
        read_extra_edits(st);
        invalidate_all(st);
        return true;
    }
    if (id == kLength || id == kPwEn || id == kPwSeg || id == kPwTrans || id == kPwFact || id == kPwInv ||
        id == kOnlyEditor) {
        if (code == EN_CHANGE || code == BN_CLICKED) {
            read_extra_edits(st);
            invalidate_all(st);
        }
        return true;
    }
    return false;
}

void refresh_phase2_ui(CreatorState* st) {
    refill_profiles(st);
    refill_paths(st);
    refill_wrap_list(st);
    refill_hp_list(st);
    sync_extra_edits(st);
}
