# Patch 9 — Undo estable + Rellenar cadena + bolas

## A) Undo / Redo
- Focus de snapshot camina **next_id y prev_id** (antes solo next → el prev no se remesheaba).
- Al aplicar undo: **siempre** se agregan al focus los IDs con geom distinta (no solo si focus vacío).
- Efecto: Ctrl+Z restaura pos/L/R de 1,2,3 y remeshea los tres.

## B) Checkbox **Rellenar cadena**
- En el panel de arriba (junto a Pegar cadena).
- OFF: clamp actual (effective_max_r + residual).
- ON: tras mover cuerpo de 2, intenta `fit_spline_to_end_heading`; si no llega, **inserta** hasta 20 tramos intermedios (mismo `.sli`) entre 1→2 y 2→3.
- Preview en el drag (`fill_temp_ids`); al LMB quedan fijos; al cancelar G / nuevo G se borran los temporales.

## C) Bolas inicio/fin
- Selección azul **exacta**: mismo `spline_id` + `is_start_side` (ya no se iluminan las dos).

## D)
- Con rellenar OFF se mantiene clamp; con ON el relleno alinea heading.

## Archivos
- mod_undo.inc.cpp
- mod_spline_chain_handles.inc.cpp
- map_editor_run_setup_03.inc.cpp (apply geom-diff)
- map_editor_run_loop_02/03/04.inc.cpp
- map_editor_imgui_panel.cpp
- editor_side_panel.hpp
- map_geo_tools.hpp
