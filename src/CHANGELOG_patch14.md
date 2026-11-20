# Patch 14 — mejoras CAD biarc + fin fijo real

Fuentes: biarc G1 (Park, Bolton, Bertolazzi), OMSI Complete-to.

## Cambios
1. **Biarc rechazado** si gap > 15 cm → pasa a MULTI (antes aceptaba gap 0.9 m).
2. **`chain_fix_next_id` / `chain_fix_prev_id`** congelados al empezar G.
   - Ya no se usa un temp (1223) como cola → el fin de la 3 no “se va”.
3. MULTI de `fill_to_fixed` **re-fit final** al (ex,ey,eh) guardado.
4. Cada frame usa esos ids fijos, no `s.next_id` (que podía ser un temp).

## Log esperado
- `fill_bridge BIARC reject gap=... → MULTI` o `gap=0.0x`
- `fill_to_fixed ... tail=1222` (id real de la 3, nunca temp)
- `fix=(x,y)` estable durante todo el G
