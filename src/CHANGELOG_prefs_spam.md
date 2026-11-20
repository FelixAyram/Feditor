# Bug: editor se congela / log inunda con "prefs saved"

## Evidencia (tu log)
Desde ~18:03:11 hasta 18:03:26, **~989×**:
```
editor UI prefs saved (global): ...\omsi_f4_editor.cfg
editor prefs saved (global omsi_f4_editor.cfg [vista]/[ui])
```
≈ **cada frame** escribe a disco. Eso clava el UI y no es el stream.

## Causa
En `map_editor_run_loop_01.inc.cpp`:
```cpp
vista_dirty = (ev.* != editor_ui_settings.*) || (stick_chain != settings);
if (vista_dirty) persist_ui_settings(); // collect + save
```
`collect_ui_settings` **no dejaba** todos los campos comparados iguales a `ev` /
`imgui_state` → `vista_dirty` seguía **true** el frame siguiente → bucle infinito
de saves.

## Fix
1. Antes de guardar, **copiar** los campos de vista desde `ev` / stick al struct.
2. Tras `collect_ui_settings`, **reaplicar** esos campos (por si collect no los
   round-trippea).
3. Quitar el log duplicado del caller (queda el de `save_editor_ui_settings`).

## Archivo
`src/app/detail/map_editor_run_loop_01.inc.cpp`
