Fix: splines transparentes al streamear + alerta IDs duplicados
================================================================

Archivo modificado:
  src/app/detail/modules/mod_streaming.inc.cpp

Problema
--------
Al entrar a un tile nuevo, PrepShiftGpu mata el GPU de splines cuyo
tile_gx/gy es doomed. PrepLists puede CONSERVAR esas splines en
edit_splines (start o end en tile cargado). Como el id ya está en
stream_have_sli, no entraban a new_spline_ids → no se re-subían.
Resultado: datos CPU OK (click/hover/nodos) pero malla invisible.

Cambios
-------
1) Merge (pass 4): si el id ya existe pero el GPU está muerto o no
   existe, se re-encola en stream_new_spline_ids + WARN.

2) Al terminar PrepLists (antes de stream_kick_spline_cpu):
   - Revive: toda spline en edit_splines sin GPU alive se agrega a
     stream_new_spline_ids.
   - Escaneo de IDs duplicados en edit_objects y edit_splines →
     log::warn + panel.set_status.

Cómo aplicar
------------
Copiá el archivo sobre tu árbol de fuentes (misma ruta relativa) y
recompilá el editor.

Qué mirar en el log
-------------------
  [WARN] stream revive dead GPU splines n=N (edit kept but tile GPU unloaded)
  [WARN] stream re-queue dead GPU spline id=...
  [WARN] DUPLICATE spline id=... count=...
  [WARN] IDs duplicados: X splines, Y objects — revisar mapa

Si revive n>0 y las splines siguen transparentes, el siguiente
sospechoso es el continue por visual_in_radius dentro de UploadSplines.
