#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace omsi::ui {

// Remappable editor chords (ImHotKey / openMSX Shortcuts.cc pattern):
// one row per command, persist next to the exe, capture-next-key in the Atajos dialog.
enum class EditorAction : int {
    Grab = 0,
    GrabHeight,
    Rotate,
    TrimSpline,
    NewItem,
    Autofill,
    DeleteSel,
    Copy,
    Paste,
    Undo,
    Redo,
    Save,
    CyclePath,
    CloneMarquee,
    GroupMarquee,
    Cancel,
    SplineFollow,
    Hide,
    Count
};

struct EditorChord {
    int vk = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

struct EditorShortcutTable {
    std::array<EditorChord, static_cast<int>(EditorAction::Count)> chords{};
    std::array<bool, static_cast<int>(EditorAction::Count)> was{};
    int listen = -1; // action index waiting for a key, or -1

    void reset_defaults();
    void load();
    void save() const;

    [[nodiscard]] bool held(EditorAction a) const;
    bool pressed(EditorAction a); // edge this frame; call once per action per frame
    bool capture_tick();          // true if a bind was assigned
    bool input_live = true;       // false while typing / editor not focused

    [[nodiscard]] static const char* name(EditorAction a);
    [[nodiscard]] static std::string format(const EditorChord& c);
};

} // namespace omsi::ui
