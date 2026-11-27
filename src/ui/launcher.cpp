#include "launcher.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"
#include "omsi/map_types.hpp"
#include "omsi/options.hpp"
#include "omsi/utf8_win32.hpp"
#include "ui/win32_dark.hpp"
#include "gfx/gfx_backend.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <cwctype>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "Normaliz.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' "\
                        "version='6.0.0.0' processorArchitecture='*' "\
                        "publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace omsi::launcher {
namespace {

constexpr int IDC_MAP = 1001;
constexpr int IDC_ENTRY = 1002;
constexpr int IDC_OK = 1003;           // Editar mapa (robocopy workspace)
constexpr int IDC_CANCEL = 1004;
constexpr int IDC_BROWSE_OMSI = 1005;
constexpr int IDC_PATH_LABEL = 1006;
constexpr int IDC_VIEW_ONLY = 1007;    // Solo visualizar (sin editar)
constexpr int IDC_TO_BEAMNG = 1008;    // cambiar al launcher de BeamNG.drive
constexpr int IDC_ROOTS = 1009;        // rutas de OMSI 2 usadas antes (desplegable)
constexpr int IDC_MAP_SEARCH = 1010;
constexpr int IDC_MAP_CLEAR = 1011;
constexpr int IDC_ENTRY_SEARCH = 1012;
constexpr int IDC_ENTRY_CLEAR = 1013;
constexpr int IDC_GFX = 1014;         // "Gráficos": auto / DirectX 12 / Vulkan / WARP

// Text read from OMSI cfg files is ANSI (system code page) or UTF-8: always keep UTF-8 in memory.
std::string utf8_text(const std::string& s) { return omsi::is_valid_utf8(s) ? s : omsi::acp_to_utf8(s); }

struct MapItem {
    std::filesystem::path global_cfg;
    std::string label;
    std::string folder_name;
    std::optional<map::GlobalMap> data;
};

std::string folder_name_for_cfg(const std::filesystem::path& global_cfg) {
    return omsi::wide_to_utf8(global_cfg.parent_path().filename().native()); // any language (no ANSI throw)
}

std::string map_display_label(const std::filesystem::path& global_cfg, const std::string& cfg_name) {
    const std::string folder = folder_name_for_cfg(global_cfg);
    if (cfg_name.empty()) return folder;
    return cfg_name + " [" + folder + "]";
}

std::wstring entrypoint_list_label(const map::GlobalMap& global, const map::EntryPoint& ep, std::size_t index) {
    const std::wstring map_folder = global.root.filename().native();
    const std::wstring name = ep.name.empty() ? (L"id=" + std::to_wstring(ep.id)) : omsi::utf8_to_wide(ep.name);
    std::wstring tile_tag;
    if (ep.category >= 0 && static_cast<std::size_t>(ep.category) < global.tiles.size()) {
        tile_tag = global.tiles[static_cast<std::size_t>(ep.category)].tile_path.stem().native();
    }
    wchar_t buf[512]{};
    if (!tile_tag.empty())
        swprintf_s(buf, L"%zu: %ls  [%ls / %ls]  (%.0f, %.0f)", index, name.c_str(), map_folder.c_str(),
                   tile_tag.c_str(), ep.position.x, ep.position.y);
    else
        swprintf_s(buf, L"%zu: %ls  [%ls]  (%.0f, %.0f)", index, name.c_str(), map_folder.c_str(), ep.position.x,
                   ep.position.y);
    return buf;
}

// Search: case and accent insensitive (n finds n and ñ, "depot sur" needs both words), any language.
std::wstring fold_text(std::wstring s) {
    if (s.empty()) return s;
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    const int n = NormalizeString(NormalizationKD, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n > 0) {
        std::wstring d(static_cast<std::size_t>(n), L'\0');
        const int m = NormalizeString(NormalizationKD, s.c_str(), static_cast<int>(s.size()), d.data(), n);
        if (m > 0) {
            d.resize(static_cast<std::size_t>(m));
            s.swap(d);
        }
    }
    std::wstring out;
    out.reserve(s.size());
    for (const wchar_t c : s)
        if (!(c >= 0x0300 && c <= 0x036F)) out.push_back(c); // combining accents
    return out;
}
std::vector<std::wstring> search_tokens(HWND edit) {
    std::vector<std::wstring> toks;
    if (!edit) return toks;
    wchar_t buf[256]{};
    GetWindowTextW(edit, buf, 256);
    const std::wstring f = fold_text(buf);
    std::size_t i = 0;
    while (i < f.size()) {
        while (i < f.size() && (f[i] == L' ' || f[i] == L'\t')) ++i;
        std::size_t j = i;
        while (j < f.size() && f[j] != L' ' && f[j] != L'\t') ++j;
        if (j > i) toks.push_back(f.substr(i, j - i));
        i = j;
    }
    return toks;
}
bool matches_search(const std::wstring& text, const std::vector<std::wstring>& toks) {
    if (toks.empty()) return true;
    const std::wstring f = fold_text(text);
    for (const auto& t : toks)
        if (f.find(t) == std::wstring::npos) return false;
    return true;
}

struct DialogState {
    std::filesystem::path omsi_root;
    std::vector<MapItem> items;
    std::size_t initial_map = 0;
    std::size_t initial_entry = 0;
    std::size_t selected_map = 0;
    std::size_t selected_entry = 0;
    bool done = false;
    bool ok = false;
    bool view_only = false; // set when user picks "Solo visualizar"
    bool switch_game = false; // "BeamNG.drive" button
    HWND owner = nullptr;
    HWND mapbox = nullptr;
    HWND entrybox = nullptr;
    HWND path_label = nullptr;
    HWND rootbox = nullptr;     // OMSI 2 folders used before
    HWND map_search = nullptr;
    HWND entry_search = nullptr;
    std::vector<std::filesystem::path> root_rows;
    std::vector<std::size_t> map_view;   // list row -> items index (search filter)
    std::vector<std::size_t> entry_view; // list row -> entrypoint index
    bool entry_initial_done = false;
    HWND gfxbox = nullptr;
    int gfx_choice = 0; // gfx::backend::Kind
};

std::wstring to_wide(const std::string& s) { return omsi::utf8_to_wide(s); }

std::string quick_map_label(const std::filesystem::path& global_cfg) {
    const auto lines = cfg::read_cfg_lines(global_cfg);
    std::string section;
    for (const auto& raw : lines) {
        std::string line = raw;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() >= 3 && line.front() == '[' && line.back() == ']') {
            section = line;
            continue;
        }
        if (section == "[friendlyname]" && !line.empty()) return utf8_text(line);
        if (section == "[name]" && !line.empty()) return utf8_text(line);
    }
    return folder_name_for_cfg(global_cfg);
}

