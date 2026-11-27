# Backends gráficos — nota de decisión (2026-11-26)

## Problema
El renderer (`src/gfx/dx12/`, ~18 k líneas, `HelloO3dRenderer`) es D3D12 puro, shaders HLSL SM 5.0/5.1
compilados con FXC (`D3DCompile`), dispositivo pedido a **feature level 11_0**. Queremos que arranque en
cualquier PC: GPUs DX11, máquinas con Vulkan pero driver D3D12 roto, y máquinas sin GPU útil.

## Opciones evaluadas
| Opción | Código nuevo | Licencia | Comentario |
|---|---|---|---|
| RHI: [bgfx](https://github.com/bkaradzic/bgfx), [Diligent](https://github.com/DiligentGraphics/DiligentEngine), [NVRHI](https://github.com/NVIDIA-RTX/NVRHI), [sokol_gfx](https://github.com/floooh/sokol), [The-Forge](https://github.com/ConfettiFX/The-Forge) | reescribir las ~18 k líneas + todos los shaders | BSD/Apache/MIT | mejor resultado final, pero meses de trabajo y choca con otras sesiones que editan `mesh/chunk/draw.cpp` |
| Cross-compilar shaders ([DXC](https://github.com/microsoft/DirectXShaderCompiler)→SPIR-V, [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)) | sólo resuelve shaders; el resto del API sigue siendo D3D12 | Apache/MIT | útil sólo junto con un RHI |
| **[vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton)** (D3D12→Vulkan) + `dxgi.dll` de **[DXVK](https://github.com/doitsujin/dxvk)** | ~250 líneas (selector + carga de DLLs) | LGPL-2.1 (DLL sin modificar, carga dinámica) / zlib | traduce DXBC SM 5.x; funciona en Windows con drivers Vulkan 1.3 |
| D3D12 nativo a FL 11_0 | 0 (ya lo hacemos) | — | cubre todas las GPUs DX11 con driver WDDM 2 (Fermi/Kepler+, GCN+, Intel Haswell+) |
| **WARP** (rasterizador D3D12 por software de Windows) | ~20 líneas | incluido en Windows 10/11 | arranca en cualquier PC, VM o escritorio remoto; lento pero usable para editar |
| D3D11On12, Dozen | — | — | van en la dirección contraria (D3D11→D3D12, Vulkan→D3D12) |
| ANGLE, Zink | — | — | GLES→D3D11/Vulkan y GL→Vulkan; no hay capa D3D12→OpenGL |

## Decisión
Cadena de backends sin reescribir el renderer:

1. **d3d12** — GPU por hardware a FL 11_0. Esto *es* el soporte "DirectX 11": toda GPU de clase DX11 con
   driver de Windows 10/11 crea un dispositivo D3D12 a FL 11_0.
2. **vulkan** — `vkd3d/d3d12.dll`, `vkd3d/d3d12core.dll` (vkd3d-proton 3.1) y `vkd3d/dxgi.dll` (DXVK 3.1.1)
   se cargan por ruta completa antes del primer uso; el exe tiene `d3d12.dll`/`dxgi.dll` en `/DELAYLOAD`,
   así que resuelve contra esos módulos. Sirve cuando el driver D3D12 falla pero el de Vulkan anda.
3. **warp** — adaptador WARP (`IDXGIFactory4::EnumWarpAdapter`). Último recurso: GPUs sólo-OpenGL, sin
   driver, VMs, RDP.

**OpenGL**: no existe una capa D3D12→OpenGL mantenida; una GPU que sólo tiene OpenGL arranca por WARP.
Un backend GL/D3D11 real requeriría el port a un RHI (opción 1) — queda como trabajo futuro.

## Uso
- Línea de comandos: `--gfx auto|d3d12|vulkan|warp`
- Variable de entorno: `FEDITOR_GFX=vulkan`
- Launcher (ventana de mapa y entrypoint): combo **Gráficos** (Automático / DirectX 12 / Vulkan /
  Software (WARP)). Se guarda en `gfx_backend.txt` junto al exe; si difiere del que corre, el editor se
  reinicia solo directo al mapa elegido.
- Archivo junto al exe: `gfx_backend.txt` con una de esas palabras (aplica al reiniciar)
- `auto` (por defecto): d3d12 → vulkan → warp. Si crear el dispositivo falla, el exe se relanza solo con el
  siguiente backend (`--gfx <siguiente>`), así un PC sin DX12 igual arranca.
- El backend activo aparece en la pantalla de carga ("Inicializando gráficos: …"), en el título de la
  ventana (`[DirectX 12]`, `[Vulkan]`, `[Software (WARP)]`) y en el log (`GFX_BACKEND`, `GFX_DEVICE`).

## OpenGL y el objetivo de 30 fps
WARP rasteriza en la CPU: en X10 Berlin se espera muy por debajo de 30 fps (sin medir: las pruebas visuales automáticas están desactivadas). Un backend OpenGL (o D3D11)
que llegue a 30 fps exige portar el renderer a un RHI (Diligent Engine soporta GL/D3D11/D3D12/Vulkan,
Apache-2.0) — estimado en semanas: ~18 k líneas de `src/gfx/dx12/` y todos los shaders HLSL.
