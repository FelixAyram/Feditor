# Notice

This program is free software: you can redistribute it and/or modify it under the terms of the
GNU Affero General Public License as published by the Free Software Foundation, version 3 of the
License (see `LICENSE`).

## Derived work

Parts of the vehicle simulation (rigid-body chassis, suspension and damping, tyre grip, steering
geometry, sound mixing, OMSI script semantics) are ported from or based on
**openOMSI** — https://github.com/openOMSI-org/openOMSI — licensed under the GNU AGPL-3.0.
Ported files say so in a header comment.

The editor's drivable bus runs **openOMSI's own vehicle simulation, unmodified**: the
`third_party/openOMSI` git submodule is compiled into `omsi_vehicle_ffi.dll` by
`ffi/omsi_vehicle_ffi` (a thin C ABI, `include/omsi/vehicle/oo_vehicle.h`). Its source is the
openOMSI repository at the pinned submodule commit.

## Third-party components

| Component | Licence |
|---|---|
| GLFW | zlib |
| Dear ImGui | MIT |
| miniaudio | Public domain / MIT-0 |
| miniz | MIT |
| SMAA | MIT |
| ECC (`third_party/ECC`) | MIT |
| Ultralight (optional, `OMSI_ULTRALIGHT_ROOT`) | Proprietary — **not** AGPL-compatible: release builds must be built without it |

### Bundled in the release zip (so nothing else has to be installed)

| Component | Licence |
|---|---|
| Microsoft Visual C++ runtime (`msvcp140*.dll`, `vcruntime140*.dll`) | Microsoft Visual Studio redistributable files |
| Python 3 runtime (frozen tools in `tools\`) | PSF License |
| PyInstaller bootloader | GPL-2.0 with the PyInstaller bootloader exception |
| pygame / SDL2 | LGPL-2.1 / zlib |
| moderngl, glcontext | MIT |
| Pillow | MIT-CMU (HPND) |
| Tcl/Tk | Tcl/Tk license (BSD-style) |

## Not part of this program

OMSI 2 and its content are © MR Software / Aerosoft and are required separately; no OMSI 2 files
are distributed here. Extracted third-party game code (for example `docs/beamng_re/lua_extract/`)
must not be published.
