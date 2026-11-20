#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "editor_prop_dialogs.hpp"
#include "omsi/utf8_win32.hpp"

#include <algorithm>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace omsi::ui {
namespace {

// Avoid Win32 IDC_* cursor macros (IDC_CROSS, etc.).
constexpr int kOkBar = 9001;
constexpr int kCancelBar = 9002;
constexpr int kDensity = 9010;
constexpr int kUvg = 9011;
constexpr int kSpeed = 9012;
constexpr int kPrio = 9013;
constexpr int kCross = 9020;
constexpr int kSpeedOn = 9021;
constexpr int kPrioOn = 9022;
constexpr int kNoCars = 9023;
constexpr int kBuses = 9024;
constexpr int kTrucks = 9025;
constexpr int kOvertake = 9026;
constexpr int kLines = 9100;

struct ModalState {
    bool done = false;
    bool ok = false;
};

HWND mk_edit(HWND p, int id, int x, int y, int w, int h) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y, w, h,
                           p, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                           nullptr);
}

HWND mk_check(HWND p, int id, const wchar_t* text, int x, int y, int w, int h, bool on) {
    HWND c = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, x, y, w, h, p,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                           nullptr);
    SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    return c;
}

HWND mk_action(HWND p, int id, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOTIFY | SS_CENTERIMAGE, x, y,
                         w, h, p, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                         nullptr);
}

void set_float(HWND e, float v) {
    wchar_t buf[64]{};
    swprintf_s(buf, L"%.4g", static_cast<double>(v));
    SetWindowTextW(e, buf);
}

float get_float(HWND e, float fb) {
    wchar_t buf[64]{};
    if (!e) return fb;
    GetWindowTextW(e, buf, 64);
    try {
        return std::stof(std::wstring(buf));
    } catch (...) {
        return fb;
    }
}

bool is_checked(HWND h) { return h && SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }

LRESULT CALLBACK modal_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<ModalState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        HWND child = reinterpret_cast<HWND>(lp);
        const int id = GetDlgCtrlID(child);
        if (id == kOkBar) {
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkColor(hdc, RGB(32, 120, 72));
            static HBRUSH br = CreateSolidBrush(RGB(32, 120, 72));
            return reinterpret_cast<LRESULT>(br);
        }
        if (id == kCancelBar) {
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkColor(hdc, RGB(120, 40, 40));
            static HBRUSH br = CreateSolidBrush(RGB(120, 40, 40));
            return reinterpret_cast<LRESULT>(br);
        }
        break;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (st && (code == STN_CLICKED || code == BN_CLICKED)) {
            if (id == kOkBar) {
                st->ok = true;
                st->done = true;
            } else if (id == kCancelBar) {
                st->ok = false;
                st->done = true;
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (st) {
            if (wp == VK_RETURN) {
                st->ok = true;
                st->done = true;
            } else if (wp == VK_ESCAPE) {
                st->ok = false;
                st->done = true;
            }
        }
        return 0;
    case WM_CLOSE:
        if (st) {
            st->ok = false;
            st->done = true;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool pump_modal(HWND owner, HWND dlg, ModalState& st) {
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)) {
            SendMessageW(dlg, WM_KEYDOWN, msg.wParam, msg.lParam);
            continue;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner) EnableWindow(owner, TRUE);
    return st.ok;
}

void ensure_class() {
    static bool reg = false;
    if (reg) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = modal_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OmsiF4PropDlg";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    reg = true;
}

} // namespace

