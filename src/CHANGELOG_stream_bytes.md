# Stream por **bytes/frame** (rápido + sin freno artificial por cantidad)

## Idea
No limitar “máx. 4 meshes”. Limitar **cuántos bytes de GPU se suben por frame**.

- 300 objetos chicos (~50 KB c/u) → caben en un frame si el total < budget  
- 2 edificios pesados (~3 MB c/u) → 1–2 por frame  

## Presupuesto
| Situación | Bytes/frame | Tiempo (ms) |
|-----------|-------------|-------------|
| Stream normal | **6 MB** | 20 |
| Teleport / force (`stream_queued_force`) | **12 MB** | 20 |

Siempre se procesa **≥1 ítem** aunque el ítem solo sea más grande que el budget
(no se puede quedar en 0/N).

## Estimación de coste
- **O3D / meshes**: `verts*32 + indices*8/14 + 128KB` overhead  
- **Terrain**: ~1 MB por tile  
- **Spline**: tamaño real de `SplineMesh` del slot CPU, o 256 KB fallback  
- **Scenery**: ~96 KB base (árboles/billboards baratos; SCO multi-mesh se acumulan)

## Continuidad (fix anterior)
- Budget de tiempo arranca **después** del join CPU  
- `(n == 0 || …)` / `stream_bytes_ok` → nunca stuck en 0/N  

## Archivo
`src/app/detail/modules/mod_streaming.inc.cpp`
