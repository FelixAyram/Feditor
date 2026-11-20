#include "ui/win32_dark.hpp"

namespace omsi::ui::dark {
namespace {

constexpr COLORREF kBg = RGB(0x21, 0x1D, 0x19);     // base (theme.hpp base)
constexpr COLORREF kPanel = RGB(0x2A, 0x25, 0x20);  // surface
constexpr COLORREF kText = RGB(0xED, 0xE4, 0xD8);   // text

HBRUSH g_bg = nullptr;
HBRUSH g_panel = nullptr;

} // namespace

HBRUSH bg_brush() {
    if (!g_bg) g_bg = CreateSolidBrush(kBg);
    return g_bg;
}
HBRUSH panel_brush() {
    if (!g_panel) g_panel = CreateSolidBrush(kPanel);
    return g_panel;
}

void apply(HWND hwnd) {
    if (!hwnd) return;
    // Dynamic load so we do not add a dwmapi.lib link dependency.
    using DwmSetAttrFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        auto fn = reinterpret_cast<DwmSetAttrFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (fn) {
            BOOL on = TRUE;
            constexpr DWORD kDarkMode = 20;          // DWMWA_USE_IMMERSIVE_DARK_MODE
            constexpr DWORD kDarkModeOld = 19;       // pre-20H1 name
            fn(hwnd, kDarkMode, &on, sizeof(on));
            fn(hwnd, kDarkModeOld, &on, sizeof(on));
        }
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

void center(HWND hwnd) {
    if (!hwnd) return;
    RECT r{};
    GetWindowRect(hwnd, &r);
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(mon, &mi)) return;
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool handle_ctlcolor(UINT msg, WPARAM wp, LRESULT& out) {
    HDC dc = reinterpret_cast<HDC>(wp);
    switch (msg) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor(dc, kText);
        SetBkColor(dc, kBg);
        out = reinterpret_cast<LRESULT>(bg_brush());
        return true;
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
        SetTextColor(dc, kText);
        SetBkColor(dc, kPanel);
        out = reinterpret_cast<LRESULT>(panel_brush());
        return true;
    default:
        return false;
    }
}

bool handle_erase(HWND hwnd, WPARAM wp, LRESULT& out) {
    HDC dc = reinterpret_cast<HDC>(wp);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, bg_brush());
    out = 1;
    return true;
}

} // namespace omsi::ui::dark
