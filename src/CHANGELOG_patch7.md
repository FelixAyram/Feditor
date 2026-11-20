# Patch 7 — bolas siempre + merge junta + clamp debug

## Bolas
- Con spline **seleccionada o hover**: siempre se dibujan **inicio + fin + centro**
- Juntas enlazadas: se **fusionan** en una sola bola (merge < 0.35 m)
- Eliminado el anti-overlap que **separaba** la bola compartida en dos círculos

## Clamp G cuerpo
- Búsqueda binaria del delta máximo factible
- Límite ≈ max(L_vecino, 2*Rmax) * 1.25 hacia prev y hacia fin fijo de next
- Logs `[chain_dbg]` en clamp, prev, next (ver consola / log del editor)

## Debug
Buscá en el log:
- `[chain_dbg] body_grab CLAMP s=...` → se está limitando
- `[chain_dbg] body_grab ok` → no hizo falta clamp
- `[chain_dbg] handles n=...` → cuántas bolas tras merge
