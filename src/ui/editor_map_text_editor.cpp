#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "omsi/fs_cow.hpp"
#include "editor_map_text_editor.hpp"

#include "omsi/logger.hpp"
#include "omsi/utf8_win32.hpp"
#include "ui/ultralight_html_dialog.hpp"

#include <commctrl.h>
#include <windows.h>
#include <windowsx.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace omsi::ui {
namespace {

constexpr int kEdit = 9301;
constexpr int kSaveBar = 9302;
constexpr int kCloseBar = 9303;
constexpr UINT WM_MAPTEXT_GOTO = WM_APP + 120;
constexpr UINT WM_MAPTEXT_SAVE = WM_APP + 121;
constexpr UINT WM_MAPTEXT_COPY = WM_APP + 122;
constexpr UINT WM_MAPTEXT_PASTE = WM_APP + 123;
constexpr UINT WM_MAPTEXT_CUT = WM_APP + 124;

struct EditorState {
    std::filesystem::path path;
    std::optional<std::filesystem::path> allowed_write_root;
    int goto_line = 1;
    bool saved = false;
    bool dirty = false;
    bool loading = false;
    bool utf16_le = false;
    bool web_ok = false;
    HWND edit = nullptr;     // native fallback only (Ultralight unavailable)
    HWND line_lbl = nullptr; // native fallback only
    std::string utf8_text;   // authoritative file content (UTF-8), refreshed after each save
    UltralightWin32View web; // primary UI
};

struct ActiveEditor {
    HWND hwnd = nullptr;
    EditorState st;
};

std::unique_ptr<ActiveEditor> g_editor;

bool path_is_under(const std::filesystem::path& file, const std::filesystem::path& root) {
    if (root.empty()) return true;
    std::error_code ec;
    const auto rel =
        std::filesystem::relative(file.lexically_normal(), root.lexically_normal(), ec);
    if (ec || rel.empty()) return false;
    for (const auto& part : rel) {
        if (part == "..") return false;
    }
    return true;
}

bool file_is_utf16_le(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    unsigned char b[2]{};
    in.read(reinterpret_cast<char*>(b), 2);
    return in.gcount() == 2 && b[0] == 0xFF && b[1] == 0xFE;
}

std::wstring load_file_for_edit(const std::filesystem::path& path, std::string& out_utf8, bool& utf16_le) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    utf16_le = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
               static_cast<unsigned char>(bytes[1]) == 0xFE;
    if (utf16_le) {
        const std::size_t byte_len = bytes.size() - 2;
        const std::size_t wchar_count = byte_len / 2;
        std::wstring ws(wchar_count, L'\0');
        if (wchar_count > 0)
            std::memcpy(ws.data(), bytes.data() + 2, byte_len);
        while (!ws.empty() && ws.back() == L'\0') ws.pop_back();
        out_utf8 = omsi::wide_to_utf8(ws);
        return ws;
    }
    out_utf8.assign(bytes.begin(), bytes.end());
    return omsi::utf8_to_wide(out_utf8);
}

bool save_file_utf8(const std::filesystem::path& path, const std::string& utf8_text, bool utf16_le) {
    omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (utf16_le) {
        const std::wstring wide = omsi::utf8_to_wide(utf8_text);
        out.put(static_cast<char>(0xFF));
        out.put(static_cast<char>(0xFE));
        for (wchar_t c : wide) {
            out.put(static_cast<char>(c & 0xFF));
            out.put(static_cast<char>((c >> 8) & 0xFF));
        }
        return static_cast<bool>(out);
    }
    out.write(utf8_text.data(), static_cast<std::streamsize>(utf8_text.size()));
    return static_cast<bool>(out);
}

// JSON string escaping for embedding file text into a JS omsiApplyState({...}) call. Matches the
// convention already used by the spline creator sidebar (sc_json_escape): control chars other than
// \n \r \t become a space instead of \uXXXX to keep this dependency-free and simple.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) out += ' ';
            else out += c;
            break;
        }
    }
    return out;
}

#ifndef EM_LINESCROLL
#define EM_LINESCROLL 0x00B6
#endif

