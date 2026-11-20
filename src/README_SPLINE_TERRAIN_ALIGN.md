# Mejora de terreno para `[spline_terrain_align_2]`

## Qué corrige

El recorte anterior dependía demasiado de la intersección de un contorno con los cuatro bordes de cada quad de terreno de 5 m. Si el contorno no se podía reconstruir como un polígono válido, podía descartarse una celda completa; también podía pasarse por alto un hueco pequeño contenido enteramente dentro de una celda. En esos casos la malla terminaba con triángulos ausentes o con una teselación incorrecta alrededor de la spline.

`editor/terrain_polycut.cpp` ahora:

- conserva el recorte exacto por contorno cuando el polígono se puede reconstruir sin ambigüedad;
- detecta contornos pequeños encerrados dentro de una celda, contornos coincidentes con un borde y casos donde se cruzan varios contornos;
- en casos degenerados subdivide recursivamente solo las celdas ambiguas. Con el terreno estándar de 300 m dividido en 60 intervalos, la celda original mide 5 m y la profundidad de rescate 6 reduce el tamaño local a 7,8125 cm;
- si el ear clipping falla, no usa una triangulación en abanico que podría volver a cubrir parte del hueco; usa el camino adaptativo de rescate;
- evalúa las alturas de los vértices nuevos sobre el mismo plano triangular que la malla regular (diagonal `c00-c11`), evitando la discrepancia que generaba la interpolación bilineal en pendientes;
- omite contornos no finitos, degenerados o autointersectados y no procesa contornos cuyo AABB queda fuera del tile.

La ruta adaptativa es una recuperación acotada, no una reproducción bit a bit del teselador privado de OMSI. El camino normal sigue siendo el recorte poligonal exacto del contorno muestreado.

## Cómo se interpreta el tag

El parser descompilado de OMSI en `_Unit126.c` almacena `[spline_terrain_align]` como modo 1 y lee el entero de `[spline_terrain_align_2]` como valor del spline. La documentación de formatos de openOMSI explica que la forma del hueco procede de `[terrainholeprofile]` del `.sli`, o de un perfil sintetizado a partir de los `[profile]` si no existe; el `terrain_align` hace que se recorte el contorno en el terreno. La documentación también indica que un contorno que se cruza consigo mismo no produce el corte.

Los modos de extremos implementados son los siguientes:

- `1`: usa los desplazamientos de perfil en ambos extremos.
- `2`: conserva el extremo lejano en el final de la spline.
- `3`: conserva el extremo cercano en el inicio de la spline.
- `4`: conserva ambos extremos en el inicio/final de la spline.

El tag no es un algoritmo para crear alturas del `.terrain`: la documentación de openOMSI distingue los valores de altura, que ya se guardan en el archivo de terreno, del recorte que realiza AlignTerrain. En este editor, el muestreo de alturas (`stamp_terrain_align`) y el recorte/topología de malla (`apply_terrain_polycut_to_mesh`) son pasos distintos. Los nuevos vértices de la línea de corte conservan además la altura Z del perfil `.sli`, sin escribir cambios en el archivo `.terrain`.

## Validación realizada

- Compilación sintáctica de `terrain_polycut.cpp` con C++20, `-Wall -Wextra -Wpedantic`, usando cabeceras shim de prueba para los includes externos que no vienen dentro del ZIP: correcta.
- Harness geométrico con terreno inclinado y dos huecos (uno atravesando varias celdas y otro menor que una celda): 17.538 vértices, 8.486 triángulos, 0 muestras de superficie ausentes fuera de los huecos, 0 triángulos detectados dentro de los huecos y error máximo de altura de 0 respecto del plano triangular de la malla original.
- No se pudo compilar el ejecutable completo de Windows desde el material entregado: faltan dependencias externas como `f4f23ma/math.hpp` y el entorno de compilación DirectX/GLFW/ImGui del proyecto. Por eso no se afirma que se haya validado con una compilación completa ni con una ejecución dentro de OMSI.

## Fuentes consultadas

- openOMSI, documentación de formatos: https://github.com/openOMSI-org/openOMSI/blob/main/docs/FORMATS.md
- Repositorio y documentación de openOMSI: https://github.com/openOMSI-org/openOMSI
- Código descompilado incluido en `units_c, codigo fuente omsi 2.zip`: `_Unit126.c` (parseo de `[spline_terrain_align]` y `[spline_terrain_align_2]`), `_Unit84.c` (generación de malla de spline) y `_Unit87.c` (refresco/corte de huecos de terreno).
