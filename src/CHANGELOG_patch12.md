# Patch 12 — invalidación de referencias + hueco + sli

## Bug principal (por eso “cualquier cosa”)
Al insertar tramos, `edit_splines.push_back` **invalida** la referencia `s` del medio.
Luego se escribía memoria basura → `missing sli id=1221`, meshes volados, ids raros.

## Fix
- Todo el camino fill usa **solo ids** (`sid`, `prev_keep`, `next_keep`) y `find_spline_by_id` tras cada insert.
- Remesh final también por id.
- Temps copian `sli_path` explícito.
- Sin relleno: snap de junta si gap 2–50 cm + log `joint prev-mid gap=`.
- Más logs: `FILL prev→mid`, `fill_bridge last gap=`.

## Siguiente (AA de verdad)
Aún el relleno usa subdivisión por distancia/ángulo, **no** el solver biarc de AA.
Cuando esto deje de corromper, el paso siguiente es llamar la geometría de `autofill_between` para los hijos de la 1.

## Probar
1. Rellenar + Pegar ON, G cuerpo medio, mirar log: NO debe salir `missing sli`.
2. `fill_bridge inserted=` y temps con sli válido.
3. Rellenar OFF: `joint prev-mid gap=` debería ser &lt; 0.05 tras el snap.