void goto_line_in_edit(HWND edit, int line) {
    if (!edit || line < 1) return;
    const int line_count = static_cast<int>(SendMessageW(edit, EM_GETLINECOUNT, 0, 0));
    if (line_count < 1) return;
    const int target = (std::min)(line, line_count) - 1;
    const int idx = static_cast<int>(SendMessageW(edit, EM_LINEINDEX, static_cast<WPARAM>(target), 0));
    if (idx < 0) return;
    const int len = static_cast<int>(SendMessageW(edit, EM_LINELENGTH, static_cast<WPARAM>(idx), 0));
    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(idx), static_cast<LPARAM>(idx + (std::max)(len, 1)));
    SendMessageW(edit, EM_SCROLLCARET, 0, 0);
}

void update_line_label(EditorState* st) {
    if (!st || !st->edit || !st->line_lbl) return;
    const DWORD start = static_cast<DWORD>(SendMessageW(st->edit, EM_GETSEL, 0, 0));
    const int line = static_cast<int>(SendMessageW(st->edit, EM_LINEFROMCHAR, start, 0)) + 1;
    wchar_t buf[64]{};
    swprintf_s(buf, L"Linea: %d", line);
    SetWindowTextW(st->line_lbl, buf);
}

void layout_editor(HWND hwnd, EditorState* st) {
    if (!st || !st->edit) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    MoveWindow(st->edit, 8, 30, w > 200 ? w - 16 : 200, h > 120 ? h - 38 : 82, TRUE);
}

HWND mk_action(HWND p, int id, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOTIFY | SS_CENTERIMAGE, x, y, w,
                         h, p, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                         nullptr);
}

// Ultralight-based UI is unavailable (missing SDK/ICU); build the plain Win32 EDIT control so the
// editor still opens and works, just without the modern chrome.
void build_native_fallback_ui(HWND hwnd, EditorState* st) {
    st->line_lbl = CreateWindowW(L"STATIC", L"Linea: 1", WS_CHILD | WS_VISIBLE, 8, 8, 120, 18, hwnd, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    mk_action(hwnd, kSaveBar, L"Guardar (Ctrl+S)", 140, 4, 120, 22);
    mk_action(hwnd, kCloseBar, L"Cerrar (Esc)", 270, 4, 90, 22);

    st->edit = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL | WS_VSCROLL |
            WS_HSCROLL | WS_TABSTOP,
        8, 30, 560, 420, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEdit)), GetModuleHandleW(nullptr),
        nullptr);
    SendMessageW(st->edit, EM_SETLIMITTEXT, 0, 1024 * 1024);
}

