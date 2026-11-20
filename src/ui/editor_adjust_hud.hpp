#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <optional>

namespace omsi::ui {

// Fixed HUD for exact height / rotation while G+Z or R edit modes (non-draggable).
class EditorAdjustHud {
public:
    enum class Mode { None, Height, Rotation };

    void show(HWND owner, Mode mode, float previous, float current);
    void hide();
    void set_previous(float v);
    void set_current(float v, bool force = false);
    void keep_on_top();
    [[nodiscard]] bool visible() const { return hwnd_ != nullptr; }
    [[nodiscard]] HWND hwnd() const { return hwnd_; }
    void pump();
    // User pressed Enter or nudge buttons.
    [[nodiscard]] std::optional<float> poll_committed();
    void request_commit(float v);
    void on_destroy();
    void notify_edit_setfocus();
    void notify_edit_change();
    void nudge_current(float delta);

private:
    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND prev_lbl_ = nullptr;
    HWND cur_edit_ = nullptr;
    Mode mode_ = Mode::None;
    float previous_ = 0.f;
    bool commit_pending_ = false;
    float commit_value_ = 0.f;
    bool suppress_edit_notify_ = false;
    bool edit_dirty_ = false;
};

} // namespace omsi::ui
