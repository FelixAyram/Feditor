# Changelog / checklist — no romper el editor

## Semáforos (Ampel) — CRÍTICO

**Síntoma si se rompe:** todas las luces (rojo+amarillo+verde) visibles a la vez, o congeladas.

**Causa típica:** `renderer.set_chunk_visible(chunk, true)` sobre `scene_places` (SCO)
después de `apply_sco_script_visibility()`, o en un bucle cada frame.

**Regla:**
- La visibilidad de meshes de SCO con `[visible]` / Red / Yellow / Green la decide
  **solo** `scenery_scripts.mesh_visible` vía `apply_sco_script_visibility()`.
- **Nunca** forzar `set_chunk_visible(sp.chunk, true)` en scene_places de objetos normales.
- OK forzar true: splines (`spline_gpu`), markers, path rails, trees sin script, trim preview.
- OK forzar false: hide (H), doom, unload.
- Opacity/ghost del panel: solo `set_chunk_opacity` en SCO, no `set_chunk_visible(true)`.

**Al final de cada frame** (loop_04): volver a llamar `apply_sco_script_visibility()`
por si un sync de GPU a mitad de frame dejó luces mal.

**Archivos sensibles:**
- `map_editor_run_loop_04.inc.cpp` — restore opacity SCO
- `map_editor_run_loop_01.inc.cpp` — unhide all, hide toggle
- `map_editor_run_setup_02.inc.cpp` — sync_object_gpu / sync_spline_attach_gpu
- `map_scenery_script.cpp` — `mesh_visible` (prefer vars del Ampel hijo, no solo parent)

## Splines transparentes

- Restaurar `set_chunk_visible(true)` + opacity en **splines** cada frame si no están hidden.
- No aplicar la misma lógica a SCO (rompe Ampel).

## Tangential / AlignTerrain

- Tangential: solo `[splineAttachement]`, flag body `1`/`0` tras repeat_range.
- `[spline_terrain_align]` en object/SA/attachObj = AlignTerrain del SCO.
- `[spline_terrain_align_2]` + N = stamp de terreno del **spline** (vía).

## Paint layer

- Firma de `paint_omsi_ground_layer_mask` debe incluir hardness + strength.
- UV mask: Y invertida respecto al mesh ground_paint.

## Surface follow

- `apply_object_surface_follow_fn` puede ser null; no asumir que está cableado.

## Al tocar visibilidad / luces / stream

1. ¿Este `set_chunk_visible(true)` es sobre `scene_places`?
2. Si sí → ¿es Ampel o SCO con `[visible]`? → no forzar true.
3. Compilar y probar un Kreuz / LED Ampel: debe ciclar, no todas las luces a la vez.
