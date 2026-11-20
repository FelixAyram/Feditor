#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui/spline_creator_dialog.hpp"

#include "gfx/dx12/dx12_texture.hpp"
#include "gfx/dx12/hello_o3d_renderer.hpp"
#include "input/luna_camera.hpp"
#include "omsi/map_types.hpp"
#include "omsi/logger.hpp"
#include "omsi/utf8_win32.hpp"

#ifdef OMSI_ULTRALIGHT_UI
#include "ui/ultralight_html_dialog.hpp"
#endif

#include "ui/imgui_overlay.hpp"
#include "ui/detail/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include <DirectXMath.h>

#include <imgui.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include <memory>
#include <optional>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <cwchar>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace omsi::ui {
namespace {

constexpr int kCancel = 9102;
constexpr int kEspejar = 9103;
constexpr int kDelPt = 9105;
constexpr int kSave = 9106;
constexpr int kCanvas = 9107;
constexpr int kGrid = 9108;
constexpr int kTexList = 9109;
constexpr int kTexAdd = 9110;
constexpr int kTexEdit = 9111;
constexpr int kHpL = 9112;
constexpr int kHpR = 9113;
constexpr int kHpH = 9114;
constexpr int kHpS = 9115;
constexpr int kWarn = 9116;
constexpr int kPathAdd = 9117;
constexpr int kPathDel = 9118;
constexpr int kPathList = 9135;
constexpr int kCenterView = 9120;
constexpr int kPreview3d = 9121;
constexpr int kTexPreview = 9122;
constexpr int kUvStart = 9123;
constexpr int kUvEnd = 9124;
constexpr int kPickStart = 9125;
constexpr int kPickEnd = 9126;
constexpr int kPtList = 9127;
constexpr int kTiling = 9128;
constexpr int kSliPreview = 9129;
constexpr int kFlip = 9104;
constexpr int kPathCreate = 9160;
constexpr int kOpenSli = 9161;
constexpr int kWrapList = 9162;
constexpr int kWrapAdd = 9163;
constexpr int kHpAdd = 9164;
constexpr int kHpDel = 9165;
constexpr int kDrawTex = 9166;
constexpr int kDrawPath = 9167;
constexpr int kDrawHp = 9168;
constexpr int kExportCopy = 9169;
constexpr int kTexDamage = 9170;
constexpr int kTexPud = 9171;
constexpr int kTexMoist = 9172;
constexpr int kHpList = 9173;
constexpr int kWrapDel = 9174;
constexpr int kWrapEdit = 9175;
constexpr int kTexPaste = 9176;
constexpr int kPathEdit = 9177;
constexpr int kProfileMat = 9178;
constexpr int kPathField0 = 9179;
constexpr int kPathField1 = 9180;
constexpr int kTexName0 = 9181;
constexpr int kTexName1 = 9182;
constexpr int kTexName2 = 9183;
constexpr int kWrapOpen = 9184;
constexpr int kApplyTex = 9186;
constexpr int kMarqueeSel = 9185;
constexpr int kPencilDraw = 9187;
constexpr int kTexSlotH = 52;
constexpr int kTexPreviewCols = 3;

constexpr float kDefaultScale = 36.f;
constexpr float kPreviewLenM = 40.f;
constexpr int kHitPx = 10;
constexpr int kHitSegPx = 8;
constexpr int kThumbPx = 48;
constexpr int kMaxUndo = 48;
constexpr int kMaxGridLines = 64;
constexpr int kDesignW = 1060;
constexpr int kDesignH = 820;
constexpr int kViewportW = 368;
constexpr int kViewportH = 360;
constexpr int kLeftPanelW = 748;
constexpr int kRightX = 762;

struct ModalState {
    bool done = false;
    bool ok = false;
};

struct View2D {
    float pan_x = 3.f;
    float pan_z = 0.05f;
    float scale = kDefaultScale;
};

struct ChildLayout {
    HWND hwnd = nullptr;
    RECT design{}; // client-space rect captured at design size
    bool combo = false;
};

struct TexPreviewCell {
    HBITMAP bmp = nullptr;
    int w = 0;
    int h = 0;
    int tex_index = -1;
};

struct PtRef {
    int profile = -1;
    int pt = -1;
};

struct DragPt {
    int profile = -1;
    int pt = -1;
    float x0 = 0.f;
    float z0 = 0.f;
};

struct CreatorState {
    ModalState modal{};
    spline::SplineDefinition def{};
    std::filesystem::path save_path;
    std::vector<ChildLayout> child_layout;
    bool path_place_mode = false;
    std::unordered_map<std::string, std::filesystem::path> tex_sources;
    std::unordered_map<std::string, std::string> tex_thumb_cache; // texture name -> data:image/png;base64,...
    std::vector<spline::SplineDefinition> undo_stack;
    std::vector<spline::ProfilePoint> clip_pts;
    HWND dlg = nullptr;
    HWND canvas = nullptr;
    HWND preview3d = nullptr;
    HWND tex_preview = nullptr;
    HWND warn_box = nullptr;
    HWND pt_list = nullptr;
    HWND tex_list = nullptr;
    HWND sli_preview = nullptr;
    HWND profile_combo = nullptr;
    HWND path_list = nullptr;
    View2D view{};
    std::vector<int> sel_pts;
    int sel_tex = 0;
    int sel_profile = 0;
    int sel_path = 0;
    int tex_pick_mode = 0;
    bool dragging_pt = false;
    bool drag_undo_done = false;
    bool panning_2d = false;
    bool panning_3d = false;
    bool grab_mode = false;
    bool grab_origin_set = false;
    int pan_sx = 0;
    int pan_sy = 0;
    float pan_ax = 0.f;
    float pan_az = 0.f;
    float grab_ax = 0.f;
    float grab_az = 0.f;
    std::vector<float> grab_x0;
    std::vector<float> grab_z0;
    float grid_m = 0.1f;
    bool grid_off = true;
    float preview_yaw = 0.45f;
    float preview_pitch = 0.30f;
    float preview_scale = 5.5f;
    float preview_pan_x = 0.f;
    float preview_pan_y = 0.f;
    bool preview_rot_drag = false;
    int preview_sx = 0;
    int preview_sy = 0;
    std::unique_ptr<gfx::dx12::HelloO3dRenderer> preview_rhi;
    std::vector<std::size_t> preview_chunks;
    input::LunaCamera preview_cam{};
    bool preview_rhi_ok = false;
    bool preview_mesh_dirty = true;
    bool preview_first_frame = true;
    std::array<TexPreviewCell, 3> tex_cells{};
    HWND wrap_list = nullptr;
    HWND hp_list = nullptr;
    int sel_hp = 0;
    bool draw_textures = true;
    bool draw_paths = true;
    bool draw_height_profiles = true;
    bool export_copy_textures = true;
    std::vector<HBITMAP> tex_thumbs;
    std::vector<std::wstring> profile_labels;
    std::vector<PtRef> sel_multi;
    std::vector<int> sel_profiles;
    std::vector<std::vector<spline::ProfilePoint>> clip_profiles;
    std::vector<DragPt> drag_group;
    bool marquee_mode = false;
    bool marquee_drag = false;
    bool pencil_mode = false;
    bool pencil_drag = false;
    float pencil_x0 = 0.f;
    float pencil_z0 = 0.f;
    float pencil_x1 = 0.f;
    float pencil_z1 = 0.f;
    std::vector<int> sel_paths;
    HBITMAP pencil_bmp = nullptr;
    HBITMAP espejar_bmp = nullptr;
    HBITMAP flip_bmp = nullptr;
    HBITMAP del_bmp = nullptr;
    HBITMAP marquee_bmp = nullptr;
    HBITMAP draw_tex_bmp = nullptr;
    HBITMAP draw_path_bmp = nullptr;
    int mar_x0 = 0;
    int mar_y0 = 0;
    int mar_x1 = 0;
    int mar_y1 = 0;
    float drag_wx0 = 0.f;
    float drag_wz0 = 0.f;
    std::unordered_map<std::string, gfx::dx12::CpuRgbaImage> preview_tex_cache;
    std::filesystem::path omsi_root;
    bool saved = false;
    bool laying_out = false;
    bool suppress_commands = false;
    bool self_test_passed = false;
#ifdef OMSI_ULTRALIGHT_UI
    UltralightWin32View sidebar_ui{};
    UltralightWin32View chrome_ui{};
    bool sidebar_ui_ok = false;
    bool chrome_ui_ok = false;
#endif
};

#ifdef OMSI_ULTRALIGHT_UI
void sync_spline_creator_sidebar(CreatorState* st);
void sync_spline_creator_chrome(CreatorState* st);
void handle_spline_sidebar_json(CreatorState* st, HWND dlg, const std::string& json);
void handle_spline_chrome_json(CreatorState* st, HWND dlg, const std::string& json);
BOOL CALLBACK hide_right_panel_child(HWND h, LPARAM lp);
BOOL CALLBACK hide_left_chrome_child(HWND h, LPARAM lp);
#endif

CreatorState* g_st = nullptr; // layout + drawitem

bool g_run_self_test = false;
bool g_self_test_result = false;

// Modal nested in GLFW map editor: owner is often WS_CHILD (side panel). Must disable GA_ROOT
// (Raymond Chen 2006/11/02) — disabling only the child leaves top-level enabled → dead keyboard/focus.
HWND modal_block_root(HWND owner) {
    if (!owner || owner == GetDesktopWindow()) return nullptr;
    return GetAncestor(owner, GA_ROOT);
}

void modal_take_foreground(HWND dlg) {
    if (!dlg) return;
    HWND fg = GetForegroundWindow();
    const DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD cur = GetCurrentThreadId();
    const bool attached = fg_tid && fg_tid != cur && AttachThreadInput(cur, fg_tid, TRUE);
    BringWindowToTop(dlg);
    SetForegroundWindow(dlg);
    SetActiveWindow(dlg);
    if (attached) AttachThreadInput(cur, fg_tid, FALSE);
}

void modal_end_block(HWND block_root, HWND restore_focus) {
    if (block_root) EnableWindow(block_root, TRUE);
    for (int vk = 0x08; vk <= 0xFE; ++vk) (void)GetAsyncKeyState(vk);
    HWND focus = restore_focus;
    if (focus && (!IsWindow(focus) || !IsWindowEnabled(focus))) focus = block_root;
    if (block_root) {
        modal_take_foreground(block_root);
        if (focus) SetFocus(focus);
    }
}

bool handle_key(CreatorState* st, WPARAM key, bool ctrl, LPARAM key_lp = 0);

void world_to_screen(const RECT& rc, const View2D& view, float x, float z, int& sx, int& sy);

void run_self_test_sequence(CreatorState* st) {
    auto fail = [&](const std::string& step) {
        omsi::log::error("spline creator self-test FAIL: " + step);
        st->self_test_passed = false;
    };
    st->self_test_passed = true;

    if (st->pt_list) SetFocus(st->pt_list);
    const int txt_before = st->pt_list ? GetWindowTextLengthW(st->pt_list) : 0;

    if (!handle_key(st, 'A', false, 0)) {
        fail("handle_key A on");
        return;
    }
    if (!st->marquee_mode) {
        fail("marquee_mode on");
        return;
    }
    if (!handle_key(st, 'A', false, 0)) {
        fail("handle_key A off");
        return;
    }
    if (st->marquee_mode) {
        fail("marquee_mode off");
        return;
    }
    const HWND chk = st->dlg ? GetDlgItem(st->dlg, kMarqueeSel) : nullptr;
    if (!chk) {
        fail("marquee checkbox hwnd");
        return;
    }
    SendMessageW(chk, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(st->dlg, WM_COMMAND, MAKEWPARAM(kMarqueeSel, BN_CLICKED), reinterpret_cast<LPARAM>(chk));
    if (!st->marquee_mode) {
        fail("checkbox click on");
        return;
    }
    SendMessageW(chk, BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(st->dlg, WM_COMMAND, MAKEWPARAM(kMarqueeSel, BN_CLICKED), reinterpret_cast<LPARAM>(chk));
    if (st->marquee_mode) {
        fail("checkbox click off");
        return;
    }
    if (!handle_key(st, 'A', false, 0)) {
        fail("handle_key A on again");
        return;
    }
    if (!st->marquee_mode) {
        fail("marquee_mode");
        return;
    }
    const HWND chk2 = st->dlg ? GetDlgItem(st->dlg, kMarqueeSel) : nullptr;
    if (!chk2 || SendMessageW(chk2, BM_GETCHECK, 0, 0) != BST_CHECKED) {
        fail("marquee checkbox");
        return;
    }
    if (st->canvas && GetFocus() != st->canvas) {
        fail("canvas focus after A");
        return;
    }
    if (st->pt_list && GetWindowTextLengthW(st->pt_list) != txt_before) {
        fail("edit received A character");
        return;
    }

    if (st->def.profiles.empty() || st->def.profiles.front().size() < 2) {
        fail("default profile points");
        return;
    }
    RECT rc{};
    GetClientRect(st->canvas, &rc);
    int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
    const auto& p0 = st->def.profiles.front()[0];
    const auto& p1 = st->def.profiles.front()[1];
    world_to_screen(rc, st->view, p0.x, p0.z, sx0, sy0);
    world_to_screen(rc, st->view, p1.x, p1.z, sx1, sy1);
    constexpr int pad = 12;
    const int bx0 = (std::min)(sx0, sx1) - pad;
    const int by0 = (std::min)(sy0, sy1) - pad;
    const int bx1 = (std::max)(sx0, sx1) + pad;
    const int by1 = (std::max)(sy0, sy1) + pad;
    SendMessageW(st->canvas, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(bx0, by0));
    SendMessageW(st->canvas, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(bx1, by1));
    SendMessageW(st->canvas, WM_LBUTTONUP, 0, MAKELPARAM(bx1, by1));
    if (st->sel_multi.size() < 2) {
        fail("marquee box selection (need >=2 points, got " + std::to_string(st->sel_multi.size()) + ")");
        return;
    }

    omsi::log::info("spline creator self-test PASS: A+marquee selected " + std::to_string(st->sel_multi.size()) +
                    " points");
}

HBITMAP load_file_hbitmap(CreatorState* st, const std::filesystem::path& path);
void draw_ghost_profiles(HDC dc, CreatorState* st, const RECT& rc);
void draw_path_lanes(HDC dc, CreatorState* st, const RECT& rc);
void draw_preview3d_mesh(HDC dc, CreatorState* st);
bool init_preview_rhi(CreatorState* st);
void shutdown_preview_rhi(CreatorState* st);
void render_preview_rhi(CreatorState* st);
void center_preview_view(CreatorState* st);
bool handle_phase2_command(HWND hwnd, CreatorState* st, int id, int code);
void build_phase2_ui(HWND dlg, CreatorState* st);
void refresh_phase2_ui(CreatorState* st);
void refill_paths(CreatorState* st);
void refill_profiles(CreatorState* st);
void sync_path_edits(CreatorState* st);
void read_path_edits(CreatorState* st);
void read_extra_edits(CreatorState* st);
void load_texture_previews(CreatorState* st);
void free_texture_previews(CreatorState* st);
void sync_texture_flags_ui(CreatorState* st);
void refill_wrap_list(CreatorState* st);
void refill_hp_list(CreatorState* st);
bool handle_parity_command(HWND hwnd, CreatorState* st, int id, int code);
void build_parity_ui(HWND dlg, CreatorState* st);
void draw_tex_preview(HDC dc, CreatorState* st);
void set_uv_from_texture_click(CreatorState* st, int sx, int w);
void remap_materials_after_tex_delete(CreatorState* st, int deleted);
bool texture_index_in_use(const CreatorState* st, int tex_idx);
void read_texture_flags_ui(CreatorState* st);
void import_sli_into(CreatorState* st, const std::filesystem::path& path);
bool run_texture_wrap_dialog(HWND owner, CreatorState* st, int profile_idx, bool create_new);
bool run_path_config_dialog(HWND owner, CreatorState* st, int path_idx, bool create_new);
void sync_tex_slot_ui(CreatorState* st);
void sync_path_field_ui(CreatorState* st);
void sync_profile_fields_ui(CreatorState* st);
void ensure_profile_labels(CreatorState* st);
void add_empty_profile(CreatorState* st);
void add_profile_from_click(CreatorState* st, float x, float z);
void get_pencil_anchor(CreatorState* st, float& x0, float& z0);
void add_profile_strip(CreatorState* st, float x0, float z0, float x1, float z1);
bool run_rename_profile_dialog(HWND owner, CreatorState* st, int idx);
void select_profile(CreatorState* st, int pi);
void select_created_endpoint(CreatorState* st, float click_x, float click_z);
void build_drag_group(CreatorState* st);
void finish_marquee(CreatorState* st);
void sync_sel_pts_for_active_profile(CreatorState* st);
void refresh_profile_selection_ui(CreatorState* st);
void set_active_profile_keep_selection(CreatorState* st, int pi);
std::optional<PtRef> hit_point_any(CreatorState* st, int sx, int sy);
void select_only_ref(CreatorState* st, PtRef r);
void refill_pt_list(CreatorState* st);
void invalidate_all(CreatorState* st);
std::wstring sli_text_for_edit(const std::string& utf8);
void set_marquee_mode(CreatorState* st, bool on);
void set_pencil_mode(CreatorState* st, bool on);
void select_path(CreatorState* st, int path_idx);
std::optional<int> hit_profile_segment(CreatorState* st, int sx, int sy);
std::optional<int> hit_path(CreatorState* st, int sx, int sy);
void sync_uv_edits(CreatorState* st);
void ensure_texture_parallel(spline::SplineDefinition& def);

float snap_val(float v, float step) {
    if (step <= 1.e-6f) return v;
    return std::round(v / step) * step;
}

// Snap solo al mover nodos existentes (drag / G), nunca al abrir .sli ni al crear puntos.
float snap_move_coord(CreatorState* st, float v) {
    if (st->grid_off || st->grid_m <= 1.e-6f) return v;
    return snap_val(v, st->grid_m);
}

spline::SplineDefinition default_street_profile() {
    spline::SplineDefinition def{};
    def.height_profiles.push_back({0.f, 6.f, 0.1f, 0.1f});
    def.textures.push_back("str_asphdrk.bmp");
    def.texture_alpha.push_back(0);
    def.texture_extra.push_back({});
    auto& strip = def.profiles.emplace_back();
    strip.push_back({0.f, 0.1f, 0.f, 0.167f, 0});
    strip.push_back({6.f, 0.1f, 1.f, 0.167f, 0});
    def.paths.push_back({1, 3.f, 0.1f, 3.f, 2});
    def.path_length = 10.f;
    return def;
}

std::vector<spline::ProfilePoint>& active_strip(CreatorState* st) {
    if (st->def.profiles.empty()) st->def.profiles.push_back({});
    st->sel_profile = (std::clamp)(st->sel_profile, 0, static_cast<int>(st->def.profiles.size()) - 1);
    return st->def.profiles[static_cast<std::size_t>(st->sel_profile)];
}

void world_to_screen(const RECT& rc, const View2D& view, float x, float z, int& sx, int& sy) {
    const float cx = (rc.left + rc.right) * 0.5f;
    const float cy = (rc.top + rc.bottom) * 0.5f;
    sx = static_cast<int>(std::lround(cx + (x - view.pan_x) * view.scale));
    sy = static_cast<int>(std::lround(cy - (z - view.pan_z) * view.scale));
}

void screen_to_world(const RECT& rc, const View2D& view, int sx, int sy, float& x, float& z) {
    const float cx = (rc.left + rc.right) * 0.5f;
    const float cy = (rc.top + rc.bottom) * 0.5f;
    x = view.pan_x + (sx - cx) / view.scale;
    z = view.pan_z - (sy - cy) / view.scale;
}

bool is_selected(const CreatorState* st, int i) {
    return std::find(st->sel_pts.begin(), st->sel_pts.end(), i) != st->sel_pts.end();
}

void select_only(CreatorState* st, int i) {
    st->sel_pts.clear();
    st->sel_multi.clear();
    st->sel_profiles = {st->sel_profile};
    if (i >= 0) {
        st->sel_pts.push_back(i);
        st->sel_multi.push_back({st->sel_profile, i});
    }
}

void toggle_select(CreatorState* st, int i) {
    if (auto it = std::find(st->sel_pts.begin(), st->sel_pts.end(), i); it != st->sel_pts.end()) {
        st->sel_pts.erase(it);
        st->sel_multi.erase(std::remove_if(st->sel_multi.begin(), st->sel_multi.end(),
                                           [&](const PtRef& r) { return r.profile == st->sel_profile && r.pt == i; }),
                            st->sel_multi.end());
    } else {
        st->sel_pts.push_back(i);
        st->sel_multi.push_back({st->sel_profile, i});
    }
    st->sel_profiles = {st->sel_profile};
}

int primary_sel(const CreatorState* st) {
    return st->sel_pts.empty() ? -1 : st->sel_pts.back();
}

constexpr float kCoinEps = 0.025f;

void collect_coincident(CreatorState* st, int profile, float x, float z, std::vector<PtRef>& out) {
    if (profile < 0 || static_cast<std::size_t>(profile) >= st->def.profiles.size()) return;
    for (std::size_t i = 0; i < st->def.profiles[static_cast<std::size_t>(profile)].size(); ++i) {
        const auto& p = st->def.profiles[static_cast<std::size_t>(profile)][i];
        if (std::abs(p.x - x) < kCoinEps && std::abs(p.z - z) < kCoinEps) {
            const PtRef r{profile, static_cast<int>(i)};
            const auto dup = std::find_if(out.begin(), out.end(), [&](const PtRef& o) {
                return o.profile == r.profile && o.pt == r.pt;
            });
            if (dup == out.end()) out.push_back(r);
        }
    }
}

void invalidate_tool_btn(CreatorState* st, int id) {
    if (st && st->dlg)
        if (HWND h = GetDlgItem(st->dlg, id)) InvalidateRect(h, nullptr, FALSE);
}

void draw_toggle_tool_btn(const DRAWITEMSTRUCT* dis, HBITMAP icon, const wchar_t* fallback_text) {
    const bool checked = SendMessageW(dis->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF bg = checked ? RGB(204, 228, 251) : RGB(255, 255, 255);
    if (pressed) bg = checked ? RGB(183, 217, 247) : RGB(229, 241, 251);
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dis->hDC, &dis->rcItem, br);
    DeleteObject(br);
    if (checked) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 120, 215));
        HGDIOBJ old_pen = SelectObject(dis->hDC, pen);
        SelectObject(dis->hDC, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right - 1, dis->rcItem.bottom - 1);
        SelectObject(dis->hDC, old_pen);
        DeleteObject(pen);
    }
    const int cw = dis->rcItem.right - dis->rcItem.left;
    const int ch = dis->rcItem.bottom - dis->rcItem.top;
    if (icon) {
        BITMAP bm{};
        GetObjectW(icon, sizeof(bm), &bm);
        const int iw = bm.bmWidth;
        const int ih = std::abs(bm.bmHeight);
        const int x = dis->rcItem.left + (cw - iw) / 2;
        const int y = dis->rcItem.top + (ch - ih) / 2;
        HDC mem = CreateCompatibleDC(dis->hDC);
        SelectObject(mem, icon);
        BitBlt(dis->hDC, x, y, iw, ih, mem, 0, 0, SRCCOPY);
        DeleteDC(mem);
    } else if (fallback_text && *fallback_text) {
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, RGB(20, 20, 20));
        RECT tr = dis->rcItem;
        DrawTextW(dis->hDC, fallback_text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (dis->itemState & ODS_FOCUS) {
        RECT focus = dis->rcItem;
        InflateRect(&focus, -2, -2);
        DrawFocusRect(dis->hDC, &focus);
    }
}

void set_marquee_mode(CreatorState* st, bool on) {
    st->marquee_mode = on;
    if (on) st->pencil_mode = false;
    if (!on) {
        st->marquee_drag = false;
        st->dragging_pt = false;
        if (st->canvas && GetCapture() == st->canvas) ReleaseCapture();
    } else {
        st->path_place_mode = false;
        st->grab_mode = false;
        st->dragging_pt = false;
    }
    if (st->dlg) {
        if (HWND h = GetDlgItem(st->dlg, kMarqueeSel))
            SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        if (HWND h = GetDlgItem(st->dlg, kPencilDraw))
            SendMessageW(h, BM_SETCHECK, BST_UNCHECKED, 0);
        invalidate_tool_btn(st, kMarqueeSel);
        invalidate_tool_btn(st, kPencilDraw);
    }
    invalidate_all(st);
}

void set_pencil_mode(CreatorState* st, bool on) {
    st->pencil_mode = on;
    if (!on) st->pencil_drag = false;
    if (on) {
        st->marquee_mode = false;
        st->marquee_drag = false;
        st->path_place_mode = false;
        st->grab_mode = false;
        st->dragging_pt = false;
        if (st->canvas && GetCapture() == st->canvas) ReleaseCapture();
    }
    if (st->dlg) {
        if (HWND h = GetDlgItem(st->dlg, kMarqueeSel))
            SendMessageW(h, BM_SETCHECK, BST_UNCHECKED, 0);
        if (HWND h = GetDlgItem(st->dlg, kPencilDraw))
            SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        invalidate_tool_btn(st, kMarqueeSel);
        invalidate_tool_btn(st, kPencilDraw);
    }
    invalidate_all(st);
}

void select_path(CreatorState* st, int path_idx) {
    if (path_idx < 0 || static_cast<std::size_t>(path_idx) >= st->def.paths.size()) return;
    st->sel_path = path_idx;
    st->sel_paths = {path_idx};
    if (st->path_list) {
        st->suppress_commands = true;
        SendMessageW(st->path_list, LB_SETCURSEL, st->sel_path, 0);
        st->suppress_commands = false;
    }
    sync_path_edits(st);
    invalidate_all(st);
}

void select_profile(CreatorState* st, int pi) {
    if (st->def.profiles.empty()) return;
    st->sel_profile = (std::clamp)(pi, 0, static_cast<int>(st->def.profiles.size()) - 1);
    st->sel_multi.clear();
    st->sel_profiles = {st->sel_profile};
    st->sel_pts.clear();
    if (st->wrap_list) {
        st->suppress_commands = true;
        SendMessageW(st->wrap_list, LB_SETCURSEL, st->sel_profile, 0);
        st->suppress_commands = false;
    }
    refill_pt_list(st);
    sync_uv_edits(st);
    invalidate_all(st);
}

void select_only_ref(CreatorState* st, PtRef r) {
    if (r.profile < 0 || static_cast<std::size_t>(r.profile) >= st->def.profiles.size()) return;
    if (r.pt < 0 || static_cast<std::size_t>(r.pt) >= st->def.profiles[static_cast<std::size_t>(r.profile)].size())
        return;
    st->sel_profile = r.profile;
    st->sel_profiles = {r.profile};
    st->sel_pts = {r.pt};
    st->sel_multi = {r};
    if (st->wrap_list) {
        st->suppress_commands = true;
        SendMessageW(st->wrap_list, LB_SETCURSEL, st->sel_profile, 0);
        st->suppress_commands = false;
    }
    refill_pt_list(st);
    sync_uv_edits(st);
    invalidate_all(st);
}

void toggle_select_ref(CreatorState* st, PtRef r) {
    if (r.profile < 0 || static_cast<std::size_t>(r.profile) >= st->def.profiles.size()) return;
    if (r.pt < 0 || static_cast<std::size_t>(r.pt) >= st->def.profiles[static_cast<std::size_t>(r.profile)].size())
        return;
    const auto it = std::find_if(st->sel_multi.begin(), st->sel_multi.end(),
                                   [&](const PtRef& o) { return o.profile == r.profile && o.pt == r.pt; });
    if (it != st->sel_multi.end())
        st->sel_multi.erase(it);
    else
        st->sel_multi.push_back(r);
    st->sel_profiles.clear();
    for (const auto& ref : st->sel_multi) {
        if (std::find(st->sel_profiles.begin(), st->sel_profiles.end(), ref.profile) == st->sel_profiles.end())
            st->sel_profiles.push_back(ref.profile);
    }
    st->sel_profile = r.profile;
    st->sel_pts.clear();
    for (const auto& ref : st->sel_multi)
        if (ref.profile == st->sel_profile) st->sel_pts.push_back(ref.pt);
    if (st->wrap_list) SendMessageW(st->wrap_list, LB_SETCURSEL, st->sel_profile, 0);
    refill_pt_list(st);
    sync_uv_edits(st);
}

void select_created_endpoint(CreatorState* st, float click_x, float click_z) {
    auto& strip = active_strip(st);
    int best = 0;
    float best_d = 1.e30f;
    for (std::size_t i = 0; i < strip.size(); ++i) {
        const float dx = strip[i].x - click_x;
        const float dz = strip[i].z - click_z;
        const float d = dx * dx + dz * dz;
        if (d < best_d) {
            best_d = d;
            best = static_cast<int>(i);
        }
    }
    st->sel_multi.clear();
    st->sel_profiles = {st->sel_profile};
    st->sel_pts = {best};
    st->sel_multi.push_back({st->sel_profile, best});
    if (st->wrap_list) SendMessageW(st->wrap_list, LB_SETCURSEL, st->sel_profile, 0);
    refill_pt_list(st);
}

void build_drag_group(CreatorState* st) {
    st->drag_group.clear();
    std::vector<PtRef> seeds;
    if (!st->sel_multi.empty())
        seeds = st->sel_multi;
    else
        for (int i : st->sel_pts) seeds.push_back({st->sel_profile, i});
    std::vector<PtRef> expanded;
    for (const auto& s : seeds) {
        if (s.profile < 0 || static_cast<std::size_t>(s.profile) >= st->def.profiles.size()) continue;
        if (s.pt < 0 || static_cast<std::size_t>(s.pt) >= st->def.profiles[static_cast<std::size_t>(s.profile)].size())
            continue;
        const auto& p = st->def.profiles[static_cast<std::size_t>(s.profile)][static_cast<std::size_t>(s.pt)];
        collect_coincident(st, s.profile, p.x, p.z, expanded);
    }
    for (const auto& r : expanded) {
        if (r.profile < 0 || static_cast<std::size_t>(r.profile) >= st->def.profiles.size()) continue;
        if (r.pt < 0 || static_cast<std::size_t>(r.pt) >= st->def.profiles[static_cast<std::size_t>(r.profile)].size())
            continue;
        const auto& p = st->def.profiles[static_cast<std::size_t>(r.profile)][static_cast<std::size_t>(r.pt)];
        st->drag_group.push_back({r.profile, r.pt, p.x, p.z});
    }
}

void finish_marquee(CreatorState* st) {
    RECT rc{};
    GetClientRect(st->canvas, &rc);
    const int minx = (std::min)(st->mar_x0, st->mar_x1);
    const int maxx = (std::max)(st->mar_x0, st->mar_x1);
    const int miny = (std::min)(st->mar_y0, st->mar_y1);
    const int maxy = (std::max)(st->mar_y0, st->mar_y1);

    if (maxx - minx < 4 && maxy - miny < 4) {
        const int cx = (minx + maxx) / 2;
        const int cy = (miny + maxy) / 2;
        if (const auto hit = hit_point_any(st, cx, cy)) {
            select_only_ref(st, *hit);
        } else if (const auto path = hit_path(st, cx, cy)) {
            select_path(st, *path);
            st->sel_multi.clear();
            st->sel_pts.clear();
            refill_pt_list(st);
        } else if (const auto prof = hit_profile_segment(st, cx, cy)) {
            select_profile(st, *prof);
            st->sel_paths.clear();
        } else {
            st->sel_multi.clear();
            st->sel_pts.clear();
            st->sel_paths.clear();
            refill_pt_list(st);
        }
        return;
    }

    st->sel_multi.clear();
    st->sel_profiles.clear();
    st->sel_pts.clear();
    st->sel_paths.clear();
    for (std::size_t pi = 0; pi < st->def.profiles.size(); ++pi) {
        bool profile_hit = false;
        for (std::size_t i = 0; i < st->def.profiles[pi].size(); ++i) {
            int sx = 0, sy = 0;
            const auto& p = st->def.profiles[pi][i];
            world_to_screen(rc, st->view, p.x, p.z, sx, sy);
            if (sx >= minx && sx <= maxx && sy >= miny && sy <= maxy) {
                st->sel_multi.push_back({static_cast<int>(pi), static_cast<int>(i)});
                profile_hit = true;
            }
        }
        if (profile_hit) st->sel_profiles.push_back(static_cast<int>(pi));
    }
    for (std::size_t i = 0; i < st->def.paths.size(); ++i) {
        const auto& p = st->def.paths[i];
        const float half = p.width * 0.5f;
        int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
        world_to_screen(rc, st->view, p.lateral - half, p.height, sx0, sy0);
        world_to_screen(rc, st->view, p.lateral + half, p.height, sx1, sy1);
        const int bx0 = (std::min)(sx0, sx1);
        const int bx1 = (std::max)(sx0, sx1);
        const int by0 = (std::min)(sy0, sy1);
        const int by1 = (std::max)(sy0, sy1);
        const bool in_box = (bx1 >= minx && bx0 <= maxx && by1 >= miny && by0 <= maxy) ||
                            (sx0 >= minx && sx0 <= maxx && sy0 >= miny && sy0 <= maxy) ||
                            (sx1 >= minx && sx1 <= maxx && sy1 >= miny && sy1 <= maxy);
        if (in_box) st->sel_paths.push_back(static_cast<int>(i));
    }
    if (!st->sel_profiles.empty()) {
        st->sel_profile = st->sel_profiles.front();
        refresh_profile_selection_ui(st);
    }
    if (!st->sel_paths.empty()) {
        st->sel_path = st->sel_paths.front();
        if (st->path_list) {
            st->suppress_commands = true;
            SendMessageW(st->path_list, LB_SETCURSEL, st->sel_path, 0);
            st->suppress_commands = false;
        }
        sync_path_edits(st);
        invalidate_all(st);
    }
}

void push_undo(CreatorState* st) {
    st->undo_stack.push_back(st->def);
    if (st->undo_stack.size() > kMaxUndo) st->undo_stack.erase(st->undo_stack.begin());
}

void do_undo(CreatorState* st) {
    if (st->undo_stack.empty()) return;
    st->def = std::move(st->undo_stack.back());
    st->undo_stack.pop_back();
    st->sel_pts.clear();
    ensure_profile_labels(st);
}

void refresh_sli_preview(CreatorState* st) {
    if (!st->sli_preview) return;
    SetWindowTextW(st->sli_preview, sli_text_for_edit(spline::format_sli(st->def)).c_str());
}

void refresh_warnings(CreatorState* st) {
    if (!st->warn_box) return;
    SendMessageW(st->warn_box, LB_RESETCONTENT, 0, 0);
    for (const auto& w : spline::validate_sli(st->def)) {
        const auto ws = utf8_to_wide(w);
        SendMessageW(st->warn_box, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ws.c_str()));
    }
    refresh_sli_preview(st);
    refill_pt_list(st);
}