bool ensure_map_loaded(MapItem& item) {
    if (item.data) return true;
    auto loaded = map::load_global_map(item.global_cfg);
    if (!loaded) {
        log::warn("failed to load map: " + omsi::path_text(item.global_cfg));
        return false;
    }
    item.data = std::move(*loaded);
    item.folder_name = folder_name_for_cfg(item.global_cfg);
    const std::string cfg_name =
        item.data->friendly_name.empty() ? item.data->name : item.data->friendly_name;
    item.label =
        map_display_label(item.global_cfg, cfg_name.empty() ? quick_map_label(item.global_cfg) : utf8_text(cfg_name));
    return true;
}

void refill_entries(DialogState* st) {
    if (!st || !st->entrybox) return;
    SendMessageW(st->entrybox, LB_RESETCONTENT, 0, 0);

    if (st->selected_map >= st->items.size()) {
        log::warn("refill_entries: selected_map out of range");
        return;
    }

    auto& item = st->items[st->selected_map];
    if (!ensure_map_loaded(item)) return;

    const auto& m = *item.data;
    std::ostringstream oss;
    oss << "refill_entries map[" << st->selected_map << "] \"" << item.label
        << "\" entrypoints=" << m.entrypoints.size();
    log::info(oss.str());

    const auto toks = search_tokens(st->entry_search);
    st->entry_view.clear();
    for (std::size_t i = 0; i < m.entrypoints.size(); ++i) {
        const auto& ep = m.entrypoints[i];
        const std::wstring label = entrypoint_list_label(m, ep, i);
        if (!matches_search(label, toks)) continue;
        st->entry_view.push_back(i);
        SendMessageW(st->entrybox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }

    if (m.entrypoints.empty()) {
        st->selected_entry = 0;
        log::warn("map has zero entrypoints: " + item.label);
        return;
    }
    if (st->entry_view.empty()) return; // the search hides all of them: keep the last choice
    std::size_t prefer = st->selected_entry;
    if (st->selected_map == st->initial_map && st->initial_entry < m.entrypoints.size() && !st->entry_initial_done)
        prefer = st->initial_entry;
    st->entry_initial_done = true;
    std::size_t row = 0;
    for (std::size_t r = 0; r < st->entry_view.size(); ++r)
        if (st->entry_view[r] == prefer) row = r;
    SendMessageW(st->entrybox, LB_SETCURSEL, static_cast<WPARAM>(row), 0);
    st->selected_entry = st->entry_view[row];
}

// Drop-down with the current OMSI 2 folder first, then the others used before.
void update_path_label(DialogState* st) {
    if (!st || !st->rootbox) return;
    st->root_rows.clear();
    st->root_rows.push_back(st->omsi_root);
    std::wstring cur = st->omsi_root.lexically_normal().native();
    for (auto& c : cur) c = static_cast<wchar_t>(towlower(c));
    for (const auto& r : options::load_recent_omsi_roots()) {
        std::wstring k = r.lexically_normal().native();
        for (auto& c : k) c = static_cast<wchar_t>(towlower(c));
        if (k != cur) st->root_rows.push_back(r);
    }
    SendMessageW(st->rootbox, CB_RESETCONTENT, 0, 0);
    for (const auto& r : st->root_rows) SendMessageW(st->rootbox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(r.c_str()));
    SendMessageW(st->rootbox, CB_SETCURSEL, 0, 0);
}

void refill_map_list(DialogState* st) {
    if (!st || !st->mapbox) return;
    SendMessageW(st->mapbox, LB_RESETCONTENT, 0, 0);
    const auto toks = search_tokens(st->map_search);
    st->map_view.clear();
    for (std::size_t i = 0; i < st->items.size(); ++i) {
        const std::wstring lab = to_wide(st->items[i].label);
        if (!matches_search(lab, toks)) continue;
        st->map_view.push_back(i);
        SendMessageW(st->mapbox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lab.c_str()));
    }
    if (!st->map_view.empty()) {
        std::size_t row = 0;
        for (std::size_t r = 0; r < st->map_view.size(); ++r)
            if (st->map_view[r] == st->selected_map) row = r;
        SendMessageW(st->mapbox, LB_SETCURSEL, static_cast<WPARAM>(row), 0);
        const bool changed = st->selected_map != st->map_view[row];
        st->selected_map = st->map_view[row];
        if (changed) st->selected_entry = 0;
        refill_entries(st);
    } else {
        SendMessageW(st->entrybox, LB_RESETCONTENT, 0, 0);
        st->entry_view.clear();
    }
}

