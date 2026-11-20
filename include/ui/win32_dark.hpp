#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace omsi::ui::dark {

// Dark title bar (DWM immersive dark mode) for a Win32 window.
void apply(HWND hwnd);
// Center the window on the work area of its monitor.
void center(HWND hwnd);

// Paint handlers to call from a WndProc. Return true when handled.
bool handle_ctlcolor(UINT msg, WPARAM wp, LRESULT& out);
bool handle_erase(HWND hwnd, WPARAM wp, LRESULT& out);

HBRUSH bg_brush();
HBRUSH panel_brush();

} // namespace omsi::ui::dark
