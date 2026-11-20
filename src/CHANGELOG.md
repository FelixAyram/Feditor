# FPS con muchos árboles — clone GPU para alpha-test

## Causa
`append_or_clone_o3d` marcaba **cualquier** material no-opaco como
`unique_tex` → cada árbol hacía `append_o3d` completo (nuevo VB/IB + log mat)
en lugar de `clone_chunk`.

Los árboles usan `matl_alpha=1` (alpha test) → miles de uploads iguales.
Log: `mat Shrubbery_04_20.o3d` repetido miles de veces.

## Fix
1. **setup_01**: `unique_tex` solo para texturas **por instancia**
   (`text_texture` / `freetex` / `on_surface`). Alpha-test y blend **sí clonables**.
2. **mesh.cpp**: log `mat` una sola vez por mesh+slot+path.

IDs de objetos y chunk indices se mantienen (clone = misma malla GPU,
matriz distinta).

## Undo
`UNDO/map_editor_run_setup_01.inc.cpp` → `src/app/detail/`
`UNDO/mesh.cpp` → `src/gfx/dx12/o3d/`

## TTData audit vs Unit121 (Tracks & Trips)
- Formatos .ttr/.ttp/.ttl, Busstops.cfg, StnLinks.cfg reescritos según el decompilado; ver docs/REVERSE_ENGINEERING_Unit121.md.
- Lógica de TimeTableMan: largos de track/StnLink, renumeración al añadir/borrar entries, inserción ordenada de estaciones, borrado de archivos obsoletos al guardar.
- Test: tests/tt_roundtrip_test.cpp.

## Tracks tab: carga real + todos los botones y subventanas
- Fix: load_ttdata borraba su propia ruta (alias con data.root) y nunca cargaba nada.
- ensure_loaded por mapa; estado visible; diálogos de nombre; Add from List; Assistent; Edit Profiles; Edit Timetable; Find Errors; Show Timetable of Station; TTData copiado a original/cambios al guardar.

## Tracks: selección, rails, undo y rendimiento
- Tracks tab: se quitó la sincronización con la vieja lista de archivos Win32 que devolvía la selección del combo cada frame (no dejaba elegir un track nuevo).
- Rails: los colores de Tracks&Trips solo se ven en las pestañas Tracks / Estaciones (antes `g_omsi_tt_mode==0` siempre era verdadero). Hover amarillo, miembros del track rojos, entrada elegida rojo-naranja; Ctrl completa la ruta desde el último path como en Traffic.
- Paradas (busstop .sco): hover amarillo, seleccionada y paradas del trip en rojo; se pueden elegir en el 3D en los modos Add Station / Show Timetable / StnLink start-end.
- Paths con sentido 1: textura girada 180° (u->1-u, v->-v), sin espejo.
- Rails: firma por dueño (spline/objeto) con comprobación periódica; solo se reconstruyen los que cambiaron (G, undo, replace_sli). Undo ya no reconstruye todos los rails.
- Undo/redo: validación de mallas antes de subirlas a la GPU, invalidación de resultados asíncronos previos al snapshot y pausa del undo mientras hay más de 24 remallas en cola.
- Edit Profiles / Edit Timetable: ventanas centradas, Trip No. con tabla de horas, Delete Line / Delete Profile.
