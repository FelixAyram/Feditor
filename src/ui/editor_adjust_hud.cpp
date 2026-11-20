#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "editor_adjust_hud.hpp"

#include <commctrl.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "comctl32.lib")

namespace omsi::ui {
namespace {

constexpr int kCurEdit = 9201;
constexpr int kCurUp = 9202;
constexpr int kCurDn = 9203;

constexpr DWORD kStyle = WS_POPUP | WS_BORDER | WS_CAPTION | WS_SYSMENU;
constexpr DWORD kExStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;

float read_edit_float(HWND e, float fb) {
    if (!e) return fb;
    wchar_t buf[64]{};
    GetWindowTextW(e, buf, 64);
    try {
        return std::stof(std::wstring(buf));
    } catch (...) {
        return fb;
    }
}

void set_edit_float(HWND e, float v) {
    if (!e) return;
    wchar_t buf[64]{};
    swprintf_s(buf, L"%.4g", static_cast<double>(v));
    SetWindowTextW(e, buf);
}

EditorAdjustHud* g_self = nullptr;
WNDPROC g_edit_prev = nullptr;
float g_prev_fallback = 0.f;

LRESULT CALLBACK edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN && g_self) {
        g_self->request_commit(read_edit_float(hwnd, g_prev_fallback));
        g_self->set_current(read_edit_float(hwnd, g_prev_fallback), true);
        return 0;
    }
    return CallWindowProcW(g_edit_prev, hwnd, msg, wp, lp);
}

LRESULT CALLBACK hud_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<EditorAdjustHud*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<EditorAdjustHud*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return 0;
    }
    case WM_COMMAND: {
        if (!self) return 0;
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (id == kCurEdit) {
            if (code == EN_SETFOCUS) self->notify_edit_setfocus();
            else if (code == EN_CHANGE) self->notify_edit_change();
            return 0;
        }
        if (code != BN_CLICKED) return 0;
        if (id == kCurUp) {
            self->nudge_current(0.1f);
            return 0;
        }
        if (id == kCurDn) {
            self->nudge_current(-0.1f);
            return 0;
        }
        return 0;
    }
    case WM_DESTROY:
        if (self) self->on_destroy();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ensure_class() {
    static bool reg = false;
    if (reg) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = hud_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OmsiF4AdjustHud";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    reg = true;
}

} // namespace

void EditorAdjustHud::show(HWND owner, Mode mode, float previous, float current) {
    if (visible() && mode_ == mode && owner_ == owner) {
        set_previous(previous);
        set_current(current, true);
        return;
    }
    if (visible()) hide();
    ensure_class();
    owner_ = owner;
    mode_ = mode;
    previous_ = previous;
    g_prev_fallback = previous;
    edit_dirty_ = false;
    commit_pending_ = false;
    g_self = this;

    constexpr int kClientW = 268;
    constexpr int kClientH = 56;
    RECT wr{};
    int x = 40, y = 80;
    RECT frame{0, 0, kClientW, kClientH};
    AdjustWindowRectEx(&frame, kStyle, FALSE, kExStyle);
    const int w = frame.right - frame.left;
    const int h = frame.bottom - frame.top;
    if (owner && GetWindowRect(owner, &wr)) {
        x = wr.left + 12;
        y = wr.bottom - h - 48;
    }

    const wchar_t* title = mode == Mode::Height ? L"Ajuste altura (Z)" : L"Ajuste rotacion";
    hwnd_ = CreateWindowExW(kExStyle, L"OmsiF4AdjustHud", title, kStyle, x, y, w, h, owner, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) return;

    const wchar_t* prev_label = mode == Mode::Height ? L"Altura anterior:" : L"Rotacion anterior:";
    const wchar_t* cur_label = mode == Mode::Height ? L"Altura actual:" : L"Rotacion actual:";
    CreateWindowW(L"STATIC", prev_label, WS_CHILD | WS_VISIBLE, 8, 8, 112, 16, hwnd_, nullptr,
                  GetModuleHandleW(nullptr), nullptr);
    wchar_t prev_buf[64]{};
    swprintf_s(prev_buf, L"%.4g", static_cast<double>(previous));
    prev_lbl_ = CreateWindowW(L"STATIC", prev_buf, WS_CHILD | WS_VISIBLE | SS_SUNKEN, 124, 6, 88, 18, hwnd_,
                              nullptr, GetModuleHandleW(nullptr), nullptr);
    CreateWindowW(L"STATIC", cur_label, WS_CHILD | WS_VISIBLE, 8, 32, 112, 16, hwnd_, nullptr,
                  GetModuleHandleW(nullptr), nullptr);
    cur_edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_RIGHT, 124, 30, 88, 20, hwnd_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCurEdit)), GetModuleHandleW(nullptr),
                                nullptr);
    CreateWindowW(L"BUTTON", L"\x25B2", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 216, 29, 22, 11, hwnd_,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCurUp)), GetModuleHandleW(nullptr), nullptr);
    CreateWindowW(L"BUTTON", L"\x25BC", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 216, 41, 22, 11, hwnd_,
                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCurDn)), GetModuleHandleW(nullptr), nullptr);
    suppress_edit_notify_ = true;
    set_edit_float(cur_edit_, current);
    suppress_edit_notify_ = false;
    g_edit_prev =
        reinterpret_cast<WNDPROC>(SetWindowLongPtrW(cur_edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(edit_proc)));
    ShowWindow(hwnd_, SW_SHOWNA);
    keep_on_top();
}