bool run_path_rule_dialog(HWND owner, EditorPathRuleProps& io) {
    ensure_class();
    ModalState st{};
    constexpr int W = 440, H = 430;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"OmsiF4PropDlg", L"Reglas del riel",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, W, H, owner,
                               nullptr, GetModuleHandleW(nullptr), &st);
    if (!dlg) return false;

    std::wstring title = L"Path ";
    title += std::to_wstring(io.path_index);
    title += L"  |  ";
    title += std::wstring(io.label.begin(), io.label.end());
    CreateWindowW(L"STATIC", title.c_str(), WS_CHILD | WS_VISIBLE, 12, 10, W - 40, 36, dlg, nullptr,
                  GetModuleHandleW(nullptr), nullptr);

    mk_check(dlg, kCross, L"Tachar riel (sin AI / dens. 0)", 12, 52, 400, 22, io.crossed_out);
    CreateWindowW(L"STATIC", L"Densidad 0..1", WS_CHILD | WS_VISIBLE, 12, 86, 100, 18, dlg, nullptr,
                  GetModuleHandleW(nullptr), nullptr);
    HWND e_den = mk_edit(dlg, kDensity, 120, 84, 80, 22);
    set_float(e_den, io.density);
    CreateWindowW(L"STATIC", L"UVG", WS_CHILD | WS_VISIBLE, 220, 86, 40, 18, dlg, nullptr,
                  GetModuleHandleW(nullptr), nullptr);
    HWND e_uvg = mk_edit(dlg, kUvg, 260, 84, 60, 22);
    set_float(e_uvg, static_cast<float>(io.density_uvg));

    mk_check(dlg, kSpeedOn, L"Velocidad km/h", 12, 120, 130, 22, io.has_speed);
    HWND e_spd = mk_edit(dlg, kSpeed, 150, 118, 80, 22);
    set_float(e_spd, io.speed_kmh);
    mk_check(dlg, kPrioOn, L"Prioridad", 12, 152, 100, 22, io.has_priority);
    HWND e_prio = mk_edit(dlg, kPrio, 120, 150, 80, 22);
    set_float(e_prio, io.priority);

    CreateWindowW(L"STATIC", L"Restricciones / tipo", WS_CHILD | WS_VISIBLE, 12, 186, 200, 18, dlg, nullptr,
                  GetModuleHandleW(nullptr), nullptr);
    mk_check(dlg, kNoCars, L"No autos", 12, 210, 100, 22, io.no_cars);
    mk_check(dlg, kBuses, L"Buses", 120, 210, 80, 22, io.buses);
    mk_check(dlg, kTrucks, L"Camiones", 210, 210, 100, 22, io.trucks);
    mk_check(dlg, kOvertake, L"Prohibido adelantar", 12, 238, 220, 22, io.no_overtake);

    mk_action(dlg, kOkBar, L"GUARDAR  (Enter)", 12, H - 78, 200, 40);
    mk_action(dlg, kCancelBar, L"CANCELAR  (Esc)", 222, H - 78, 190, 40);

    if (!pump_modal(owner, dlg, st)) {
        DestroyWindow(dlg);
        return false;
    }

    io.crossed_out = is_checked(GetDlgItem(dlg, kCross));
    io.density = get_float(e_den, io.density);
    io.density_uvg = static_cast<int>(get_float(e_uvg, static_cast<float>(io.density_uvg)));
    io.has_speed = is_checked(GetDlgItem(dlg, kSpeedOn));
    io.speed_kmh = get_float(e_spd, io.speed_kmh);
    io.has_priority = is_checked(GetDlgItem(dlg, kPrioOn));
    io.priority = get_float(e_prio, io.priority);
    io.no_cars = is_checked(GetDlgItem(dlg, kNoCars));
    io.buses = is_checked(GetDlgItem(dlg, kBuses));
    io.trucks = is_checked(GetDlgItem(dlg, kTrucks));
    io.no_overtake = is_checked(GetDlgItem(dlg, kOvertake));
    if (io.crossed_out) io.density = 0.f;
    io.valid = true;
    DestroyWindow(dlg);
    return true;
}