struct ProgressDlg {
    HWND hwnd = nullptr;
    HWND bar = nullptr;
    HWND label = nullptr;
    bool ready = false;
};

LRESULT CALLBACK ProgressWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    {
        LRESULT out = 0;
        if (omsi::ui::dark::handle_ctlcolor(msg, wp, out)) return out;
        if (msg == WM_ERASEBKGND) {
            omsi::ui::dark::handle_erase(hwnd, wp, out);
            return out;
        }
    }
    switch (msg) {
    case WM_CLOSE:
        return 0; // no cancel during scan
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

ProgressDlg begin_progress(HWND owner, const wchar_t* title) {
    ProgressDlg p{};
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    static bool registered = false;
    const wchar_t* cls = L"OmsiF4ScanProgress";
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ProgressWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        registered = true;
    }

    p.hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, cls, title,
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 480, 140,
                             owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!p.hwnd) return p;
    p.label = CreateWindowExW(0, L"STATIC", L"Escaneando mapas…", WS_CHILD | WS_VISIBLE, 16, 16, 440, 20, p.hwnd,
                              nullptr, nullptr, nullptr);
    p.bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 16, 48, 440, 22, p.hwnd, nullptr, nullptr,
                            nullptr);
    SendMessageW(p.bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(p.bar, PBM_SETPOS, 0, 0);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(p.hwnd, SW_SHOW);
    omsi::ui::dark::apply(p.hwnd);
    omsi::ui::dark::center(p.hwnd);
    UpdateWindow(p.hwnd);
    p.ready = true;
    return p;
}