void sync_pt_list_sel(CreatorState* st) {
    (void)st;
}

std::wstring sli_text_for_edit(const std::string& utf8) {
    const std::wstring w = utf8_to_wide(utf8);
    std::wstring crlf;
    crlf.reserve(w.size() + 64);
    for (const wchar_t c : w) {
        if (c == L'\n') crlf.push_back(L'\r');
        crlf.push_back(c);
    }
    return crlf;
}

void refill_pt_list(CreatorState* st) {
    if (!st->pt_list) return;
    SetWindowTextW(st->pt_list, sli_text_for_edit(spline::format_sli(st->def)).c_str());
}

void apply_texture_to_profile(CreatorState* st) {
    if (!st || st->def.profiles.empty() || st->def.textures.empty()) return;
    push_undo(st);
    auto& strip = active_strip(st);
    const int mat =
        (std::clamp)(st->sel_tex, 0, static_cast<int>(st->def.textures.size()) - 1);
    for (auto& pt : strip) pt.material_id = mat;
    ensure_texture_parallel(st->def);
    sync_profile_fields_ui(st);
    refill_pt_list(st);
    st->preview_mesh_dirty = true;
    invalidate_all(st);
}

void free_thumbs(CreatorState* st) {
    for (HBITMAP b : st->tex_thumbs)
        if (b) DeleteObject(b);
    st->tex_thumbs.clear();
}

