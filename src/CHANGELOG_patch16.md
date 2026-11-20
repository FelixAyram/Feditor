# Patch 16 — unión estricta (sin huecos)

## Bug raíz
`fill_bridge` usaba referencias `left`/`right` y hacía `push_back` → **referencia inválida**.
Los L/R del prev se perdían y el gap quedaba en 8–12 m.

## Fix
- Todo por **id** + `find_spline_by_id` tras cada insert.
- SINGLE → BIARC → MULTI (pasos de ángulo/distancia).
- Cada nodo nuevo nace en `evaluate(prev)`.
- Último tramo: `fit_end_heading` y si gap > 5 cm → `fit_to_xy` (posición exacta).
- Walk final de continuidad.
- Radio sin límite en modo relleno (no usa el R max del panel).

## Log esperado
```
fill_bridge SINGLE gap=0.0x   ó
fill_bridge BIARC gap=0.0x    ó
SEG[i] ...
fill_bridge STRICT gap=0.0x
fill_bridge FINAL gap=0.0x
```
Gap debe ser **< 0.15 m**. Si ves > 1 m, algo más falla.

## Nota
El log que pegaste (20:42) es **anterior** a patch 15/16 (no hay `SEG[` ni `FINAL`).
Recompilá este y probá de nuevo.
