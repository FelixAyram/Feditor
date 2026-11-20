#include "loading_screen.hpp"
#include "omsi/utf8_win32.hpp"

#include <windows.h>
#include <commctrl.h>

#include <string>

#pragma comment(lib, "comctl32.lib")

namespace omsi::ui {
namespace {

constexpr int IDC_BAR = 2001;
constexpr int IDC_LABEL = 2002;

std::wstring to_wide(const std::string& s) { return omsi::utf8_to_wide(s); }

LRESULT CALLBACK LoadingWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

bool LoadingScreen::open(const wchar_t* title) {
    if (hwnd_) {
        if (title) SetWindowTextW(static_cast<HWND>(hwnd_), title);
        ShowWindow(static_cast<HWND>(hwnd_), SW_SHOW);
        pump_messages();
        return true;
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    const wchar_t* cls = L"OmsiF4Loading";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = LoadingWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    RegisterClassExW(&wc);

    const int win_w = 520, win_h = 140;
    const int scr_w = GetSystemMetrics(SM_CXSCREEN);
    const int scr_h = GetSystemMetrics(SM_CYSCREEN);
    const int win_x = (std::max)(0, (scr_w - win_w) / 2);
    const int win_y = (std::max)(0, (scr_h - win_h) / 2);
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, cls, title,
                                WS_OVERLAPPED | WS_CAPTION, win_x, win_y, win_w, win_h, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) return false;

    HWND label = CreateWindowExW(0, L"STATIC", L"Iniciando...", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 16, 12,
                                 480, 36, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LABEL)), nullptr,
                                 nullptr);
    HWND bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 16, 56, 480, 24, hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BAR)), nullptr, nullptr);
    SendMessageW(bar, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    hwnd_ = hwnd;
    bar_ = bar;
    label_ = label;
    pump_messages();
    return true;
}

void LoadingScreen::update(float fraction, const std::string& status_utf8) {
    if (!hwnd_ || !bar_) return;
    const int pos = static_cast<int>(fraction * 1000.f);
    SendMessageW(static_cast<HWND>(bar_), PBM_SETPOS, pos, 0);
    if (label_) SetWindowTextW(static_cast<HWND>(label_), to_wide(status_utf8).c_str());
    pump_messages();
}

void LoadingScreen::close() {
    if (hwnd_) DestroyWindow(static_cast<HWND>(hwnd_));
    hwnd_ = nullptr;
    bar_ = nullptr;
    label_ = nullptr;
}

void LoadingScreen::pump_messages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

} // namespace omsi::ui