HBITMAP make_thumb(HBITMAP src, int sw, int sh) {
    if (!src || sw <= 0 || sh <= 0) return nullptr;
    HDC screen = GetDC(nullptr);
    HDC src_dc = CreateCompatibleDC(screen);
    HDC dst_dc = CreateCompatibleDC(screen);
    HBITMAP thumb = CreateCompatibleBitmap(screen, kThumbPx, kThumbPx);
    SelectObject(src_dc, src);
    SelectObject(dst_dc, thumb);
    SetStretchBltMode(dst_dc, HALFTONE);
    StretchBlt(dst_dc, 0, 0, kThumbPx, kThumbPx, src_dc, 0, 0, sw, sh, SRCCOPY);
    DeleteDC(src_dc);
    DeleteDC(dst_dc);
    ReleaseDC(nullptr, screen);
    return thumb;
}

std::optional<std::filesystem::path> resolve_tex_file(CreatorState* st, const std::string& name) {
    if (const auto it = st->tex_sources.find(name); it != st->tex_sources.end()) return it->second;
    std::filesystem::path base = st->def.sli_path;
    if (base.empty() && !st->save_path.empty()) base = st->save_path;
    if (base.empty() && !st->omsi_root.empty()) base = st->omsi_root / L"Splines" / L"dummy.sli";
    if (base.empty()) base = L"dummy.sli";
    return gfx::dx12::resolve_texture_path(base, name, st->omsi_root);
}

void refill_textures(CreatorState* st) {
    free_thumbs(st);
    if (st->tex_list) SendMessageW(st->tex_list, LB_RESETCONTENT, 0, 0);
    for (const auto& t : st->def.textures) {
        if (st->tex_list)
            SendMessageW(st->tex_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(utf8_to_wide(t).c_str()));
        HBITMAP thumb = nullptr;
        if (const auto path = resolve_tex_file(st, t)) {
            if (HBITMAP full = load_file_hbitmap(st, *path)) {
                BITMAP bm{};
                GetObjectW(full, sizeof(bm), &bm);
                thumb = make_thumb(full, bm.bmWidth, std::abs(bm.bmHeight));
                DeleteObject(full);
            }
        }
        st->tex_thumbs.push_back(thumb);
    }
    if (!st->def.textures.empty()) {
        st->sel_tex = (std::clamp)(st->sel_tex, 0, static_cast<int>(st->def.textures.size()) - 1);
        if (st->tex_list) SendMessageW(st->tex_list, LB_SETCURSEL, st->sel_tex, 0);
    }
    sync_profile_fields_ui(st);
    if (st->tex_list) InvalidateRect(st->tex_list, nullptr, FALSE);
}

void invalidate_all(CreatorState* st) {
    st->preview_mesh_dirty = true;
    if (st->canvas) InvalidateRect(st->canvas, nullptr, FALSE);
    if (st->preview3d) {
        if (st->preview_rhi_ok)
            render_preview_rhi(st);
        else
            InvalidateRect(st->preview3d, nullptr, FALSE);
    }
    if (st->tex_preview) InvalidateRect(st->tex_preview, nullptr, FALSE);
    refresh_warnings(st);
#ifdef OMSI_ULTRALIGHT_UI
    sync_spline_creator_sidebar(st);
    sync_spline_creator_chrome(st);
#endif
}

void profile_bounds(CreatorState* st, float& minx, float& maxx, float& minz, float& maxz) {
    minx = maxx = minz = maxz = 0.f;
    bool any = false;
    for (const auto& pt : active_strip(st)) {
        if (!any) {
            minx = maxx = pt.x;
            minz = maxz = pt.z;
            any = true;
        } else {
            minx = std::min(minx, pt.x);
            maxx = std::max(maxx, pt.x);
            minz = std::min(minz, pt.z);
            maxz = std::max(maxz, pt.z);
        }
    }
    if (!any) {
        minx = -1.f;
        maxx = 7.f;
        minz = -0.5f;
        maxz = 1.f;
    }
}

void center_view(CreatorState* st) {
    float minx = 0.f, maxx = 0.f, minz = 0.f, maxz = 0.f;
    profile_bounds(st, minx, maxx, minz, maxz);
    st->view.pan_x = (minx + maxx) * 0.5f;
    st->view.pan_z = (minz + maxz) * 0.5f;
    RECT rc{};
    GetClientRect(st->canvas, &rc);
    const float w = std::max(maxx - minx, 0.5f);
    const float h = std::max(maxz - minz, 0.5f);
    st->view.scale =
        std::min(static_cast<float>(std::max(1L, rc.right - rc.left - 48)) / w,
                 static_cast<float>(std::max(1L, rc.bottom - rc.top - 48)) / h);
}

void sync_hp_edits(HWND dlg, CreatorState* st) {
    if (st->def.height_profiles.empty()) st->def.height_profiles.push_back({});
    st->sel_hp = (std::clamp)(st->sel_hp, 0, static_cast<int>(st->def.height_profiles.size()) - 1);
    const auto& hp = st->def.height_profiles[static_cast<std::size_t>(st->sel_hp)];
    st->suppress_commands = true;
    auto setf = [&](int id, float v) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.3f", static_cast<double>(v));
        SetWindowTextW(GetDlgItem(dlg, id), buf);
    };
    setf(kHpL, hp.left);
    setf(kHpR, hp.right);
    setf(kHpH, hp.height);
    setf(kHpS, hp.skew);
    st->suppress_commands = false;
}

void read_hp_edits(HWND dlg, CreatorState* st) {
    if (st->def.height_profiles.empty()) st->def.height_profiles.push_back({});
    st->sel_hp = (std::clamp)(st->sel_hp, 0, static_cast<int>(st->def.height_profiles.size()) - 1);
    auto& hp = st->def.height_profiles[static_cast<std::size_t>(st->sel_hp)];
    auto getf = [&](int id, float fb) {
        wchar_t buf[32]{};
        GetWindowTextW(GetDlgItem(dlg, id), buf, 32);
        try {
            return std::stof(std::wstring(buf));
        } catch (...) {
            return fb;
        }
    };
    hp.left = getf(kHpL, hp.left);
    hp.right = getf(kHpR, hp.right);
    hp.height = getf(kHpH, hp.height);
    hp.skew = getf(kHpS, hp.skew);
}

void sync_uv_edits(CreatorState* st) {
    if (!st->dlg) return;
    const int i = primary_sel(st);
    const auto& strip = active_strip(st);
    st->suppress_commands = true;
    if (i < 0 || static_cast<std::size_t>(i) >= strip.size()) {
        SetWindowTextW(GetDlgItem(st->dlg, kUvStart), L"");
        SetWindowTextW(GetDlgItem(st->dlg, kUvEnd), L"");
        SetWindowTextW(GetDlgItem(st->dlg, kTiling), L"");
        st->suppress_commands = false;
        return;
    }
    wchar_t buf[32];
    if (i > 0)
        swprintf_s(buf, L"%.3f", static_cast<double>(strip[static_cast<std::size_t>(i - 1)].uv_x));
    else
        swprintf_s(buf, L"%.3f", static_cast<double>(strip[0].uv_x));
    SetWindowTextW(GetDlgItem(st->dlg, kUvStart), buf);
    if (static_cast<std::size_t>(i + 1) < strip.size())
        swprintf_s(buf, L"%.3f", static_cast<double>(strip[static_cast<std::size_t>(i + 1)].uv_x));
    else
        swprintf_s(buf, L"%.3f", static_cast<double>(strip[static_cast<std::size_t>(i)].uv_x));
    SetWindowTextW(GetDlgItem(st->dlg, kUvEnd), buf);
    swprintf_s(buf, L"%.3f", static_cast<double>(strip[static_cast<std::size_t>(i)].y_tiling));
    SetWindowTextW(GetDlgItem(st->dlg, kTiling), buf);
    st->suppress_commands = false;
}

void apply_uv_from_edits(CreatorState* st) {
    const int i = primary_sel(st);
    if (!st->dlg || i < 0) return;
    auto& strip = active_strip(st);
    if (static_cast<std::size_t>(i) >= strip.size()) return;
    auto getf = [&](int id, float fb) {
        wchar_t buf[32]{};
        GetWindowTextW(GetDlgItem(st->dlg, id), buf, 32);
        try {
            return std::stof(std::wstring(buf));
        } catch (...) {
            return fb;
        }
    };
    const float uv0 = getf(kUvStart, strip[static_cast<std::size_t>(i)].uv_x);
    const float uv1 = getf(kUvEnd, strip[static_cast<std::size_t>(i)].uv_x);
    strip[static_cast<std::size_t>(i)].y_tiling = getf(kTiling, strip[static_cast<std::size_t>(i)].y_tiling);
    if (i > 0) strip[static_cast<std::size_t>(i - 1)].uv_x = uv0;
    else strip[0].uv_x = uv0;
    if (static_cast<std::size_t>(i + 1) < strip.size()) strip[static_cast<std::size_t>(i + 1)].uv_x = uv1;
    else strip[static_cast<std::size_t>(i)].uv_x = uv1;
}

int hit_point(CreatorState* st, int sx, int sy) {
    if (const auto hit = hit_point_any(st, sx, sy)) {
        if (hit->profile == st->sel_profile) return hit->pt;
    }
    return -1;
}

std::optional<PtRef> hit_point_any(CreatorState* st, int sx, int sy) {
    RECT rc{};
    GetClientRect(st->canvas, &rc);
    for (int pi = static_cast<int>(st->def.profiles.size()) - 1; pi >= 0; --pi) {
        const auto& strip = st->def.profiles[static_cast<std::size_t>(pi)];
        for (int i = static_cast<int>(strip.size()) - 1; i >= 0; --i) {
            int px = 0, py = 0;
            world_to_screen(rc, st->view, strip[static_cast<std::size_t>(i)].x, strip[static_cast<std::size_t>(i)].z,
                            px, py);
            if (std::abs(sx - px) <= kHitPx && std::abs(sy - py) <= kHitPx)
                return PtRef{pi, i};
        }
    }
    return std::nullopt;
}

float dist_sq_point_seg(float px, float py, float ax, float ay, float bx, float by) {
    const float abx = bx - ax;
    const float aby = by - ay;
    const float apx = px - ax;
    const float apy = py - ay;
    const float ab2 = abx * abx + aby * aby;
    if (ab2 < 1.e-6f) {
        const float dx = px - ax;
        const float dy = py - ay;
        return dx * dx + dy * dy;
    }
    float t = (apx * abx + apy * aby) / ab2;
    t = (std::clamp)(t, 0.f, 1.f);
    const float cx = ax + t * abx;
    const float cy = ay + t * aby;
    const float dx = px - cx;
    const float dy = py - cy;
    return dx * dx + dy * dy;
}

std::optional<int> hit_profile_segment(CreatorState* st, int sx, int sy) {
    RECT rc{};
    GetClientRect(st->canvas, &rc);
    const float thr2 = static_cast<float>(kHitSegPx * kHitSegPx);
    int best_pi = -1;
    float best_d = thr2;
    for (int pi = static_cast<int>(st->def.profiles.size()) - 1; pi >= 0; --pi) {
        const auto& strip = st->def.profiles[static_cast<std::size_t>(pi)];
        for (std::size_t i = 1; i < strip.size(); ++i) {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            world_to_screen(rc, st->view, strip[i - 1].x, strip[i - 1].z, x0, y0);
            world_to_screen(rc, st->view, strip[i].x, strip[i].z, x1, y1);
            const float d = dist_sq_point_seg(static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(x0),
                                              static_cast<float>(y0), static_cast<float>(x1), static_cast<float>(y1));
            if (d <= best_d) {
                best_d = d;
                best_pi = pi;
            }
        }
    }
    if (best_pi >= 0) return best_pi;
    return std::nullopt;
}

std::optional<int> hit_path(CreatorState* st, int sx, int sy) {
    if (!st->draw_paths || st->def.paths.empty()) return std::nullopt;
    RECT rc{};
    GetClientRect(st->canvas, &rc);
    const float thr2 = static_cast<float>(kHitSegPx * kHitSegPx);
    int best = -1;
    float best_d = thr2;
    for (int i = static_cast<int>(st->def.paths.size()) - 1; i >= 0; --i) {
        const auto& p = st->def.paths[static_cast<std::size_t>(i)];
        const float half = p.width * 0.5f;
        int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
        world_to_screen(rc, st->view, p.lateral - half, p.height, sx0, sy0);
        world_to_screen(rc, st->view, p.lateral + half, p.height, sx1, sy1);
        const float d = dist_sq_point_seg(static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(sx0),
                                          static_cast<float>(sy0), static_cast<float>(sx1), static_cast<float>(sy1));
        if (d <= best_d) {
            best_d = d;
            best = i;
        }
    }
    if (best >= 0) return best;
    return std::nullopt;
}

