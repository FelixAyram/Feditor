#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace omsi::ui {

// Standalone Ultralight HTML dialog (Win32 host + CPU blit). No GLFW/ImGui required.
// HTML lives under assets/ui/<name>/ — Tailwind-style local CSS (offline-safe).
struct UltralightHtmlDialogConfig {
    HWND owner = nullptr;
    std::wstring title = L"OMSI F4";
    int width = 720;
    int height = 520;
    // Folder containing index.html (absolute or relative). Resolved via assets/ui candidates.
    std::filesystem::path html_subdir; // e.g. "launcher" or full path to folder
    // Optional JSON state for omsiGetState() (launcher catalog, etc.).
    std::string initial_script;
    // Called on omsiPost(json). Return true to close. Set ok=true if confirmed.
    std::function<bool(const std::string& json, bool& ok)> on_event;
    // Optional: receives eval fn once the dialog host is ready (for live state push).
    std::function<void(const std::string& js)>* script_sink = nullptr;
};

[[nodiscard]] std::optional<bool> run_ultralight_html_dialog(const UltralightHtmlDialogConfig& cfg);

// Child HWND hosting an Ultralight view (for embedding in .sli creator, etc.).
class UltralightWin32View {
public:
    using EventCallback = std::function<void(const std::string& json)>;

    UltralightWin32View() = default;
    ~UltralightWin32View();

    UltralightWin32View(const UltralightWin32View&) = delete;
    UltralightWin32View& operator=(const UltralightWin32View&) = delete;

    [[nodiscard]] bool create(HWND parent, int x, int y, int w, int h, const std::filesystem::path& html_subdir,
                            const char* entry_html = "index.html", const char* script_file = nullptr);
    void destroy();
    void set_bounds(int x, int y, int w, int h);
    void set_event_callback(EventCallback cb);
    void evaluate_script(const std::string& script);
    // Runs script and returns its JS completion value synchronously (safe: local in-process eval,
    // no IPC). Used to pull authoritative state (e.g. edited text) out of the DOM on demand instead
    // of streaming it on every keystroke.
    [[nodiscard]] std::string evaluate_script_sync(const std::string& script);
    // Opt-in: routes native WM_KEYDOWN/WM_KEYUP/WM_CHAR (and click-to-focus) to this view. Default
    // off so read-only/button-only panels (spline creator sidebar/chrome) never steal keyboard focus
    // from the 3D viewport. Enable only for views that host actual text input (e.g. map text editor).
    void set_accept_keyboard(bool on);
    // Marks the page as focused at the WebCore level (View::Focus()). Normally implied by a mouse
    // click, but a keyboard-only view (e.g. map text editor opened without a prior click) needs this
    // called explicitly or FireKeyEvent may be delivered to an unfocused page.
    void focus_view();
    void tick(); // Update/Render/invalidate — call from parent message pump / timer
    void fire_scroll(int delta, int x, int y);
    void reload();

    [[nodiscard]] bool active() const { return hwnd_ != nullptr; }
    [[nodiscard]] bool js_ready() const;
    [[nodiscard]] HWND hwnd() const { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
    void* host_ = nullptr;
    EventCallback event_cb_;
};

[[nodiscard]] std::filesystem::path resolve_ui_assets_subdir(const std::filesystem::path& subdir);
[[nodiscard]] std::filesystem::path resolve_ui_runtime_dir_for(const std::filesystem::path& html_dir);

} // namespace omsi::ui
