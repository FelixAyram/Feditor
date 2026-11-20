# Terrain Align v3: altura exacta en bordes y splines encadenadas

## Qué cambia en esta iteración

- `editor/terrain_polycut.hpp`: `TerrainHoleRing` admite alturas `z` opcionales junto a sus coordenadas XY. Los anillos de objetos siguen usando la altura muestreada del terreno; los anillos de `[spline_terrain_align_2]` transportan las alturas del perfil de spline.
- `editor/terrain_polycut.cpp`: el contorno se evalúa con el offset vertical `y` de cada `[terrainholeprofilepnt]` y el mismo `chain_end_z` usado por el generador de mallas. Se conserva la altura de perfil en las intersecciones con el borde de cada celda, en los puntos de los arcos y en los vértices nuevos de la triangulación. Antes estos vértices siempre leían el plano triangular del heightfield anterior, que podía discrepar de la spline y dejar una separación visible.
- `spline/spline_geometry.cpp`: el estampado de alturas también llama a `resolve_spline_mesh_chain_end_z`, usando las elevaciones por ID disponibles en el conjunto de splines. Las splines normales enlazadas ahora usan el mismo endpoint vertical que el generador de su malla. Las curvas con `use_delta_height` / `[spline_h]`, gradientes, radio/curvatura, cant y skew siguen pasando por `evaluate_spline`.
- Se eliminó una variable temporal sin uso en la inversión mundo→spline.

## Correspondencia con OMSI

En el código descompilado de `_Unit84.c`, la malla de la spline se construye desplazando longitudinalmente cada punto del perfil con `skew * profile.x`; el perfil de recorte aporta offsets verticales y longitudinales individuales. El editor aplica ahora esas convenciones para construir el contorno y para colocar sus vértices de borde en Z. `resolve_spline_mesh_chain_end_z` es la misma función ya utilizada por el generador de malla del editor.

El margen de mezcla del terreno vecino sigue siendo una aproximación explícita del editor, no una afirmación de que se haya recuperado el algoritmo privado completo de OMSI. El cambio v3 mejora la unión vertical exacta en el borde; no cambia el archivo `.terrain` en disco.

## Validación realizada

- Compilación sintáctica C++20 de `editor/terrain_polycut.cpp` con advertencias `-Wall -Wextra -Wpedantic`: correcta.
- Prueba de integración del contorno con spline recta y `skew_start != skew_end`, perfil sintético con offsets longitudinales `0` y `-0.5 m`, y recorte real sobre una malla regular: correcta.
- La prueba confirma que los vértices del contorno conservan Z de perfil incluso cuando el heightfield base tiene otra altura.
- Prueba de los bloques de proyección y estampado: inversión de skew, gradiente, apron lateral/frontal y elevación de extremo enlazado: correcta.

**Límite de validación:** el ZIP de fuentes original contiene `src/`, pero no todos los headers de `include/` ni las dependencias/configuración completas. Por eso no se compiló el ejecutable Windows completo en este entorno. El código debe recompilarse en la carpeta de tu proyecto para confirmar el resultado visual allí.
