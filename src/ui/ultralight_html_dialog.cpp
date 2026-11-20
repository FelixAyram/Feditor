#include "ui/ultralight_html_dialog.hpp"

#include "omsi/logger.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

#ifdef OMSI_ULTRALIGHT_UI

#include <AppCore/AppCore.h>
#include <AppCore/JSHelpers.h>
#include <Ultralight/KeyCodes.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/platform/Surface.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace omsi::ui {
namespace {

using ultralight::BitmapSurface;
using ultralight::KeyEvent;
using ultralight::LoadListener;
using ultralight::MessageLevel;
using ultralight::MessageSource;
using ultralight::MouseEvent;
using ultralight::RefPtr;
using ultralight::String;
using ultralight::View;
using ultralight::ViewListener;

constexpr UINT kUlTimerId = 0xF401;
constexpr UINT kUlTimerMs = 16;

bool g_platform_ready = false;

// Ultralight requires exactly one Renderer for the entire process lifetime (creating a second
// one after a prior instance is released hangs/crashes inside Renderer::Create()). Every
// ViewHost (spline creator sidebar, chrome, modal dialogs, ...) shares this single instance;
// each View is independently created/destroyed via renderer->CreateView()/RefPtr release.
RefPtr<ultralight::Renderer> g_shared_renderer;

RefPtr<ultralight::Renderer> shared_ultralight_renderer() {
    if (!g_shared_renderer) {
        g_shared_renderer = ultralight::Renderer::Create();
        if (!g_shared_renderer) log::error("Ultralight Renderer::Create failed (shared)");
    }
    return g_shared_renderer;
}

class HostViewListener : public ViewListener {
public:
    void OnAddConsoleMessage(View*, MessageSource, MessageLevel, const String& message, uint32_t, uint32_t,
                             const String&) override {
        log::warn("Ultralight console: " + std::string(message.utf8().data()));
    }
};

class HostLoadListener : public LoadListener {
public:
    std::function<void(const std::string&)> event_cb_;
    std::function<void(View*)> on_ready_;
    std::function<std::string()> get_state_;

    void bind_native(View* caller) {
        if (!caller) return;
        RefPtr<ultralight::JSContext> ctx = caller->LockJSContext();
        ultralight::SetJSContext(ctx->ctx());
        ultralight::JSObject global = ultralight::JSGlobalObject();
        global["omsiPost"] = ultralight::JSCallback([this](const ultralight::JSObject&,
                                                              const ultralight::JSArgs& args) {
            if (args.size() < 1 || !event_cb_) return;
            event_cb_(static_cast<String>(args[0].ToString()).utf8().data());
        });
        global["omsiGetState"] = ultralight::JSCallbackWithRetval([this](const ultralight::JSObject&,
                                                                          const ultralight::JSArgs&) -> ultralight::JSValue {
            const std::string s = get_state_ ? get_state_() : std::string("{}");
            log::info("omsiGetState called, returning " + std::to_string(s.size()) + " bytes");
            return ultralight::JSValue(ultralight::String(s.c_str()));
        });
    }

    void OnWindowObjectReady(View* caller, uint64_t, bool is_main_frame, const String&) override {
        if (!is_main_frame) return;
        bind_native(caller);
    }

    void OnDOMReady(View* caller, uint64_t, bool is_main_frame, const String&) override {
        if (!is_main_frame || !caller) return;
        bind_native(caller);
        if (on_ready_) on_ready_(caller);
    }
};

std::filesystem::path exe_ui_assets_root() {
    wchar_t buf[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0) return {};
    return std::filesystem::path(buf).parent_path() / "assets" / "ui";
}

std::vector<std::filesystem::path> ui_root_candidates() {
    namespace fs = std::filesystem;
    std::vector<fs::path> out;
    // Deploy folder next to exe (post-build copy) — must beat source-tree compile paths.
    if (const auto exe_ui = exe_ui_assets_root(); !exe_ui.empty()) out.emplace_back(exe_ui);
    if (const char* env = std::getenv("OMSI_UI_ASSETS_DIR"); env && env[0])
        out.emplace_back(fs::path(env).parent_path());
#ifdef OMSI_UI_ASSETS_DIR
    out.emplace_back(fs::path(OMSI_UI_ASSETS_DIR).parent_path());
#endif
    out.emplace_back(fs::current_path() / "assets" / "ui");
    out.emplace_back(fs::path("assets") / "ui");
    out.emplace_back(fs::current_path() / ".." / ".." / "assets" / "ui");
    return out;
}

std::filesystem::path canonical_existing(const std::filesystem::path& p) {
    std::error_code ec;
    const auto abs = std::filesystem::weakly_canonical(p, ec);
    return ec ? p : abs;
}

bool has_icu(const std::filesystem::path& dir) {
    return std::filesystem::exists(dir / "resources" / "icudt67l.dat") ||
           std::filesystem::exists(dir / "resources" / "icudt.dat");
}

// Prevent "</script>" inside embedded JSON from terminating the HTML script block.
std::string sanitize_for_script_tag(std::string s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<' && i + 1 < s.size() && s[i + 1] == '/') {
            out += "<\\/";
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>{});
}

