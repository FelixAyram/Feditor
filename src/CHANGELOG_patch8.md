# Patch 8 — 4 pasadas de perfeccionamiento

## Pasada 1 — Por qué no clampeaba
El límite usaba `max_r` del UI (a menudo 200 m) → casi nunca limitaba.
**Fix:** `effective_max_r(vecino)` = acotado por la forma actual (≈ max(2|R|, 2L) y el R max UI).

## Pasada 2 — Clamp real por residual
No basta mirar distancia al punto: se hace `fit` y se mide si el **final evaluado** queda a ≤ 0.6 m del objetivo.
Búsqueda binaria del factor `s` desde **grab_start** (no desde la pose del frame).

## Pasada 3 — Bolas dobles
Merge por:
- mismo par de ids (prev↔next)
- o distancia mundo < 1.0 m  
Recalcula pantalla al centro promediado. Sin anti-overlap.

Siempre se emiten inicio + fin + centro en sel/hover.

## Pasada 4 — Cuerpo rígido estable
Cada frame de G: restaura `length/radius/heading` del inicio del grab antes de trasladar.

## Logs
`[chain_dbg] CLAMP s=...` solo cuando cambia el factor de clamp.
`prev residual=` / `next residual=` si el fit no llega a 0.6 m.
