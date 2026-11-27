Vulkan backend DLLs for Feditor (--gfx vulkan, see docs/GFX_BACKENDS.md). Unmodified upstream x64 builds,
loaded at run time by full path; the editor never links against them.

d3d12.dll, d3d12core.dll  vkd3d-proton 3.1 (D3D12 -> Vulkan)
                          https://github.com/HansKristian-Work/vkd3d-proton/releases/tag/v3.1
                          LGPL-2.1-or-later. Source: the tag above. You may replace these files with your
                          own build of vkd3d-proton.
dxgi.dll                  DXVK 3.1.1 (DXGI on Vulkan, provides the swap chain for vkd3d-proton)
                          https://github.com/doitsujin/dxvk/releases/tag/v3.1.1
                          zlib/libpng licence.

Needs a GPU driver with Vulkan 1.3. Without it the editor uses native D3D12 or WARP.
