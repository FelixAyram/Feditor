# Patch 11 — Rellenar cadena realmente inserta + sin huecos

## Problemas
1. Con **Rellenar OFF** o clamp fuerte, no se podía alejar lo suficiente para necesitar tramos.
2. `try_end_heading` con R max=200 “siempre ok” → **0 inserts**.
3. Huecos en juntas (residual / follow).

## Fix
- **Rellenar ON**: movimiento **libre** de la 2 (sin clamp); los puentes absorben.
- Subdivisión forzada por **distancia (>8 m)** y **giro (>25°)** aunque el fit de un tramo “pase”.
- `fill_bridge_between(prev, s)` entre 1 y 2.
- `fill_to_fixed_end(s, next, fin_fijo)` entre 2 y 3 (la 3 es el último tramo, fin fijo).
- Logs: `[chain_dbg] fill_bridge inserted=N` / `fill_to_fixed_end inserted=N`.

## Cómo probar
1. Activar **Rellenar cadena** + **Pegar cadena**.
2. G en cuerpo de la spline del medio, alejar varios metros.
3. Deben aparecer tramos nuevos; juntas sin hueco.
4. LMB confirma; Esc/GG borra temporales.