bool run_extra_lines_dialog(HWND owner, EditorObjectProps& io) {
    // OMSI Form_objlabels: ValueListEditor (label|value) + read-only memo + OK/Cancel.
    ensure_class();
    ModalState st{};
    constexpr int W = 560, H = 430;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"OmsiF4PropDlg", L"Object Labels",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, W, H, owner,
                               nullptr, GetModuleHandleW(nullptr), &st);
    if (!dlg) return false;

    CreateWindowW(L"STATIC",
                  L"Every line = one label, please type '@' to add further lines to a label.",
                  WS_CHILD | WS_VISIBLE, 8, 8, W - 30, 18, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    CreateWindowW(L"STATIC", L"Schl\u00fcssel", WS_CHILD | WS_VISIBLE, 8, 32, 270, 16, dlg, nullptr,
                  GetModuleHandleW(nullptr), nullptr);
    CreateWindowW(L"STATIC", L"Wert", WS_CHILD | WS_VISIBLE, 290, 32, 250, 16, dlg, nullptr,
                  GetModuleHandleW(nullptr), nullptr);

    HWND e_lines = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
                                       ES_WANTRETURN,
                                   8, 50, W - 30, 160, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLines)),
                                   GetModuleHandleW(nullptr), nullptr);

    std::wstring body;
    auto labels = io.string_labels;
    if (labels.empty() && !io.traffic_light_names.empty()) labels.emplace_back("Nr. Traffic Light");
    const std::size_t n = (std::max)(labels.size(), io.strings.size());
    const std::size_t rows = (std::max)(n, std::size_t{1});
    for (std::size_t i = 0; i < rows; ++i) {
        std::string key = (i < labels.size()) ? labels[i] : ("string" + std::to_string(i));
        std::string val = (i < io.strings.size()) ? io.strings[i] : "";
        if (i) body += L"\r\n";
        body += std::wstring(key.begin(), key.end()) + L"=" + std::wstring(val.begin(), val.end());
    }
    SetWindowTextW(e_lines, body.c_str());

    HWND memo = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_READONLY |
                                    ES_AUTOVSCROLL,
                                8, 220, W - 30, 130, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!io.traffic_light_names.empty()) {
        std::wstring help = L"Traffic Light Indices:\r\n";
        for (std::size_t i = 0; i < io.traffic_light_names.size(); ++i) {
            help += std::to_wstring(i) + L": ";
            help += std::wstring(io.traffic_light_names[i].begin(), io.traffic_light_names[i].end());
            help += L"\r\n";
        }
        SetWindowTextW(memo, help.c_str());
    } else {
        SetWindowTextW(memo,
                       L".map layout (Unit126 Part5):\r\n"
                       L"  <string_count>\r\n"
                       L"  <string0>\r\n"
                       L"  <string1>\r\n"
                       L"  ...\r\n"
                       L"Keys come from SCO [stringvarnamelist]; only values are stored in the tile .map.");
    }

    mk_action(dlg, kOkBar, L"OK", 8, H - 55, 90, 32);
    mk_action(dlg, kCancelBar, L"Cancel", 108, H - 55, 90, 32);

    if (!pump_modal(owner, dlg, st)) {
        DestroyWindow(dlg);
        return false;
    }

    const int len = GetWindowTextLengthW(e_lines);
    std::wstring w(static_cast<std::size_t>(len) + 1, L'\0');
    if (len > 0) GetWindowTextW(e_lines, w.data(), len + 1);
    w.resize(static_cast<std::size_t>(len));
    io.strings.clear();
    std::size_t start_i = 0;
    while (start_i <= w.size()) {
        const auto end_i = w.find(L'\n', start_i);
        std::wstring row =
            w.substr(start_i, end_i == std::wstring::npos ? std::wstring::npos : end_i - start_i);
        if (!row.empty() && row.back() == L'\r') row.pop_back();
        std::string line = wide_to_utf8(row);
        const auto eq = line.find('=');
        if (eq != std::string::npos) line = line.substr(eq + 1);
        io.strings.push_back(line);
        if (end_i == std::wstring::npos) break;
        start_i = end_i + 1;
    }
    while (!io.strings.empty() && io.strings.back().empty()) io.strings.pop_back();
    io.valid = true;
    DestroyWindow(dlg);
    return true;
}

int run_entrypoint_picker_dialog(HWND owner, const map::GlobalMap& global,
                                 const std::vector<map::EntryPoint>& entries, int current_sel) {
    if (entries.empty()) {
        MessageBoxW(owner, L"No hay entrypoints en global.cfg", L"Entrypoints", MB_OK | MB_ICONINFORMATION);
        return -1;
    }
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = modal_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"OmsiF4EntrypointDlg";
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        reg = true;
    }
    ModalState st{};
    constexpr int kList = 9200;
    constexpr int W = 520;
    constexpr int H = 360;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"OmsiF4EntrypointDlg", L"Entrypoints",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, W, H, owner, nullptr,
                               GetModuleHandleW(nullptr), &st);
    if (!dlg) return -1;
    if (owner) {
        RECT rc{};
        GetWindowRect(owner, &rc);
        SetWindowPos(dlg, nullptr, (rc.left + rc.right - W) / 2, (rc.top + rc.bottom - H) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);
    }
    const std::string map_folder = global.root.filename().string();
    CreateWindowW(L"STATIC", L"Elegí un entrypoint (Enter / doble click / Ir):", WS_CHILD | WS_VISIBLE, 12, 10,
                  490, 18, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 12, 34,
                                490, 240, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)),
                                GetModuleHandleW(nullptr), nullptr);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& ep = entries[i];
        std::string tile_tag;
        if (ep.category >= 0 && static_cast<std::size_t>(ep.category) < global.tiles.size()) {
            const auto& ref = global.tiles[static_cast<std::size_t>(ep.category)];
            tile_tag = ref.tile_path.stem().string();
        }
        wchar_t buf[384]{};
        if (!ep.name.empty()) {
            if (!tile_tag.empty())
                swprintf_s(buf, L"%zu  id=%d  %hs  [%hs / %hs]", i, ep.id, ep.name.c_str(), map_folder.c_str(),
                           tile_tag.c_str());
            else
                swprintf_s(buf, L"%zu  id=%d  %hs  [%hs]", i, ep.id, ep.name.c_str(), map_folder.c_str());
        } else {
            if (!tile_tag.empty())
                swprintf_s(buf, L"%zu  id=%d  (%.0f, %.0f)  [%hs / %hs]", i, ep.id, ep.position.x, ep.position.y,
                           map_folder.c_str(), tile_tag.c_str());
            else
                swprintf_s(buf, L"%zu  id=%d  (%.0f, %.0f)  [%hs]", i, ep.id, ep.position.x, ep.position.y,
                           map_folder.c_str());
        }
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }
    if (current_sel < 0) current_sel = 0;
    if (current_sel >= static_cast<int>(entries.size())) current_sel = 0;
    SendMessageW(list, LB_SETCURSEL, current_sel, 0);
    SetFocus(list);
    mk_action(dlg, kOkBar, L"Ir", 12, H - 55, 100, 32);
    mk_action(dlg, kCancelBar, L"Cancelar", 122, H - 55, 100, 32);

    SetWindowSubclass(
        list,
        [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) -> LRESULT {
            if (msg == WM_LBUTTONDBLCLK) {
                HWND parent = GetParent(hwnd);
                if (parent) SendMessageW(parent, WM_COMMAND, MAKEWPARAM(kOkBar, STN_CLICKED), 0);
                return 0;
            }
            if (msg == WM_KEYDOWN && wp == VK_RETURN) {
                HWND parent = GetParent(hwnd);
                if (parent) SendMessageW(parent, WM_COMMAND, MAKEWPARAM(kOkBar, STN_CLICKED), 0);
                return 0;
            }
            return DefSubclassProc(hwnd, msg, wp, lp);
        },
        1, 0);

    if (!pump_modal(owner, dlg, st)) {
        DestroyWindow(dlg);
        return -1;
    }
    int sel = -1;
    if (st.ok) sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
    DestroyWindow(dlg);
    return sel;
}