bool confirm_discard(HWND hwnd) {
    const int ans = MessageBoxW(
        hwnd,
        L"Hay cambios sin guardar. Cerrar sin guardar?",
        L".map editor",
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    return ans == IDYES;
}

void close_editor_window(HWND hwnd, EditorState* st) {
    if (st && st->dirty && !confirm_discard(hwnd)) return;
    DestroyWindow(hwnd);
}

// Pushes the (re)loaded file into the Ultralight editor once its JS has signalled "ready".
void push_editor_state(EditorState* st) {
    if (!st || !st->web_ok) return;
    const std::string file_name = omsi::wide_to_utf8(st->path.filename().wstring());
    std::string js = "window.omsiApplyState({fileName:\"" + json_escape(file_name) + "\",text:\"" +
                      json_escape(st->utf8_text) + "\",gotoLine:" + std::to_string(st->goto_line) + "});";
    st->web.evaluate_script(js);
}

void do_save(HWND hwnd, EditorState* st) {
    if (!st) return;
    if (st->allowed_write_root && !path_is_under(st->path, *st->allowed_write_root)) {
        MessageBoxW(hwnd, L"Solo se puede guardar en la copia de edicion (*_actual), no en el mapa original.",
                    L".map editor", MB_OK | MB_ICONWARNING);
        return;
    }
    std::string utf8;
    if (st->web_ok) {
        utf8 = st->web.evaluate_script_sync("document.getElementById('editor').value");
    } else if (st->edit) {
        const int len = GetWindowTextLengthW(st->edit);
        std::wstring w(static_cast<std::size_t>(len) + 1, L'\0');
        GetWindowTextW(st->edit, w.data(), len + 1);
        w.resize(static_cast<std::size_t>(len));
        utf8 = omsi::wide_to_utf8(w);
    }
    if (save_file_utf8(st->path, utf8, st->utf16_le)) {
        st->utf8_text = utf8;
        st->saved = true;
        st->dirty = false;
        if (st->web_ok)
            st->web.evaluate_script("window.omsiSetStatus&&window.omsiSetStatus('Guardado','ok');");
        else
            MessageBoxW(hwnd, L"Archivo guardado.", L".map editor", MB_OK | MB_ICONINFORMATION);
        log::info("map text editor saved " + st->path.string());
    } else {
        if (st->web_ok)
            st->web.evaluate_script("window.omsiSetStatus&&window.omsiSetStatus('Error al guardar','err');");
        MessageBoxW(hwnd, L"No se pudo guardar.", L".map editor", MB_OK | MB_ICONERROR);
        log::error("map text editor save failed " + st->path.string());
    }
}

// Runs on the omsiPost() bridge callback, i.e. potentially nested inside the Ultralight view's own
// JS/event dispatch call stack. Never destroy/teardown the view from here directly (WM_CLOSE /
// WM_MAPTEXT_SAVE are posted instead of handled inline) to avoid reentering View::EvaluateScript /
// FireMouseEvent while they are still executing.
bool set_clipboard_utf8(const std::string& utf8) {
    const std::wstring wide = omsi::utf8_to_wide(utf8);
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        CloseClipboard();
        return false;
    }
    void* p = GlobalLock(mem);
    if (p) {
        std::memcpy(p, wide.c_str(), bytes);
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
    return p != nullptr;
}

std::string get_clipboard_utf8() {
    if (!OpenClipboard(nullptr)) return {};
    std::string out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = omsi::wide_to_utf8(p);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

void do_native_clipboard(HWND hwnd, EditorState* st, const char* op) {
    if (!st) return;
    if (st->web_ok) {
        if (std::strcmp(op, "copy") == 0 || std::strcmp(op, "cut") == 0) {
            const std::string t = st->web.evaluate_script_sync("window._omsiClip||''");
            set_clipboard_utf8(t);
        } else if (std::strcmp(op, "paste") == 0) {
            const std::string t = get_clipboard_utf8();
            st->web.evaluate_script("window.omsiInsertPaste&&window.omsiInsertPaste(\"" + json_escape(t) +
                                    "\");");
        }
        return;
    }
    if (!st->edit) return;
    if (std::strcmp(op, "copy") == 0) SendMessageW(st->edit, WM_COPY, 0, 0);
    else if (std::strcmp(op, "cut") == 0) SendMessageW(st->edit, WM_CUT, 0, 0);
    else if (std::strcmp(op, "paste") == 0) SendMessageW(st->edit, WM_PASTE, 0, 0);
    (void)hwnd;
}

void show_native_edit_menu(HWND hwnd, EditorState* st, int x, int y) {
    if (!st || !st->edit) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 9401, L"Cortar\tCtrl+X");
    AppendMenuW(menu, MF_STRING, 9402, L"Copiar\tCtrl+C");
    AppendMenuW(menu, MF_STRING, 9403, L"Pegar\tCtrl+V");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 9404, L"Seleccionar todo\tCtrl+A");
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN, x, y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == 9401) SendMessageW(st->edit, WM_CUT, 0, 0);
    else if (cmd == 9402) SendMessageW(st->edit, WM_COPY, 0, 0);
    else if (cmd == 9403) SendMessageW(st->edit, WM_PASTE, 0, 0);
    else if (cmd == 9404) SendMessageW(st->edit, EM_SETSEL, 0, -1);
}