void pump_progress(ProgressDlg& p) {
    if (!p.hwnd) return;
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            // Never re-post: this dialog is nested in the launcher loop and a WM_QUIT would close the
            // next launcher (or the app) as soon as the scan finishes.
            log::warn("launcher: WM_QUIT durante el escaneo descartado (" +
                      std::to_string(static_cast<int>(msg.wParam)) + ")");
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// A stale WM_QUIT (left by the WM_DESTROY of a previous dialog) makes the next nested loop return at once.
void drain_pending_quit() {
    MSG m{};
    while (PeekMessageW(&m, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
        log::warn("launcher: WM_QUIT pendiente descartado (" + std::to_string(static_cast<int>(m.wParam)) + ")");
}

void set_progress(ProgressDlg& p, int cur, int total, const std::wstring& text) {
    if (!p.ready) return;
    SetWindowTextW(p.label, text.c_str());
    const int pct = (total > 0) ? static_cast<int>((100.0 * cur) / total) : 0;
    SendMessageW(p.bar, PBM_SETPOS, static_cast<WPARAM>(pct), 0);
    pump_progress(p);
}

void end_progress(ProgressDlg& p, HWND owner) {
    if (p.hwnd) {
        DestroyWindow(p.hwnd);
        p.hwnd = nullptr;
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    p.ready = false;
}

std::vector<MapItem> discover_map_catalog_with_progress(const std::filesystem::path& omsi_root, HWND owner) {
    std::vector<MapItem> items;
    const auto maps_dir = omsi_root / "maps";
    ProgressDlg prog = begin_progress(owner, L"Feditor — Cargando mapas");
    set_progress(prog, 0, 1, L"Listando carpetas en maps\\…");

    std::vector<std::filesystem::path> dirs;
    std::error_code ec;
    if (std::filesystem::is_directory(maps_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(maps_dir, ec)) {
            if (!entry.is_directory()) continue;
            const std::wstring name = entry.path().filename().native();
            if (name.size() >= 9 && _wcsicmp(name.c_str() + name.size() - 9, L"_original") == 0) continue;
            dirs.push_back(entry.path());
            if ((dirs.size() % 8) == 0) {
                set_progress(prog, 0, 1,
                             L"Listando carpetas… (" + std::to_wstring(dirs.size()) + L")");
            }
        }
    }
    std::sort(dirs.begin(), dirs.end());

    const int total = static_cast<int>(dirs.size());
    log::info("launcher scan maps_dir dirs=" + std::to_string(total));
    for (int i = 0; i < total; ++i) {
        const auto& dir = dirs[static_cast<std::size_t>(i)];
        const auto global_cfg = dir / "global.cfg";
        std::wstring status = L"Leyendo " + dir.filename().wstring() + L"… (" + std::to_wstring(i + 1) + L"/" +
                              std::to_wstring(total) + L")";
        set_progress(prog, i + 1, total, status);
        if (!std::filesystem::exists(global_cfg)) continue;
        MapItem item{};
        item.global_cfg = global_cfg;
        item.folder_name = folder_name_for_cfg(global_cfg);
        item.label = map_display_label(global_cfg, quick_map_label(global_cfg));
        items.push_back(std::move(item));
    }

    set_progress(prog, total, (std::max)(total, 1),
                 L"Listo: " + std::to_wstring(items.size()) + L" mapas");
    pump_progress(prog);
    Sleep(120);
    end_progress(prog, owner);
    log::info("discovered " + std::to_string(items.size()) + " global.cfg files (lazy catalog)");
    return items;
}

std::size_t find_map_index(const std::vector<MapItem>& items, const std::filesystem::path& global_cfg) {
    if (global_cfg.empty() || items.empty()) return 0;
    std::error_code ec;
    const auto want = global_cfg.lexically_normal();
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].global_cfg.lexically_normal() == want) return i;
        if (std::filesystem::equivalent(items[i].global_cfg, global_cfg, ec)) return i;
    }
    auto map_dir_key = [](std::filesystem::path p) -> std::wstring {
        p = p.lexically_normal();
        std::wstring dir = p.parent_path().filename().native();
        const auto lower = [&](const wchar_t* suf, std::size_t n) {
            if (dir.size() >= n && _wcsicmp(dir.c_str() + dir.size() - n, suf) == 0) dir.erase(dir.size() - n);
        };
        lower(L"_actual", 7);
        lower(L"_original", 9);
        for (auto& c : dir) c = static_cast<wchar_t>(towlower(c));
        return dir;
    };
    const std::wstring want_key = map_dir_key(want);
    if (want_key.empty()) return 0;
    std::size_t found = items.size();
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (map_dir_key(items[i].global_cfg) != want_key) continue;
        if (found < items.size()) return 0;
        found = i;
    }
    return found < items.size() ? found : 0;
}

