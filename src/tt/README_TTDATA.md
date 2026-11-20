# TTData — Tracks & Trips / Station Links (fidelidad OMSI 2 Unit121)

Implementación C++ de `TTimeTableMan` con parsers/writers y API de edición que replican el comportamiento del editor nativo.

## Integración rápida

```cpp
#include "omsi/tt_man.hpp"

// Al abrir mapa
omsi::tt::global_tt_man().load(map_root);

// Click en path (modo Add Trackentry / Add Path Segments)
omsi::tt::PathPickInfo pick{owner_id, path_index, tile_index, internal_no, length_m};
if (man.mode == omsi::tt::TtEditMode::AddTrackEntry)
    man.add_track_entry(man.selected_track, man.selected_track_entry, pick);
else if (man.mode == omsi::tt::TtEditMode::AddPathSegments)
    man.add_stnlink_entry(man.selected_stnlink, man.selected_stnlink_entry, pick);

// Click en busstop
if (man.mode == omsi::tt::TtEditMode::AddStation)
    man.add_trip_busstop(man.selected_trip, object_id, name, tile_index);
else if (man.mode == omsi::tt::TtEditMode::SetStartStation)
    man.set_stnlink_busstop(man.selected_stnlink, object_id, true);
else if (man.mode == omsi::tt::TtEditMode::SetEndStation)
    man.set_stnlink_busstop(man.selected_stnlink, object_id, false);
else if (man.mode == omsi::tt::TtEditMode::ShowStationTT)
    auto s = man.show_timetable_for_station(object_id);

// Guardar
if (man.dirty) man.save();

// Find Errors
man.validate_all();
for (const auto& e : man.data.errors) /* mostrar */;

// Search Next Erroneous (Station Links)
int next = man.search_next_erroneous_stnlink(man.selected_stnlink);
```

## Archivos

| Archivo | Rol |
|---------|-----|
| `include/omsi/tt_types.hpp` | Estructuras (TrackEntry 0x28, Trip, StnLink, …) |
| `include/omsi/tt_loader.hpp` | load/save de .ttr .ttp .ttl Busstops.cfg StnLinks.cfg |
| `include/omsi/tt_man.hpp`    | TimeTableMan (selección, modos, CRUD, validación) |
| `src/tt/tt_loader.cpp`       | Parsers + writers con headers exactos OMSI 2.3.004 |
| `src/tt/tt_man.cpp`          | Lógica de edición y validate_all / SearchNextErroneous |
| `docs/REVERSE_ENGINEERING_Unit121.md` | Mapa de funciones/offsets del decompilado |

## CMake

Añadir `src/tt/tt_loader.cpp` y `src/tt/tt_man.cpp` a las sources del core y el include path de `include/`.


## UI y pruebas
- `include/omsi/tt_editor_ui.hpp` + `src/tt/tt_editor_ui.cpp`: pestaña Tracks y todas sus subventanas (ImGui puro).
- `tests/tt_roundtrip_test.cpp` (formatos y lógica) y `tests/tt_ui_test.cpp` (ImGui sin ventana):
  `g++ -std=c++20 -Iinclude -I<imgui> tests/tt_ui_test.cpp src/tt/*.cpp tests/tt_test_stubs.cpp <imgui core .cpp> -o tt_ui_test`
