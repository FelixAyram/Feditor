#pragma once
// Graphics backend selection for the D3D12 renderer (see docs/GFX_BACKENDS.md).
//   d3d12  - native D3D12 on a hardware adapter (feature level 11_0: every DX11-class GPU)
//   vulkan - same renderer through vkd3d-proton (D3D12->Vulkan) + DXVK dxgi, DLLs in <exe>/vkd3d/
//   warp   - Windows' software D3D12 rasterizer (any PC, VM or remote desktop)
// auto = d3d12 -> vulkan -> warp: when device creation fails the process relaunches itself with the next one.
#include <string>

namespace omsi::gfx::backend {

enum class Kind { Auto, D3D12, Vulkan, Warp };

// Call first thing in main(), before anything touches d3d12.dll / dxgi.dll (both are /DELAYLOAD).
// Reads --gfx <name>, then FEDITOR_GFX, then <exe>/gfx_backend.txt. For vulkan it preloads the vkd3d DLLs.
void select_early();

Kind requested();      // what the user asked for (Auto if nothing)
Kind active();         // what this process runs (never Auto)
const char* name(Kind k);
bool use_warp();       // device creation must take the WARP adapter
bool vulkan_available(); // <exe>/vkd3d DLLs present and a Vulkan loader installed

// Device creation failed: relaunch this exe with the next backend in the chain. Returns true when a new
// process was started (the caller should exit), false when there is nothing left to try.
bool relaunch_next();

const char* label(Kind k);          // UI text: "Automático", "DirectX 12", "Vulkan", "Software (WARP)"
Kind saved();                       // <exe>/gfx_backend.txt (Auto if missing)
void save(Kind k);                  // writes <exe>/gfx_backend.txt (the launcher's "Gráficos" choice)
// Does choosing `k` (launcher) need another process? True when it is not what runs now.
bool needs_relaunch(Kind k);
// Restart this exe with --gfx <k> plus extra arguments (e.g. the map picked in the launcher).
bool relaunch_as(Kind k, const std::wstring& extra_args);

} // namespace omsi::gfx::backend