HBITMAP scale_hbitmap(HBITMAP src, int w, int h) {
    if (!src || w <= 0 || h <= 0) return nullptr;
    BITMAP bm{};
    GetObjectW(src, sizeof(bm), &bm);
    HDC screen = GetDC(nullptr);
    HDC src_dc = CreateCompatibleDC(screen);
    HDC dst_dc = CreateCompatibleDC(screen);
    SelectObject(src_dc, src);
    HBITMAP dst = CreateCompatibleBitmap(screen, w, h);
    SelectObject(dst_dc, dst);
    RECT fill{0, 0, w, h};
    FillRect(dst_dc, &fill, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetStretchBltMode(dst_dc, HALFTONE);
    StretchBlt(dst_dc, 0, 0, w, h, src_dc, 0, 0, bm.bmWidth, std::abs(bm.bmHeight), SRCCOPY);
    DeleteDC(src_dc);
    DeleteDC(dst_dc);
    ReleaseDC(nullptr, screen);
    return dst;
}

HBITMAP rgba_white_bg_hbitmap(const gfx::dx12::CpuRgbaImage& img) {
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
    const std::size_t px = img.width * img.height;
    for (std::size_t i = 0; i < px; ++i) {
        const std::size_t si = i * 4;
        const std::size_t di = i * 4;
        const float a = img.rgba[si + 3] / 255.f;
        const float ia = 1.f - a;
        dst[di + 0] = static_cast<std::uint8_t>(std::lround(img.rgba[si + 2] * a + 255.f * ia));
        dst[di + 1] = static_cast<std::uint8_t>(std::lround(img.rgba[si + 1] * a + 255.f * ia));
        dst[di + 2] = static_cast<std::uint8_t>(std::lround(img.rgba[si + 0] * a + 255.f * ia));
        dst[di + 3] = 255;
    }
    return bmp;
}

HBITMAP load_tool_icon_hbitmap(const std::filesystem::path& path, int w, int h) {
    const auto img = gfx::dx12::load_image_cpu_rgba(path);
    if (!img) return nullptr;
    HBITMAP full = rgba_white_bg_hbitmap(*img);
    if (!full) return nullptr;
    HBITMAP scaled = scale_hbitmap(full, w, h);
    DeleteObject(full);
    return scaled;
}

std::filesystem::path resolve_tool_icon(const wchar_t* asset_name, const wchar_t* user_path = nullptr) {
    if (user_path && *user_path && std::filesystem::exists(user_path)) return user_path;
    wchar_t mod[MAX_PATH]{};
    GetModuleFileNameW(nullptr, mod, MAX_PATH);
    const auto exe = std::filesystem::path(mod).parent_path();
    const auto src =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / L"assets" / asset_name;
    const std::array paths = {exe / L"assets" / asset_name, std::filesystem::path(L"assets") / asset_name, src};
    for (const auto& p : paths)
        if (std::filesystem::exists(p)) return p;
    return user_path && *user_path ? std::filesystem::path(user_path) : paths[0];
}

HWND mk_icon_btn(HWND dlg, int id, HBITMAP& bmp_store, const std::filesystem::path& path, int x, int y, int w, int h,
                 const wchar_t* fallback, bool toggle = false) {
    bmp_store = load_tool_icon_hbitmap(path, w - 2, h - 2);
    const DWORD style = toggle ? (BS_OWNERDRAW | BS_PUSHLIKE) : (BS_PUSHBUTTON | (bmp_store ? BS_BITMAP : 0));
    HWND btn = CreateWindowW(L"BUTTON", toggle || !bmp_store ? fallback : L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, x, y, w, h, dlg,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (!toggle && bmp_store) SendMessageW(btn, BM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(bmp_store));
    return btn;
}

HWND mk_text_toggle_btn(HWND dlg, int id, const wchar_t* label, int x, int y, int w, int h) {
    return CreateWindowW(L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_PUSHLIKE, x, y, w, h,
                         dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

void finish_pencil_stroke(CreatorState* st) {
    if (!st) return;
    const float dx = st->pencil_x1 - st->pencil_x0;
    const float dz = st->pencil_z1 - st->pencil_z0;
    if (dx * dx + dz * dz <= 1.e-8f) return;
    push_undo(st);
    add_profile_strip(st, st->pencil_x0, st->pencil_z0, st->pencil_x1, st->pencil_z1);
    refill_wrap_list(st);
    select_created_endpoint(st, st->pencil_x1, st->pencil_z1);
    sync_uv_edits(st);
}

void free_tool_bitmaps(CreatorState* st) {
    if (!st) return;
    auto free_one = [](HBITMAP& b) {
        if (b) {
            DeleteObject(b);
            b = nullptr;
        }
    };
    free_one(st->pencil_bmp);
    free_one(st->espejar_bmp);
    free_one(st->flip_bmp);
    free_one(st->del_bmp);
    free_one(st->marquee_bmp);
    free_one(st->draw_tex_bmp);
    free_one(st->draw_path_bmp);
}

void delete_selection(CreatorState* st) {
    if (st->sel_pts.empty()) return;
    auto& strip = active_strip(st);
    if (strip.size() - st->sel_pts.size() < 2) return;
    push_undo(st);
    std::vector<int> del = st->sel_pts;
    std::sort(del.begin(), del.end(), std::greater<int>());
    for (int i : del) {
        if (i >= 0 && static_cast<std::size_t>(i) < strip.size()) strip.erase(strip.begin() + i);
    }
    st->sel_pts.clear();
    refill_pt_list(st);
    sync_uv_edits(st);
    invalidate_all(st);
}

void copy_selection(CreatorState* st) {
    if (!st->sel_profiles.empty()) {
        st->clip_profiles.clear();
        st->clip_pts.clear();
        for (int pi : st->sel_profiles) {
            if (pi >= 0 && static_cast<std::size_t>(pi) < st->def.profiles.size())
                st->clip_profiles.push_back(st->def.profiles[static_cast<std::size_t>(pi)]);
        }
        return;
    }
    st->clip_profiles.clear();
    st->clip_pts.clear();
    const auto& strip = active_strip(st);
    std::vector<int> ids = st->sel_pts;
    std::sort(ids.begin(), ids.end());
    for (int i : ids)
        if (i >= 0 && static_cast<std::size_t>(i) < strip.size()) st->clip_pts.push_back(strip[static_cast<std::size_t>(i)]);
}

void paste_clipboard(CreatorState* st) {
    if (!st->clip_profiles.empty()) {
        push_undo(st);
        st->sel_profiles.clear();
        for (const auto& prof : st->clip_profiles) {
            auto np = prof;
            for (auto& pt : np) pt.x += 0.3f;
            st->def.profiles.push_back(std::move(np));
            st->def.profile_comments.push_back({});
            st->sel_profiles.push_back(static_cast<int>(st->def.profiles.size()) - 1);
        }
        ensure_profile_labels(st);
        for (int pi : st->sel_profiles) {
            if (pi >= 0 && static_cast<std::size_t>(pi) < st->profile_labels.size()) {
                st->profile_labels[static_cast<std::size_t>(pi)] =
                    L"Perfil " + std::to_wstring(static_cast<std::size_t>(pi) + 1);
            }
        }
        select_profile(st, st->sel_profiles.back());
        refill_wrap_list(st);
        invalidate_all(st);
        return;
    }
    if (st->clip_pts.empty()) return;
    push_undo(st);
    auto& strip = active_strip(st);
    st->sel_pts.clear();
    for (const auto& pt : st->clip_pts) {
        auto p = pt;
        p.x += 0.2f;
        strip.push_back(p);
        st->sel_pts.push_back(static_cast<int>(strip.size()) - 1);
    }
    std::sort(strip.begin(), strip.end(),
              [](const spline::ProfilePoint& a, const spline::ProfilePoint& b) { return a.x < b.x; });
    st->sel_pts.clear();
    for (std::size_t i = 0; i < strip.size(); ++i) {
        for (const auto& c : st->clip_pts)
            if (std::abs(strip[i].x - (c.x + 0.2f)) < 1.e-3f) st->sel_pts.push_back(static_cast<int>(i));
    }
    refill_pt_list(st);
    invalidate_all(st);
}

void start_grab(CreatorState* st) {
    if (st->sel_pts.empty() && st->sel_multi.empty()) return;
    build_drag_group(st);
    if (st->drag_group.empty()) return;
    st->grab_mode = true;
    st->grab_ax = st->grab_az = 0.f;
    st->grab_origin_set = false;
    st->drag_undo_done = false;
    st->grab_x0.clear();
    st->grab_z0.clear();
    for (const auto& d : st->drag_group) {
        st->grab_x0.push_back(d.x0);
        st->grab_z0.push_back(d.z0);
    }
    SetCapture(st->canvas);
}

void cancel_grab(CreatorState* st) {
    if (!st->grab_mode) return;
    do_undo(st);
    st->grab_mode = false;
    ReleaseCapture();
    invalidate_all(st);
}

void confirm_grab(CreatorState* st) {
    st->grab_mode = false;
    ReleaseCapture();
    refill_pt_list(st);
    invalidate_all(st);
}

void move_selection(CreatorState* st, float dx, float dz) {
    auto& strip = active_strip(st);
    for (int i : st->sel_pts) {
        if (i < 0 || static_cast<std::size_t>(i) >= strip.size()) continue;
        strip[static_cast<std::size_t>(i)].x = snap_move_coord(st, strip[static_cast<std::size_t>(i)].x + dx);
        strip[static_cast<std::size_t>(i)].z = snap_move_coord(st, strip[static_cast<std::size_t>(i)].z + dz);
    }
}

void add_texture_name(CreatorState* st, const std::string& name, const std::filesystem::path& src = {}) {
    if (name.empty()) return;
    for (const auto& t : st->def.textures)
        if (t == name) return;
    push_undo(st);
    st->def.textures.push_back(name);
    st->def.texture_alpha.push_back(0);
    st->def.texture_extra.push_back({});
    if (!src.empty()) st->tex_sources[name] = src;
    st->preview_tex_cache.erase(name);
    refill_textures(st);
    st->sel_tex = static_cast<int>(st->def.textures.size()) - 1;
    if (!st->def.profiles.empty()) {
        auto& strip = active_strip(st);
        for (auto& pt : strip) pt.material_id = st->sel_tex;
        refill_wrap_list(st);
    }
    load_texture_previews(st);
    sync_texture_flags_ui(st);
    if (st->preview_rhi && !src.empty()) st->preview_rhi->invalidate_texture_path(src);
    invalidate_all(st);
}

void add_texture_from_path(CreatorState* st, const std::filesystem::path& path) {
    if (path.empty()) return;
    add_texture_name(st, path.filename().string(), path);
    if (st->dlg) SetWindowTextW(GetDlgItem(st->dlg, kTexEdit), path.filename().c_str());
}

std::optional<std::filesystem::path> browse_texture_file(CreatorState* st, HWND owner) {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Texturas (*.bmp;*.tga;*.dds;*.png;*.jpg)\0*.bmp;*.tga;*.dds;*.png;*.jpg\0"
                        L"Bitmap (*.bmp)\0*.bmp\0"
                        L"Todos (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    static std::wstring init_dir;
    if (!st->omsi_root.empty()) {
        const auto tex = st->omsi_root / L"texture";
        const auto spl = st->omsi_root / L"Splines";
        init_dir = (std::filesystem::exists(tex) ? tex : spl).wstring();
        ofn.lpstrInitialDir = init_dir.c_str();
    }
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return std::filesystem::path(file);
}

void paste_texture_clipboard(CreatorState* st) {
    if (!OpenClipboard(st->dlg)) return;
    if (const HANDLE data = GetClipboardData(CF_HDROP)) {
        const HDROP drop = static_cast<HDROP>(data);
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t file[MAX_PATH]{};
            if (DragQueryFileW(drop, i, file, MAX_PATH)) add_texture_from_path(st, file);
        }
    } else if (const HANDLE text_data = GetClipboardData(CF_UNICODETEXT)) {
        const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(text_data));
        if (text) {
            std::wstring w(text);
            GlobalUnlock(text_data);
            const auto p = std::filesystem::path(w);
            if (p.has_extension()) add_texture_from_path(st, p);
            else add_texture_name(st, wide_to_utf8(w));
        }
    }
    CloseClipboard();
}

std::vector<int> mirror_invert_targets(const CreatorState* st) {
    std::vector<int> out;
    auto add_pi = [&](int pi) {
        if (pi < 0 || static_cast<std::size_t>(pi) >= st->def.profiles.size()) return;
        if (std::find(out.begin(), out.end(), pi) == out.end()) out.push_back(pi);
    };
    for (const auto& r : st->sel_multi) add_pi(r.profile);
    if (!out.empty()) return out;
    for (int pi : st->sel_profiles) add_pi(pi);
    if (!out.empty()) return out;
    return {st->sel_profile};
}

void sync_sel_pts_for_active_profile(CreatorState* st) {
    st->sel_pts.clear();
    for (const auto& r : st->sel_multi)
        if (r.profile == st->sel_profile) st->sel_pts.push_back(r.pt);
}

void refresh_profile_selection_ui(CreatorState* st) {
    st->sel_profiles.clear();
    for (const auto& r : st->sel_multi) {
        if (std::find(st->sel_profiles.begin(), st->sel_profiles.end(), r.profile) == st->sel_profiles.end())
            st->sel_profiles.push_back(r.profile);
    }
    if (st->wrap_list) {
        st->suppress_commands = true;
        SendMessageW(st->wrap_list, LB_SETCURSEL, st->sel_profile, 0);
        st->suppress_commands = false;
    }
    sync_sel_pts_for_active_profile(st);
    refill_pt_list(st);
    sync_uv_edits(st);
    invalidate_all(st);
}

void set_active_profile_keep_selection(CreatorState* st, int pi) {
    if (st->def.profiles.empty()) return;
    st->sel_profile = (std::clamp)(pi, 0, static_cast<int>(st->def.profiles.size()) - 1);
    if (st->sel_multi.empty()) {
        st->sel_profiles = {st->sel_profile};
        st->sel_pts.clear();
    } else {
        refresh_profile_selection_ui(st);
        return;
    }
    if (st->wrap_list) {
        st->suppress_commands = true;
        SendMessageW(st->wrap_list, LB_SETCURSEL, st->sel_profile, 0);
        st->suppress_commands = false;
    }
    refill_pt_list(st);
    sync_uv_edits(st);
    invalidate_all(st);
}

// Flip: invierte el orden del strip in-place (el 1.º pasa al final con sus x/z/uv; von↔bis).
void flip_profile(CreatorState* st) {
    push_undo(st);
    const auto targets = mirror_invert_targets(st);
    for (int pi : targets) {
        if (pi < 0 || static_cast<std::size_t>(pi) >= st->def.profiles.size()) continue;
        auto& strip = st->def.profiles[static_cast<std::size_t>(pi)];
        if (strip.size() < 2) continue;
        std::reverse(strip.begin(), strip.end());
    }
    sync_uv_edits(st);
    invalidate_all(st);
}

// Espejar: clona perfil al lado opuesto (-X, strip invertido; UV sin 1-uv).
void espejar_profile(CreatorState* st) {
    if (st->def.profiles.empty()) return;
    push_undo(st);
    const auto targets = mirror_invert_targets(st);
    std::vector<int> created;
    for (int pi : targets) {
        if (pi < 0 || static_cast<std::size_t>(pi) >= st->def.profiles.size()) continue;
        const auto& strip = st->def.profiles[static_cast<std::size_t>(pi)];
        if (strip.empty()) continue;
        if (static_cast<std::size_t>(pi) < st->def.profile_comments.size()) {
            const auto& c = st->def.profile_comments[pi];
            if (c.find("(espejo)") != std::string::npos) continue;
        }
        float avg_x = 0.f;
        for (const auto& pt : strip) avg_x += pt.x;
        avg_x /= static_cast<float>(strip.size());
        if (avg_x < -1.e-4f) continue;
        std::vector<spline::ProfilePoint> mirrored;
        mirrored.reserve(strip.size());
        for (const auto& pt : strip) {
            if (std::abs(pt.x) <= 1.e-4f) continue;
            spline::ProfilePoint m = pt;
            m.x = -pt.x;
            mirrored.push_back(m);
        }
        if (mirrored.size() < 2) continue;
        std::reverse(mirrored.begin(), mirrored.end());
        st->def.profiles.push_back(std::move(mirrored));
        ensure_profile_labels(st);
        const auto label = (static_cast<std::size_t>(pi) < st->profile_labels.size())
                               ? st->profile_labels[static_cast<std::size_t>(pi)] + L" (espejo)"
                               : L"Perfil espejo";
        st->profile_labels.back() = label;
        st->def.profile_comments.push_back(wide_to_utf8(label));
        created.push_back(static_cast<int>(st->def.profiles.size()) - 1);
    }
    if (!created.empty()) {
        const std::size_t npath = st->def.paths.size();
        for (std::size_t i = 0; i < npath; ++i) {
            const auto& p = st->def.paths[i];
            if (std::abs(p.lateral) <= 1.e-4f) continue;
            spline::SliPath mp = p;
            mp.lateral = -p.lateral;
            st->def.paths.push_back(mp);
        }
        st->sel_profiles = created;
        select_profile(st, created.back());
        refill_paths(st);
        sync_path_field_ui(st);
    }
    refill_wrap_list(st);
    sync_uv_edits(st);
    invalidate_all(st);
}

void draw_grid(HDC dc, CreatorState* st, const RECT& rc) {
    if (st->grid_off || st->grid_m <= 0.f) return;
    const float step_px = st->grid_m * st->view.scale;
    if (step_px < 8.f) return;
    float wx0 = 0.f, wz0 = 0.f, wx1 = 0.f, wz1 = 0.f;
    screen_to_world(rc, st->view, rc.left, rc.bottom, wx0, wz0);
    screen_to_world(rc, st->view, rc.right, rc.top, wx1, wz1);
    if (wx0 > wx1) std::swap(wx0, wx1);
    if (wz0 > wz1) std::swap(wz0, wz1);
    const int ix0 = static_cast<int>(std::floor(wx0 / st->grid_m));
    const int ix1 = static_cast<int>(std::ceil(wx1 / st->grid_m));
    const int iz0 = static_cast<int>(std::floor(wz0 / st->grid_m));
    const int iz1 = static_cast<int>(std::ceil(wz1 / st->grid_m));
    HPEN pen = CreatePen(PS_DOT, 1, RGB(225, 225, 225));
    HGDIOBJ old = SelectObject(dc, pen);
    int lines = 0;
    for (int ix = ix0; ix <= ix1 && lines < kMaxGridLines; ++ix, ++lines) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        const float x = ix * st->grid_m;
        world_to_screen(rc, st->view, x, wz0, x0, y0);
        world_to_screen(rc, st->view, x, wz1, x1, y1);
        MoveToEx(dc, x0, y0, nullptr);
        LineTo(dc, x1, y1);
    }
    for (int iz = iz0; iz <= iz1 && lines < kMaxGridLines * 2; ++iz, ++lines) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        const float z = iz * st->grid_m;
        world_to_screen(rc, st->view, wx0, z, x0, y0);
        world_to_screen(rc, st->view, wx1, z, x1, y1);
        MoveToEx(dc, x0, y0, nullptr);
        LineTo(dc, x1, y1);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

bool is_point_in_sel_multi(const CreatorState* st, int profile, int pt) {
    for (const auto& r : st->sel_multi)
        if (r.profile == profile && r.pt == pt) return true;
    return false;
}

void draw_canvas(HDC dc, CreatorState* st) {
    RECT rc{};
    GetClientRect(st->canvas, &rc);
    FillRect(dc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    HPEN axis_pen = CreatePen(PS_SOLID, 1, RGB(140, 140, 140));
    HPEN prof_dim_pen = CreatePen(PS_SOLID, 1, RGB(160, 180, 210));
    HPEN prof_sel_pen = CreatePen(PS_SOLID, 3, RGB(30, 90, 180));
    HFONT font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);

    int axis_x = 0, axis_y = 0;
    world_to_screen(rc, st->view, 0.f, 0.f, axis_x, axis_y);
    SelectObject(dc, axis_pen);
    MoveToEx(dc, rc.left, axis_y, nullptr);
    LineTo(dc, rc.right, axis_y);
    MoveToEx(dc, axis_x, rc.top, nullptr);
    LineTo(dc, axis_x, rc.bottom);

    if (st->draw_paths) draw_path_lanes(dc, st, rc);

    for (std::size_t pi = 0; pi < st->def.profiles.size(); ++pi) {
        const auto& ps = st->def.profiles[pi];
        const bool active = static_cast<int>(pi) == st->sel_profile;
        SelectObject(dc, active ? prof_sel_pen : prof_dim_pen);
        for (std::size_t i = 1; i < ps.size(); ++i) {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            world_to_screen(rc, st->view, ps[i - 1].x, ps[i - 1].z, x0, y0);
            world_to_screen(rc, st->view, ps[i].x, ps[i].z, x1, y1);
            MoveToEx(dc, x0, y0, nullptr);
            LineTo(dc, x1, y1);
        }
    }

    HBRUSH pt_dim_br = CreateSolidBrush(RGB(190, 190, 200));
    HBRUSH pt_br = CreateSolidBrush(RGB(200, 60, 40));
    HBRUSH sel_br = CreateSolidBrush(RGB(40, 160, 60));
    HBRUSH multi_br = CreateSolidBrush(RGB(60, 120, 220));
    for (std::size_t pi = 0; pi < st->def.profiles.size(); ++pi) {
        const auto& ps = st->def.profiles[pi];
        for (std::size_t i = 0; i < ps.size(); ++i) {
            int px = 0, py = 0;
            world_to_screen(rc, st->view, ps[i].x, ps[i].z, px, py);
            const bool active = static_cast<int>(pi) == st->sel_profile;
            const bool sel = is_point_in_sel_multi(st, static_cast<int>(pi), static_cast<int>(i)) ||
                             (active && is_selected(st, static_cast<int>(i)));
            const bool multi_prof = st->sel_profiles.size() > 1 &&
                                    std::find(st->sel_profiles.begin(), st->sel_profiles.end(), static_cast<int>(pi)) !=
                                        st->sel_profiles.end();
            const int rad = (active || sel || multi_prof) ? 5 : 3;
            SelectObject(dc, sel ? sel_br : (multi_prof ? multi_br : (active ? pt_br : pt_dim_br)));
            Ellipse(dc, px - rad, py - rad, px + rad, py + rad);
            if (active || sel) {
                wchar_t lbl[64];
                swprintf_s(lbl, L"%.2f, %.2f", static_cast<double>(ps[i].x), static_cast<double>(ps[i].z));
                SetTextColor(dc, RGB(20, 20, 20));
                SetBkMode(dc, TRANSPARENT);
                TextOutW(dc, px + 8, py - 16, lbl, static_cast<int>(wcslen(lbl)));
            }
        }
    }

    if (st->pencil_drag) {
        int px0 = 0, py0 = 0, px1 = 0, py1 = 0;
        world_to_screen(rc, st->view, st->pencil_x0, st->pencil_z0, px0, py0);
        world_to_screen(rc, st->view, st->pencil_x1, st->pencil_z1, px1, py1);
        HPEN pencil_pen = CreatePen(PS_DOT, 2, RGB(220, 80, 40));
        SelectObject(dc, pencil_pen);
        MoveToEx(dc, px0, py0, nullptr);
        LineTo(dc, px1, py1);
        DeleteObject(pencil_pen);
    }

    if (st->marquee_drag) {
        HPEN box_pen = CreatePen(PS_DOT, 1, RGB(30, 90, 200));
        SelectObject(dc, box_pen);
        SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        const int x0 = (std::min)(st->mar_x0, st->mar_x1);
        const int y0 = (std::min)(st->mar_y0, st->mar_y1);
        const int x1 = (std::max)(st->mar_x0, st->mar_x1);
        const int y1 = (std::max)(st->mar_y0, st->mar_y1);
        Rectangle(dc, x0, y0, x1, y1);
        DeleteObject(box_pen);
    }

    SetBkMode(dc, TRANSPARENT);
    const wchar_t* hint =
        st->path_place_mode ? L"CREAR PATH: clic en el perfil para colocar el path (Esc cancela)"
        : st->pencil_mode    ? L"LAPIZ: arrastra para crear perfil  |  clic nodo = origen  |  Esc desactiva"
        : st->marquee_mode   ? L"SELECCION CAJA: nodos + paths  |  A o Esc desactiva  |  Ctrl+clic multi  |  G mover"
        : st->grab_mode      ? L"G: mover nodos \u2014 clic confirma, Esc cancela"
                             : L"A caja  |  lapiz dibujar  |  clic perfil/path  |  Ctrl+clic multi  |  G mover";
    if (st->path_place_mode) SetTextColor(dc, RGB(180, 60, 20));
    TextOutW(dc, 8, 4, hint, static_cast<int>(wcslen(hint)));

    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteObject(axis_pen);
    DeleteObject(prof_dim_pen);
    DeleteObject(prof_sel_pen);
    DeleteObject(pt_dim_br);
    DeleteObject(pt_br);
    DeleteObject(sel_br);
    DeleteObject(multi_br);
}

void project3d(float x, float y, float z, float yaw, float pitch, float pan_x, float pan_y, float cx, float cy,
               float scale, int& sx, int& sy) {
    const float cyaw = std::cos(yaw);
    const float syaw = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float rx = x * cyaw - y * syaw;
    const float ry = x * syaw + y * cyaw;
    sx = static_cast<int>(std::lround(cx + (rx + pan_x) * scale));
    sy = static_cast<int>(std::lround(cy - (z * cp + ry * sp) * scale * 0.55f + pan_y));
}

void draw_preview3d_wire(HDC dc, CreatorState* st) {
    RECT rc{};
    GetClientRect(st->preview3d, &rc);
    FillRect(dc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    const float cx = (rc.left + rc.right) * 0.5f;
    const float cy = (rc.top + rc.bottom) * 0.55f;
    if (st->def.profiles.empty()) return;
    HPEN edge = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
    HPEN face = CreatePen(PS_SOLID, 1, RGB(120, 150, 200));
    HGDIOBJ old = SelectObject(dc, face);
    auto draw_seg = [&](float x0, float z0, float x1, float z1) {
        int a0x = 0, a0y = 0, a1x = 0, a1y = 0, b0x = 0, b0y = 0, b1x = 0, b1y = 0;
        project3d(x0, 0.f, z0, st->preview_yaw, st->preview_pitch, st->preview_pan_x, st->preview_pan_y, cx, cy,
                  st->preview_scale, a0x, a0y);
        project3d(x1, 0.f, z1, st->preview_yaw, st->preview_pitch, st->preview_pan_x, st->preview_pan_y, cx, cy,
                  st->preview_scale, a1x, a1y);
        project3d(x0, kPreviewLenM, z0, st->preview_yaw, st->preview_pitch, st->preview_pan_x, st->preview_pan_y, cx,
                  cy, st->preview_scale, b0x, b0y);
        project3d(x1, kPreviewLenM, z1, st->preview_yaw, st->preview_pitch, st->preview_pan_x, st->preview_pan_y, cx,
                  cy, st->preview_scale, b1x, b1y);
        MoveToEx(dc, a0x, a0y, nullptr);
        LineTo(dc, a1x, a1y);
        LineTo(dc, b1x, b1y);
        LineTo(dc, b0x, b0y);
        LineTo(dc, a0x, a0y);
        SelectObject(dc, edge);
        LineTo(dc, b0x, b0y);
        MoveToEx(dc, a1x, a1y, nullptr);
        LineTo(dc, b1x, b1y);
        SelectObject(dc, face);
    };
    for (const auto& strip : st->def.profiles) {
        if (strip.size() < 2) continue;
        for (std::size_t i = 1; i < strip.size(); ++i)
            draw_seg(strip[i - 1].x, strip[i - 1].z, strip[i].x, strip[i].z);
    }
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, 8, 4, L"Preview 3D  |  rueda=zoom  |  rueda clic=pan  |  LMB=rotar", 52);
    SelectObject(dc, old);
    DeleteObject(edge);
    DeleteObject(face);
}

void layout_dialog(CreatorState* st) {
    if (!st->dlg || st->laying_out || st->child_layout.empty()) return;
    st->laying_out = true;
    RECT cr{};
    GetClientRect(st->dlg, &cr);
    const int cw = (std::max)(static_cast<int>(cr.right), 200);
    const int ch = (std::max)(static_cast<int>(cr.bottom), 200);
    const float sx = static_cast<float>(cw) / static_cast<float>(kDesignW);
    const float sy = static_cast<float>(ch) / static_cast<float>(kDesignH);
    HDWP dwp = BeginDeferWindowPos(static_cast<int>(st->child_layout.size()));
    for (const auto& c : st->child_layout) {
        if (!c.hwnd) continue;
        const int x = static_cast<int>(std::lround(c.design.left * sx));
        const int y = static_cast<int>(std::lround(c.design.top * sy));
        const int w = static_cast<int>(std::lround((c.design.right - c.design.left) * sx));
        // Never stretch combos to drop-down height (220) — that covers controls below on resize.
        const int h = static_cast<int>(std::lround((c.design.bottom - c.design.top) * sy));
        if (dwp) dwp = DeferWindowPos(dwp, c.hwnd, nullptr, x, y, (std::max)(w, 4), (std::max)(h, 4),
                                      SWP_NOZORDER | SWP_NOACTIVATE);
        else MoveWindow(c.hwnd, x, y, (std::max)(w, 4), (std::max)(h, 4), FALSE);
    }
    if (dwp) EndDeferWindowPos(dwp);
    st->laying_out = false;
#ifdef OMSI_ULTRALIGHT_UI
    if (st->sidebar_ui_ok) {
        const float sx = static_cast<float>(cw) / static_cast<float>(kDesignW);
        const float sy = static_cast<float>(ch) / static_cast<float>(kDesignH);
        const int rx = static_cast<int>(std::lround(kRightX * sx));
        const int ry = static_cast<int>(std::lround(8.f * sy));
        const int rw = static_cast<int>(std::lround((kDesignW - kRightX - 8) * sx));
        const int rh = (std::max)(ch - ry - 4, 120);
        st->sidebar_ui.set_bounds(rx, ry, rw, rh);
    }
    if (st->chrome_ui_ok) {
        const float sx = static_cast<float>(cw) / static_cast<float>(kDesignW);
        const float sy = static_cast<float>(ch) / static_cast<float>(kDesignH);
        const int lx = static_cast<int>(std::lround(12.f * sx));
        const int ly = static_cast<int>(std::lround((kViewportH + 24.f) * sy));
        const int lw = static_cast<int>(std::lround(kLeftPanelW * sx));
        const int lh = (std::max)(ch - ly - 4, 100);
        st->chrome_ui.set_bounds(lx, ly, lw, lh);
    }
#endif
    InvalidateRect(st->dlg, nullptr, TRUE);
}

void capture_child_layout(CreatorState* st) {
    st->child_layout.clear();
    EnumChildWindows(
        st->dlg,
        [](HWND h, LPARAM lp) -> BOOL {
            auto* out = reinterpret_cast<std::vector<ChildLayout>*>(lp);
            ChildLayout c{};
            c.hwnd = h;
            RECT r{};
            GetWindowRect(h, &r);
            POINT tl{r.left, r.top};
            POINT br{r.right, r.bottom};
            ScreenToClient(GetParent(h), &tl);
            ScreenToClient(GetParent(h), &br);
            c.design = {tl.x, tl.y, br.x, br.y};
            wchar_t cls[32]{};
            GetClassNameW(h, cls, 32);
            c.combo = (wcscmp(cls, L"ComboBox") == 0);
            out->push_back(c);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&st->child_layout));
}

bool handle_key(CreatorState* st, WPARAM key, bool ctrl, LPARAM key_lp) {
    if (!st) return false;
    // A — alternar selección por caja; interceptar antes de edits (ignorar autorepeat).
    if (!ctrl && (key == 'A' || key == 'a')) {
        if (key_lp & (1u << 30)) return true;
        if (st->grab_mode) cancel_grab(st);
        set_marquee_mode(st, !st->marquee_mode);
        if (st->marquee_mode && st->canvas) SetFocus(st->canvas);
        return true;
    }
    if (ctrl && key == 'Z') {
        do_undo(st);
        refill_pt_list(st);
        refill_textures(st);
        load_texture_previews(st);
        refresh_phase2_ui(st);
        refill_wrap_list(st);
        refill_hp_list(st);
        invalidate_all(st);
        return true;
    }
    if (ctrl && key == 'C') {
        copy_selection(st);
        return true;
    }
    if (ctrl && key == 'V') {
        if (st->dlg && GetFocus() == GetDlgItem(st->dlg, kTexEdit))
            paste_texture_clipboard(st);
        else if (!st->clip_pts.empty())
            paste_clipboard(st);
        else
            paste_texture_clipboard(st);
        return true;
    }
    if (key == 'G' && !st->grab_mode) {
        start_grab(st);
        return true;
    }
    if (key == VK_DELETE) {
        delete_selection(st);
        return true;
    }
    if (st->grab_mode && key == VK_ESCAPE) {
        cancel_grab(st);
        return true;
    }
    if (key == VK_ESCAPE && st->marquee_mode) {
        set_marquee_mode(st, false);
        return true;
    }
    if (key == VK_ESCAPE && st->pencil_mode) {
        set_pencil_mode(st, false);
        return true;
    }
    if (key == VK_ESCAPE && st->marquee_drag) {
        st->marquee_drag = false;
        ReleaseCapture();
        invalidate_all(st);
        return true;
    }
    if (st->grab_mode && key == VK_RETURN) {
        confirm_grab(st);
        return true;
    }
    return false;
}

bool handle_wheel(CreatorState* st, HWND target, short delta, bool ctrl) {
    (void)ctrl;
    const float factor = delta > 0 ? 1.12f : 0.89f;
    if (target == st->canvas) {
        st->view.scale = (std::clamp)(st->view.scale * factor, 8.f, 320.f);
        invalidate_all(st);
        return true;
    }
    if (target == st->preview3d) {
        const float zoom = delta > 0 ? 0.9f : 1.11f;
        if (st->preview_rhi_ok) {
            st->preview_cam.radius = (std::clamp)(st->preview_cam.radius * zoom, st->preview_cam.min_radius,
                                                  st->preview_cam.max_radius);
            render_preview_rhi(st);
        } else {
            st->preview_scale = (std::clamp)(st->preview_scale * factor, 1.5f, 30.f);
            InvalidateRect(st->preview3d, nullptr, FALSE);
        }
        return true;
    }
    return false;
}

LRESULT CALLBACK canvas_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<CreatorState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE:
        st = reinterpret_cast<CreatorState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        st->canvas = hwnd;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (st) draw_canvas(dc, st);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (st) handle_wheel(st, hwnd, GET_WHEEL_DELTA_WPARAM(wp), (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        return 0;
    case WM_MBUTTONDOWN:
        if (st) {
            st->panning_2d = true;
            st->pan_sx = GET_X_LPARAM(lp);
            st->pan_sy = GET_Y_LPARAM(lp);
            st->pan_ax = st->view.pan_x;
            st->pan_az = st->view.pan_z;
            SetCapture(hwnd);
        }
        return 0;
    case WM_MBUTTONUP:
        if (st) {
            st->panning_2d = false;
            ReleaseCapture();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        if (!st) break;
        if (st->grab_mode) {
            confirm_grab(st);
            return 0;
        }
        if (st->path_place_mode) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            float x = 0.f, z = 0.f;
            screen_to_world(rc, st->view, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), x, z);
            push_undo(st);
            spline::SliPath np{};
            np.typ = 1;
            np.lateral = x;
            np.height = z;
            np.width = (!st->def.paths.empty() && st->sel_path >= 0 &&
                        static_cast<std::size_t>(st->sel_path) < st->def.paths.size())
                           ? st->def.paths[static_cast<std::size_t>(st->sel_path)].width
                           : 3.f;
            np.direction = 2;
            st->def.paths.push_back(np);
            st->sel_path = static_cast<int>(st->def.paths.size()) - 1;
            st->path_place_mode = false;
            refill_paths(st);
            sync_path_field_ui(st);
            invalidate_all(st);
            return 0;
        }
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const auto hit_any = hit_point_any(st, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (st->marquee_mode) {
            if (ctrl && hit_any) {
                toggle_select_ref(st, *hit_any);
                build_drag_group(st);
                invalidate_all(st);
                return 0;
            }
            if (!ctrl) {
                st->marquee_drag = true;
                st->mar_x0 = st->mar_x1 = GET_X_LPARAM(lp);
                st->mar_y0 = st->mar_y1 = GET_Y_LPARAM(lp);
                SetCapture(hwnd);
                invalidate_all(st);
                return 0;
            }
        }
        if (st->pencil_mode && !ctrl) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int mx = GET_X_LPARAM(lp);
            const int my = GET_Y_LPARAM(lp);
            if (hit_any) select_only_ref(st, *hit_any);
            get_pencil_anchor(st, st->pencil_x0, st->pencil_z0);
            screen_to_world(rc, st->view, mx, my, st->pencil_x1, st->pencil_z1);
            st->pencil_drag = true;
            SetCapture(hwnd);
            invalidate_all(st);
            return 0;
        }
        if (hit_any) {
            if (ctrl)
                toggle_select_ref(st, *hit_any);
            else
                select_only_ref(st, *hit_any);
            build_drag_group(st);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            screen_to_world(rc, st->view, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), st->drag_wx0, st->drag_wz0);
            st->dragging_pt = true;
            st->drag_undo_done = false;
            SetCapture(hwnd);
            invalidate_all(st);
            return 0;
        }
        if (!st->marquee_mode) {
            const int mx = GET_X_LPARAM(lp);
            const int my = GET_Y_LPARAM(lp);
            if (const auto path = hit_path(st, mx, my)) {
                select_path(st, *path);
                return 0;
            }
            if (const auto prof = hit_profile_segment(st, mx, my)) {
                select_profile(st, *prof);
                st->sel_paths.clear();
                return 0;
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!st) break;
        if (st->panning_2d) {
            const int dx = GET_X_LPARAM(lp) - st->pan_sx;
            const int dy = GET_Y_LPARAM(lp) - st->pan_sy;
            st->view.pan_x = st->pan_ax - dx / st->view.scale;
            st->view.pan_z = st->pan_az + dy / st->view.scale;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (st->marquee_drag) {
            st->mar_x1 = GET_X_LPARAM(lp);
            st->mar_y1 = GET_Y_LPARAM(lp);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (st->pencil_drag) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            screen_to_world(rc, st->view, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), st->pencil_x1, st->pencil_z1);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (st->grab_mode) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            float x = 0.f, z = 0.f;
            screen_to_world(rc, st->view, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), x, z);
            if (!st->drag_undo_done) {
                push_undo(st);
                st->drag_undo_done = true;
            }
            if (!st->grab_origin_set) {
                st->grab_ax = x;
                st->grab_az = z;
                st->grab_origin_set = true;
            }
            const float dx = x - st->grab_ax;
            const float dz = z - st->grab_az;
            for (std::size_t j = 0; j < st->drag_group.size(); ++j) {
                auto& d = st->drag_group[j];
                if (d.profile < 0 || static_cast<std::size_t>(d.profile) >= st->def.profiles.size()) continue;
                auto& strip = st->def.profiles[static_cast<std::size_t>(d.profile)];
                if (d.pt < 0 || static_cast<std::size_t>(d.pt) >= strip.size()) continue;
                strip[static_cast<std::size_t>(d.pt)].x = snap_move_coord(st, st->grab_x0[j] + dx);
                strip[static_cast<std::size_t>(d.pt)].z = snap_move_coord(st, st->grab_z0[j] + dz);
            }
            invalidate_all(st);
            return 0;
        }
        if (!st->dragging_pt || st->drag_group.empty()) break;
        if (!st->drag_undo_done) {
            push_undo(st);
            st->drag_undo_done = true;
        }
        RECT rc{};
        GetClientRect(hwnd, &rc);
        float x = 0.f, z = 0.f;
        screen_to_world(rc, st->view, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), x, z);
        const float dx = x - st->drag_wx0;
        const float dz = z - st->drag_wz0;
        for (std::size_t j = 0; j < st->drag_group.size(); ++j) {
            auto& d = st->drag_group[j];
            if (d.profile < 0 || static_cast<std::size_t>(d.profile) >= st->def.profiles.size()) continue;
            auto& strip = st->def.profiles[static_cast<std::size_t>(d.profile)];
            if (d.pt < 0 || static_cast<std::size_t>(d.pt) >= strip.size()) continue;
            strip[static_cast<std::size_t>(d.pt)].x = snap_move_coord(st, d.x0 + dx);
            strip[static_cast<std::size_t>(d.pt)].z = snap_move_coord(st, d.z0 + dz);
        }
        refill_pt_list(st);
        invalidate_all(st);
        return 0;
    }
    case WM_LBUTTONUP:
        if (st) {
            if (st->pencil_drag) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                screen_to_world(rc, st->view, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), st->pencil_x1, st->pencil_z1);
                st->pencil_drag = false;
                finish_pencil_stroke(st);
                ReleaseCapture();
                invalidate_all(st);
                return 0;
            }
            if (st->marquee_drag) {
                st->marquee_drag = false;
                finish_marquee(st);
                ReleaseCapture();
                invalidate_all(st);
                return 0;
            }
            st->dragging_pt = false;
            if (!st->panning_2d && !st->grab_mode) ReleaseCapture();
        }
        return 0;
    case WM_CANCELMODE:
        if (st) {
            st->marquee_drag = false;
            st->pencil_drag = false;
            st->dragging_pt = false;
            st->panning_2d = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            invalidate_all(st);
        }
        return TRUE;
    case WM_CAPTURECHANGED:
        if (st && reinterpret_cast<HWND>(lp) != hwnd) {
            if (st->marquee_drag) {
                st->marquee_drag = false;
                finish_marquee(st);
            }
            st->dragging_pt = false;
            st->panning_2d = false;
            invalidate_all(st);
        }
        return 0;
    case WM_KEYDOWN:
        if (st && handle_key(st, wp, (GetKeyState(VK_CONTROL) & 0x8000) != 0, lp)) return 0;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

