# Menos tirones en "GPU add meshes / splines / scenery" (stream)

Fecha: 2026-09-16

## Síntoma
Toast "Cargando tiles" → "GPU add meshes…" / "GPU add splines…" / "Stream markers…"
con tirones visibles mientras se cargan tiles nuevos (SCO/O3D y SLI).

## Causas
1. **Budget de 20 ms/frame** en fases Upload* (`pump_stream_apply`) — suficiente para
   notar stutter en el editor.
2. **Sin tope de ítems**: el loop de meshes solo miraba el reloj; un O3D pesado
   podía pasar de 20 ms solo y el frame se iba a 40–80 ms.
3. **`wait_gpu = true`** en `apply_spline_gpu_meshes` también en path `stream_upload`
   → el hilo de UI esperaba el fence de DX12 por cada strip del spline.

(El CPU de splines ya estaba en paralelo; el cuello era el **upload GPU síncrono**.)

## Cambios

### `src/app/detail/modules/mod_streaming.inc.cpp`
| Fase | Antes | Ahora |
|------|--------|--------|
| budget Upload*/Finish | 20 ms | **8 ms** |
| PrepShiftGpu | 28 ms | **12 ms** |
| PrepLists | 16 ms | **10 ms** |
| default | 8 ms | **6 ms** |
| meshes/frame | ilimitado (hasta budget) | **máx. 4** |
| scenery/frame | ilimitado | **máx. 4** |
| splines/frame | 8 | **3** |
| terrain/frame | 8 | **4** |

Más frames de stream, pero cada frame más liviano → menos tirones.

### `src/app/detail/map_editor_run_setup_02.inc.cpp`
```cpp
// antes
const bool wait_gpu = !preview_only;
// ahora
const bool wait_gpu = !preview_only && !stream_upload;
```
En stream el GPU no bloquea; las texturas terminan async (prefetch ya existe).

## Archivos en este patch (incluye async remesh anterior)
```
src/app/map_editor_app.cpp
src/app/detail/map_editor_run_setup_02.inc.cpp
src/app/detail/map_editor_run_loop_01.inc.cpp
src/app/detail/map_editor_run_loop_04.inc.cpp
src/app/detail/modules/mod_streaming.inc.cpp
CHANGELOG_async_remesh.md (si aplica el patch anterior)
CHANGELOG_stream_gpu_hitch.md
```

## Si aún hay tirones
- Bajar más `budget_ms` Upload* a 5–6.
- Bajar `kMeshesPerSlice` a 2.
- Revisar `append_or_clone_o3d` / creación de buffers: si un solo mesh >15 ms,
  conviene staging + copy queue en el renderer (siguiente nivel).