LRESULT CALLBACK DialogWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    {
        LRESULT out = 0;
        if (omsi::ui::dark::handle_ctlcolor(msg, wp, out)) return out;
        if (msg == WM_ERASEBKGND) {
            omsi::ui::dark::handle_erase(hwnd, wp, out);
            return out;
        }
    }
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = static_cast<DialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

        CreateWindowExW(0, L"STATIC", L"Feditor  —  OMSI 2", WS_CHILD | WS_VISIBLE, 12, 10, 300, 18, hwnd, nullptr,
                        nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"BeamNG.drive %BA", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 440, 2, 108, 24, hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TO_BEAMNG)), nullptr, nullptr);
        auto mk = [&](DWORD ex, const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id) {
            return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
        };
        mk(0, L"STATIC", L"OMSI 2:", SS_NOPREFIX, 12, 34, 52, 18, IDC_PATH_LABEL);
        st->rootbox = mk(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 66, 30, 366, 260, IDC_ROOTS);
        SetWindowTheme(st->rootbox, L"DarkMode_CFD", nullptr);
        mk(0, L"BUTTON", L"Agregar ruta…", WS_TABSTOP, 440, 29, 108, 26, IDC_BROWSE_OMSI);
        mk(0, L"STATIC", L"Mapa:", 0, 12, 66, 52, 18, 0);
        st->map_search = mk(WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 66, 62, 448, 24, IDC_MAP_SEARCH);
        SendMessageW(st->map_search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Buscar mapa…"));
        mk(0, L"BUTTON", L"\u2715", WS_TABSTOP, 518, 62, 30, 24, IDC_MAP_CLEAR);
        st->mapbox = mk(WS_EX_CLIENTEDGE, WC_LISTBOXW, L"", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, 12, 90, 536, 150,
                        IDC_MAP);
        mk(0, L"STATIC", L"Entrypoint:", 0, 12, 252, 80, 18, 0);
        st->entry_search =
            mk(WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 94, 248, 420, 24, IDC_ENTRY_SEARCH);
        SendMessageW(st->entry_search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Buscar entrypoint…"));
        mk(0, L"BUTTON", L"\u2715", WS_TABSTOP, 518, 248, 30, 24, IDC_ENTRY_CLEAR);
        st->entrybox = mk(WS_EX_CLIENTEDGE, WC_LISTBOXW, L"", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, 12, 276, 536, 150,
                          IDC_ENTRY);
        SetWindowTheme(st->map_search, L"DarkMode_CFD", nullptr);
        SetWindowTheme(st->entry_search, L"DarkMode_CFD", nullptr);
        mk(0, L"STATIC",
           L"Editar = copia workspace (enlaces).  Solo visualizar = carga rápida, sin mover/crear/borrar.",
           SS_LEFTNOWORDWRAP, 12, 432, 340, 32, 0);
        // Graphics backend (docs/GFX_BACKENDS.md): saved to gfx_backend.txt; a different one restarts the editor.
        mk(0, L"STATIC", L"Gráficos:", SS_NOPREFIX, 12, 474, 58, 18, 0);
        st->gfxbox = mk(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 72, 470, 170, 200, IDC_GFX);
        for (int k = 0; k < 4; ++k)
            SendMessageW(st->gfxbox, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(
                             to_wide(gfx::backend::label(static_cast<gfx::backend::Kind>(k))).c_str()));
        SendMessageW(st->gfxbox, CB_SETCURSEL, static_cast<WPARAM>(gfx::backend::requested()), 0);
        mk(0, L"BUTTON", L"Editar mapa", BS_DEFPUSHBUTTON | WS_TABSTOP, 250, 468, 110, 28, IDC_OK);
        mk(0, L"BUTTON", L"Solo visualizar", WS_TABSTOP, 368, 468, 120, 28, IDC_VIEW_ONLY);
        mk(0, L"BUTTON", L"Cancelar", WS_TABSTOP, 496, 468, 80, 28, IDC_CANCEL);

        update_path_label(st);
        log::info("launcher Win32 dialog: " + std::to_string(st->items.size()) + " maps");
        refill_map_list(st);
        return 0;
    }
    case WM_COMMAND: {
        if (!st) break;
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        // switch to another OMSI 2 folder and reload its maps right away
        auto switch_root = [&](const std::filesystem::path& picked) {
            if (!options::is_valid_omsi_root(picked)) {
                MessageBoxW(hwnd, L"La carpeta elegida no parece OMSI 2 (falta Omsi.exe o maps\\).", L"Ruta inválida",
                            MB_OK | MB_ICONWARNING);
                update_path_label(st);
                return;
            }
            st->omsi_root = picked.lexically_normal();
            options::save_omsi_root(st->omsi_root);
            update_path_label(st);
            st->items = discover_map_catalog_with_progress(st->omsi_root, hwnd);
            st->initial_map = 0;
            st->selected_map = 0;
            st->selected_entry = 0;
            st->entry_initial_done = true;
            refill_map_list(st);
            if (st->items.empty())
                MessageBoxW(hwnd, L"No se encontraron mapas (global.cfg) en maps\\.", L"Sin mapas", MB_OK | MB_ICONWARNING);
        };
        if (id == IDC_BROWSE_OMSI) {
            if (auto picked = browse_omsi_root(hwnd)) switch_root(*picked);
            return 0;
        }
        if (id == IDC_ROOTS && code == CBN_SELCHANGE) {
            const LRESULT sel = SendMessageW(st->rootbox, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR && sel > 0 && static_cast<std::size_t>(sel) < st->root_rows.size())
                switch_root(st->root_rows[static_cast<std::size_t>(sel)]);
            return 0;
        }
        if (id == IDC_MAP_SEARCH && code == EN_CHANGE) {
            refill_map_list(st);
            return 0;
        }
        if (id == IDC_ENTRY_SEARCH && code == EN_CHANGE) {
            refill_entries(st);
            return 0;
        }
        if (id == IDC_MAP_CLEAR || id == IDC_ENTRY_CLEAR) {
            HWND e = id == IDC_MAP_CLEAR ? st->map_search : st->entry_search;
            SetWindowTextW(e, L""); // EN_CHANGE refills the list
            SetFocus(e);
            return 0;
        }
        if (id == IDC_MAP && code == LBN_SELCHANGE) {
            const LRESULT sel = SendMessageW(st->mapbox, LB_GETCURSEL, 0, 0);
            if (sel == LB_ERR || static_cast<std::size_t>(sel) >= st->map_view.size()) return 0;
            st->selected_map = st->map_view[static_cast<std::size_t>(sel)];
            st->selected_entry = 0;
            log::info("map changed -> index " + std::to_string(st->selected_map) + " \"" +
                      st->items[st->selected_map].label + "\"");
            refill_entries(st);
            return 0;
        }
        if (id == IDC_ENTRY && code == LBN_SELCHANGE) {
            const LRESULT sel = SendMessageW(st->entrybox, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR && static_cast<std::size_t>(sel) < st->entry_view.size())
                st->selected_entry = st->entry_view[static_cast<std::size_t>(sel)];
            return 0;
        }
        if (id == IDC_ENTRY && code == LBN_DBLCLK) {
            st->ok = true;
            st->done = true;
            log::info("launcher entry double-click OK");
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDC_OK || id == IDC_VIEW_ONLY) {
            const LRESULT g = SendMessageW(st->gfxbox, CB_GETCURSEL, 0, 0);
            st->gfx_choice = g == CB_ERR ? 0 : static_cast<int>(g);
        }
        if (id == IDC_OK) {
            st->ok = true;
            st->view_only = false;
            st->done = true;
            log::info("launcher OK (edit mode — workspace robocopy)");
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDC_VIEW_ONLY) {
            st->ok = true;
            st->view_only = true;
            st->done = true;
            log::info("launcher OK (view-only — no workspace, mutations blocked)");
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDC_TO_BEAMNG) {
            st->switch_game = true;
            st->done = true;
            log::info("launcher -> BeamNG.drive");
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDC_CANCEL) {
            st->done = true;
            log::info("launcher cancel");
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
        if (st && wp == VK_RETURN) {
            st->ok = true;
            st->view_only = false; // Enter = edit mode (default)
            st->done = true;
            log::info("launcher Enter OK (edit mode)");
            DestroyWindow(hwnd);
            return 0;
        }
        if (st && wp == VK_ESCAPE) {
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (st) {
            st->done = true;
            log::info("launcher close");
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        // No PostQuitMessage: nested dialog, a WM_QUIT here would kill the next launcher loop.
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool run_launcher_dialog(DialogState* st) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    const wchar_t* cls = L"OmsiF4Launcher";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DialogWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    RegisterClassExW(&wc);

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"Feditor - Mapa y Entrypoint",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                               600, 548, st->owner, nullptr, wc.hInstance, st);
    if (!dlg) {
        log::error("CreateWindowEx launcher failed");
        return false;
    }
    if (st->owner) EnableWindow(st->owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    omsi::ui::dark::apply(dlg);
    omsi::ui::dark::center(dlg);
    UpdateWindow(dlg);

    drain_pending_quit(); // a stale WM_QUIT would end this loop before the user touches anything
    MSG msg{};
    while (!st->done) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) break; // WM_QUIT / error: never spin
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    drain_pending_quit();
    if (st->owner) {
        EnableWindow(st->owner, TRUE);
        SetForegroundWindow(st->owner);
    }
    return st->ok;
}

std::vector<MapItem> load_map_items_full(const std::filesystem::path& omsi_root) {
    auto items = discover_map_catalog_with_progress(omsi_root, nullptr);
    for (auto& item : items) ensure_map_loaded(item);
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!items[i].data) continue;
        std::ostringstream oss;
        oss << "  [" << i << "] \"" << items[i].label << "\" tiles=" << items[i].data->tiles.size()
            << " entrypoints=" << items[i].data->entrypoints.size() << " path=" << omsi::path_text(items[i].global_cfg);
        log::info(oss.str());
    }
    return items;
}

} // namespace