#include "detail/spline_creator_dx12_preview.inc.cpp"

LRESULT CALLBACK preview3d_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<CreatorState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE:
        st = reinterpret_cast<CreatorState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        st->preview3d = hwnd;
        init_preview_rhi(st);
        return 0;
    case WM_ERASEBKGND:
        if (st && st->preview_rhi_ok) return 1;
        break;
    case WM_SIZE:
        if (st && st->preview_rhi_ok && st->preview_rhi) {
            const auto w = static_cast<std::uint32_t>((std::max)(static_cast<int>(LOWORD(lp)), 1));
            const auto h = static_cast<std::uint32_t>((std::max)(static_cast<int>(HIWORD(lp)), 1));
            st->preview_rhi->resize(w, h);
            render_preview_rhi(st);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        if (st && st->preview_rhi_ok)
            render_preview_rhi(st);
        else if (st)
            draw_preview3d_mesh(ps.hdc, st);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (st) handle_wheel(st, hwnd, GET_WHEEL_DELTA_WPARAM(wp), false);
        return 0;
    case WM_MBUTTONDOWN:
        if (st) {
            st->panning_3d = true;
            st->pan_sx = GET_X_LPARAM(lp);
            st->pan_sy = GET_Y_LPARAM(lp);
            SetCapture(hwnd);
        }
        return 0;
    case WM_MBUTTONUP:
        if (st) {
            st->panning_3d = false;
            ReleaseCapture();
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (st) {
            st->preview_rot_drag = true;
            st->preview_sx = GET_X_LPARAM(lp);
            st->preview_sy = GET_Y_LPARAM(lp);
            SetCapture(hwnd);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (!st) break;
        if (st->panning_3d) {
            const int dx = GET_X_LPARAM(lp) - st->pan_sx;
            const int dy = GET_Y_LPARAM(lp) - st->pan_sy;
            st->preview_cam.target_x += dx * 0.015f;
            st->preview_cam.target_y -= dy * 0.015f;
            st->pan_sx = GET_X_LPARAM(lp);
            st->pan_sy = GET_Y_LPARAM(lp);
            if (st->preview_rhi_ok)
                render_preview_rhi(st);
            else
                InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (st->preview_rot_drag) {
            const int dx = GET_X_LPARAM(lp) - st->preview_sx;
            const int dy = GET_Y_LPARAM(lp) - st->preview_sy;
            const float dtheta = DirectX::XMConvertToRadians(0.25f * static_cast<float>(dx));
            const float dphi = DirectX::XMConvertToRadians(0.25f * static_cast<float>(dy));
            st->preview_cam.theta += st->preview_cam.mirror_x ? -dtheta : dtheta;
            st->preview_cam.phi = (std::clamp)(st->preview_cam.phi - dphi, 0.1f, DirectX::XM_PI - 0.1f);
            st->preview_sx = GET_X_LPARAM(lp);
            st->preview_sy = GET_Y_LPARAM(lp);
            if (st->preview_rhi_ok)
                render_preview_rhi(st);
            else
                InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (st) {
            st->preview_rot_drag = false;
            if (!st->panning_3d) ReleaseCapture();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK tex_preview_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<CreatorState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE:
        st = reinterpret_cast<CreatorState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        st->tex_preview = hwnd;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (st) draw_tex_preview(dc, st);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (st) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            set_uv_from_texture_click(st, GET_X_LPARAM(lp) - rc.left, rc.right - rc.left);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool pump_modal(HWND owner, HWND dlg, CreatorState* st) {
    const HWND block_root = modal_block_root(owner);
    if (block_root) EnableWindow(block_root, FALSE);

    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    modal_take_foreground(dlg);
    if (st && st->canvas) SetFocus(st->canvas);

    if (g_run_self_test) {
        run_self_test_sequence(st);
        g_self_test_result = st->self_test_passed;
        st->modal.done = true;
        modal_end_block(block_root, owner);
        return g_self_test_result;
    }

    MSG msg{};
    while (!st->modal.done) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                // Nested inside GLFW map editor: do not consume app-wide WM_QUIT.
                PostQuitMessage(static_cast<int>(msg.wParam));
                continue;
            }
            if (msg.message == WM_KEYDOWN) {
                const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (handle_key(st, msg.wParam, ctrl, msg.lParam)) continue;
                if (msg.wParam == VK_ESCAPE && !st->grab_mode && !st->marquee_mode && !st->marquee_drag &&
                    !st->path_place_mode) {
                    st->modal.done = true;
                    break;
                }
                if (msg.wParam == VK_ESCAPE && st->path_place_mode) {
                    st->path_place_mode = false;
                    invalidate_all(st);
                    continue;
                }
            }
            if (msg.message == WM_MOUSEWHEEL) {
                const short delta = GET_WHEEL_DELTA_WPARAM(msg.wParam);
                HWND target = WindowFromPoint(msg.pt);
                while (target && target != dlg) {
                    if (target == st->canvas || target == st->preview3d) {
                        handle_wheel(st, target, delta, false);
                        break;
                    }
                    wchar_t cls[32]{};
                    GetClassNameW(target, cls, 32);
                    if (wcscmp(cls, L"OmsiF4UlChild") == 0) {
#ifdef OMSI_ULTRALIGHT_UI
                        POINT pt = msg.pt;
                        ScreenToClient(target, &pt);
                        if (st->sidebar_ui_ok && target == st->sidebar_ui.hwnd())
                            st->sidebar_ui.fire_scroll(delta / 4, pt.x, pt.y);
                        else if (st->chrome_ui_ok && target == st->chrome_ui.hwnd())
                            st->chrome_ui.fire_scroll(delta / 4, pt.x, pt.y);
#endif
                        break;
                    }
                    target = GetParent(target);
                }
                continue;
            }
            if (!IsDialogMessageW(dlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (st->modal.done) break;
        if (st->preview_rhi_ok) render_preview_rhi(st);
#ifdef OMSI_ULTRALIGHT_UI
        if (st->sidebar_ui_ok) st->sidebar_ui.tick();
        if (st->chrome_ui_ok) st->chrome_ui.tick();
#endif
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
    shutdown_preview_rhi(st);
    modal_end_block(block_root, owner);
    return st->modal.ok;
}

LRESULT CALLBACK dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<CreatorState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE:
        st = reinterpret_cast<CreatorState*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        st->dlg = hwnd;
        g_st = st;
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    case WM_SIZE:
        if (st && !st->laying_out && LOWORD(lp) > 64 && HIWORD(lp) > 64) layout_dialog(st);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = 900;
        mmi->ptMinTrackSize.y = 620;
        return 0;
    }
    case WM_DROPFILES: {
        if (!st) break;
        const HDROP drop = reinterpret_cast<HDROP>(wp);
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t file[MAX_PATH]{};
            DragQueryFileW(drop, i, file, MAX_PATH);
            const auto p = std::filesystem::path(file);
            if (p.extension() == L".sli")
                import_sli_into(st, p);
            else
                add_texture_from_path(st, file);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_CTLCOLORBTN: {
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, OPAQUE);
        if (id == kOpenSli) {
            SetBkColor(dc, RGB(255, 220, 60));
            SetTextColor(dc, RGB(20, 20, 20));
            static HBRUSH br_open = CreateSolidBrush(RGB(255, 220, 60));
            return reinterpret_cast<LRESULT>(br_open);
        }
        if (id == kSave) {
            SetBkColor(dc, RGB(90, 190, 90));
            SetTextColor(dc, RGB(255, 255, 255));
            static HBRUSH br_save = CreateSolidBrush(RGB(90, 190, 90));
            return reinterpret_cast<LRESULT>(br_save);
        }
        if (id == kCancel) {
            SetBkColor(dc, RGB(220, 80, 70));
            SetTextColor(dc, RGB(255, 255, 255));
            static HBRUSH br_cancel = CreateSolidBrush(RGB(220, 80, 70));
            return reinterpret_cast<LRESULT>(br_cancel);
        }
        break;
    }
    case WM_MEASUREITEM: {
        if (wp == kTexList) {
            reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = kTexSlotH;
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (!st) break;
        if (dis->CtlID == kPencilDraw) {
            draw_toggle_tool_btn(dis, st->pencil_bmp, L"\u270E");
            return TRUE;
        }
        if (dis->CtlID == kMarqueeSel) {
            draw_toggle_tool_btn(dis, st->marquee_bmp, L"A");
            return TRUE;
        }
        if (dis->CtlID == kDrawTex) {
            draw_toggle_tool_btn(dis, st->draw_tex_bmp, L"T");
            return TRUE;
        }
        if (dis->CtlID == kDrawPath) {
            draw_toggle_tool_btn(dis, st->draw_path_bmp, L"P");
            return TRUE;
        }
        if (dis->CtlID != kTexList) break;
        FillRect(dis->hDC, &dis->rcItem, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        const int idx = dis->itemID;
        if (idx < 0) return TRUE;
        // Slot: thumbnail izquierda + nombre (wireframe: imagen | textura name)
        const int thumb = kThumbPx;
        if (static_cast<std::size_t>(idx) < st->tex_thumbs.size() && st->tex_thumbs[static_cast<std::size_t>(idx)]) {
            HDC mem = CreateCompatibleDC(dis->hDC);
            SelectObject(mem, st->tex_thumbs[static_cast<std::size_t>(idx)]);
            BitBlt(dis->hDC, dis->rcItem.left + 4, dis->rcItem.top + 2, thumb, thumb, mem, 0, 0, SRCCOPY);
            DeleteDC(mem);
        } else {
            SetBkMode(dis->hDC, TRANSPARENT);
            TextOutW(dis->hDC, dis->rcItem.left + 8, dis->rcItem.top + 16, L"imagen", 6);
        }
        wchar_t text[260]{};
        SendMessageW(dis->hwndItem, LB_GETTEXT, idx, reinterpret_cast<LPARAM>(text));
        SetBkMode(dis->hDC, TRANSPARENT);
        TextOutW(dis->hDC, dis->rcItem.left + thumb + 12, dis->rcItem.top + 16, text,
                 static_cast<int>(wcslen(text)));
        if (dis->itemState & ODS_SELECTED) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(60, 120, 200));
            SelectObject(dis->hDC, pen);
            SelectObject(dis->hDC, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right - 1, dis->rcItem.bottom - 1);
            DeleteObject(pen);
        }
        return TRUE;
    }
    case WM_COMMAND: {
        if (!st) break;
        if (st->suppress_commands) break;
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (handle_parity_command(hwnd, st, id, code)) return 0;
        if (handle_phase2_command(hwnd, st, id, code)) return 0;
        if (id == kCancel) {
            st->modal.done = true;
            return 0;
        }
        if (id == kGrid && code == CBN_SELCHANGE) {
            const int sel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, kGrid), CB_GETCURSEL, 0, 0));
            static constexpr float gm[4] = {0.01f, 0.05f, 0.1f, 0.5f};
            if (sel >= 0 && sel < 4) {
                st->grid_off = false;
                st->grid_m = gm[sel];
            } else {
                st->grid_off = true;
                st->grid_m = 0.f;
            }
            invalidate_all(st);
            return 0;
        }
        if (id == kCenterView) {
            center_view(st);
            center_preview_view(st);
            invalidate_all(st);
            return 0;
        }
        if (id == kEspejar) {
            espejar_profile(st);
            refill_pt_list(st);
            invalidate_all(st);
            return 0;
        }
        if (id == kFlip) {
            flip_profile(st);
            refill_pt_list(st);
            invalidate_all(st);
            return 0;
        }
        if (id == kPathCreate) {
            st->path_place_mode = !st->path_place_mode;
            if (st->path_place_mode) {
                set_marquee_mode(st, false);
                set_pencil_mode(st, false);
            }
            invalidate_all(st);
            return 0;
        }
        if (id == kMarqueeSel && code == BN_CLICKED) {
            set_marquee_mode(st, !st->marquee_mode);
            if (st->marquee_mode && st->canvas) SetFocus(st->canvas);
            return 0;
        }
        if (id == kPencilDraw && code == BN_CLICKED) {
            set_pencil_mode(st, !st->pencil_mode);
            if (st->pencil_mode && st->canvas) SetFocus(st->canvas);
            return 0;
        }
        if (id == kWrapOpen) {
            if (st->def.profiles.empty()) return 0;
            run_texture_wrap_dialog(hwnd, st, st->sel_profile, false);
            return 0;
        }
        if (id == kApplyTex) {
            apply_texture_to_profile(st);
            return 0;
        }
        if (id == kWrapDel && st->def.profiles.size() > 1) {
            push_undo(st);
            st->def.profiles.erase(st->def.profiles.begin() + st->sel_profile);
            if (static_cast<std::size_t>(st->sel_profile) < st->def.profile_comments.size())
                st->def.profile_comments.erase(st->def.profile_comments.begin() + st->sel_profile);
            if (static_cast<std::size_t>(st->sel_profile) < st->profile_labels.size())
                st->profile_labels.erase(st->profile_labels.begin() + st->sel_profile);
            st->sel_profile = (std::clamp)(st->sel_profile, 0, static_cast<int>(st->def.profiles.size()) - 1);
            st->sel_pts.clear();
            ensure_profile_labels(st);
            refill_wrap_list(st);
            refill_profiles(st);
            refill_pt_list(st);
            invalidate_all(st);
            return 0;
        }
        if (id == kPathEdit) {
            if (st->def.paths.empty()) return 0;
            run_path_config_dialog(hwnd, st, st->sel_path, false);
            return 0;
        }
        if (id == kTexPaste) {
            paste_texture_clipboard(st);
            return 0;
        }
        if (id == kDelPt) {
            delete_selection(st);
            return 0;
        }
        if (id == kTexAdd) {
            if (const auto picked = browse_texture_file(st, hwnd)) add_texture_from_path(st, *picked);
            return 0;
        }
        if (id == kTexList && code == LBN_SELCHANGE) {
            st->sel_tex = static_cast<int>(SendMessageW(st->tex_list, LB_GETCURSEL, 0, 0));
            sync_texture_flags_ui(st);
            invalidate_all(st);
            return 0;
        }
        if (id == kPathList && code == LBN_SELCHANGE) {
            read_path_edits(st);
            st->sel_path = static_cast<int>(SendMessageW(st->path_list, LB_GETCURSEL, 0, 0));
            sync_path_edits(st);
            sync_path_field_ui(st);
            invalidate_all(st);
            return 0;
        }
        if (id == kPathList && code == LBN_DBLCLK) {
            if (!st->def.paths.empty())
                run_path_config_dialog(hwnd, st, st->sel_path, false);
            return 0;
        }
        if (id == kPickStart) {
            st->tex_pick_mode = 1;
            InvalidateRect(st->tex_preview, nullptr, FALSE);
            return 0;
        }
        if (id == kPickEnd) {
            st->tex_pick_mode = 2;
            InvalidateRect(st->tex_preview, nullptr, FALSE);
            return 0;
        }
        if ((id == kUvStart || id == kUvEnd || id == kTiling) && code == EN_CHANGE) apply_uv_from_edits(st);
        if (id == kPathAdd) {
            run_path_config_dialog(hwnd, st, st->sel_path, true);
            return 0;
        }
        if (id == kPathDel && !st->def.paths.empty()) {
            push_undo(st);
            const int i = (std::clamp)(st->sel_path, 0, static_cast<int>(st->def.paths.size()) - 1);
            st->def.paths.erase(st->def.paths.begin() + i);
            st->sel_path = static_cast<int>(st->def.paths.size()) - 1;
            refill_paths(st);
            sync_path_field_ui(st);
            invalidate_all(st);
            return 0;
        }
        if (id == kSave) {
            read_hp_edits(hwnd, st);
            read_extra_edits(st);
            const auto warns = spline::validate_sli(st->def);
            if (!warns.empty()) {
                std::wstring warn_msg = L"Advertencias:\n";
                for (const auto& w : warns) warn_msg += utf8_to_wide(w) + L"\n";
                warn_msg += L"\n\u00BFGuardar igual?";
                if (MessageBoxW(hwnd, warn_msg.c_str(), L"Validador .sli", MB_YESNO | MB_ICONWARNING) != IDYES)
                    return 0;
            }
            wchar_t file[MAX_PATH]{L"nueva_spline.sli"};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Splines (*.sli)\0*.sli\0";
            ofn.lpstrDefExt = L"sli";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (!st->omsi_root.empty()) {
                static std::wstring init_dir = (st->omsi_root / L"Splines").wstring();
                ofn.lpstrInitialDir = init_dir.c_str();
            }
            if (!GetSaveFileNameW(&ofn)) return 0;
            read_texture_flags_ui(st);
            st->export_copy_textures =
                SendMessageW(GetDlgItem(hwnd, kExportCopy), BM_GETCHECK, 0, 0) == BST_CHECKED;
            const bool ok_save = st->export_copy_textures
                                     ? spline::export_sli_with_textures(st->def, file, st->omsi_root, st->tex_sources)
                                     : spline::write_sli(st->def, file);
            if (!ok_save) {
                MessageBoxW(hwnd, L"No se pudo escribir el archivo.", L"Error", MB_OK | MB_ICONERROR);
                return 0;
            }
            st->save_path = file;
            st->saved = true;
            st->modal.ok = true;
            st->modal.done = true;
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (st) st->modal.done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ensure_subclass(const wchar_t* cls, WNDPROC proc, LPCWSTR cursor = reinterpret_cast<LPCWSTR>(IDC_ARROW)) {
    WNDCLASSW wc{};
    if (GetClassInfoW(GetModuleHandleW(nullptr), cls, &wc)) return;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(nullptr, cursor);
    RegisterClassW(&wc);
}

HWND mk_btn(HWND p, int id, const wchar_t* t, int x, int y, int w, int h) {
    return CreateWindowW(L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, y, w, h, p,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

HWND mk_edit(HWND p, int id, int x, int y, int w, int h, DWORD ex = 0) {
    return CreateWindowExW(ex, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y, w, h, p,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

HWND mk_static(HWND p, const wchar_t* t, int x, int y, int w, int h) {
    return CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE, x, y, w, h, p, nullptr, GetModuleHandleW(nullptr),
                         nullptr);
}

#include "detail/spline_creator_phase2.inc.cpp"

LRESULT CALLBACK path_list_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* st = reinterpret_cast<CreatorState*>(ref);
    if (msg == WM_LBUTTONDOWN && st) {
        const LRESULT item = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
        if (HIWORD(item) != 0) {
            SendMessageW(hwnd, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
            st->sel_path = -1;
            st->sel_paths.clear();
            sync_path_field_ui(st);
            invalidate_all(st);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

#include "detail/spline_creator_parity.inc.cpp"
#include "detail/spline_creator_subdialogs.inc.cpp"
#include "detail/spline_creator_imgui.inc.cpp"

bool run_texture_wrap_dialog(HWND owner, CreatorState* st, int profile_idx, bool create_new) {
    return run_texture_wrap_dialog_imgui(owner, st, profile_idx, create_new);
}

#ifdef OMSI_ULTRALIGHT_UI

std::string sc_base64_encode(const std::vector<std::uint8_t>& data) {
    static const char kTbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out += kTbl[(n >> 18) & 0x3F];
        out += kTbl[(n >> 12) & 0x3F];
        out += kTbl[(n >> 6) & 0x3F];
        out += kTbl[n & 0x3F];
    }
    const std::size_t rem = data.size() - i;
    if (rem == 1) {
        const std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        out += kTbl[(n >> 18) & 0x3F];
        out += kTbl[(n >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        const std::uint32_t n =
            (static_cast<std::uint32_t>(data[i]) << 16) | (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out += kTbl[(n >> 18) & 0x3F];
        out += kTbl[(n >> 12) & 0x3F];
        out += kTbl[(n >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

// Nearest-neighbor square resample (matches the old native make_thumb() StretchBlt behavior).
std::vector<std::uint8_t> sc_downscale_rgba(const std::vector<std::uint8_t>& src, std::uint32_t sw, std::uint32_t sh,
                                            std::uint32_t dw, std::uint32_t dh) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(dw) * dh * 4u);
    for (std::uint32_t y = 0; y < dh; ++y) {
        const std::uint32_t sy = (std::min)(sh - 1, y * sh / dh);
        for (std::uint32_t x = 0; x < dw; ++x) {
            const std::uint32_t sx = (std::min)(sw - 1, x * sw / dw);
            const std::uint8_t* s = &src[(static_cast<std::size_t>(sy) * sw + sx) * 4u];
            std::uint8_t* d = &out[(static_cast<std::size_t>(y) * dw + x) * 4u];
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
        }
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> sc_encode_png_rgba(const std::uint8_t* rgba, std::uint32_t w,
                                                            std::uint32_t h) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return std::nullopt;
    ComPtr<IStream> stream(SHCreateMemStream(nullptr, 0));
    if (!stream) return std::nullopt;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return std::nullopt;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return std::nullopt;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return std::nullopt;
    if (FAILED(frame->Initialize(props.Get()))) return std::nullopt;
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    frame->SetPixelFormat(&fmt);
    if (FAILED(frame->WritePixels(h, w * 4u, w * h * 4u, const_cast<BYTE*>(rgba)))) return std::nullopt;
    if (FAILED(frame->Commit())) return std::nullopt;
    if (FAILED(encoder->Commit())) return std::nullopt;
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return std::nullopt;
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart));
    ULONG read = 0;
    if (bytes.empty() || FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)))
        return std::nullopt;
    bytes.resize(read);
    return bytes;
}

// Small square PNG data URI for the HTML texture list — cached per texture name so we decode/encode once.
const std::string& sc_texture_thumb_data_uri(CreatorState* st, const std::string& name) {
    static const std::string kEmpty;
    if (const auto it = st->tex_thumb_cache.find(name); it != st->tex_thumb_cache.end()) return it->second;
    std::string uri;
    if (const auto* img = preview_tex_pixels(st, name); img && img->width && img->height && !img->rgba.empty()) {
        constexpr std::uint32_t kDim = 40;
        const auto thumb_rgba = sc_downscale_rgba(img->rgba, img->width, img->height, kDim, kDim);
        if (const auto png = sc_encode_png_rgba(thumb_rgba.data(), kDim, kDim))
            uri = "data:image/png;base64," + sc_base64_encode(*png);
    }
    return st->tex_thumb_cache.emplace(name, std::move(uri)).first->second;
}

std::string sc_json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) out += ' ';
            else out += c;
            break;
        }
    }
    return out;
}

void sync_spline_creator_sidebar(CreatorState* st) {
    if (!st || !st->sidebar_ui_ok || !st->sidebar_ui.js_ready()) return;
    ensure_profile_labels(st);
    std::string js = "window.omsiApplyState({";
    js += "selProfile:" + std::to_string(st->sel_profile) + ",selTex:" + std::to_string(st->sel_tex);
    js += ",selPath:" + std::to_string(st->sel_path);
    js += ",exportCopy:" + std::string(st->export_copy_textures ? "true" : "false");
    js += ",profiles:[";
    for (std::size_t i = 0; i < st->def.profiles.size(); ++i) {
        if (i) js += ',';
        js += "{index:" + std::to_string(i) + ",label:\"" +
              sc_json_escape(wide_to_utf8(format_profile_slot_label(st, i))) + "\"}";
    }
    js += "],textures:[";
    for (std::size_t i = 0; i < st->def.textures.size(); ++i) {
        if (i) js += ',';
        js += "{index:" + std::to_string(i) + ",label:\"" + sc_json_escape(st->def.textures[i]) + "\",thumb:\"" +
              sc_texture_thumb_data_uri(st, st->def.textures[i]) + "\"}";
    }
    js += "],paths:[";
    for (std::size_t i = 0; i < st->def.paths.size(); ++i) {
        if (i) js += ',';
        const auto& p = st->def.paths[i];
        char buf[128];
        std::snprintf(buf, sizeof(buf), "typ=%d lat=%.2f h=%.2f w=%.2f", p.typ, static_cast<double>(p.lateral),
                      static_cast<double>(p.height), static_cast<double>(p.width));
        js += "{index:" + std::to_string(i) + ",label:\"" + sc_json_escape(buf) + "\"}";
    }
    js += "]});";
    st->sidebar_ui.evaluate_script(js);
}

void fire_spline_sidebar_cmd(HWND dlg, int id) {
    HWND ctl = GetDlgItem(dlg, id);
    SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(static_cast<WPARAM>(id), BN_CLICKED),
                 reinterpret_cast<LPARAM>(ctl));
}

void handle_spline_sidebar_json(CreatorState* st, HWND dlg, const std::string& json) {
    if (!st || !dlg) return;
    if (json.find("\"type\":\"ready\"") != std::string::npos) {
        sync_spline_creator_sidebar(st);
        return;
    }
    auto extract_index = [&json]() {
        const auto pos = json.find("\"index\":");
        return pos == std::string::npos ? -1 : std::atoi(json.c_str() + pos + 8);
    };
    if (json.find("\"type\":\"select\"") != std::string::npos) {
        const int index = extract_index();
        if (index < 0) return;
        if (json.find("\"kind\":\"profile\"") != std::string::npos) {
            set_active_profile_keep_selection(st, index);
            sync_spline_creator_sidebar(st);
        } else if (json.find("\"kind\":\"texture\"") != std::string::npos) {
            st->sel_tex = index;
            if (st->tex_list) SendMessageW(st->tex_list, LB_SETCURSEL, index, 0);
            sync_profile_fields_ui(st);
            invalidate_all(st);
        } else if (json.find("\"kind\":\"path\"") != std::string::npos) {
            st->sel_path = index;
            if (st->path_list) SendMessageW(st->path_list, LB_SETCURSEL, index, 0);
            read_path_edits(st);
            sync_path_edits(st);
            sync_path_field_ui(st);
            invalidate_all(st);
        }
        return;
    }
    if (json.find("\"type\":\"check\"") != std::string::npos) {
        st->export_copy_textures = json.find("\"value\":true") != std::string::npos;
        if (HWND h = GetDlgItem(dlg, kExportCopy))
            SendMessageW(h, BM_SETCHECK, st->export_copy_textures ? BST_CHECKED : BST_UNCHECKED, 0);
        return;
    }
    if (json.find("\"type\":\"cmd\"") != std::string::npos) {
        if (json.find("\"name\":\"wrapDel\"") != std::string::npos) fire_spline_sidebar_cmd(dlg, kWrapDel);
        else if (json.find("wrapEdit") != std::string::npos) fire_spline_sidebar_cmd(dlg, kWrapEdit);
        else if (json.find("wrapAdd") != std::string::npos) fire_spline_sidebar_cmd(dlg, kWrapAdd);
        else if (json.find("wrapOpen") != std::string::npos) fire_spline_sidebar_cmd(dlg, kWrapOpen);
        else if (json.find("applyTex") != std::string::npos) fire_spline_sidebar_cmd(dlg, kApplyTex);
        else if (json.find("texDel") != std::string::npos) fire_spline_sidebar_cmd(dlg, kTexDel);
        else if (json.find("texPaste") != std::string::npos) fire_spline_sidebar_cmd(dlg, kTexPaste);
        else if (json.find("texAdd") != std::string::npos) fire_spline_sidebar_cmd(dlg, kTexAdd);
        else if (json.find("pathDel") != std::string::npos) fire_spline_sidebar_cmd(dlg, kPathDel);
        else if (json.find("pathCreate") != std::string::npos) fire_spline_sidebar_cmd(dlg, kPathCreate);
        else if (json.find("pathAdd") != std::string::npos) fire_spline_sidebar_cmd(dlg, kPathAdd);
        else if (json.find("pathConfig") != std::string::npos) fire_spline_sidebar_cmd(dlg, kPathEdit);
        else if (json.find("openSli") != std::string::npos) fire_spline_sidebar_cmd(dlg, kOpenSli);
        else if (json.find("\"name\":\"save\"") != std::string::npos) fire_spline_sidebar_cmd(dlg, kSave);
        else if (json.find("\"name\":\"cancel\"") != std::string::npos) fire_spline_sidebar_cmd(dlg, kCancel);
        sync_spline_creator_sidebar(st);
    }
}

int snap_index_from_state(const CreatorState* st) {
    if (!st || st->grid_off) return 4;
    static constexpr float gm[4] = {0.01f, 0.05f, 0.1f, 0.5f};
    for (int i = 0; i < 4; ++i)
        if (std::fabs(st->grid_m - gm[i]) < 1e-5f) return i;
    return 4;
}

void sync_spline_creator_chrome(CreatorState* st) {
    if (!st || !st->chrome_ui_ok || !st->chrome_ui.js_ready()) return;
    const std::string sli = wide_to_utf8(sli_text_for_edit(spline::format_sli(st->def)));
    bool draw_tex = st->draw_textures;
    bool draw_path = st->draw_paths;
    if (st->dlg) {
        if (HWND h = GetDlgItem(st->dlg, kDrawTex))
            draw_tex = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (HWND h = GetDlgItem(st->dlg, kDrawPath))
            draw_path = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    std::string js = "window.omsiApplyState&&window.omsiApplyState({";
    js += "snap:" + std::to_string(snap_index_from_state(st));
    js += ",pencil:" + std::string(st->pencil_mode ? "true" : "false");
    js += ",marquee:" + std::string(st->marquee_mode ? "true" : "false");
    js += ",drawTex:" + std::string(draw_tex ? "true" : "false");
    js += ",drawPath:" + std::string(draw_path ? "true" : "false");
    js += ",sliText:\"" + sc_json_escape(sli) + "\"});";
    st->chrome_ui.evaluate_script(js);
}

void handle_spline_chrome_json(CreatorState* st, HWND dlg, const std::string& json) {
    if (!st || !dlg) return;
    if (json.find("\"type\":\"ready\"") != std::string::npos) {
        sync_spline_creator_chrome(st);
        return;
    }
    if (json.find("\"type\":\"snap\"") != std::string::npos) {
        const auto pos = json.find("\"index\":");
        if (pos == std::string::npos) return;
        const int sel = std::atoi(json.c_str() + pos + 8);
        if (HWND grid = GetDlgItem(dlg, kGrid)) {
            SendMessageW(grid, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
            SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(kGrid, CBN_SELCHANGE), reinterpret_cast<LPARAM>(grid));
        }
        sync_spline_creator_chrome(st);
        return;
    }
    if (json.find("\"type\":\"cmd\"") != std::string::npos) {
        if (json.find("centerView") != std::string::npos) fire_spline_sidebar_cmd(dlg, kCenterView);
        else if (json.find("espejar") != std::string::npos) fire_spline_sidebar_cmd(dlg, kEspejar);
        else if (json.find("flip") != std::string::npos) fire_spline_sidebar_cmd(dlg, kFlip);
        else if (json.find("delPt") != std::string::npos) fire_spline_sidebar_cmd(dlg, kDelPt);
        else if (json.find("pencil") != std::string::npos) fire_spline_sidebar_cmd(dlg, kPencilDraw);
        else if (json.find("marquee") != std::string::npos) fire_spline_sidebar_cmd(dlg, kMarqueeSel);
        else if (json.find("drawTex") != std::string::npos) fire_spline_sidebar_cmd(dlg, kDrawTex);
        else if (json.find("drawPath") != std::string::npos) fire_spline_sidebar_cmd(dlg, kDrawPath);
        sync_spline_creator_chrome(st);
    }
}

BOOL CALLBACK hide_left_chrome_child(HWND h, LPARAM lp) {
    const HWND parent = reinterpret_cast<HWND>(lp);
    if (GetParent(h) != parent) return TRUE;
    wchar_t cls[32]{};
    GetClassNameW(h, cls, 32);
    if (wcscmp(cls, L"OmsiF4UlChild") == 0) return TRUE;
    if (wcscmp(cls, L"OmsiF4SplineCanvas") == 0 || wcscmp(cls, L"OmsiF4SplinePreview3D") == 0 ||
        wcscmp(cls, L"OmsiF4SplineTexPrev") == 0)
        return TRUE;
    RECT r{};
    GetWindowRect(h, &r);
    POINT tl{r.left, r.top};
    ScreenToClient(parent, &tl);
    if (tl.x < kRightX - 2 && tl.y >= kViewportH + 8) ShowWindow(h, SW_HIDE);
    return TRUE;
}

BOOL CALLBACK hide_right_panel_child(HWND h, LPARAM lp) {
    const HWND parent = reinterpret_cast<HWND>(lp);
    if (GetParent(h) != parent) return TRUE;
    wchar_t cls[32]{};
    GetClassNameW(h, cls, 32);
    if (wcscmp(cls, L"OmsiF4UlChild") == 0) return TRUE;
    RECT r{};
    GetWindowRect(h, &r);
    POINT tl{r.left, r.top};
    ScreenToClient(parent, &tl);
    if (tl.x >= kRightX - 2) ShowWindow(h, SW_HIDE);
    return TRUE;
}

#endif

} // namespace

bool run_spline_creator_dialog(HWND owner, const std::filesystem::path& omsi_root,
                               std::optional<spline::SplineDefinition> initial, SplineCreatorResult& out) {
    try {
    ensure_subclass(L"OmsiF4SplineCanvas", canvas_proc, reinterpret_cast<LPCWSTR>(IDC_CROSS));
    ensure_subclass(L"OmsiF4SplinePreview3D", preview3d_proc);
    ensure_subclass(L"OmsiF4SplineTexPrev", tex_preview_proc);
    ensure_subclass(L"OmsiF4SplineCreator", dlg_proc);

    CreatorState st{};
    g_st = &st;
    st.omsi_root = omsi_root;
    st.def = initial ? std::move(*initial) : default_street_profile();
    ensure_profile_labels(&st);

    // Controls are authored at the design client size; we create at that size (so the captured
    // layout is the design), then scale to whatever window size fits the screen.
    constexpr DWORD kDlgStyle = WS_OVERLAPPEDWINDOW & ~WS_VISIBLE;
    RECT wr{0, 0, kDesignW, kDesignH};
    AdjustWindowRectEx(&wr, kDlgStyle, FALSE, WS_EX_DLGMODALFRAME);
    const int W = wr.right - wr.left;
    const int H = wr.bottom - wr.top;
    const HWND owner_root = modal_block_root(owner);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"OmsiF4SplineCreator", L"Crear spline (.sli)",
                               kDlgStyle, CW_USEDEFAULT, CW_USEDEFAULT, W, H, owner_root, nullptr,
                               GetModuleHandleW(nullptr), &st);
    if (!dlg) {
        log::error("spline creator: CreateWindow failed");
        return false;
    }

    CreateWindowExW(WS_EX_CLIENTEDGE, L"OmsiF4SplineCanvas", L"", WS_CHILD | WS_TABSTOP, 12, 24, kViewportW,
                    kViewportH, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCanvas)), GetModuleHandleW(nullptr),
                    &st);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"OmsiF4SplinePreview3D", L"", WS_CHILD, 12 + kViewportW + 8, 24, kViewportW,
                    kViewportH, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreview3d)), GetModuleHandleW(nullptr),
                    &st);

    mk_static(dlg, L"2D", 12, 8, 28, 16);
    mk_static(dlg, L"3D", 12 + kViewportW + 8, 8, 28, 16);

    mk_static(dlg, L"Snap:", 12, kViewportH + 32, 70, 16);
    HWND grid = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 84, kViewportH + 28, 100,
                              160, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGrid)), GetModuleHandleW(nullptr),
                              nullptr);
    SendMessageW(grid, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"0.01 m"));
    SendMessageW(grid, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"0.05 m"));
    SendMessageW(grid, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"0.1 m"));
    SendMessageW(grid, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"0.5 m"));
    SendMessageW(grid, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Libre (sin snap)"));
    SendMessageW(grid, CB_SETCURSEL, 4, 0);
    st.grid_off = true;
    st.grid_m = 0.f;

    mk_btn(dlg, kCenterView, L"Centrar vista", 196, kViewportH + 26, 96, 24);
    constexpr int kIconBtn = 28;
    mk_icon_btn(dlg, kEspejar, st.espejar_bmp,
                resolve_tool_icon(L"spline_icon_espejar.png"), 298,
                kViewportH + 26, kIconBtn, kIconBtn, L"Espejar");
    mk_icon_btn(dlg, kFlip, st.flip_bmp,
                resolve_tool_icon(L"spline_icon_flip.png"),
                330, kViewportH + 26, kIconBtn, kIconBtn, L"Flip");
    mk_icon_btn(dlg, kDelPt, st.del_bmp,
                resolve_tool_icon(L"spline_icon_delete.png"), 362,
                kViewportH + 26, kIconBtn, kIconBtn, L"Borrar");
    mk_icon_btn(dlg, kPencilDraw, st.pencil_bmp, resolve_tool_icon(L"spline_pencil_tool.png"), 394, kViewportH + 26,
                kIconBtn, kIconBtn, L"\u270E", true);
    mk_icon_btn(dlg, kMarqueeSel, st.marquee_bmp,
                resolve_tool_icon(L"spline_icon_select.png"),
                426, kViewportH + 26, kIconBtn, kIconBtn, L"A", true);
    mk_icon_btn(dlg, kDrawTex, st.draw_tex_bmp,
                resolve_tool_icon(L"spline_icon_texture.png"),
                458, kViewportH + 26, kIconBtn, kIconBtn, L"T", true);
    mk_icon_btn(dlg, kDrawPath, st.draw_path_bmp,
                resolve_tool_icon(L"spline_icon_path.webp"),
                490, kViewportH + 26, kIconBtn, kIconBtn, L"P", true);
    SendMessageW(GetDlgItem(dlg, kDrawTex), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(GetDlgItem(dlg, kDrawPath), BM_SETCHECK, BST_CHECKED, 0);

    mk_static(dlg, L"Spline (.sli)", 12, kViewportH + 58, 140, 16);
    st.pt_list = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 12, kViewportH + 76,
        kLeftPanelW, kDesignH - kViewportH - 88, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPtList)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(st.pt_list, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(ANSI_FIXED_FONT)), TRUE);

    build_phase2_ui(dlg, &st);
    build_parity_ui(dlg, &st);

    center_view(&st);
    sync_hp_edits(dlg, &st);
    refill_textures(&st);
    load_texture_previews(&st);
    refill_pt_list(&st);
    refill_wrap_list(&st);
    refill_hp_list(&st);
    refresh_phase2_ui(&st);
    sync_tex_slot_ui(&st);
    sync_path_field_ui(&st);
    sync_texture_flags_ui(&st);
    refresh_warnings(&st);

    // Capture the design layout, then size the window to the screen work area (layout scales to fit).
    capture_child_layout(&st);
