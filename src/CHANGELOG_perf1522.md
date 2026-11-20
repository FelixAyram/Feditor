# Cambios sobre el exe de las 15:22 (SIN COMPILAR: revisar errores de build)

## Qué bajó más los fps (log)
1. `stream_apply` (~116 s acumulados en frames lentos), luego `editor_logic`.
2. En ventanas lentas: `sco_visibility` ~4.2 ms/frame (normal 0.02), además `distance_cull`, `scripts`, `spline_handles`.
3. GPU sin llegar al 100%: el hilo principal (CPU) es el límite; encima hay mallas de terreno polycut de 40-76 MB por tile (hasta 22 M índices) y buffers en heap UPLOAD (`replace_textured_mesh`).

## Cambios
- **Paths / splines / decals / superficies / terreno**: exentos del recorte por % de pantalla; solo los limita el radio de visión.
- **% de pantalla mejorado**: la esfera es solo prefiltro; se oculta únicamente si la extensión proyectada mayor (alto o ancho) del AABB es menor al umbral. Objetos largos y planos ya no desaparecen. Si cruza el plano cercano, se mantiene.
- **LOD de terreno** (`terrain_lod.hpp`, `draw.cpp`, `csm.cpp`): solo tiles pesados (>=90000 índices). Anillo 0 = original; anillo 1 = nivel 2; anillo 2 = nivel 3; >=3 = nivel 4. Clustering de vértices reales (no cambia alturas), bordes intactos, construido en hilo de trabajo. Las alturas de objetos usan el heightfield, no la malla. Sombras usan el mismo LOD. `set_terrain_lod_enabled(bool)` para apagarlo. Constantes: `kRingToLevel` (draw.cpp), `kCellMult`/`kMinIndicesForLod` (terrain_lod.hpp).
- **Carga más rápida** (attachments / GPU add splines): cola SA recorre todas las entradas (ya no se bloquea con `break`), give-up por tiempo (>4 s y 20 reintentos, o 120), índices persistentes en UploadSplines.
- **Visibilidad**: `distance_cull` sale temprano si la cámara se movió <0.75 m; `sco_visibility` por cursor round-robin con presupuesto de tiempo; bucle hidden/ghost solo si hace falta.
- **matl_alpha**: shaders con modos extra (descarte por alpha); fallback de [matl] por nombre de textura sin carpeta/extensión (`mesh.cpp`) para que alpha 1/2 se apliquen aunque el nombre difiera.

## No hecho / incierto
- Alpha 2 traslúcido (Bushaltestelle): la separación en dos pasadas (caras traseras) NO está conectada; solo los modos de shader. La causa de alpha 1 (Stadttor) es inferida sin ver los .o3d.
- La causa exacta de "objeto largo se oculta" podría estar también en la banda LOD de `setup_06` (radio desde `half_xy`).
- Nada se compiló ni probó aquí.
