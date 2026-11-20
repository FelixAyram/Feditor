# Async interactive spline remesh (menos tirones)

Fecha: 2026-09-16

## Problema
`editor remesh spline id=…` corría **síncrono en el hilo de UI/render**
(`remesh_spline` / `remesh_spline_one` en `map_editor_run_setup_02.inc.cpp`).
Cada `build_spline_meshes` + upload GPU (y hops de cadena) sumaba ms al frame
→ “leves tirones”.

El path de **streaming** ya hacía CPU en paralelo (`stream_kick_spline_cpu` +
`std::execution::par`); el editor interactivo no.

## Solución (patrón ideal: job queue + time-slice GPU)
Referencias típicas (StackOverflow / engines):
- CPU pesado en worker; GPU solo en main thread (D3D/OpenGL no thread-safe).
- Cola de jobs + resultados listos; descartar generaciones viejas.
- Upload amortizado por frame (budget en ms / N items).
- Paralelo sobre jobs independientes (`std::execution::par`), igual que streaming.

### Comportamiento nuevo
| Caso | Comportamiento |
|------|----------------|
| `preview_only` (drag G/R/etc.) | **Sync** (feedback inmediato) |
| `stream_upload` | **Sync** (path de stream intacto) |
| Commit / full remesh interactivo | **Async**: encola CPU → worker paralelo → `pump_async_remesh` sube GPU con budget |

- Generación por `id`: si el usuario vuelve a editar el mismo spline, el job
  viejo se descarta al llegar.
- Cadena (`spline_stick_chain`): posiciones se actualizan sync; cada hop encola
  su remesh async (ya no bloquea el frame con N× remesh).
- `pump_undo_remesh_queue` se beneficia: `remesh_spline_one` ahora encola en
  lugar de bloquear.

### Budget por frame
`pump_async_remesh(4, 6.0)` → máx. 4 uploads o ~6 ms de GPU por frame.
Ajustable en `map_editor_run_loop_01.inc.cpp`.

## Archivos modificados
```
src/app/map_editor_app.cpp
  + #include <condition_variable>
  + #include <deque>

src/app/detail/map_editor_run_setup_02.inc.cpp
  + AsyncRemeshJob / AsyncRemeshResult
  + worker thread + cola pending/ready + gen map
  + async_remesh_enqueue / ensure_worker / join / pump_async_remesh
  + remesh_spline_one: encola si !preview_only && !stream_upload

src/app/detail/map_editor_run_loop_01.inc.cpp
  + pump_async_remesh(4, 6.0) al inicio del frame (junto a pump_undo)

src/app/detail/map_editor_run_loop_04.inc.cpp
  + async_remesh_join() antes de stream_join_cpu_workers al salir
```

## Cómo aplicar
Copiar los 4 archivos sobre tu árbol `src/` (misma jerarquía) y recompilar el
target del map editor.

## Notas
- Preview sigue síncrono a propósito (si se hace async se nota lag al arrastrar).
- Si aún hay tirón en preview de cadenas muy largas, el siguiente paso sería
  limitar hops de preview o un LOD más barato solo para drag.