std::optional<std::filesystem::path> browse_omsi_root(HWND owner) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* dlg = nullptr;
    std::optional<std::filesystem::path> result;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(L"Seleccionar carpeta de OMSI 2");
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR wpath = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wpath))) {
                    result = std::filesystem::path(wpath);
                    CoTaskMemFree(wpath);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return result;
}

std::filesystem::path ensure_omsi_root(const std::filesystem::path& hint, HWND owner) {
    if (const auto saved = options::load_saved_omsi_root()) {
        log::info("omsi_root from prefs: " + saved->string());
        return *saved;
    }
    if (options::is_valid_omsi_root(hint)) {
        options::save_omsi_root(hint);
        log::info("omsi_root from hint (saved): " + hint.string());
        return hint.lexically_normal();
    }

    for (;;) {
        MessageBoxW(owner,
                    L"Elegí la carpeta de instalación de OMSI 2\n"
                    L"(donde está Omsi.exe o la carpeta maps).\n\n"
                    L"Esta ruta se guardará para la próxima vez.",
                    L"Feditor — Ruta de OMSI 2", MB_OK | MB_ICONINFORMATION);
        auto picked = browse_omsi_root(owner);
        if (!picked) {
            const int ans =
                MessageBoxW(owner, L"Sin carpeta OMSI 2 no se puede continuar.\n\n¿Salir?", L"Feditor",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (ans == IDYES) return {};
            continue;
        }
        if (!options::is_valid_omsi_root(*picked)) {
            MessageBoxW(owner, L"La carpeta no parece OMSI 2 (falta Omsi.exe o maps\\).", L"Ruta inválida",
                        MB_OK | MB_ICONWARNING);
            continue;
        }
        const auto root = picked->lexically_normal();
        options::save_omsi_root(root);
        log::info("omsi_root chosen: " + root.string());
        return root;
    }
}

std::optional<LaunchSelection> show_launcher(const std::filesystem::path& omsi_root,
                                             const std::filesystem::path& default_map_cfg,
                                             const std::size_t default_entrypoint_index, HWND owner) {
    std::filesystem::path root = omsi_root;
    if (!options::is_valid_omsi_root(root)) {
        root = ensure_omsi_root(root, owner);
        if (root.empty()) return std::nullopt;
    }

    DialogState st{};
    st.omsi_root = root.lexically_normal();
    st.owner = owner;
    st.items = discover_map_catalog_with_progress(st.omsi_root, owner);
    if (st.items.empty()) {
        log::error("no maps loaded");
        MessageBoxW(owner, L"No se encontraron mapas (global.cfg) bajo maps\\.\n"
                           L"Usá «Cambiar ruta…» o revisá la instalación de OMSI 2.",
                    L"Sin mapas", MB_OK | MB_ICONERROR);
        // Still open dialog so user can change path.
    }

    if (!default_map_cfg.empty()) st.initial_map = find_map_index(st.items, default_map_cfg);
    st.initial_entry = default_entrypoint_index;
    st.selected_map = st.initial_map;
    st.selected_entry = st.initial_entry;

    if (!run_launcher_dialog(&st)) {
        if (st.switch_game) {
            LaunchSelection sw{};
            sw.switch_to_beamng = true;
            return sw;
        }
        return std::nullopt;
    }
    if (st.selected_map >= st.items.size()) return std::nullopt;

    auto& picked = st.items[st.selected_map];
    if (!ensure_map_loaded(picked)) return std::nullopt;
    if (picked.data->entrypoints.empty())
        log::warn("map \"" + picked.label + "\" has no entrypoints — will spawn at (0,0,0)");
    else if (st.selected_entry >= picked.data->entrypoints.size()) {
        log::warn("entrypoint index invalid, clamping to 0");
        st.selected_entry = 0;
    }

    options::save_omsi_root(st.omsi_root);

    LaunchSelection sel{};
    sel.omsi_root = st.omsi_root;
    sel.global_cfg = picked.global_cfg;
    sel.map_friendly_name = picked.label;
    sel.entrypoint_index = st.selected_entry;
    sel.confirmed = true;
    sel.view_only = st.view_only;
    sel.gfx_backend = st.gfx_choice;

    if (!picked.data->entrypoints.empty()) {
        const auto& ep = picked.data->entrypoints[sel.entrypoint_index];
        std::ostringstream oss;
        oss << "launch: map=\"" << sel.map_friendly_name << "\" entry[" << sel.entrypoint_index << "]=\"" << ep.name
            << "\" pos=(" << ep.position.x << "," << ep.position.y << "," << ep.position.z << ")"
            << (sel.view_only ? " [VIEW-ONLY]" : " [EDIT]");
        log::info(oss.str());
    } else {
        log::info("launch: map=\"" + sel.map_friendly_name + "\" entry=(origin 0,0,0)" +
                  (sel.view_only ? " [VIEW-ONLY]" : " [EDIT]"));
    }
    return sel;
}

namespace {

constexpr int IDC_EP_ONLY = 1101;
constexpr int IDC_EP_OK = 1102;
constexpr int IDC_EP_CANCEL = 1103;

struct EntryPickState {
    const map::GlobalMap* map = nullptr;
    std::size_t default_index = 0;
    std::size_t selected = 0;
    bool done = false;
    bool ok = false;
    HWND list = nullptr;
};

std::wstring ep_to_wide(const std::string& s) { return omsi::utf8_to_wide(s); }

LRESULT CALLBACK EntryPickWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<EntryPickState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    {
        LRESULT out = 0;
        if (omsi::ui::dark::handle_ctlcolor(msg, wp, out)) return out;
        if (msg == WM_ERASEBKGND) {
            omsi::ui::dark::handle_erase(hwnd, wp, out);
            return out;
        }
    }
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = static_cast<EntryPickState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

        const std::string cfg_name =
            st->map->friendly_name.empty() ? st->map->name : st->map->friendly_name;
        const std::wstring title =
            L"Mapa: " + ep_to_wide(map_display_label(st->map->global_cfg_path, cfg_name));
        CreateWindowExW(0, L"STATIC", title.c_str(), WS_CHILD | WS_VISIBLE, 12, 12, 480, 20, hwnd, nullptr, nullptr,
                        nullptr);
        CreateWindowExW(0, L"STATIC", L"¿A qué entrypoint querés ir?", WS_CHILD | WS_VISIBLE, 12, 36, 480, 18, hwnd,
                        nullptr, nullptr, nullptr);
        st->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTBOXW, L"",
                                   WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, 12, 60, 480, 220,
                                   hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EP_ONLY)), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Ir", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 300, 300, 90, 28,
                        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EP_OK)), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancelar", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 400, 300, 90, 28, hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EP_CANCEL)), nullptr, nullptr);

        for (std::size_t i = 0; i < st->map->entrypoints.size(); ++i) {
            const auto& ep = st->map->entrypoints[i];
            const std::wstring label = entrypoint_list_label(*st->map, ep, i);
            SendMessageW(st->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        const std::size_t sel = st->default_index < st->map->entrypoints.size() ? st->default_index : 0;
        SendMessageW(st->list, LB_SETCURSEL, static_cast<WPARAM>(sel), 0);
        st->selected = sel;
        return 0;
    }
    case WM_COMMAND: {
        if (!st) break;
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (id == IDC_EP_ONLY && code == LBN_SELCHANGE) {
            const LRESULT sel = SendMessageW(st->list, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) st->selected = static_cast<std::size_t>(sel);
            return 0;
        }
        if (id == IDC_EP_ONLY && code == LBN_DBLCLK) {
            st->ok = true;
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDC_EP_OK) {
            const LRESULT sel = SendMessageW(st->list, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) st->selected = static_cast<std::size_t>(sel);
            st->ok = true;
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDC_EP_CANCEL) {
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
        if (st && wp == VK_RETURN) {
            const LRESULT sel = SendMessageW(st->list, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) st->selected = static_cast<std::size_t>(sel);
            st->ok = true;
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (st && wp == VK_ESCAPE) {
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (st) st->done = true;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        // No PostQuitMessage: the picker is a nested dialog (see launcher loop).
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

std::optional<std::size_t> pick_entrypoint(const map::GlobalMap& map, std::size_t default_index) {
    if (map.entrypoints.empty()) return std::nullopt;
    if (map.entrypoints.size() == 1) return 0;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    EntryPickState st{};
    st.map = &map;
    st.default_index = default_index;

    const wchar_t* cls = L"OmsiF4EntryPick";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = EntryPickWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    RegisterClassExW(&wc);

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"Feditor - Elegir Entrypoint",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                               520, 380, nullptr, nullptr, wc.hInstance, &st);
    if (!dlg) {
        log::error("CreateWindowEx entrypoint picker failed");
        return std::nullopt;
    }
    ShowWindow(dlg, SW_SHOW);
    omsi::ui::dark::apply(dlg);
    omsi::ui::dark::center(dlg);
    UpdateWindow(dlg);

    drain_pending_quit(); // a stale WM_QUIT would end this loop before the user touches anything
    MSG msg{};
    while (!st.done) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) break; // WM_QUIT / error: never spin
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    drain_pending_quit();
    if (!st.ok) {
        log::info("entrypoint picker cancelled");
        return std::nullopt;
    }
    if (st.selected >= map.entrypoints.size()) st.selected = 0;
    log::info("entrypoint picker ok index=" + std::to_string(st.selected) +
              " name=" + map.entrypoints[st.selected].name);
    return st.selected;
}

int dump_maps_catalog(const std::filesystem::path& omsi_root) {
    log::info("=== dump-maps catalog ===");
    const auto items = load_map_items_full(omsi_root);
    log::info("total loaded maps: " + std::to_string(items.size()));
    for (std::size_t mi = 0; mi < items.size(); ++mi) {
        const auto& m = items[mi];
        if (!m.data) continue;
        log::info("MAP " + std::to_string(mi) + " \"" + m.label +
                  "\" tiles=" + std::to_string(m.data->tiles.size()));
        for (std::size_t ei = 0; ei < m.data->entrypoints.size(); ++ei) {
            const auto& ep = m.data->entrypoints[ei];
            std::ostringstream oss;
            oss << "  EP " << ei << ": \"" << ep.name << "\" (" << ep.position.x << "," << ep.position.y << ","
                << ep.position.z << ")";
            log::info(oss.str());
        }
    }
    return static_cast<int>(items.size());
}

} // namespace omsi::launcher