std::string build_composite_html(const std::filesystem::path& html_dir, const std::string& state_json) {
    std::string html = read_text_file(html_dir / "index.html");
    std::string script = read_text_file(html_dir / "app.js");
    if (!state_json.empty()) {
        script += "\nwindow.omsiApplyState&&window.omsiApplyState(" + sanitize_for_script_tag(state_json) + ");\n";
    }
    std::string inject;
    if (!script.empty()) inject += "<script>\n" + script + "\n</script>\n";
    const auto pos = html.rfind("</body>");
    if (pos != std::string::npos) html.insert(pos, inject);
    else html += inject;
    return html;
}

void ensure_platform() {
    if (g_platform_ready) return;
    ultralight::Config config;
    config.resource_path_prefix = ultralight::String("map_editor/resources/");
    ultralight::Platform::instance().set_config(config);
    ultralight::Platform::instance().set_font_loader(ultralight::GetPlatformFontLoader());
    g_platform_ready = true;
}

std::string file_url_from_path(const std::filesystem::path& file) {
    auto s = std::filesystem::absolute(file).generic_string();
    return "file:///" + s;
}

int win_vk_to_ul(int vk) {
    if (vk >= 'A' && vk <= 'Z') return vk;
    if (vk >= '0' && vk <= '9') return vk;
    switch (vk) {
    case VK_BACK: return ultralight::KeyCodes::GK_BACK;
    case VK_TAB: return ultralight::KeyCodes::GK_TAB;
    case VK_RETURN: return ultralight::KeyCodes::GK_RETURN;
    case VK_ESCAPE: return ultralight::KeyCodes::GK_ESCAPE;
    case VK_DELETE: return ultralight::KeyCodes::GK_DELETE;
    case VK_LEFT: return ultralight::KeyCodes::GK_LEFT;
    case VK_RIGHT: return ultralight::KeyCodes::GK_RIGHT;
    case VK_UP: return ultralight::KeyCodes::GK_UP;
    case VK_DOWN: return ultralight::KeyCodes::GK_DOWN;
    case VK_HOME: return ultralight::KeyCodes::GK_HOME;
    case VK_END: return ultralight::KeyCodes::GK_END;
    case VK_PRIOR: return ultralight::KeyCodes::GK_PRIOR;
    case VK_NEXT: return ultralight::KeyCodes::GK_NEXT;
    default: return 0;
    }
}

std::string js_escape_single_quoted(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\'': out += "\\'"; break;
        case '\n': out += "\\n"; break;
        case '\r': break;
        default: out += c;
        }
    }
    return out;
}

void inject_css_into_view(View* view, const std::filesystem::path& path, const char* element_id) {
    const std::string css = read_text_file(path);
    if (css.empty() || !view) return;
    const std::string js = "(function(){var o=document.getElementById('" + std::string(element_id) +
                           "');if(o)o.remove();var s=document.createElement('style');s.id='" +
                           std::string(element_id) + "';s.textContent='" + js_escape_single_quoted(css) +
                           "';document.head.appendChild(s);})();";
    view->EvaluateScript(String(js.c_str()));
}

struct ViewHost {
    RefPtr<ultralight::Renderer> renderer;
    RefPtr<View> view;
    HostLoadListener load_listener;
    HostViewListener view_listener;
    std::filesystem::path html_dir;
    std::filesystem::path runtime_dir;
    std::vector<std::uint8_t> bgra;
    BITMAPINFO bmi{};
    bool dirty = true;
    int last_w = 0;
    int last_h = 0;
    std::function<void(const std::string&)> event_cb_;
    std::function<void()> paint_request_;
    std::string state_json_;
    std::string entry_html_ = "index.html";
    std::string script_file_ = "app.js";
    std::string page_css_ = "style.css";
    bool dom_ready_ = false;
    bool js_booted_ = false;
    bool mouse_button_down_ = false;
    MouseEvent::Button mouse_down_button_ = MouseEvent::kButton_None;
    int last_mouse_x_ = 0;
    int last_mouse_y_ = 0;

