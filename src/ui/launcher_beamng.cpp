// BeamNG.drive launcher: same look as the OMSI one (dark Win32), picks install + user folder, level, spawn point
// and the load radius (metres around the camera that get streamed in).
#include "launcher.hpp"

#include "beamng/bng_level.hpp"
#include "beamng/bng_session.hpp"
#include "beamng/bng_vfs.hpp"
#include "omsi/logger.hpp"
#include "omsi/utf8_win32.hpp"
#include "ui/win32_dark.hpp"

#include <commctrl.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <cwctype>

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#pragma comment(lib, "uxtheme.lib")

namespace omsi::launcher {
namespace {

constexpr int IDB_LEVELS = 2001;
constexpr int IDB_SPAWNS = 2002;
constexpr int IDB_OK = 2003;
constexpr int IDB_VIEW = 2004;
constexpr int IDB_CANCEL = 2005;
constexpr int IDB_INSTALL = 2006;
constexpr int IDB_USER = 2007;
constexpr int IDB_RADIUS = 2008;
constexpr int IDB_TO_OMSI = 2009;
constexpr int IDB_INSTALL_LIST = 2010;
constexpr int IDB_USER_LIST = 2011;

// Mount state shared with the worker: the dialog may be closed while the mount is still running, so the
// worker never touches the dialog — it fills this and posts WM_BNG_MOUNTED with its own pointer as token.
struct MountJob {
    std::atomic<bool> cancel{false};
    std::mutex mu;
    std::shared_ptr<bng::Vfs> vfs;
    std::vector<bng::LevelInfo> levels;
    bool ok = false;
    std::string error;
    HWND notify = nullptr;
};

// Scene scan for the selected level, run off the UI thread: info.json often lists no spawnPoints at all.
struct SpawnScanJob {
    std::atomic<bool> cancel{false};
    std::mutex mu;
    int level_index = -1;
    std::vector<std::string> names;
    bool ok = false;
    HWND notify = nullptr;
};

struct BngDialog {
    bng::Prefs prefs;
    std::shared_ptr<bng::Vfs> vfs;
    std::vector<bng::LevelInfo> levels;
    int sel_level = -1;
    int sel_spawn = 0; // 0 = default
    bool done = false, ok = false, view_only = false, to_omsi = false;
    HWND hwnd = nullptr, levelbox = nullptr, spawnbox = nullptr, install_label = nullptr, user_label = nullptr,
         radius_edit = nullptr, info_label = nullptr;
    HWND owner = nullptr;
    // Background mount (the dialog shows immediately); cancelled + detached on close, never joined by the UI.
    std::shared_ptr<MountJob> mount;
    bool loading = false;
    // Background spawn scan for the selected level (info.json often has no spawnPoints).
    std::shared_ptr<SpawnScanJob> scan;
    std::vector<bng::SpawnPointInfo> scanned; // scene spawns found for scanned_level
    int scanned_level = -1;
    // rows of the install / user folder drop-down lists
    std::vector<std::filesystem::path> install_rows, user_rows;
};
constexpr UINT WM_BNG_MOUNTED = WM_APP + 41;
constexpr UINT WM_BNG_SPAWNS = WM_APP + 42;

// A stale WM_QUIT (posted by the WM_DESTROY of a previous dialog) makes the next launcher loop return at
// once, which looked like "the app closes when switching games". Nested dialog loops drop it instead.
void drain_pending_quit() {
    MSG m{};
    while (PeekMessageW(&m, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
        log::warn("launcher: WM_QUIT pendiente descartado (" + std::to_string(static_cast<int>(m.wParam)) + ")");
}

std::wstring W(const std::string& s) { return omsi::utf8_to_wide(s); }
std::wstring Wp(const std::filesystem::path& p) { return p.wstring(); }

std::optional<std::filesystem::path> pick_folder(HWND owner, const wchar_t* title) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* dlg = nullptr;
    std::optional<std::filesystem::path> result;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(title);
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

bool valid_install(const std::filesystem::path& p) {
    std::error_code ec;
    return !p.empty() && (std::filesystem::exists(p / "gameengine.zip", ec) || std::filesystem::is_directory(p / "content" / "levels", ec));
}

void refill_spawns(BngDialog* d) {
    SendMessageW(d->spawnbox, LB_RESETCONTENT, 0, 0);
    SendMessageW(d->spawnbox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(spawn por defecto del nivel)"));
    d->sel_spawn = 0;
    if (d->sel_level < 0 || d->sel_level >= static_cast<int>(d->levels.size())) return;
    const auto& lv = d->levels[static_cast<std::size_t>(d->sel_level)];
    // info.json spawnPoints first, then whatever the background scene scan found for this level.
    std::vector<const bng::SpawnPointInfo*> list;
    list.reserve(lv.spawn_points.size() + d->scanned.size());
    for (const auto& sp : lv.spawn_points) list.push_back(&sp);
    if (d->scanned_level == d->sel_level)
        for (const auto& sp : d->scanned) {
            bool dup = false;
            for (const auto* s : list)
                if (s->object_name == sp.object_name) {
                    dup = true;
                    break;
                }
            if (!dup) list.push_back(&sp);
        }
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& sp = *list[i];
        std::string lab = sp.object_name;
        if (!sp.translation_id.empty() && sp.translation_id.find('.') == std::string::npos) lab += "  -  " + sp.translation_id;
        if (sp.object_name == lv.default_spawn) lab += "  (por defecto)";
        SendMessageW(d->spawnbox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(lab).c_str()));
        if (sp.object_name == d->prefs.last_spawn) d->sel_spawn = static_cast<int>(i) + 1;
    }
    SendMessageW(d->spawnbox, LB_SETCURSEL, static_cast<WPARAM>(d->sel_spawn), 0);
    std::string info = lv.name + "   tamano " + std::to_string(static_cast<int>(lv.size[0])) + " x " +
                       std::to_string(static_cast<int>(lv.size[1])) + " m";
    if (!lv.source.empty()) info += "\n" + lv.source;
    if (list.empty()) {
        const bool scanning = d->scan && d->scan->level_index == d->sel_level && d->scanned_level != d->sel_level;
        info += scanning ? "\nBuscando spawns en la escena..."
                         : "\nEste nivel no declara spawns: se usara el centro del terreno.";
    }
    SetWindowTextW(d->info_label, W(info).c_str());
}

// Scans the scene for spawn objects on a detached worker: the UI keeps working while main/ is read.
void start_spawn_scan(BngDialog* d, int index) {
    if (!d->vfs || index < 0 || index >= static_cast<int>(d->levels.size())) return;
    if (d->scan) d->scan->cancel.store(true);
    auto job = std::make_shared<SpawnScanJob>();
    job->notify = d->hwnd;
    job->level_index = index;
    d->scan = job;
    auto vfs = d->vfs; // keeps the mount alive for the scan
    const bng::LevelInfo info = d->levels[static_cast<std::size_t>(index)];
    std::thread([job, vfs, info]() {
        try {
            auto names = bng::scan_scene_spawns(*vfs, info);
            if (job->cancel.load()) return;
            std::lock_guard<std::mutex> lk(job->mu);
            job->names = std::move(names);
            job->ok = true;
            log::info("beamng launcher: " + std::to_string(job->names.size()) + " spawns en escena para " + info.name);
        } catch (const std::exception& e) {
            log::warn(std::string("beamng launcher: escaneo de spawns fallo: ") + (e.what() ? e.what() : "?"));
        } catch (...) {
            log::warn("beamng launcher: escaneo de spawns fallo");
        }
        if (job->cancel.load() || !job->notify) return;
        PostMessageW(job->notify, WM_BNG_SPAWNS, reinterpret_cast<WPARAM>(job.get()), 0);
    }).detach();
}

void fill_levels(BngDialog* d) {
    SendMessageW(d->levelbox, LB_RESETCONTENT, 0, 0);
    d->sel_level = -1;
    for (std::size_t i = 0; i < d->levels.size(); ++i) {
        const auto& lv = d->levels[i];
        std::string title = lv.title;
        if (title.rfind("levels.", 0) == 0) title = lv.name; // untranslated id
        const std::string lab = title + (title != lv.name ? "   [" + lv.name + "]" : "") + "   (" +
                                std::to_string(lv.spawn_points.size()) + " spawns)";
        SendMessageW(d->levelbox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(lab).c_str()));
        if (lv.name == d->prefs.last_level) d->sel_level = static_cast<int>(i);
    }
    if (d->sel_level < 0 && !d->levels.empty()) d->sel_level = 0;
    if (d->sel_level >= 0) SendMessageW(d->levelbox, LB_SETCURSEL, static_cast<WPARAM>(d->sel_level), 0);
    refill_spawns(d);
    if (d->levels.empty()) SetWindowTextW(d->info_label, L"No se encontraron niveles.");
    // First level is auto-selected: give it its scene spawns right away (background, never blocks).
    if (d->sel_level >= 0 && d->scanned_level != d->sel_level &&
        d->levels[static_cast<std::size_t>(d->sel_level)].spawn_points.empty())
        start_spawn_scan(d, d->sel_level);
}

// Drop-down of folders used before: current first, then the others (most recent first).
void fill_path_combo(HWND combo, std::vector<std::filesystem::path>& rows, const std::filesystem::path& current,
                     const std::vector<std::filesystem::path>& recent) {
    rows.clear();
    if (!current.empty()) rows.push_back(current);
    for (const auto& r : recent) {
        std::wstring a = r.lexically_normal().native(), b = current.lexically_normal().native();
        for (auto& c : a) c = static_cast<wchar_t>(towlower(c));
        for (auto& c : b) c = static_cast<wchar_t>(towlower(c));
        if (!r.empty() && a != b) rows.push_back(r);
    }
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto& r : rows) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(r.c_str()));
    if (rows.empty()) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(sin carpeta)"));
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

void set_buttons(BngDialog* d, bool enabled) {
    for (int id : {IDB_OK, IDB_VIEW, IDB_INSTALL, IDB_USER, IDB_INSTALL_LIST, IDB_USER_LIST})
        EnableWindow(GetDlgItem(d->hwnd, id), enabled ? TRUE : FALSE);
}

// Mount + level discovery on a worker; WM_BNG_MOUNTED brings the result back to the UI thread.
void remount(BngDialog* d) {
    // remember the folders (valid ones) and show them in the drop-down lists
    {
        std::error_code ec;
        bool changed = false;
        if (valid_install(d->prefs.install)) {
            bng::remember_path(d->prefs.recent_installs, d->prefs.install);
            changed = true;
        }
        if (!d->prefs.user_folder.empty() && std::filesystem::is_directory(d->prefs.user_folder, ec)) {
            bng::remember_path(d->prefs.recent_users, d->prefs.user_folder);
            changed = true;
        }
        if (changed) bng::save_prefs(d->prefs);
    }
    fill_path_combo(d->install_label, d->install_rows, d->prefs.install, d->prefs.recent_installs);
    fill_path_combo(d->user_label, d->user_rows, d->prefs.user_folder, d->prefs.recent_users);
    SendMessageW(d->levelbox, LB_RESETCONTENT, 0, 0);
    SendMessageW(d->spawnbox, LB_RESETCONTENT, 0, 0);
    d->levels.clear();
    d->sel_level = -1;
    d->scanned.clear();
    d->scanned_level = -1;
    if (d->scan) d->scan->cancel.store(true);
    d->scan.reset();
    if (!valid_install(d->prefs.install)) {
        if (d->mount) d->mount->cancel.store(true);
        d->mount.reset();
        d->loading = false;
        set_buttons(d, true);
        SetWindowTextW(d->info_label, L"Elegi la carpeta de instalacion de BeamNG.drive (donde esta gameengine.zip).");
        return;
    }
    // Abandon the previous mount: its thread finishes on its own and its token is ignored.
    if (d->mount) d->mount->cancel.store(true);
    d->loading = true;
    set_buttons(d, false);
    SendMessageW(d->levelbox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Cargando niveles de BeamNG.drive..."));
    SetWindowTextW(d->info_label, L"Leyendo los archivos del juego (la primera vez puede tardar unos segundos).");
    const auto install = d->prefs.install, user = d->prefs.user_folder;
    auto job = std::make_shared<MountJob>();
    job->notify = d->hwnd;
    d->mount = job;
    std::thread([job, install, user]() {
        try {
            auto vfs = std::make_shared<bng::Vfs>();
            vfs->mount_beamng(install, user);
            if (job->cancel.load()) return;
            auto levels = bng::discover_levels(*vfs);
            if (job->cancel.load()) return;
            for (const auto& f : vfs->failed_mounts) log::warn("beamng mount: " + f);
            log::info("beamng launcher: " + std::to_string(vfs->file_count()) + " files, " +
                      std::to_string(levels.size()) + " levels");
            std::lock_guard<std::mutex> lk(job->mu);
            job->vfs = std::move(vfs);
            job->levels = std::move(levels);
            job->ok = true;
        } catch (const std::exception& e) {
            log::error(std::string("beamng mount: excepcion: ") + (e.what() ? e.what() : "?"));
            std::lock_guard<std::mutex> lk(job->mu);
            job->ok = false;
            job->error = e.what() ? e.what() : "excepcion";
        } catch (...) {
            log::error("beamng mount: excepcion desconocida");
            std::lock_guard<std::mutex> lk(job->mu);
            job->ok = false;
            job->error = "excepcion desconocida";
        }
        // The dialog may be gone or already mounting another folder: the token tells them apart.
        if (job->cancel.load() || !job->notify) return;
        PostMessageW(job->notify, WM_BNG_MOUNTED, reinterpret_cast<WPARAM>(job.get()), 0);
    }).detach();
}

void finish(BngDialog* d, bool ok, bool view_only) {
    if (ok && d->loading) return;
    if (ok && (d->sel_level < 0 || d->levels.empty())) {
        MessageBoxW(d->hwnd, L"Elegi un nivel.", L"BeamNG.drive", MB_OK | MB_ICONINFORMATION);
        return;
    }
    wchar_t buf[32]{};
    GetWindowTextW(d->radius_edit, buf, 32);
    const float r = static_cast<float>(_wtof(buf));
    if (r >= 50.f) d->prefs.load_radius = r;
    d->ok = ok;
    d->view_only = view_only;
    d->done = true;
    DestroyWindow(d->hwnd);
}

LRESULT CALLBACK BngWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<BngDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    {
        LRESULT out = 0;
        if (omsi::ui::dark::handle_ctlcolor(msg, wp, out)) return out;
        if (msg == WM_ERASEBKGND) {
            omsi::ui::dark::handle_erase(hwnd, wp, out);
            return out;
        }
    }
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    };
    switch (msg) {
    case WM_CREATE: {
        d = static_cast<BngDialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
        d->hwnd = hwnd;
        mk(L"STATIC", L"OMSI F4 Engine  -  BeamNG.drive", 0, 12, 10, 300, 18, 0);
        mk(L"BUTTON", L"\x25C4 OMSI 2", WS_TABSTOP, 440, 2, 108, 24, IDB_TO_OMSI);
        mk(L"STATIC", L"Juego:", SS_NOPREFIX, 12, 34, 60, 18, 0);
        d->install_label = mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 74, 30, 358, 240,
                              IDB_INSTALL_LIST);
        mk(L"BUTTON", L"Agregar...", WS_TABSTOP, 440, 30, 108, 24, IDB_INSTALL);
        mk(L"STATIC", L"Usuario:", SS_NOPREFIX, 12, 60, 60, 18, 0);
        d->user_label = mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 74, 56, 358, 240, IDB_USER_LIST);
        mk(L"BUTTON", L"Agregar...", WS_TABSTOP, 440, 56, 108, 24, IDB_USER);
        SetWindowTheme(d->install_label, L"DarkMode_CFD", nullptr);
        SetWindowTheme(d->user_label, L"DarkMode_CFD", nullptr);
        mk(L"STATIC", L"Nivel:", 0, 12, 88, 80, 18, 0);
        d->levelbox = mk(WC_LISTBOXW, L"", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, 12, 106, 536, 170, IDB_LEVELS, WS_EX_CLIENTEDGE);
        mk(L"STATIC", L"Punto de spawn:", 0, 12, 282, 150, 18, 0);
        d->spawnbox = mk(WC_LISTBOXW, L"", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, 12, 300, 536, 110, IDB_SPAWNS, WS_EX_CLIENTEDGE);
        mk(L"STATIC", L"Radio de carga (m):", 0, 12, 420, 130, 18, 0);
        d->radius_edit = mk(L"EDIT", std::to_wstring(static_cast<int>(d->prefs.load_radius)).c_str(), ES_NUMBER | WS_TABSTOP, 146, 418,
                            80, 22, IDB_RADIUS, WS_EX_CLIENTEDGE);
        mk(L"STATIC", L"Se cargan modelos, texturas, forest y carreteras dentro de este radio; al moverte carga el resto.",
           SS_LEFTNOWORDWRAP, 236, 420, 312, 32, 0);
        d->info_label = mk(L"STATIC", L"", SS_NOPREFIX, 12, 452, 420, 34, 0);
        mk(L"BUTTON", L"Editar nivel", BS_DEFPUSHBUTTON | WS_TABSTOP, 250, 492, 110, 28, IDB_OK);
        mk(L"BUTTON", L"Solo visualizar", WS_TABSTOP, 368, 492, 120, 28, IDB_VIEW);
        mk(L"BUTTON", L"Cancelar", WS_TABSTOP, 496, 492, 80, 28, IDB_CANCEL);
        remount(d);
        return 0;
    }
    case WM_COMMAND: {
        if (!d) break;
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDB_TO_OMSI) {
            d->to_omsi = true;
            d->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDB_INSTALL) {
            if (auto p = pick_folder(hwnd, L"Carpeta de instalacion de BeamNG.drive")) {
                if (!valid_install(*p)) {
                    MessageBoxW(hwnd, L"La carpeta no parece BeamNG.drive (falta gameengine.zip / content\\levels).", L"Ruta invalida",
                                MB_OK | MB_ICONWARNING);
                    return 0;
                }
                d->prefs.install = *p;
                remount(d);
            }
            return 0;
        }
        if (id == IDB_USER) {
            if (auto p = pick_folder(hwnd, L"Carpeta de usuario de BeamNG.drive (mods / niveles guardados)")) {
                d->prefs.user_folder = *p;
                remount(d);
            }
            return 0;
        }
        if ((id == IDB_INSTALL_LIST || id == IDB_USER_LIST) && code == CBN_SELCHANGE) {
            // another folder used before: switch and reload the levels right away
            const bool inst = id == IDB_INSTALL_LIST;
            const LRESULT s2 = SendMessageW(inst ? d->install_label : d->user_label, CB_GETCURSEL, 0, 0);
            const auto& rows = inst ? d->install_rows : d->user_rows;
            if (s2 == CB_ERR || s2 <= 0 || static_cast<std::size_t>(s2) >= rows.size()) return 0;
            const auto pick = rows[static_cast<std::size_t>(s2)];
            if (inst && !valid_install(pick)) {
                MessageBoxW(hwnd, L"Esa carpeta ya no parece BeamNG.drive.", L"Ruta invalida", MB_OK | MB_ICONWARNING);
                fill_path_combo(d->install_label, d->install_rows, d->prefs.install, d->prefs.recent_installs);
                return 0;
            }
            if (inst) d->prefs.install = pick;
            else d->prefs.user_folder = pick;
            remount(d);
            return 0;
        }
        if (id == IDB_LEVELS && code == LBN_SELCHANGE) {
            const LRESULT s = SendMessageW(d->levelbox, LB_GETCURSEL, 0, 0);
            if (s != LB_ERR) d->sel_level = static_cast<int>(s);
            d->scanned.clear();
            d->scanned_level = -1;
            refill_spawns(d);
            if (d->sel_level >= 0 && d->sel_level < static_cast<int>(d->levels.size()) &&
                d->levels[static_cast<std::size_t>(d->sel_level)].spawn_points.empty())
                start_spawn_scan(d, d->sel_level);
            return 0;
        }
        if (id == IDB_SPAWNS && code == LBN_SELCHANGE) {
            const LRESULT s = SendMessageW(d->spawnbox, LB_GETCURSEL, 0, 0);
            if (s != LB_ERR) d->sel_spawn = static_cast<int>(s);
            return 0;
        }
        if ((id == IDB_LEVELS || id == IDB_SPAWNS) && code == LBN_DBLCLK) {
            finish(d, true, false);
            return 0;
        }
        if (id == IDB_OK) { finish(d, true, false); return 0; }
        if (id == IDB_VIEW) { finish(d, true, true); return 0; }
        if (id == IDB_CANCEL) {
            d->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_BNG_MOUNTED: {
        if (!d) break;
        if (!d->mount || d->mount.get() != reinterpret_cast<MountJob*>(wp)) return 0; // cancelled / superseded
        std::shared_ptr<bng::Vfs> mounted;
        std::vector<bng::LevelInfo> levels;
        std::string error;
        {
            std::lock_guard<std::mutex> lk(d->mount->mu);
            mounted = d->mount->vfs;
            levels = d->mount->levels;
            error = d->mount->error;
        }
        d->loading = false;
        d->vfs = std::move(mounted);
        d->levels = d->vfs ? std::move(levels) : std::vector<bng::LevelInfo>{};
        set_buttons(d, true);
        fill_levels(d);
        if (!d->vfs)
            SetWindowTextW(d->info_label,
                           (error.empty() ? std::wstring(L"No se pudieron leer los archivos del juego.") : W(error))
                               .c_str());
        return 0;
    }
    case WM_BNG_SPAWNS: {
        if (!d) break;
        if (!d->scan || d->scan.get() != reinterpret_cast<SpawnScanJob*>(wp)) return 0; // cancelled / superseded
        std::vector<std::string> names;
        int index = -1;
        {
            std::lock_guard<std::mutex> lk(d->scan->mu);
            names = d->scan->names;
            index = d->scan->level_index;
        }
        d->scan.reset();
        if (index < 0 || index >= static_cast<int>(d->levels.size())) return 0;
        d->scanned.clear();
        d->scanned.reserve(names.size());
        for (auto& n : names) {
            bng::SpawnPointInfo sp;
            sp.object_name = std::move(n);
            d->scanned.push_back(std::move(sp));
        }
        d->scanned_level = index;
        if (d->sel_level == index) refill_spawns(d);
        return 0;
    }
    case WM_CLOSE:
        if (d) {
            d->done = true;
            if (d->mount) d->mount->cancel.store(true);
            if (d->scan) d->scan->cancel.store(true);
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        // No PostQuitMessage here: this is a nested dialog, a WM_QUIT would kill the next launcher loop.
        if (d && d->mount) d->mount->cancel.store(true);
        if (d && d->scan) d->scan->cancel.store(true);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

std::optional<BeamngLaunchSelection> show_beamng_launcher(HWND owner) {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    BngDialog d;
    d.owner = owner;
    d.prefs = bng::load_prefs();
    if (!valid_install(d.prefs.install)) d.prefs.install = bng::find_beamng_install();
    if (d.prefs.user_folder.empty()) d.prefs.user_folder = bng::find_beamng_user_folder();

    const wchar_t* cls = L"OmsiF4BeamngLauncher";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BngWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    RegisterClassExW(&wc);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"OMSI F4 Engine - BeamNG.drive: nivel y spawn",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 600, 570, owner,
                               nullptr, wc.hInstance, &d);
    if (!dlg) return std::nullopt;
    ShowWindow(dlg, SW_SHOW);
    omsi::ui::dark::apply(dlg);
    omsi::ui::dark::center(dlg);
    UpdateWindow(dlg);
    drain_pending_quit(); // a stale WM_QUIT would end this loop before the user touches anything
    MSG msg{};
    while (!d.done) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) break; // WM_QUIT / error: never spin
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    drain_pending_quit();
    // Never block the UI on the mount: cancel and let the worker finish on its own.
    if (d.mount) d.mount->cancel.store(true);
    if (d.scan) d.scan->cancel.store(true);
    BeamngLaunchSelection sel;
    sel.switch_to_omsi = d.to_omsi;
    if (d.to_omsi) {
        // The "game" preference is saved only when a launcher confirms a real selection.
        return sel;
    }
    if (!d.ok || d.sel_level < 0) return std::nullopt;
    const auto& lv = d.levels[static_cast<std::size_t>(d.sel_level)];
    sel.install = d.prefs.install;
    sel.user_folder = d.prefs.user_folder;
    sel.level = lv.name;
    sel.spawn = d.sel_spawn > 0 && d.sel_spawn - 1 < static_cast<int>(lv.spawn_points.size())
                    ? lv.spawn_points[static_cast<std::size_t>(d.sel_spawn - 1)].object_name
                    : std::string();
    sel.load_radius = d.prefs.load_radius;
    sel.view_only = d.view_only;
    sel.confirmed = true;
    d.prefs.game = "beamng";
    d.prefs.last_level = sel.level;
    d.prefs.last_spawn = sel.spawn;
    bng::save_prefs(d.prefs);
    // reuse the mounted VFS for the session
    bng::set_active_vfs(d.vfs);
    return sel;
}

} // namespace omsi::launcher