#ifdef OMSI_ULTRALIGHT_UI
    {
        const int rx = kRightX;
        const int rw = kDesignW - rx - 8;
        const int rh = kDesignH - 16;
        st.sidebar_ui.set_event_callback([&st, dlg](const std::string& json) {
            handle_spline_sidebar_json(&st, dlg, json);
        });
        st.sidebar_ui_ok = st.sidebar_ui.create(dlg, rx, 8, rw, rh, L"spline_creator");
        if (st.sidebar_ui_ok) {
            EnumChildWindows(dlg, hide_right_panel_child, reinterpret_cast<LPARAM>(dlg));
            log::info("Spline creator sidebar: Ultralight/Tailwind UI active");
        } else {
            log::warn("Spline creator sidebar: Ultralight unavailable — Win32 panel");
        }
        const int chrome_y = kViewportH + 24;
        const int chrome_h = kDesignH - chrome_y - 8;
        st.chrome_ui.set_event_callback([&st, dlg](const std::string& json) {
            handle_spline_chrome_json(&st, dlg, json);
        });
        st.chrome_ui_ok =
            st.chrome_ui.create(dlg, 12, chrome_y, kLeftPanelW, chrome_h, L"spline_creator", "chrome.html",
                                 "chrome.js");
        if (st.chrome_ui_ok) {
            EnumChildWindows(dlg, hide_left_chrome_child, reinterpret_cast<LPARAM>(dlg));
            log::info("Spline creator chrome: HTML toolbar + .sli preview active");
        } else {
            log::warn("Spline creator chrome: Ultralight unavailable — Win32 toolbar");
        }
    }