    void inject_ui_assets(View* caller) {
        if (!caller) return;
        const auto shared_tw = html_dir.parent_path() / "shared" / "tw.css";
        inject_css_into_view(caller, shared_tw, "omsi-tw");
        if (!page_css_.empty()) inject_css_into_view(caller, html_dir / page_css_, "omsi-page-css");
        const auto script_path = html_dir / script_file_;
        const std::string script = read_text_file(script_path);
        if (script.empty()) {
            log::error("Ultralight script missing: " + script_path.string());
            return;
        }
        // Must be true before EvaluateScript: app.js posts "ready" synchronously and C++ sync checks js_ready().
        js_booted_ = true;
        caller->EvaluateScript(String(script.c_str()));
        log::info("Ultralight injected " + script_file_ + " (" + std::to_string(script.size()) + " bytes) from " +
                  html_dir.string());
    }

    void pull_state_to_ui() {
        if (!view || !js_booted_ || state_json_.empty()) return;
        const std::string js =
            "window.omsiApplyState&&window.omsiApplyState(" + sanitize_for_script_tag(state_json_) + ");";
        view->EvaluateScript(String(js.c_str()));
        dirty = true;
    }

    void set_state_json(std::string json) {
        state_json_ = std::move(json);
        pull_state_to_ui();
    }

    bool init(const std::filesystem::path& html_subdir, int w, int h, const std::string& initial_state_json = {},
              const char* entry_html = "index.html", const char* script_file = nullptr,
              const char* page_css = nullptr) {
        ensure_platform();
        html_dir = resolve_ui_assets_subdir(html_subdir);
        runtime_dir = resolve_ui_runtime_dir_for(html_dir);
        entry_html_ = (entry_html && entry_html[0]) ? entry_html : "index.html";
        if (script_file && script_file[0])
            script_file_ = script_file;
        else if (entry_html_ == "chrome.html")
            script_file_ = "chrome.js";
        else
            script_file_ = "app.js";
        page_css_ = (page_css && page_css[0]) ? page_css : (entry_html_ == "chrome.html" ? "chrome.css" : "style.css");
        const auto entry_path = html_dir / entry_html_;
        if (!std::filesystem::exists(entry_path)) {
            log::error("Ultralight HTML not found: " + entry_path.string());
            return false;
        }
        if (!has_icu(runtime_dir)) {
            log::error("Ultralight ICU missing under " + runtime_dir.string());
            return false;
        }
        state_json_ = initial_state_json;
        dom_ready_ = false;
        js_booted_ = false;
        const auto fs_root = html_dir.parent_path();
        ultralight::Platform::instance().set_file_system(
            ultralight::GetPlatformFileSystem(String(fs_root.string().c_str())));
        log::info("Ultralight filesystem root=" + fs_root.string() + " html=" + html_dir.string() + " entry=" +
                  entry_html_);
        load_listener.event_cb_ = [this](const std::string& json) {
            if (event_cb_) event_cb_(json);
        };
        load_listener.get_state_ = [this]() { return state_json_; };
        load_listener.on_ready_ = [this](View* caller) {
            inject_ui_assets(caller);
            dom_ready_ = true;
            dirty = true;
            pull_state_to_ui();
            if (paint_request_) paint_request_();
            log::info("Ultralight DOM ready (" + entry_html_ + "), state_json=" +
                      std::to_string(state_json_.size()) + " bytes");
        };
        renderer = shared_ultralight_renderer();
        if (!renderer) return false;
        ultralight::ViewConfig vc;
        vc.is_accelerated = false;
        vc.initial_device_scale = 1.0;
        vc.font_family_standard = String("Segoe UI");
        view = renderer->CreateView(static_cast<uint32_t>(w), static_cast<uint32_t>(h), vc, nullptr);
        if (!view) return false;
        view->set_load_listener(&load_listener);
        view->set_view_listener(&view_listener);
        const auto url = file_url_from_path(entry_path);
        log::info("Ultralight LoadURL " + url);
        view->LoadURL(String(url.c_str()));
        last_w = w;
        last_h = h;
        dirty = true;
        return true;
    }

