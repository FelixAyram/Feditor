# Patch 15 — debug completo por segmento + place remesh de toda la cadena

## Debug nuevo
Cada segmento:
```
SEG[i] id=… start=(x,y) h0=… L=… R=… end=(x,y) eh=…
```
Cada ~15 frames durante G:
```
=== DUMP prev-mid / mid-next ===
TEMP id=… start=… L=… R=… end=…
```
Al LMB place:
```
PLACE begin …
=== DUMP PLACE-before/after ===
PLACE remesh id=… start=… end=… next=…
```

## Place
- Remesh **toda** la cadena (walk prev→…→next), no solo vecinos inmediatos
- Remesh de todos los `fill_temp_ids` antes de `clear` (siguen en `edit_splines`)

## MULTI
- Trial L/R + walk + refit del último tramo al target
- Log de gap y last_end vs right_start

Pegá del log un bloque `SEG[0]`…`SEG[n]` + `PLACE remesh` si sigue fallando.
