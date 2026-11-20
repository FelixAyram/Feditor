# Patch 6 — G cuerpo/centro/unión según reglas finales

## A) G cuerpo + pegar cadena
- Mueve spline **rígida** (mismo L/R/heading)
- Clamp al alcance de prev y de next (fin de next **congelado** al empezar G)
- Adapta L/R de prev; mueve inicio de next y adapta su L/R al fin fijo
- `remesh_spline_one` (sin follow) durante el drag

## B) Bola de unión única
- Solo se dibuja desde el lado `next_id` (padre) → una bola por junta
- Click + G arrastra fin A + inicio B; fin de B fijo

## C) Centro amarillo + G
- Aislada (sin prev/next): fit rot/L/R manteniendo extremos fijos
- Con prev+next o Z (GrabHeight): solo gradientes (panza)
- Click cuerpo (sin handle) + G = movimiento de cuerpo normal

## D) Wireframe
- Al confirmar LMB: remesh completo (preview=false) de s + prev + next

## Colores
- Solo el handle clickeado es azul; hover amarillo; idle blanco/amarillo suave
