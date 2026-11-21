#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include "app/spline_creator_app.hpp"
#include "omsi/logger.hpp"
#include "omsi/spline_types.hpp"
#include "omsi/utf8_win32.hpp"
#include "ui/spline_creator_dialog.hpp"

#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include <objbase.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace omsi::app {
namespace {

bool warg_is(const wchar_t* w, const wchar_t* lit) { return wcscmp(w, lit) == 0; }

bool ends_with_icase(std::wstring_view s, std::wstring_view ext) {
    if (s.size() < ext.size()) return false;
    const auto tail = s.substr(s.size() - ext.size());
    for (std::size_t i = 0; i < ext.size(); ++i) {
        if (towlower(tail[i]) != towlower(ext[i])) return false;
    }
    return true;
}

void init_common_controls() {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);
}

void print_help() {
    std::wcerr
        << L"Feditor — Creador de spline (.sli)\n\n"
        << L"Uso:\n"
        << L"  omsi_spline_creator [--omsi \"F:\\...\\OMSI 2\"] [archivo.sli]\n"
        << L"  omsi_spline_creator --sli ruta\\perfil.sli\n\n"
        << L"Opciones:\n"
        << L"  --omsi PATH   Raíz de OMSI 2 (texturas / Splines)\n"
        << L"  --sli PATH    Abrir .sli existente\n"
        << L"  --self-test   Prueba automatica (tecla A + seleccion caja)\n"
        << L"  --help        Esta ayuda\n";
}

} // namespace

std::filesystem::path default_omsi_root() {
    const std::vector<std::filesystem::path> candidates = {
        R"(C:\Program Files (x86)\Steam\steamapps\common\OMSI 2)",
    };
    for (const auto& p : candidates)
        if (std::filesystem::exists(p / "Omsi.exe")) return p;
    return {};
}

bool parse_spline_creator_cli(int argc, wchar_t* argv[], SplineCreatorCli& out) {
    out = {};
    out.omsi_root = default_omsi_root();
    for (int i = 1; i < argc; ++i) {
        const wchar_t* arg = argv[i];
        if (warg_is(arg, L"--help") || warg_is(arg, L"-h") || warg_is(arg, L"/?")) {
            out.show_help = true;
            return true;
        }
        if (warg_is(arg, L"--self-test")) {
            out.self_test = true;
            continue;
        }
        if (warg_is(arg, L"--omsi") && i + 1 < argc) {
            out.omsi_root = std::filesystem::path(argv[++i]);
            continue;
        }
        if ((warg_is(arg, L"--sli") || warg_is(arg, L"--open")) && i + 1 < argc) {
            out.open_sli = std::filesystem::path(argv[++i]);
            continue;
        }
        if (arg[0] == L'-') {
            std::wcerr << L"Opción desconocida: " << arg << L"\n";
            return false;
        }
        if (ends_with_icase(arg, L".sli")) {
            out.open_sli = std::filesystem::path(arg);
            continue;
        }
        std::wcerr << L"Argumento no reconocido: " << arg << L"\n";
        return false;
    }
    return true;
}

std::filesystem::path python_spline_creator_script() {
#ifdef OMSI_F4_SOURCE_DIR
    const std::filesystem::path from_src =
        std::filesystem::path(OMSI_F4_SOURCE_DIR) / "tools" / "spline_creator" / "app.py";
    if (std::filesystem::exists(from_src)) return from_src;
#endif
    wchar_t mod[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, mod, MAX_PATH)) {
        const auto dir = std::filesystem::path(mod).parent_path();
        const auto a = dir / "tools" / "spline_creator" / "app.py";
        if (std::filesystem::exists(a)) return a;
        const auto b = dir.parent_path() / "tools" / "spline_creator" / "app.py";
        if (std::filesystem::exists(b)) return b;
    }
    return {};
}

std::wstring quote_arg(const std::filesystem::path& p) { return L"\"" + p.wstring() + L"\""; }

bool find_python_exe(std::wstring& out) {
    wchar_t buf[MAX_PATH]{};
    if (SearchPathW(nullptr, L"py.exe", nullptr, MAX_PATH, buf, nullptr)) {
        out = buf;
        return true;
    }
    if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, buf, nullptr)) {
        out = buf;
        return true;
    }
    return false;
}

