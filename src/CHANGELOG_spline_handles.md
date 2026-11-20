# Changelog — Spline chain handles & prev/next linking

Fecha: 2026-09-13

## Resumen

Implementación parcial de las 5 fases pedidas para bolas de unión, círculos de gradiente, encadenado prev/next al crear con N, limpieza al borrar, y helpers de movimiento restringido por cadena.

**Importante:** los helpers de dibujo/pick/movimiento están en un módulo nuevo listo para integrar en el run-loop. La parte visual (ImGui draw de círculos + pick) y el hook completo del grab con G aún requieren el cableado final en el frame loop (ver sección "Pendiente de integración").

---

## Archivos modificados

| Archivo | Cambio |
|---------|--------|
| `include/omsi/map_geo_tools.hpp` | Nuevos campos en `EditorUiSettings`: `union_ball_screen_pct` (default 0.02), `grad_circle_screen_pct` (default 0.01) |
| `src/ui/editor_prop_dialogs.hpp` | Firma de `run_editor_options_dialog` ampliada con punteros opcionales a los % de tamaño |
| `src/ui/editor_prop_dialogs.cpp` | Diálogo de opciones más alto con 2 trackbars nuevos (bola unión / círculo gradiente) |
| `src/app/detail/map_editor_run_loop_02.inc.cpp` | 1) N-continue: solo linkea si el padre no tiene `next_id`; al colocar actualiza `padre.next_id`. 2) Delete single/group: limpia `prev_id`/`next_id` que apuntaban al id borrado. 3) Call site del diálogo de opciones pasa y guarda los nuevos %. |
| `src/app/detail/modules/mod_spline_chain_handles.inc.cpp` | **NUEVO** — helpers de cadena, fit clamped, recolección/pick de handles, fit de centro en spline aislada |

---

## Comportamiento implementado

### Encadenado al crear con N (Fase 2) ✅
- Si la spline seleccionada **no** tiene `next_id`:
  - Nueva recibe `prev_id = padre.id`
  - Padre recibe `next_id = nueva.id` (y se limpia cualquier otro que apuntara al mismo hijo)
- Si la seleccionada **ya tiene** `next_id`:
  - Se crea suelta (prev=0, next=0), solo arranca en el final del padre
- `push_undo("n_continue_spline")` ya existía → cubierto por undo/redo

### Borrado (Fase 2) ✅
- Al borrar una spline (simple o grupo):
  - Cualquier otra con `next_id == sid` → `next_id = 0`
  - Cualquier otra con `prev_id == sid` → `prev_id = 0`
- Cubierto por `push_undo("delete_single")` / `push_undo("delete_group")`

### Opciones (Fase 1 parcial) ✅
- Trackbar "Tamaño bola unión (% pantalla)" — 0.5 % … 5.0 %
- Trackbar "Tamaño círculo gradiente (% pantalla)" — 0.5 % … 5.0 %
- Se persisten en `EditorUiSettings` vía `save_editor_ui_settings`

### Helpers listos (Fases 1/3/4) — módulo nuevo
- `clear_spline_chain_links` / `assign_next_id_unique` / `validate_and_break_spline_cycles`
- `fit_spline_end_to_xy_clamped` (respeta `spline_max_radius`, fallback a recta)
- `constrain_chain_after_start_move` / `constrain_chain_end_fixed`
- `collect_spline_handles` + anti-solape + `pick_spline_handle` (prioriza UnionBall)
- `fit_isolated_center_drag` (axis lock 0=free, 1=X, 2=Y, 3=Z/grad)

---

## Pendiente de integración (para terminar las 5 fases)

1. **Incluir el módulo** en el setup del editor (junto a `mod_undo.inc.cpp`):
   ```cpp
   #include "app/detail/modules/mod_spline_chain_handles.inc.cpp"
   ```
   (o el path relativo que use el proyecto)

2. **Cada frame** (antes del pick de LMB):
   - Llamar `collect_spline_handles(handles, fw, fh, wvp, hovered_spline_idx, union_pct, grad_pct)`
   - En el pick de click: `pick_spline_handle(mx, my, handles)` y si hit → seleccionar ambas splines de la unión / entrar en modo gradiente

3. **Dibujo ImGui** (en el bloque `imgui_overlay.begin_frame()`):
   - Para cada handle: `ImGui::GetBackgroundDrawList()->AddCircleFilled(...)` con los colores acordados
   - Resaltado naranja de extremos cuando hay UnionBall seleccionada (muestrear la spline y dibujar polyline)

4. **Grab (G)** cuando stick_chain está activo:
   - Tras mover posición de una spline, llamar `constrain_chain_after_start_move` o `constrain_chain_end_fixed`
   - En spline aislada + handle GradCircle: `fit_isolated_center_drag` con axis lock según teclas X/Y/Z

5. **Botón "ajustar altura a mesh debajo"** (límite grad ±1000): UI + sample del terreno/mesh bajo el centro

6. **Persistencia de los nuevos %** en el loader/saver de `EditorUiSettings` (si el .cfg no los escribe aún, añadir claves en `load_editor_ui_settings` / `save_editor_ui_settings`)

---

## Colores acordados (referencia)

| Handle | Estado | Color |
|--------|--------|-------|
| Bola unión | idle+hover | blanco semitransparente → amarillo intenso |
| Bola unión | seleccionada | azul brillante |
| Bola unión | >2 splines | naranja |
| Círculo grad | neutro / hover / sel | amarillo suave / amarillo intenso / cian o azul |
| Extremos unión | seleccionada | naranja brillante #FF8C00 |

---

## Undo / Redo

Todas las operaciones que mutan splines ya pasan por `push_undo(...)` existente:
- `n_continue_spline`
- `delete_single` / `delete_group`
- `grab_g_start` (cuando se integre el constrain en el grab)

El focus de remesh del undo ya incluye la cadena forward por `next_id` (ver `fill_snapshot_focus` en `mod_undo.inc.cpp`).

---

## Cómo probar lo ya cableado

1. Seleccionar una spline sin `next_id` → N → verificar en el panel / log que la nueva tiene `prev_id` y el padre tiene `next_id`.
2. Seleccionar una que ya tenga `next_id` → N → la nueva debe quedar suelta (`prev=0`).
3. Borrar la del medio de una cadena → los vecinos deben quedar con prev/next en 0.
4. Abrir Opciones del editor → deben aparecer los dos sliders nuevos de tamaño de handles.
