#include "gfx/gfx_backend.hpp"

#include "omsi/logger.hpp"

#include <windows.h>
#include <shellapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace omsi::gfx::backend {
namespace {

Kind g_requested = Kind::Auto;
Kind g_active = Kind::D3D12;

std::filesystem::path exe_dir() {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool parse(const std::wstring& raw, Kind& out) {
    std::wstring s = lower(raw);
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    while (!s.empty() && std::iswspace(s.front())) s.erase(s.begin());
    if (s == L"auto") out = Kind::Auto;
    else if (s == L"d3d12" || s == L"dx12" || s == L"dx11" || s == L"d3d11") out = Kind::D3D12;
    else if (s == L"vulkan" || s == L"vk" || s == L"vkd3d") out = Kind::Vulkan;
    else if (s == L"warp" || s == L"software" || s == L"cpu") out = Kind::Warp;
    else return false;
    return true;
}

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

std::string module_path(HMODULE m) {
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD n = m ? GetModuleFileNameW(m, buf, static_cast<DWORD>(std::size(buf))) : 0;
    return narrow(std::wstring(buf, n));
}

// Preload vkd3d-proton + DXVK dxgi by full path. The exe imports d3d12.dll / dxgi.dll with /DELAYLOAD, and
// LoadLibrary("d3d12.dll") returns an already loaded module with that base name, so the renderer binds to
// these instead of the system DLLs.
bool preload_vkd3d() {
    if (GetModuleHandleW(L"d3d12.dll") || GetModuleHandleW(L"dxgi.dll")) {
        omsi::log::warn("GFX_BACKEND vulkan: d3d12/dxgi already loaded (" + module_path(GetModuleHandleW(L"d3d12.dll")) +
                        "), cannot switch");
        return false;
    }
    const auto dir = exe_dir() / L"vkd3d";
    for (const wchar_t* f : {L"dxgi.dll", L"d3d12core.dll", L"d3d12.dll"}) {
        const auto p = dir / f;
        if (!LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
            omsi::log::warn("GFX_BACKEND vulkan: cannot load " + narrow(p.wstring()) + " err=" +
                            std::to_string(GetLastError()));
            return false;
        }
    }
    omsi::log::info("GFX_BACKEND vulkan: d3d12=" + module_path(GetModuleHandleW(L"d3d12.dll")) +
                    " dxgi=" + module_path(GetModuleHandleW(L"dxgi.dll")));
    return true;
}

// Cheap "would a device be created?" test: D3D12CreateDevice with a null out pointer returns S_FALSE on
// success without creating anything. d3d12.dll missing (Windows 7/8) makes the delay-load throw an SEH
// exception, caught here.
bool probe_device_impl() {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf())))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    if (g_active == Kind::Warp) {
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.GetAddressOf())))) return false;
        return SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
    }
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            return true;
    }
    return false;
}