// The release ships the spline creator frozen (PyInstaller): tools\spline_creator\spline_creator.exe next to
// the editor, so the user's PC needs no Python.
std::filesystem::path bundled_spline_creator_exe() {
    wchar_t mod[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, mod, MAX_PATH)) return {};
    const auto exe = std::filesystem::path(mod).parent_path() / "tools" / "spline_creator" / "spline_creator.exe";
    std::error_code ec;
    return std::filesystem::exists(exe, ec) ? exe : std::filesystem::path{};
}

SplineCreatorLaunchResult launch_python_spline_creator(const SplineCreatorCli& cli, void* owner_hwnd) {
    SplineCreatorLaunchResult r{};
    const auto frozen = bundled_spline_creator_exe();
    const auto script = frozen.empty() ? python_spline_creator_script() : frozen;
    if (script.empty()) {
        log::warn("spline creator python: no se encontró tools/spline_creator/app.py");
        return r;
    }
    std::wstring py;
    if (frozen.empty() && !find_python_exe(py)) {
        log::warn("spline creator python: no hay py.exe / python.exe en PATH");
        return r;
    }
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const auto status = std::filesystem::path(tmp) / L"omsi_f4_spline_creator_last.txt";
    std::error_code ec;
    std::filesystem::remove(status, ec);

    std::wstring cmd;
    if (!frozen.empty()) {
        cmd = quote_arg(frozen);
    } else {
        const auto py_name = std::filesystem::path(py).filename().wstring();
        cmd = quote_arg(py);
        if (_wcsicmp(py_name.c_str(), L"py.exe") == 0) cmd += L" -3";
        cmd += L" -u " + quote_arg(script);
    }
    if (!cli.omsi_root.empty()) cmd += L" --omsi " + quote_arg(cli.omsi_root);
    if (!cli.open_sli.empty()) cmd += L" --sli " + quote_arg(cli.open_sli);
    cmd += L" --status-file " + quote_arg(status);

    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const HWND owner = static_cast<HWND>(owner_hwnd);
    if (owner) EnableWindow(owner, FALSE);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                        script.parent_path().c_str(), &si, &pi)) {
        if (owner) EnableWindow(owner, TRUE);
        log::warn("spline creator python: CreateProcess failed");
        return r;
    }
    r.started = true;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    r.exit_code = static_cast<int>(code);
    if (std::filesystem::exists(status)) {
        const auto bytes = std::filesystem::file_size(status, ec);
        (void)bytes;
        std::ifstream in(status);
        std::string line;
        if (in && std::getline(in, line) && !line.empty()) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            r.saved_path = std::filesystem::u8path(line);
        }
    }
    log::info("spline creator python done saved=" + r.saved_path.string());
    return r;
}

int run_spline_creator_app(const SplineCreatorCli& cli) {
    if (cli.show_help) {
        print_help();
        return 0;
    }

    init_common_controls();
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool com_owned = com_hr == S_OK || com_hr == S_FALSE;
    omsi::log::init();

    if (cli.self_test) {
        omsi::log::info("spline creator self-test start omsi_root=" + cli.omsi_root.string());
        const bool pass = ui::run_spline_creator_self_test(cli.omsi_root);
        omsi::log::info(std::string("spline creator self-test ") + (pass ? "PASS" : "FAIL"));
        omsi::log::shutdown();
        if (com_owned) CoUninitialize();
        return pass ? 0 : 1;
    }

    omsi::log::info("spline creator start omsi_root=" + cli.omsi_root.string());

    std::optional<spline::SplineDefinition> initial;
    if (!cli.open_sli.empty()) {
        if (auto loaded = spline::load_sli(cli.open_sli))
            initial = std::move(*loaded);
        else
            omsi::log::warn("no se pudo cargar " + cli.open_sli.string() + " — perfil vacío");
    }

    ui::SplineCreatorResult result{};
    const bool ran = ui::run_spline_creator_dialog(nullptr, cli.omsi_root, initial, result);
    if (ran && result.ok) {
        omsi::log::info("spline creator saved " + result.saved_path.string());
        omsi::log::shutdown();
        if (com_owned) CoUninitialize();
        return 0;
    }
    omsi::log::info("spline creator cancelled");
    omsi::log::shutdown();
    if (com_owned) CoUninitialize();
    return 1;
}

} // namespace omsi::app