    void shutdown() {
        if (view) {
            view->set_load_listener(nullptr);
            view->set_view_listener(nullptr);
        }
        view = nullptr;
        renderer = nullptr;
    }

    void resize(int w, int h) {
        w = (std::max)(w, 1);
        h = (std::max)(h, 1);
        if (view && (w != last_w || h != last_h)) {
            view->Resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
            last_w = w;
            last_h = h;
            dirty = true;
        }
    }

    void tick() {
        if (!renderer || !view) return;
        renderer->Update();
        renderer->Render();
        auto* surface = static_cast<BitmapSurface*>(view->surface());
        if (surface && !surface->dirty_bounds().IsEmpty()) dirty = true;
    }

    void paint(HDC dc, int w, int h) {
        tick();
        if (!view) return;
        auto* surface = static_cast<BitmapSurface*>(view->surface());
        if (!surface) return;
        auto bmp = surface->bitmap();
        if (!bmp) return;
        const std::uint32_t bw = bmp->width();
        const std::uint32_t bh = bmp->height();
        if (bw == 0 || bh == 0) {
            log::warn("Ultralight paint skipped: bitmap " + std::to_string(bw) + "x" + std::to_string(bh));
            return;
        }
        const void* src = bmp->LockPixels();
        if (!src) return;
        const auto* px = static_cast<const std::uint8_t*>(src);
        const std::uint32_t stride = bmp->row_bytes();
        bgra.assign(static_cast<std::size_t>(bw) * bh * 4u, 0);
        for (std::uint32_t y = 0; y < bh; ++y)
            std::memcpy(bgra.data() + y * bw * 4u, px + y * stride, bw * 4u);
        bmp->UnlockPixels();
        bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(bw);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(bh);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        static bool logged_once = false;
        if (!logged_once) {
            logged_once = true;
            log::info("Ultralight paint bitmap " + std::to_string(bw) + "x" + std::to_string(bh) + " -> client " +
                      std::to_string(w) + "x" + std::to_string(h));
        }
        StretchDIBits(dc, 0, 0, w, h, 0, 0, static_cast<int>(bw), static_cast<int>(bh), bgra.data(), &bmi,
                      DIB_RGB_COLORS, SRCCOPY);
        dirty = false;
        surface->ClearDirtyBounds();
    }

    void fire_mouse(int x, int y, MouseEvent::Type type, MouseEvent::Button btn) {
        if (!view) return;
        view->Focus();
        // Ultralight expects a MouseMoved at the click location before MouseDown; after down, moves need the
        // pressed button or click state is lost (ultralight#426).
        if (type == MouseEvent::kType_MouseDown) {
            MouseEvent pre{};
            pre.type = MouseEvent::kType_MouseMoved;
            pre.x = x;
            pre.y = y;
            pre.button = btn;
            view->FireMouseEvent(pre);
            mouse_button_down_ = true;
            mouse_down_button_ = btn;
        }
        MouseEvent e{};
        e.type = type;
        e.x = x;
        e.y = y;
        if (type == MouseEvent::kType_MouseMoved && mouse_button_down_)
            e.button = mouse_down_button_;
        else
            e.button = btn;
        view->FireMouseEvent(e);
        if (type == MouseEvent::kType_MouseUp) {
            mouse_button_down_ = false;
            mouse_down_button_ = MouseEvent::kButton_None;
        }
        dirty = true;
    }

    void fire_scroll(int dy, int x, int y) {
        if (!view || dy == 0) return;
        last_mouse_x_ = x;
        last_mouse_y_ = y;
        view->Focus();
        {
            MouseEvent move{};
            move.type = MouseEvent::kType_MouseMoved;
            move.x = x;
            move.y = y;
            move.button = mouse_button_down_ ? mouse_down_button_ : MouseEvent::kButton_None;
            view->FireMouseEvent(move);
        }
        ultralight::ScrollEvent se{};
        se.type = ultralight::ScrollEvent::kType_ScrollByPixel;
        se.delta_y = dy;
        view->FireScrollEvent(se);
        if (js_booted_) {
            const int js_px = -dy;
            char js[256];
            std::snprintf(js, sizeof(js), "if(window.omsiScroll)window.omsiScroll(%d,%d,%d);", js_px, x, y);
            view->EvaluateScript(String(js));
        }
        dirty = true;
    }

