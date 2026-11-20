#include "ui/editor_shortcuts.hpp"

#include "omsi/options.hpp"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace omsi::ui {
namespace {

constexpr int kN = static_cast<int>(EditorAction::Count);

const char* kNames[] = {
    "Mover (G)",
    "Altura Z (Z en grab)",
    "Rotar (R) — RR usa la misma tecla",
    "Cortar/split spline (Ctrl+R)",
    "Nuevo objeto/spline (N)",
    "Autofill (A)",
    "Borrar (C / Del)",
    "Copiar (Ctrl+C)",
    "Pegar (Ctrl+V)",
    "Deshacer (Ctrl+Z)",
    "Rehacer (Ctrl+Y / Ctrl+Shift+Z / Ctrl+Shift+Y)",
    "Guardar (Ctrl+S)",
    "Ciclar path (F9)",
    "Clonar marquee (Shift+D)",
    "Grupo marquee (Shift+S)",
    "Cancelar (Esc)",
    "Follow spline (RR / 2.ª pulsación de Rotar)",
    "Ocultar selección (H)",
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == kN);

std::filesystem::path shortcuts_path() {
    return options::editor_ui_prefs_path().parent_path() / "omsi_f4_shortcuts.cfg";
}

bool mod_vk(int vk) {
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL || vk == VK_LCONTROL ||
           vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
}

} // namespace

void EditorShortcutTable::reset_defaults() {
    auto set = [&](EditorAction a, int vk, bool c = false, bool s = false, bool al = false) {
        chords[static_cast<int>(a)] = EditorChord{vk, c, s, al};
    };
    set(EditorAction::Grab, 'G');
    set(EditorAction::GrabHeight, 'Z');
    set(EditorAction::Rotate, 'R');
    set(EditorAction::TrimSpline, 'R', true);
    set(EditorAction::NewItem, 'N');
    set(EditorAction::Autofill, 'A');
    set(EditorAction::DeleteSel, 'C');
    set(EditorAction::Copy, 'C', true);
    set(EditorAction::Paste, 'V', true);
    set(EditorAction::Undo, 'Z', true);
    set(EditorAction::Redo, 'Y', true);
    set(EditorAction::Save, 'S', true);
    set(EditorAction::CyclePath, VK_F9);
    set(EditorAction::CloneMarquee, 'D', false, true);
    set(EditorAction::GroupMarquee, 'S', false, true);
    set(EditorAction::Cancel, VK_ESCAPE);
    set(EditorAction::SplineFollow, 'R');
    set(EditorAction::Hide, 'H');
}

const char* EditorShortcutTable::name(EditorAction a) {
    const int i = static_cast<int>(a);
    if (i < 0 || i >= kN) return "?";
    return kNames[i];
}

std::string EditorShortcutTable::format(const EditorChord& c) {
    if (c.vk <= 0) return "(ninguno)";
    std::string s;
    if (c.ctrl) s += "Ctrl+";
    if (c.shift) s += "Shift+";
    if (c.alt) s += "Alt+";
    UINT scan = MapVirtualKeyW(static_cast<UINT>(c.vk), MAPVK_VK_TO_VSC);
    wchar_t buf[64]{};
    if (GetKeyNameTextW(static_cast<LONG>(scan << 16), buf, 64) > 0) {
        char utf[128]{};
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf, 128, nullptr, nullptr);
        s += utf;
    } else if (c.vk >= 'A' && c.vk <= 'Z') {
        s += static_cast<char>(c.vk);
    } else {
        s += "VK" + std::to_string(c.vk);
    }
    return s;
}

bool EditorShortcutTable::held(EditorAction a) const {
    if (!input_live) return false;
    const auto& c = chords[static_cast<int>(a)];
    if (c.vk <= 0) return false;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    if (ctrl != c.ctrl || shift != c.shift || alt != c.alt) return false;
    return (GetAsyncKeyState(c.vk) & 0x8000) != 0;
}

bool EditorShortcutTable::pressed(EditorAction a) {
    const int i = static_cast<int>(a);
    const bool h = held(a);
    const bool p = h && !was[static_cast<std::size_t>(i)];
    was[static_cast<std::size_t>(i)] = h;
    return p;
}

bool EditorShortcutTable::capture_tick() {
    if (listen < 0 || listen >= kN) return false;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    for (int vk = 1; vk < 256; ++vk) {
        if (mod_vk(vk)) continue;
        if ((GetAsyncKeyState(vk) & 0x0001) == 0) continue;
        chords[static_cast<std::size_t>(listen)] = EditorChord{vk, ctrl, shift, alt};
        listen = -1;
        save();
        return true;
    }
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x0001) != 0) {
        listen = -1;
        return false;
    }
    return false;
}

void EditorShortcutTable::load() {
    reset_defaults();
    std::ifstream in(shortcuts_path());
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line[0] == '[') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const int id = std::atoi(line.substr(0, eq).c_str());
        if (id < 0 || id >= kN) continue;
        std::istringstream ss(line.substr(eq + 1));
        int vk = 0, c = 0, s = 0, al = 0;
        char comma = 0;
        if (ss >> vk >> comma >> c >> comma >> s >> comma >> al)
            chords[static_cast<std::size_t>(id)] = EditorChord{vk, c != 0, s != 0, al != 0};
    }
}

void EditorShortcutTable::save() const {
    std::ofstream out(shortcuts_path(), std::ios::trunc);
    if (!out) return;
    out << "# omsi_f4_shortcuts.cfg  id=vk,ctrl,shift,alt\n";
    for (int i = 0; i < kN; ++i) {
        const auto& c = chords[static_cast<std::size_t>(i)];
        out << i << '=' << c.vk << ',' << (c.ctrl ? 1 : 0) << ',' << (c.shift ? 1 : 0) << ','
            << (c.alt ? 1 : 0) << '\n';
    }
}

} // namespace omsi::ui
