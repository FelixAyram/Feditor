# Carga + Stream optimizado (HDD)

## Ya cubierto (boot + stream add tiles)
- `AsyncWorldLoader` (boot y stream `to_add`) usa:
  - Parse `.map` ordenado; en HDD **secuencial**
  - `preload_scos` únicos + orden + HDD seq / SSD par
  - `.sli` / `.o3d` igual
  - `FILE_FLAG_SEQUENTIAL_SCAN` + 1 MiB en lectura cfg

## Stream (runtime) — este zip completa
- Worker **spline**: prefetch `.sli` únicos ordenados antes del tessellate paralelo (RAM)
- Worker **terrain**: paths ordenados; en HDD build secuencial

## Env
```
set OMSI_HDD_LOAD=1
```

Stream reutiliza el mismo `AsyncWorldLoader` al añadir tiles → el prefetch SCO del job ya aplica.
