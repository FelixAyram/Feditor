#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui/editor_side_panel_ids.hpp"
#include "omsi/utf8_win32.hpp"

#include <commctrl.h>
#include <string>

namespace omsi::ui::editor_side_panel_ids {

const wchar_t* kTabNames[kTabCount] = {L"Tile",         L"Objects",      L"Splines",
                                       L"Terrain",      L"Traffic Rules", L"Tracks & Trips",
                                       L"Station Links", L"Spline-Exp.",  L"Signal Rts"};

const wchar_t* kRuleRadioLabels[static_cast<int>(RuleRadio::Count)] = {
    L"Speed Limit:",
    L"Overtaking Prohib.",
    L"No Unscheduled Traffic",
    L"Unsched. Trfc. Den. Extreme Low",
    L"Unscheduled Traffic Density V. Low",
    L"Unscheduled Traffic Density Low",
    L"Unscheduled Traffic Density Normal",
    L"Unscheduled Traffic Density High",
    L"Higher Priority",
    L"Lower Priority",
    L"Cars prohibited",
    L"Trucks allowed",
    L"Buses & Cabs allowed",
};

std::wstring to_wide(const std::string& s) { return omsi::utf8_to_wide(s); }

HWND mk_static(HWND p, const wchar_t* t, int x, int y, int w, int h) {
    return CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE, x, y, w, h, p, nullptr, GetModuleHandleW(nullptr),
                         nullptr);
}
HWND mk_edit(HWND p, int id, int x, int y, int w, int h) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y, w, h, p,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}
HWND mk_btn(HWND p, int id, const wchar_t* t, int x, int y, int w, int h) {
    return CreateWindowW(L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, y, w, h, p,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}
HWND mk_check(HWND p, int id, const wchar_t* t, int x, int y, int w, int h) {
    return CreateWindowW(L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, x, y, w, h, p,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}
HWND mk_radio(HWND p, int id, const wchar_t* t, int x, int y, int w, int h, bool first) {
    const DWORD style = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | (first ? WS_GROUP : 0);
    return CreateWindowW(L"BUTTON", t, style, x, y, w, h, p, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         GetModuleHandleW(nullptr), nullptr);
}
HWND mk_list(HWND p, int id, int x, int y, int w, int h) {
    return CreateWindowW(WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, x, y, w, h, p,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}
HWND mk_combo(HWND p, int id, int x, int y, int w, int h) {
    return CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, h, p,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

} // namespace omsi::ui::editor_side_panel_ids