    void fire_key(int vk, bool down, bool repeat) {
        if (!view) return;
        const int ul = win_vk_to_ul(vk);
        if (!ul) return;
        (void)repeat;
        KeyEvent e{};
        e.type = down ? KeyEvent::kType_RawKeyDown : KeyEvent::kType_KeyUp;
        e.virtual_key_code = ul;
        e.native_key_code = vk;
        unsigned mods = 0;
        if (GetKeyState(VK_SHIFT) & 0x8000) mods |= KeyEvent::kMod_ShiftKey;
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= KeyEvent::kMod_CtrlKey;
        if (GetKeyState(VK_MENU) & 0x8000) mods |= KeyEvent::kMod_AltKey;
        e.modifiers = mods;
        view->FireKeyEvent(e);
        dirty = true;
    }

    void fire_char(wchar_t wc) {
        if (!view || wc < 32) return;
        KeyEvent e{};
        e.type = KeyEvent::kType_Char;
        const char ch = static_cast<char>(wc & 0xFF);
        e.text = ultralight::String(std::string(1, ch).c_str());
        e.unmodified_text = e.text;
        view->FireKeyEvent(e);
        dirty = true;
    }

    void eval(const std::string& js) {
        if (!view || !js_booted_) return;
        view->EvaluateScript(String(js.c_str()));
        dirty = true;
    }

    std::string eval_sync(const std::string& js) {
        if (!view || !js_booted_) return {};
        const String result = view->EvaluateScript(String(js.c_str()));
        return std::string(result.utf8().data());
    }

    void focus() {
        if (view) view->Focus();
    }

    [[nodiscard]] bool js_ready() const { return js_booted_; }
};

struct ModalHost {
    UltralightHtmlDialogConfig cfg;
    ViewHost host;
    HWND hwnd = nullptr;
    bool done = false;
    bool ok = false;
    bool init_failed = false;
    int cw = 0;
    int ch = 0;
};

ModalHost* g_modal = nullptr;

