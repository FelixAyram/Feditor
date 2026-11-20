# Feditor

Editor y visor de mapas para **OMSI 2**, con renderer propio en Direct3D 12: carga los mapas, splines,
objetos `.sco`, terreno y vehículos de OMSI 2, permite editarlos y guardarlos, y además se puede manejar
un bus y ver tráfico IA usando la simulación de [openOMSI](https://github.com/openOMSI-org/openOMSI).

> Feditor necesita una instalación de **OMSI 2** (no incluye contenido de OMSI).

## Descargar

- **Programa listo para usar:** en [Releases](https://github.com/FelixAyram/Feditor/releases) bajá el
  `.zip` de la última versión, descomprimilo y abrí `omsi_f4_viewer.exe`.
- **Código fuente:** `git clone --recursive https://github.com/FelixAyram/Feditor.git`

Al abrirse, Feditor revisa si hay una versión nueva y pregunta si querés actualizar (se descarga,
se cierra y vuelve a abrirse). Para desactivarlo: variable de entorno `FEDITOR_NO_UPDATE_CHECK=1`.

## Requisitos

- Windows 10/11 de 64 bits, GPU con **Direct3D 12**.
- [Microsoft Visual C++ Redistributable 2015-2022 (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe).
- OMSI 2 instalado (el editor pide la carpeta la primera vez).

## Compilar desde el código

Necesitás:

- Visual Studio 2022 (o Build Tools) con "Desarrollo de escritorio con C++".
- [CMake](https://cmake.org/) 3.24 o más nuevo.
- [vcpkg](https://github.com/microsoft/vcpkg) con `glfw3` e `imgui[glfw-binding,dx12-binding,win32-binding]` (x64-windows).
- [Rust](https://rustup.rs/) (cargo), para la simulación de vehículos de openOMSI.

```bat
git clone --recursive https://github.com/FelixAyram/Feditor.git
cd Feditor
set VCPKG_ROOT=C:\ruta\a\vcpkg
build.bat
```

El ejecutable queda en `build\Release\omsi_f4_viewer.exe`. `package_release.ps1` arma el `.zip`
de un release a partir de esa carpeta.

## Controles básicos

- Cámara: botón derecho girar, rueda zoom, botón medio desplazar.
- **F7**: modo peatón editor (la mira selecciona y coloca objetos) → peatón solo → salir.
  En modo peatón **E** muestra el cursor para usar la interfaz.
- Vista → Ayudas del editor / Cielo y tiempo / Gráficos (sombras, distancia de sombras, niebla) / Tráfico.

## Licencia

Feditor es software libre bajo la **GNU Affero General Public License v3.0** (ver `LICENSE`).
Usa openOMSI (AGPL-3.0) como submódulo; componentes de terceros y sus licencias en `NOTICE.md`.
OMSI y OMSI 2 son marcas de sus respectivos dueños; este proyecto no está afiliado a ellos.
