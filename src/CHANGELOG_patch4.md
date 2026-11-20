# Patch 4 — Handles visuales + G cadena + bake paths con offset

## Cambios

### 1. Bolas / círculos dibujados (ImGui)
- Union ball y grad circle en `GetBackgroundDrawList` cada frame
- Colores: blanco/amarillo/azul (unión), amarillo/cian (grad)
- Resaltado naranja del trazo de las splines de la unión al hover del ball
- Tamaños desde `EditorUiSettings.union_ball_screen_pct` / `grad_circle_screen_pct`

### 2. G con pegar cadena
- Si stick_chain ON y la spline tiene next_id → `constrain_chain_end_fixed` (refit L/R)
- Si tiene prev_id → adapta L/R del prev al nuevo inicio
- Clamp a `spline_max_radius`

### 3. Bake SLI
- Al iniciar "Seleccionar SLI (bake)" **incluye la selección actual** en el grupo
- Paths en el .sco: `R' = R - lateral`, `L' = L * |R'|/|R|` (ej. R=20 L=20 lat=-5 → R'=25 L'=25)

### Archivos
- run_loop_01/02/03/04, modules, mod_spline_chain_handles, editor_sli_bake.cpp, prop_dialogs, map_geo_tools.hpp

