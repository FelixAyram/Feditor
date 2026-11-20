# Fix: stream se queda en "GPU add terrain 0/5" (y fases 0/N)

## Causa real (no era “demasiados ítems”)
El **presupuesto de ms del frame se arrancaba al entrar a `pump_stream_apply`**,
*antes* de esperar/joinear el worker CPU de terrain/splines.

Secuencia rota:
1. `budget_end = now + 8ms`
2. `join()` del worker de terrain (o espera) se come ese tiempo
3. `while (... && budget_ok())` → **nunca entra**
4. Status queda **0/5** para siempre (o muchos frames)

Por eso “antes cargaba bien” y tras bajar el budget a 8 ms empezó a colgarse
en 0/N: no era el tamaño de los archivos, era **el reloj del budget**.

## Solución robusta (continuidad, no limitar)
1. **`stream_budget_begin()`** se llama **después** del wait/join de CPU, justo
   antes del loop de upload GPU.
2. Condición del loop: `(n == 0 || budget_ok())` → **siempre avanza al menos 1
   ítem por frame** aunque el budget esté justo. Así no hay deadlock en 0/N.
3. **Restaurados** budgets ~20 ms y slices (terrain 8, splines 8, scenery 16,
   meshes 32 soft). El time-slice sigue existiendo; ya no frena la continuidad.
4. Se mantiene `try/catch` + log SLOW en scenery (un SCO malo no mata el stream).
5. Se mantiene `wait_gpu = false` en path stream (setup_02 del patch anterior).

## Archivo
`src/app/detail/modules/mod_streaming.inc.cpp`

Copiar, recompilar, probar RMB stream hasta ver terrain 1/5, 2/5… sin quedarse en 0.