void handle_editor_json(HWND hwnd, EditorState* st, const std::string& json) {
    if (!st) return;
    if (json.find("\"type\":\"ready\"") != std::string::npos) {
        push_editor_state(st);
        return;
    }
    if (json.find("\"type\":\"dirty\"") != std::string::npos) {
        if (!st->loading) st->dirty = true;
        return;
    }
    if (json.find("\"type\":\"cmd\"") != std::string::npos) {
        if (json.find("\"name\":\"save\"") != std::string::npos) {
            PostMessageW(hwnd, WM_MAPTEXT_SAVE, 0, 0);
        } else if (json.find("\"name\":\"close\"") != std::string::npos) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return;
    }
    if (json.find("\"type\":\"clipboard\"") != std::string::npos) {
        if (json.find("\"op\":\"paste\"") != std::string::npos)
            PostMessageW(hwnd, WM_MAPTEXT_PASTE, 0, 0);
        else if (json.find("\"op\":\"cut\"") != std::string::npos)
            PostMessageW(hwnd, WM_MAPTEXT_CUT, 0, 0);
        else
            PostMessageW(hwnd, WM_MAPTEXT_COPY, 0, 0);
    }
}

LRESULT CALLBACK editor_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<EditorState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<EditorState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        if (!st) return -1;

        st->utf16_le = file_is_utf16_le(st->path);
        std::wstring wide_text;
        {
            st->loading = true;
            wide_text = load_file_for_edit(st->path, st->utf8_text, st->utf16_le);
            st->loading = false;
            st->dirty = false;
        }

        RECT cr{};
        GetClientRect(hwnd, &cr);
        st->web.set_event_callback(
            [st, hwnd](const std::string& json) { handle_editor_json(hwnd, st, json); });
        st->web_ok = st->web.create(hwnd, 0, 0, (std::max)(static_cast<int>(cr.right), 1),
                                    (std::max)(static_cast<int>(cr.bottom), 1), L"map_text_editor");
        if (st->web_ok) {
            st->web.set_accept_keyboard(true);
            st->web.focus_view();
            SetFocus(st->web.hwnd());
        } else {
            log::warn("map text editor: Ultralight UI unavailable, using native fallback");
            build_native_fallback_ui(hwnd, st);
            SetWindowTextW(st->edit, wide_text.c_str());
            layout_editor(hwnd, st);
            if (st->goto_line > 1) PostMessageW(hwnd, WM_MAPTEXT_GOTO, static_cast<WPARAM>(st->goto_line), 0);
            update_line_label(st);
        }
        return 0;
    }
    case WM_MAPTEXT_GOTO:
        if (st && st->edit) {
            goto_line_in_edit(st->edit, static_cast<int>(wp));
            update_line_label(st);
        }
        return 0;
    case WM_MAPTEXT_SAVE:
        if (st) do_save(hwnd, st);
        return 0;
    case WM_MAPTEXT_COPY:
        if (st) do_native_clipboard(hwnd, st, "copy");
        return 0;
    case WM_MAPTEXT_PASTE:
        if (st) do_native_clipboard(hwnd, st, "paste");
        return 0;
    case WM_MAPTEXT_CUT:
        if (st) do_native_clipboard(hwnd, st, "cut");
        return 0;
    case WM_CONTEXTMENU:
        if (st && st->edit && reinterpret_cast<HWND>(wp) == st->edit) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);
            show_native_edit_menu(hwnd, st, pt.x, pt.y);
            return 0;
        }
        break;
    case WM_SHOWWINDOW:
        if (st && wp) {
            if (st->web_ok) {
                st->web.focus_view();
                SetFocus(st->web.hwnd());
            } else if (st->edit && st->goto_line > 1) {
                goto_line_in_edit(st->edit, st->goto_line);
                update_line_label(st);
            }
        }
        return 0;
    case WM_SETFOCUS:
        if (st && st->web_ok) {
            st->web.focus_view();
            SetFocus(st->web.hwnd());
        }
        return 0;
    case WM_SIZE:
        if (st) {
            if (st->web_ok) {
                RECT crr{};
                GetClientRect(hwnd, &crr);
                st->web.set_bounds(0, 0, (std::max)(static_cast<int>(crr.right), 1),
                                   (std::max)(static_cast<int>(crr.bottom), 1));
            } else {
                layout_editor(hwnd, st);
            }
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        if (id == kSaveBar) {
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkColor(hdc, RGB(32, 120, 72));
            static HBRUSH br_save = CreateSolidBrush(RGB(32, 120, 72));
            return reinterpret_cast<LRESULT>(br_save);
        }
        if (id == kCloseBar) {
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkColor(hdc, RGB(120, 40, 40));
            static HBRUSH br_close = CreateSolidBrush(RGB(120, 40, 40));
            return reinterpret_cast<LRESULT>(br_close);
        }
        break;
    }
    case WM_COMMAND: {
        if (!st) return 0;
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (id == kEdit && code == EN_CHANGE) {
            if (!st->loading) st->dirty = true;
            update_line_label(st);
            return 0;
        }
        if (code == STN_CLICKED || code == BN_CLICKED) {
            if (id == kSaveBar) {
                do_save(hwnd, st);
                return 0;
            }
            if (id == kCloseBar) {
                close_editor_window(hwnd, st);
                return 0;
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (st) {
            if (wp == VK_ESCAPE) {
                close_editor_window(hwnd, st);
                return 0;
            }
            if (wp == 'S' && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
                do_save(hwnd, st);
                return 0;
            }
        }
        break;
    case WM_CLOSE:
        close_editor_window(hwnd, st);
        return 0;
    case WM_DESTROY:
        if (g_editor && g_editor->hwnd == hwnd) g_editor.reset();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ensure_class() {
    static bool reg = false;
    if (reg) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = editor_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OmsiF4MapTextEditor";
    wc.hbrBackground = CreateSolidBrush(RGB(15, 23, 42)); // slate-900, avoids a white flash pre-paint
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    reg = true;
}

} // namespace

void open_map_text_editor(HWND owner, const std::filesystem::path& path, int goto_line,
                          const std::filesystem::path* allowed_write_root) {
    if (path.empty() || !std::filesystem::exists(path)) return;
    ensure_class();

    if (g_editor && IsWindow(g_editor->hwnd)) {
        if (g_editor->st.dirty && !confirm_discard(g_editor->hwnd)) return;
        DestroyWindow(g_editor->hwnd);
        g_editor.reset();
    }

    g_editor = std::make_unique<ActiveEditor>();
    g_editor->st.path = path;
    g_editor->st.goto_line = (std::max)(goto_line, 1);
    if (allowed_write_root && !allowed_write_root->empty())
        g_editor->st.allowed_write_root = *allowed_write_root;

    RECT wr{};
    int x = 80, y = 60, w = 760, h = 560;
    if (owner && GetWindowRect(owner, &wr)) {
        x = wr.left + 40;
        y = wr.top + 40;
    }

    std::wstring title = L".map - " + path.filename().wstring();
    g_editor->hwnd =
        CreateWindowExW(WS_EX_TOOLWINDOW, L"OmsiF4MapTextEditor", title.c_str(),
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_SIZEBOX, x, y, w, h, owner,
                        nullptr, GetModuleHandleW(nullptr), &g_editor->st);
    if (!g_editor->hwnd) {
        g_editor.reset();
        return;
    }
    ShowWindow(g_editor->hwnd, SW_SHOW);
    SetForegroundWindow(g_editor->hwnd);
    // SetForegroundWindow can reset focus to the top-level window; re-assert it on the Ultralight
    // child (and the WebCore-level page focus) so typing works immediately, no extra click needed.
    if (g_editor->st.web_ok && g_editor->st.web.hwnd()) {
        g_editor->st.web.focus_view();
        SetFocus(g_editor->st.web.hwnd());
    }
}

void pump_map_text_editor() {
    if (!g_editor || !IsWindow(g_editor->hwnd)) return;
    const HWND dlg = g_editor->hwnd;
    MSG msg{};
    while (HIWORD(GetQueueStatus(QS_ALLINPUT)) != 0 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            continue;
        }
        // Keyboard focus normally sits on the embedded Ultralight child (for typing), so Esc/Ctrl+S
        // must be intercepted here and redirected to the top-level window's WM_KEYDOWN handler
        // instead of being forwarded into the page as ordinary keystrokes.
        if ((msg.hwnd == dlg || IsChild(dlg, msg.hwnd)) && msg.message == WM_KEYDOWN &&
            (msg.wParam == VK_ESCAPE || (msg.wParam == 'S' && (GetAsyncKeyState(VK_CONTROL) & 0x8000)))) {
            SendMessageW(dlg, WM_KEYDOWN, msg.wParam, msg.lParam);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool map_text_editor_is_open() { return g_editor && IsWindow(g_editor->hwnd); }

} // namespace omsi::ui