bool probe_device() {
    __try {
        return probe_device_impl();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

const char* name(Kind k) {
    switch (k) {
    case Kind::Auto: return "auto";
    case Kind::D3D12: return "d3d12";
    case Kind::Vulkan: return "vulkan";
    case Kind::Warp: return "warp";
    }
    return "?";
}

Kind requested() { return g_requested; }
Kind active() { return g_active; }
bool use_warp() { return g_active == Kind::Warp; }

bool vulkan_available() {
    const auto dir = exe_dir() / L"vkd3d";
    std::error_code ec;
    for (const wchar_t* f : {L"dxgi.dll", L"d3d12core.dll", L"d3d12.dll"})
        if (!std::filesystem::exists(dir / f, ec)) return false;
    static const bool loader = LoadLibraryW(L"vulkan-1.dll") != nullptr; // kept loaded: vkd3d needs it anyway
    return loader;
}

void select_early() {
    Kind k = Kind::Auto;
    const char* source = "default";
    // 1) command line: --gfx <name> (last one wins: a fallback relaunch appends its own)
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; ++i)
            if (lower(argv[i]) == L"--gfx" && parse(argv[i + 1], k)) source = "command line";
        LocalFree(argv);
    }
    // 2) environment, 3) <exe>/gfx_backend.txt
    if (std::string(source) == "default") {
        wchar_t env[64]{};
        if (GetEnvironmentVariableW(L"FEDITOR_GFX", env, 64) > 0 && parse(env, k)) source = "FEDITOR_GFX";
    }
    if (std::string(source) == "default") {
        std::wifstream f(exe_dir() / L"gfx_backend.txt");
        std::wstring line;
        if (f && std::getline(f, line) && parse(line, k)) source = "gfx_backend.txt";
    }
    g_requested = k;
    g_active = (k == Kind::Auto) ? Kind::D3D12 : k;

    if (g_active == Kind::Vulkan && !(vulkan_available() && preload_vkd3d())) {
        omsi::log::warn("GFX_BACKEND vulkan unavailable (vkd3d folder or Vulkan driver missing)");
        if (relaunch_next()) ExitProcess(0);
    }
    omsi::log::info(std::string("GFX_BACKEND requested=") + name(g_requested) + " (" + source +
                    ") active=" + name(g_active));

    if (!probe_device()) {
        omsi::log::warn(std::string("GFX_BACKEND ") + name(g_active) + ": no D3D12 device can be created");
        if (relaunch_next()) ExitProcess(0);
    }
}

const char* label(Kind k) {
    switch (k) {
    case Kind::Auto: return "Automático";
    case Kind::D3D12: return "DirectX 12";
    case Kind::Vulkan: return "Vulkan";
    case Kind::Warp: return "Software (WARP)";
    }
    return "?";
}

Kind saved() {
    Kind k = Kind::Auto;
    std::wifstream f(exe_dir() / L"gfx_backend.txt");
    std::wstring line;
    if (f && std::getline(f, line)) parse(line, k);
    return k;
}

void save(Kind k) {
    std::ofstream f(exe_dir() / L"gfx_backend.txt", std::ios::trunc);
    if (f) f << name(k) << '\n';
    omsi::log::info(std::string("GFX_BACKEND saved choice=") + name(k));
}

bool needs_relaunch(Kind k) {
    if (k == Kind::Auto) return g_requested != Kind::Auto && g_active != Kind::D3D12;
    return k != g_active;
}

bool relaunch_as(Kind k, const std::wstring& extra_args) {
    wchar_t exe[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    std::wstring cmd = GetCommandLineW();
    cmd += L" " + extra_args + L" --gfx ";
    const char* n = name(k);
    while (*n) cmd.push_back(static_cast<wchar_t>(*n++));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        omsi::log::error("GFX_BACKEND: relaunch failed err=" + std::to_string(GetLastError()));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    omsi::log::info(std::string("GFX_BACKEND: relaunched with ") + name(k) + " (launcher choice)");
    omsi::log::shutdown();
    return true;
}

bool relaunch_next() {
    Kind next;
    switch (g_active) {
    case Kind::D3D12: next = vulkan_available() ? Kind::Vulkan : Kind::Warp; break;
    case Kind::Vulkan: next = Kind::Warp; break;
    default:
        omsi::log::error("GFX_BACKEND: every backend failed (d3d12, vulkan, warp)");
        MessageBoxW(nullptr,
                    L"No se pudo inicializar ningún backend gráfico (D3D12, Vulkan, WARP).\n"
                    L"Actualizá el driver de la placa de video. Detalles en el log.",
                    L"Feditor", MB_OK | MB_ICONERROR);
        return false;
    }
    wchar_t exe[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    std::wstring cmd = GetCommandLineW();
    cmd += L" --gfx ";
    cmd += (next == Kind::Vulkan) ? L"vulkan" : L"warp";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        omsi::log::error("GFX_BACKEND: relaunch failed err=" + std::to_string(GetLastError()));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    omsi::log::warn(std::string("GFX_BACKEND: ") + name(g_active) + " failed, relaunched with " + name(next));
    omsi::log::shutdown();
    return true;
}

} // namespace omsi::gfx::backend