void EditorAdjustHud::set_previous(float v) {
    previous_ = v;
    g_prev_fallback = v;
    if (!prev_lbl_) return;
    wchar_t prev_buf[64]{};
    swprintf_s(prev_buf, L"%.4g", static_cast<double>(v));
    SetWindowTextW(prev_lbl_, prev_buf);
}

void EditorAdjustHud::hide() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    prev_lbl_ = nullptr;
    cur_edit_ = nullptr;
    mode_ = Mode::None;
    commit_pending_ = false;
    edit_dirty_ = false;
    g_self = nullptr;
}

void EditorAdjustHud::set_current(float v, bool force) {
    if (!cur_edit_) return;
    if (!force && edit_dirty_ && GetFocus() == cur_edit_) return;
    suppress_edit_notify_ = true;
    set_edit_float(cur_edit_, v);
    suppress_edit_notify_ = false;
    if (force) edit_dirty_ = false;
}

void EditorAdjustHud::keep_on_top() {
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void EditorAdjustHud::pump() {
    if (!hwnd_) return;
    // Only this window and its children (the edit box): PeekMessage(nullptr) took every message of
    // the thread and dropped the 3D window's ones (lost clicks/keys) while the HUD was open.
    MSG msg{};
    int n = 0;
    while (n++ < 64 && HIWORD(GetQueueStatus(QS_ALLINPUT)) != 0 && PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

std::optional<float> EditorAdjustHud::poll_committed() {
    if (!commit_pending_) return std::nullopt;
    commit_pending_ = false;
    return commit_value_;
}

void EditorAdjustHud::request_commit(float v) {
    commit_pending_ = true;
    commit_value_ = v;
    edit_dirty_ = false;
}

void EditorAdjustHud::on_destroy() {
    hwnd_ = nullptr;
    prev_lbl_ = nullptr;
    cur_edit_ = nullptr;
}

void EditorAdjustHud::notify_edit_setfocus() {
    edit_dirty_ = false;
}

void EditorAdjustHud::notify_edit_change() {
    if (!suppress_edit_notify_) edit_dirty_ = true;
}

void EditorAdjustHud::nudge_current(float delta) {
    const float cur = read_edit_float(cur_edit_, previous_);
    const float nv = cur + delta;
    set_current(nv, true);
    request_commit(nv);
}

} // namespace omsi::ui
