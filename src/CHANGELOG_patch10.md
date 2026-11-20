# Patch 10 — rotación final en juntas (la causa real)

## Causa
1. `fit_spline_to_xy` solo iguala **XY del final**, no la **heading**.
2. `remesh_spline` + stick_chain **teleportaba** el inicio del next y dejaba L/R igual → el final se iba y la rotación de la junta se rompía.
3. Al seleccionar, se llamaba `remesh_spline` → follow en cascada destruía lo editado.

## Fix
1. `fit_spline_to_end_heading` en adapt prev→cur y next→fin fijo (pos + heading).
2. Follow de stick_chain: guarda el final del next, mueve el inicio, **re-fit** L/R para recuperar ese final (pos+heading).
3. Select + stick_chain: solo `remesh_spline_one` (sin follow forzado).
4. G-start guarda `chain_fix_next_eh` (heading del final de next).

## Probar
- Cadena 1-2-3, Pegar cadena ON, G cuerpo 2, soltar LMB.
- En la junta 1↔2 y 2↔3 el camino debe quedar **tangente** (sin codo).
- Seleccionar otra spline y volver: no debe “saltar” el final de 3.