LRESULT CALLBACK modal_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<ModalHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = static_cast<ModalHost*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        st->hwnd = hwnd;
        st->cw = (std::max)(static_cast<int>(st->cfg.width), 1);
        st->ch = (std::max)(static_cast<int>(st->cfg.height), 1);
        st->host.event_cb_ = [&](const std::string& json) {
            if (st->cfg.on_event && st->cfg.on_event(json, st->ok)) {
                st->done = true;
                DestroyWindow(hwnd);
            }
        };
        if (st->cfg.script_sink)
            *st->cfg.script_sink = [&st](const std::string& json) { st->host.set_state_json(json); };
        st->host.paint_request_ = [hwnd]() { InvalidateRect(hwnd, nullptr, FALSE); };
        if (!st->host.init(st->cfg.html_subdir, st->cw, st->ch, st->cfg.initial_script)) {
            log::error("Ultralight modal init failed");
            st->init_failed = true;
            st->done = true;
            return -1;
        }
        SetTimer(hwnd, kUlTimerId, kUlTimerMs, nullptr);
        st->host.tick();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SIZE:
        if (st) {
            st->cw = LOWORD(lp);
            st->ch = HIWORD(lp);
            st->host.resize(st->cw, st->ch);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (st && wp == kUlTimerId) {
            st->host.tick();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (st) st->host.paint(dc, st->cw, st->ch);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (st) {
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            st->host.last_mouse_x_ = x;
            st->host.last_mouse_y_ = y;
            st->host.fire_mouse(x, y, MouseEvent::kType_MouseMoved, MouseEvent::kButton_None);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        if (st) {
            st->host.fire_mouse(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), MouseEvent::kType_MouseDown,
                                MouseEvent::kButton_Left);
            st->host.tick();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        if (st) {
            st->host.fire_mouse(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), MouseEvent::kType_MouseUp,
                                MouseEvent::kButton_Left);
            st->host.tick();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (st) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            st->host.fire_scroll(GET_WHEEL_DELTA_WPARAM(wp) / 4, pt.x, pt.y);
        }
        return 0;
    case WM_KEYDOWN:
        if (st) st->host.fire_key(static_cast<int>(wp), true, (lp & (1 << 30)) != 0);
        return 0;
    case WM_KEYUP:
        if (st) st->host.fire_key(static_cast<int>(wp), false, false);
        return 0;
    case WM_CHAR:
        if (st) st->host.fire_char(static_cast<wchar_t>(wp));
        return 0;
    case WM_CLOSE:
        if (st) st->done = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kUlTimerId);
        if (st) st->host.shutdown();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct ChildHost {
    ViewHost host;
    std::function<void(const std::string&)> event_cb_;
    int cw = 0;
    int ch = 0;
    bool accept_keyboard = false;
};

ChildHost* child_from_hwnd(HWND hwnd) {
    return reinterpret_cast<ChildHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

LRESULT CALLBACK child_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = child_from_hwnd(hwnd);
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = static_cast<ChildHost*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        RECT cr{};
        GetClientRect(hwnd, &cr);
        st->cw = cr.right;
        st->ch = cr.bottom;
        SetTimer(hwnd, kUlTimerId, kUlTimerMs, nullptr);
        return 0;
    }
    case WM_SIZE:
        if (st) {
            st->cw = LOWORD(lp);
            st->ch = HIWORD(lp);
            st->host.resize(st->cw, st->ch);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (st && wp == kUlTimerId) {
            st->host.tick();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        if (st) st->host.paint(dc, st->cw, st->ch);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (st) {
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            st->host.last_mouse_x_ = x;
            st->host.last_mouse_y_ = y;
            st->host.fire_mouse(x, y, MouseEvent::kType_MouseMoved, MouseEvent::kButton_None);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (st && st->accept_keyboard) SetFocus(hwnd);
        SetCapture(hwnd);
        if (st) {
            st->host.fire_mouse(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), MouseEvent::kType_MouseDown,
                                MouseEvent::kButton_Left);
            st->host.tick();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        if (st) {
            st->host.fire_mouse(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), MouseEvent::kType_MouseUp,
                                MouseEvent::kButton_Left);
            st->host.tick();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (st) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            st->host.fire_scroll(GET_WHEEL_DELTA_WPARAM(wp) / 4, pt.x, pt.y);
            st->host.tick();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (st && st->accept_keyboard) {
            st->host.fire_key(static_cast<int>(wp), true, (lp & (1 << 30)) != 0);
            st->host.tick();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_KEYUP:
        if (st && st->accept_keyboard) st->host.fire_key(static_cast<int>(wp), false, false);
        return 0;
    case WM_CHAR:
        if (st && st->accept_keyboard) {
            st->host.fire_char(static_cast<wchar_t>(wp));
            st->host.tick();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool register_child_class() {
    static bool reg = false;
    if (reg) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = child_wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OmsiF4UlChild";
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    reg = RegisterClassExW(&wc) != 0;
    return reg;
}

} // namespace

std::filesystem::path resolve_ui_assets_subdir(const std::filesystem::path& subdir) {
    namespace fs = std::filesystem;
    if (subdir.is_absolute() && fs::exists(subdir / "index.html")) return fs::absolute(subdir);
    for (const auto& root : ui_root_candidates()) {
        const auto p = fs::absolute(canonical_existing(root / subdir));
        if (fs::exists(p / "index.html")) return p;
    }
    return subdir;
}

std::filesystem::path resolve_ui_runtime_dir_for(const std::filesystem::path& html_dir) {
    namespace fs = std::filesystem;
    if (has_icu(html_dir)) return html_dir;
    for (const auto& root : ui_root_candidates()) {
        const auto map_editor = fs::absolute(canonical_existing(root / "map_editor"));
        if (has_icu(map_editor)) return map_editor;
    }
    return html_dir;
}

std::optional<bool> run_ultralight_html_dialog(const UltralightHtmlDialogConfig& cfg) {
    const wchar_t* cls = L"OmsiF4UlModal";
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = modal_wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        reg = true;
    }

    ModalHost st{};
    st.cfg = cfg;
    RECT wr{0, 0, cfg.width, cfg.height};
    AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    const int ww = wr.right - wr.left;
    const int wh = wr.bottom - wr.top;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.left + ((work.right - work.left) - ww) / 2;
    int y = work.top + ((work.bottom - work.top) - wh) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, cfg.title.c_str(),
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, ww, wh, cfg.owner,
                               nullptr, GetModuleHandleW(nullptr), &st);
    if (!dlg || st.init_failed) return std::nullopt;

    if (cfg.owner) EnableWindow(cfg.owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    SetForegroundWindow(dlg);

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (cfg.owner) EnableWindow(cfg.owner, TRUE);
    return st.ok;
}

UltralightWin32View::~UltralightWin32View() { destroy(); }

bool UltralightWin32View::create(HWND parent, int x, int y, int w, int h, const std::filesystem::path& html_subdir,
                                 const char* entry_html, const char* script_file) {
    destroy();
    if (!register_child_class()) return false;
    auto* st = new ChildHost{};
    st->cw = w;
    st->ch = h;
    if (!st->host.init(html_subdir, w, h, {}, entry_html, script_file)) {
        delete st;
        return false;
    }
    st->host.event_cb_ = [this](const std::string& json) {
        if (event_cb_) event_cb_(json);
    };
    hwnd_ = CreateWindowExW(0, L"OmsiF4UlChild", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, x, y, w, h, parent,
                            nullptr, GetModuleHandleW(nullptr), st);
    if (!hwnd_) {
        st->host.shutdown();
        delete st;
        return false;
    }
    const HWND child_hwnd = hwnd_;
    st->host.paint_request_ = [child_hwnd]() { InvalidateRect(child_hwnd, nullptr, FALSE); };
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    host_ = st;
    return true;
}

void UltralightWin32View::destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    if (host_) {
        auto* st = static_cast<ChildHost*>(host_);
        st->host.shutdown();
        delete st;
    }
    host_ = nullptr;
}

void UltralightWin32View::set_bounds(int x, int y, int w, int h) {
    if (hwnd_)
        SetWindowPos(hwnd_, HWND_TOP, x, y, w, h, SWP_NOACTIVATE);
    if (host_) static_cast<ChildHost*>(host_)->host.resize(w, h);
}

void UltralightWin32View::set_event_callback(EventCallback cb) { event_cb_ = std::move(cb); }

void UltralightWin32View::evaluate_script(const std::string& script) {
    if (host_) static_cast<ChildHost*>(host_)->host.eval(script);
}

std::string UltralightWin32View::evaluate_script_sync(const std::string& script) {
    if (!host_) return {};
    return static_cast<ChildHost*>(host_)->host.eval_sync(script);
}

void UltralightWin32View::set_accept_keyboard(bool on) {
    if (host_) static_cast<ChildHost*>(host_)->accept_keyboard = on;
}

void UltralightWin32View::focus_view() {
    if (host_) static_cast<ChildHost*>(host_)->host.focus();
}

bool UltralightWin32View::js_ready() const {
    if (!host_) return false;
    return static_cast<const ChildHost*>(host_)->host.js_ready();
}

void UltralightWin32View::tick() {
    if (hwnd_ && host_) {
        auto* st = static_cast<ChildHost*>(host_);
        st->host.tick();
        if (st->host.dirty) InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void UltralightWin32View::fire_scroll(const int delta, const int x, const int y) {
    if (host_) static_cast<ChildHost*>(host_)->host.fire_scroll(delta, x, y);
}

void UltralightWin32View::reload() {
    if (!host_) return;
    auto* st = static_cast<ChildHost*>(host_);
    const auto url = file_url_from_path(st->host.html_dir / "index.html") + "?r=" + std::to_string(GetTickCount());
    st->host.view->LoadURL(String(url.c_str()));
}

} // namespace omsi::ui

#else // !OMSI_ULTRALIGHT_UI

namespace omsi::ui {

std::filesystem::path resolve_ui_assets_subdir(const std::filesystem::path& subdir) { return subdir; }
std::filesystem::path resolve_ui_runtime_dir_for(const std::filesystem::path& html_dir) { return html_dir; }

std::optional<bool> run_ultralight_html_dialog(const UltralightHtmlDialogConfig&) { return std::nullopt; }

UltralightWin32View::~UltralightWin32View() = default;
bool UltralightWin32View::create(HWND, int, int, int, int, const std::filesystem::path&, const char*, const char*) {
    return false;
}
bool UltralightWin32View::js_ready() const { return false; }
void UltralightWin32View::destroy() {}
void UltralightWin32View::set_bounds(int, int, int, int) {}
void UltralightWin32View::set_event_callback(EventCallback) {}
void UltralightWin32View::evaluate_script(const std::string&) {}
std::string UltralightWin32View::evaluate_script_sync(const std::string&) { return {}; }
void UltralightWin32View::set_accept_keyboard(bool) {}
void UltralightWin32View::focus_view() {}
void UltralightWin32View::tick() {}
void UltralightWin32View::fire_scroll(const int, const int, const int) {}
void UltralightWin32View::reload() {}

} // namespace omsi::ui

#endif