#endif
    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) work = {0, 0, 1280, 800};
    const int work_w = work.right - work.left;
    const int work_h = work.bottom - work.top;
    const int win_w = (std::min)(W, work_w);
    const int win_h = (std::min)(H, work_h);
    int win_x = work.left + (work_w - win_w) / 2;
    int win_y = work.top + (work_h - win_h) / 2;
    if (owner) {
        RECT orc{};
        if (GetWindowRect(owner, &orc)) {
            win_x = (std::clamp)(orc.left + ((orc.right - orc.left) - win_w) / 2, work.left,
                                 (std::max)(work.left, work.right - win_w));
            win_y = (std::clamp)(orc.top + ((orc.bottom - orc.top) - win_h) / 2, work.top,
                                 (std::max)(work.top, work.bottom - win_h));
        }
    }
    SetWindowPos(dlg, nullptr, win_x, win_y, win_w, win_h, SWP_NOZORDER | SWP_NOACTIVATE);
    layout_dialog(&st);
#ifdef OMSI_ULTRALIGHT_UI
    sync_spline_creator_sidebar(&st);
    sync_spline_creator_chrome(&st);
#endif
    ShowWindow(st.canvas, SW_SHOW);
    ShowWindow(st.preview3d, SW_SHOW);
    if (st.tex_preview) ShowWindow(st.tex_preview, SW_SHOW);

    const bool ok = pump_modal(owner, dlg, &st);
    free_texture_previews(&st);
    free_thumbs(&st);
    free_tool_bitmaps(&st);
    g_st = nullptr;
    if (ok && st.saved) {
        out.ok = true;
        out.def = st.def;
        out.saved_path = st.save_path;
    }
    DestroyWindow(dlg);
    return ok && st.saved;
    } catch (const std::exception& ex) {
        log::error(std::string("spline creator: ") + ex.what());
        g_st = nullptr;
        MessageBoxW(owner, L"Error al abrir el editor de spline.", L"Crear .sli", MB_OK | MB_ICONERROR);
        return false;
    } catch (...) {
        log::error("spline creator: unknown exception");
        g_st = nullptr;
        MessageBoxW(owner, L"Error al abrir el editor de spline.", L"Crear .sli", MB_OK | MB_ICONERROR);
        return false;
    }
}

bool run_spline_creator_self_test(const std::filesystem::path& omsi_root, HWND owner) {
    g_run_self_test = true;
    g_self_test_result = false;
    SplineCreatorResult dummy{};
    (void)run_spline_creator_dialog(owner, omsi_root, std::nullopt, dummy);
    g_run_self_test = false;
    return g_self_test_result;
}

} // namespace omsi::ui