bool run_editor_options_dialog(HWND owner, int& io_tile_radius, int& io_visual_radius_m,
                               float* io_union_ball_pct, float* io_grad_circle_pct) {
    // Replaced by ImGui modal (Vista → Opciones). Keep symbol for link compatibility.
    (void)owner;
    (void)io_tile_radius;
    (void)io_visual_radius_m;
    (void)io_union_ball_pct;
    (void)io_grad_circle_pct;
    return false;
#if 0
    ensure_class();
    ModalState st{};
    constexpr int kRadius = 9300;
    constexpr int kVisualRadius = 9301;
    constexpr int kUnionBall = 9302;
    constexpr int kGradCircle = 9303;
    constexpr int W = 360;
    constexpr int H = 380;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"OmsiF4PropDlg", L"Opciones del editor",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, W, H, owner,
                               nullptr, GetModuleHandleW(nullptr), &st);
    if (!dlg) return false;
    if (owner) {
        RECT rc{};
        GetWindowRect(owner, &rc);
        SetWindowPos(dlg, nullptr, (rc.left + rc.right - W) / 2, (rc.top + rc.bottom - H) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);
    }
    CreateWindowW(L"STATIC", L"Radio de tiles (Chebyshev):", WS_CHILD | WS_VISIBLE, 12, 12, 320, 18, dlg,
                  nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND lbl = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 32, 320, 18, dlg, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    HWND track = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 12, 52, 320, 28, dlg,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRadius)), GetModuleHandleW(nullptr),
                               nullptr);
    SendMessageW(track, TBM_SETRANGE, TRUE, MAKELPARAM(1, 20));
    int pos = io_tile_radius;
    if (pos < 1) pos = 1;
    if (pos > 20) pos = 20;
    SendMessageW(track, TBM_SETPOS, TRUE, pos);
    auto update_lbl = [&](int r) {
        const int side = 2 * r + 1;
        const int count = side * side;
        wchar_t buf[128]{};
        swprintf_s(buf, L"Radio %d  →  %d×%d = %d tiles", r, side, side, count);
        SetWindowTextW(lbl, buf);
    };
    update_lbl(pos);

    CreateWindowW(L"STATIC", L"Radio visual .sco/.sli (m, 0 = sin límite):", WS_CHILD | WS_VISIBLE, 12, 90, 320,
                  18, dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND vis_lbl = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 110, 320, 18, dlg, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    HWND vis_track = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 12, 130, 320, 28,
                                 dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVisualRadius)),
                                 GetModuleHandleW(nullptr), nullptr);
    SendMessageW(vis_track, TBM_SETRANGE, TRUE, MAKELPARAM(0, 40));
    auto meters_to_pos = [](int m) {
        if (m <= 0) return 0;
        const int p = (m + 25) / 50;
        return (std::max)(1, (std::min)(40, p));
    };
    auto pos_to_meters = [](int p) { return p <= 0 ? 0 : p * 50; };
    int vis_pos = meters_to_pos(io_visual_radius_m);
    SendMessageW(vis_track, TBM_SETPOS, TRUE, vis_pos);
    auto update_vis_lbl = [&](int p) {
        wchar_t buf[128]{};
        if (p <= 0)
            swprintf_s(buf, L"Sin límite (carga todo lo streamado)");
        else
            swprintf_s(buf, L"%d m — solo objetos .sco y splines .sli", pos_to_meters(p));
        SetWindowTextW(vis_lbl, buf);
    };
    update_vis_lbl(vis_pos);

    CreateWindowW(L"STATIC", L"Tamaño bola unión (% pantalla):", WS_CHILD | WS_VISIBLE, 12, 170, 320, 18, dlg,
                  nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND union_lbl = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 190, 320, 18, dlg, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
    HWND union_track = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 12, 210, 320,
                                     28, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUnionBall)),
                                     GetModuleHandleW(nullptr), nullptr);
    SendMessageW(union_track, TBM_SETRANGE, TRUE, MAKELPARAM(1, 10));
    float union_pct = io_union_ball_pct ? *io_union_ball_pct : 0.02f;
    int union_pos = static_cast<int>(std::lround(union_pct * 200.f));
    if (union_pos < 1) union_pos = 1;
    if (union_pos > 10) union_pos = 10;
    SendMessageW(union_track, TBM_SETPOS, TRUE, union_pos);
    auto update_union_lbl = [&](int p) {
        wchar_t buf[64]{};
        swprintf_s(buf, L"%.1f %%  (bola de unión prev/next)", p * 0.5);
        SetWindowTextW(union_lbl, buf);
    };
    update_union_lbl(union_pos);

    CreateWindowW(L"STATIC", L"Tamaño círculo gradiente (% pantalla):", WS_CHILD | WS_VISIBLE, 12, 248, 320, 18,
                  dlg, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND grad_lbl = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 268, 320, 18, dlg, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    HWND grad_track = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 12, 288, 320,
                                    28, dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGradCircle)),
                                    GetModuleHandleW(nullptr), nullptr);
    SendMessageW(grad_track, TBM_SETRANGE, TRUE, MAKELPARAM(1, 10));
    float grad_pct = io_grad_circle_pct ? *io_grad_circle_pct : 0.01f;
    int grad_pos = static_cast<int>(std::lround(grad_pct * 200.f));
    if (grad_pos < 1) grad_pos = 1;
    if (grad_pos > 10) grad_pos = 10;
    SendMessageW(grad_track, TBM_SETPOS, TRUE, grad_pos);
    auto update_grad_lbl = [&](int p) {
        wchar_t buf[64]{};
        swprintf_s(buf, L"%.1f %%  (círculo de gradiente / centro)", p * 0.5);
        SetWindowTextW(grad_lbl, buf);
    };
    update_grad_lbl(grad_pos);

    mk_action(dlg, kOkBar, L"OK", 12, H - 52, 90, 28);
    mk_action(dlg, kCancelBar, L"Cancelar", 110, H - 52, 90, 28);
    if (!pump_modal(owner, dlg, st)) {
        DestroyWindow(dlg);
        return false;
    }
    if (st.ok) {
        pos = static_cast<int>(SendMessageW(track, TBM_GETPOS, 0, 0));
        if (pos < 1) pos = 1;
        if (pos > 20) pos = 20;
        io_tile_radius = pos;
        vis_pos = static_cast<int>(SendMessageW(vis_track, TBM_GETPOS, 0, 0));
        if (vis_pos < 0) vis_pos = 0;
        if (vis_pos > 40) vis_pos = 40;
        io_visual_radius_m = pos_to_meters(vis_pos);
        union_pos = static_cast<int>(SendMessageW(union_track, TBM_GETPOS, 0, 0));
        if (union_pos < 1) union_pos = 1;
        if (union_pos > 10) union_pos = 10;
        if (io_union_ball_pct) *io_union_ball_pct = union_pos * 0.005f;
        grad_pos = static_cast<int>(SendMessageW(grad_track, TBM_GETPOS, 0, 0));
        if (grad_pos < 1) grad_pos = 1;
        if (grad_pos > 10) grad_pos = 10;
        if (io_grad_circle_pct) *io_grad_circle_pct = grad_pos * 0.005f;
    }
    DestroyWindow(dlg);
    return st.ok;
#endif
}

} // namespace omsi::ui
