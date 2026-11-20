# Patch 5 — proyección bolas + color + G cadena sin mover next

## Bugs corregidos

### 1. Bolas mal posicionadas / se mueven con la cámara
Causa: `world_xy_to_screen` multiplicaba el WVP **transpuesto** (layout HLSL) sin deshacer el transpose.
Fix: igual que `screen_dist2` — `XMMatrixTranspose` + `XMVector4Transform`.
Además: escala framebuffer → coordenadas ImGui (`DisplaySize`) por si hay DPI ≠ 1.

### 2. Las 3 bolas se ponían azules al seleccionar la spline
Causa: `is_sel` miraba “cualquier handle de la spline seleccionada”.
Fix: solo el handle **clickeado** queda azul (union o grad). Hover → amarillo; idle → blanco/amarillo suave.

### 3. G + pegar cadena movía la spline 3 (next) y no fijaba su final
Causa: `remesh_spline` hace follow y teletransporta el next al final de la actual.
Fix con stick_chain ON:
- `constrain_chain_end_fixed` (refit L/R de la actual → inicio de next)
- refit del prev al nuevo inicio
- `remesh_spline_one` (sin follow) para actual y prev → next no se mueve

Con stick_chain OFF se sigue usando `remesh_spline` normal (y no se tocan prev/next ids).

## Archivos
- `src/app/detail/modules/mod_spline_chain_handles.inc.cpp`
- `src/app/detail/map_editor_run_loop_03.inc.cpp`
- `src/app/detail/map_editor_run_loop_04.inc.cpp`
