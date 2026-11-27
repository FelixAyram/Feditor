# Feditor — editor de mapas para OMSI 2, **gratis** y de código abierto

**Feditor** es un **editor de mapas para OMSI 2** (*OMSI 2 map editor*) y visor 3D, **100 % gratis**,
sin publicidad, sin cuentas y de código abierto. Abre los mapas de OMSI 2 tal como son (tiles `.map`,
splines `.sli`, objetos `.sco`, terreno, texturas, entrypoints, tráfico), los muestra con un renderer
moderno en **DirectX 12** pensado para **60 fps**, y te deja editarlos, recorrerlos a pie, manejar un bus
con la simulación de [openOMSI](https://github.com/openOMSI-org/openOMSI) y **construir mapas en
cooperativo con amigos**.

> 💸 **Feditor es gratis y siempre lo va a ser.** Si alguien te lo vende, te están estafando: la única
> descarga oficial está en [Releases](https://github.com/FelixAyram/Feditor/releases).

**[⬇️ Descargar la última versión](https://github.com/FelixAyram/Feditor/releases/latest)** ·
**[🌐 Página oficial](https://felixayram.github.io/Feditor/)** ·
[English below](#english) · [Preguntas frecuentes](#preguntas-frecuentes) ·
[Problemas comunes](#problemas-comunes)

---

## Índice

- [Por qué Feditor](#por-qué-feditor)
- [Descargar e instalar (30 segundos)](#descargar-e-instalar-30-segundos)
- [Requisitos](#requisitos)
- [Primer uso](#primer-uso)
- [Controles](#controles)
- [Modo peatón y editar caminando](#modo-peatón-y-editar-caminando)
- [Manejar el bus](#manejar-el-bus)
- [Modo cooperativo (construir con amigos)](#modo-cooperativo-construir-con-amigos)
- [Herramientas](#herramientas)
- [Rendimiento: cómo llegar a 60 fps](#rendimiento-cómo-llegar-a-60-fps)
- [Actualizaciones automáticas](#actualizaciones-automáticas)
- [Preguntas frecuentes](#preguntas-frecuentes)
- [Problemas comunes](#problemas-comunes)
- [Compilar desde el código](#compilar-desde-el-código)
- [Licencia y créditos](#licencia-y-créditos)
- [English](#english)

## Por qué Feditor

| | Feditor | Editor original de OMSI 2 |
|---|---|---|
| Precio | **Gratis** | Incluido con el juego |
| Gráficos | DirectX 12, sombras en cascada, SMAA, niebla, cielo y hora del día | DirectX 9 |
| Rendimiento | Multihilo, carga por streaming, objetivo 60 fps con varios tiles a la vista | Un hilo |
| Cooperativo | **Sí**: varias personas editan el mismo mapa a la vez (LAN o internet) | No |
| Caminar por el mapa | Sí, y editar caminando (mira en el centro de la pantalla) | No |
| Manejar un bus | Sí (simulación openOMSI), con tráfico IA y autos estacionados | Solo en el juego |
| Empaquetar un mapa en `.zip` con sus dependencias | Sí, excluyendo lo que ya viene con OMSI 2 y sus DLC de Steam | No |
| Código abierto | Sí (AGPL-3.0) | No |

Funciones principales:

- **Editor de mapas OMSI 2 completo**: mover, rotar, copiar, borrar y crear objetos `.sco` y splines
  `.sli`, editar tiles, terreno, entrypoints, paths, semáforos y car parks; deshacer (**Ctrl+Z**) y
  guardar (**Ctrl+S**).
- **Visor rápido**: carga por streaming en segundo plano, LODs, recorte por tamaño en pantalla, texturas
  que se suben a la GPU sin congelar el editor.
- **Info de cada objeto**: al seleccionar un `.sco` ves vértices, texturas, LODs y archivos que usa.
- **Tráfico IA y autos estacionados**: hacé click en un auto IA para ver sus detalles o borrarlo; click en
  un auto estacionado para seleccionar su car park. La opción **Car park real** alterna entre los autos de
  verdad y las cajas de edición.
- **Cielo y tiempo**: hora del día, "el tiempo corre", estaciones, niebla on/off, distancia de sombras.
- **Creador de splines** y **empaquetador de mapas** incluidos, sin instalar nada aparte.
- **Interfaz en español e inglés** (elige el idioma de tu Windows la primera vez).
- **Se actualiza solo**: al abrirlo te avisa si hay versión nueva.

## Descargar e instalar (30 segundos)

1. Entrá a **[Releases](https://github.com/FelixAyram/Feditor/releases/latest)** y bajá
   `Feditor-<versión>.zip` (no hace falta bajar "Source code").
2. Descomprimilo en una carpeta tuya, por ejemplo `C:\Juegos\Feditor` (evitá `Archivos de programa`).
3. Abrí **`omsi_f4_viewer.exe`**.

**No hay que instalar nada más**: el `.zip` ya trae todo lo que necesita (runtime de Visual C++,
shaders, fuentes, el creador de splines y el empaquetador con su propio Python). No toca el registro ni
tu instalación de OMSI 2 hasta que guardes cambios en un mapa.

Si Windows SmartScreen muestra "Windows protegió su PC", tocá **Más información → Ejecutar de todas
formas** (pasa con cualquier programa nuevo que no está firmado con un certificado pago).

## Requisitos

- **Windows 10 u 11 de 64 bits.**
- Cualquier placa de video de clase **DirectX 11** o más nueva con driver de Windows 10/11 (NVIDIA GTX 400+,
  AMD HD 7000 / RX, Intel HD 4400+). Feditor elige solo el backend: **DirectX 12** → **Vulkan**
  (vkd3d-proton) → **WARP** (por software, sin placa). Ver [Backends gráficos](#backends-gráficos).
- **OMSI 2** instalado (Steam u otra versión). Feditor no incluye ningún archivo de OMSI 2.
- Recomendado: 8 GB de RAM, SSD, 4 núcleos o más.

## Primer uso

1. La primera vez Feditor te pide la **carpeta de OMSI 2** (la que tiene `Omsi.exe`, por ejemplo
   `C:\Program Files (x86)\Steam\steamapps\common\OMSI 2`). Se guarda y no te la vuelve a pedir.
2. Elegí un **mapa** de la lista y un **entrypoint** (lugar de inicio).
3. Elegí si querés **editar** o solo **ver** el mapa (modo solo lectura: no se puede modificar nada).
4. Al editar, Feditor trabaja sobre una copia del mapa y guarda con **Ctrl+S**.

Consejo: antes de editar un mapa que te importa, hacé una copia de su carpeta en `OMSI 2\maps`.

## Controles

| Acción | Tecla / mouse |
|---|---|
| Girar la cámara | Botón derecho + arrastrar |
| Acercar / alejar | Rueda del mouse |
| Desplazar | Botón del medio + arrastrar |
| Seleccionar | Click izquierdo |
| Deshacer | **Ctrl+Z** |
| Guardar | **Ctrl+S** |
| Modo peatón (ciclo) | **F7** |
| Mostrar el cursor en modo peatón | **E** |
| Lista completa de atajos | Menú **Herramientas → Atajos…** |

Menús principales:

- **Archivo**: cargar mapa (`global.cfg`), abrir un tile `.map`, **Empaquetar mapa (.zip)…**.
- **Herramientas**: creador de splines, editar mesh `.o3d`, bake de SLI a `.sco`, entrypoints,
  opciones del editor, atajos.
- **Vista**: *Ayudas del editor* (flechas de splines/paths, car park real…), *Cielo y tiempo*,
  *Gráficos* (sombras, distancia de sombras, niebla, límite de texturas), *Tráfico* (IA y autos
  estacionados).

## Modo peatón y editar caminando

**F7** alterna: **peatón editor → peatón solo → salir**.

- **WASD** caminar, **Shift** correr, **Espacio** saltar, el mouse mira.
- En **peatón editor** aparece una mira en el centro: **click** selecciona un objeto y lo "lleva" con
  la mira, **click** otra vez o **click derecho** lo suelta, **R** rota, **Supr** borra, **N** crea el
  objeto elegido en el catálogo donde apuntás, **Ctrl+Z** deshace.
- **E** muestra el cursor para usar los paneles sin salir del modo peatón.
- Abajo de la pantalla siempre ves un recordatorio de las teclas del modo en el que estás.

## Manejar el bus

- Elegí un bus de tu OMSI 2 y aparecelo en el mapa; la física, motor, caja y sonidos son los de
  **openOMSI**.
- Con el bus **detenido (menos de 5 km/h)** apretá **F7** para **bajarte**. A pie, en modo peatón
  editor, podés hacer **click en el bus para moverlo como un objeto** (rueda del mouse = girarlo,
  click o click derecho = soltarlo).
- Parado al lado del bus, **F7** te vuelve a **subir** al asiento del conductor.

## Modo cooperativo (construir con amigos)

Varias personas editan **el mismo mapa al mismo tiempo**, cada una en su PC, y ven en vivo lo que
hacen las demás.

1. La primera vez Feditor te explica el modo cooperativo y te pide permiso de red. Al aceptar, Windows
   pregunta si permitís el acceso del programa a la red: elegí **Permitir**.
2. **Anfitrión**: panel lateral → pestaña **Sesión** → **Crear sala** → **Invitar**.
3. Elegí cómo se conectan tus amigos:
   - **Solo red local**: misma casa / LAN, o una VPN tipo Radmin, ZeroTier o Tailscale.
   - **Online (internet)**: amigos en otras casas. Feditor arma solo un túnel seguro de Cloudflare
     (sin abrir puertos en el router); la primera vez tarda unos segundos mientras se prepara.
4. Tocá **Copiar código** y pasáselo a tus amigos.
5. **Invitados**: pestaña **Sesión** → pegan el código en **Unirse con un código** → **Unirse**.

Todos necesitan el mismo mapa y los mismos objetos en su OMSI 2 (usá **Empaquetar mapa** para pasarlos).

## Herramientas

- **Creador de splines** (*Herramientas → Creador de splines…*): perfil, texturas y vista previa 3D de
  un `.sli` nuevo o existente; al guardar aparece en el catálogo del editor.
- **Empaquetar mapa** (*Archivo → Empaquetar mapa (.zip)…*): junta el mapa y **todo lo que usa**
  (objetos, mallas, texturas, scripts, sonidos, splines, vehículos IA) en un `.zip` listo para
  compartir. Puede **excluir el contenido original y los DLC de Steam** para que el paquete sea chico y
  legal de compartir.
- **Editar mesh .o3d** y **bake SLI → .sco**.

## Rendimiento: cómo llegar a 60 fps

En *Vista → Gráficos* y *Herramientas → Opciones editor…*:

- **Distancia de tiles**: 1–2 tiles alcanza para editar; 3–4 para recorrer.
- **Tamaño mínimo en pantalla** (por defecto 0 %, se dibuja todo): subilo (por ejemplo a 0,5 %) para
  que no se dibujen objetos chiquitos lejanos.
- **Distancia de sombras**: bajala si la GPU no da más; **Sombras solares (CSM)** se pueden apagar.
- **Límite de texturas 256 px** para placas con poca VRAM.
- **Tráfico IA**: menos autos = más fps.
- **El tiempo corre** recalcula las sombras con el sol; si molesta, dejá la hora fija.

## Actualizaciones automáticas

Al abrirse, Feditor revisa si hay una versión nueva en GitHub y te pregunta si querés actualizar. Si
aceptás, la descarga, se cierra, reemplaza sus archivos y vuelve a abrirse solo. Tus ajustes y mapas no
se tocan. Para desactivar la consulta: variable de entorno `FEDITOR_NO_UPDATE_CHECK=1`.

## Preguntas frecuentes

**¿Feditor es gratis?**
Sí. Gratis, sin publicidad, sin cuenta y de código abierto. No hay versión paga.

**¿Necesito OMSI 2?**
Sí: Feditor usa los archivos de tu instalación de OMSI 2. No incluye ni descarga contenido de OMSI.

**¿Los mapas que edito siguen funcionando en OMSI 2?**
Sí, Feditor guarda en los mismos formatos de OMSI 2 (`.map`, `.sli`, `.sco`, `global.cfg`…).

**¿Reemplaza al editor de OMSI 2?**
Lo complementa: podés usar los dos sobre el mismo mapa.

**¿Qué tiene que ver con openOMSI?**
Feditor usa la simulación de vehículos de [openOMSI](https://github.com/openOMSI-org/openOMSI)
(proyecto libre AGPL-3.0) para manejar buses dentro del editor. Son proyectos distintos.

**¿Funciona en Linux / Mac?**
No de forma oficial (usa DirectX 12). En Linux puede andar con Proton/Wine + VKD3D, sin garantías.

**¿Funciona con OpenGL o DirectX 11?**
Las placas DirectX 11 funcionan: el renderer pide DirectX 12 a *feature level* 11_0, que soporta toda
placa DX11 con driver de Windows 10/11. Si DirectX 12 falla, Feditor se reinicia solo con **Vulkan**
(vkd3d-proton) y, si tampoco hay, con **WARP** (render por software de Windows, lento pero anda en
cualquier PC, máquina virtual o escritorio remoto; también es lo que usan las placas que solo tienen
OpenGL).

**¿Tiene virus?**
No. El código está acá completo y el `.zip` se arma desde este mismo código. Algunos antivirus
desconfían de programas nuevos sin firma digital; podés subir el `.zip` a
[VirusTotal](https://www.virustotal.com/) para comprobarlo.

## Problemas comunes

| Problema | Solución |
|---|---|
| No abre / se cierra al iniciar | Descomprimí **todo** el `.zip` (no lo abras desde adentro del zip) en una carpeta sin permisos especiales. Mirá `logfile.log` y `crash_report.txt` al lado del `.exe`. |
| "Windows protegió su PC" | **Más información → Ejecutar de todas formas.** |
| Pide la carpeta de OMSI 2 cada vez | La carpeta del editor tiene que permitir escritura (no la pongas en `Archivos de programa`). |
| Texturas blancas | Esperá unos segundos a que terminen de subir; si queda alguna, *Recargar*. |
| Pocos fps | Ver [Rendimiento](#rendimiento-cómo-llegar-a-60-fps). Actualizá los drivers de video. |
| Mi amigo no se puede unir | Usá **Online (internet)** si no están en la misma red; revisá que los dos aceptaron el permiso de red. |
| Mi placa no tiene DirectX 12 | Feditor pasa solo a Vulkan o a WARP (el título muestra el backend). Elegilo en el combo **Gráficos** del launcher, o `omsi_f4_viewer.exe --gfx vulkan` o un archivo `gfx_backend.txt` al lado del `.exe` con `d3d12`, `vulkan` o `warp`. |

### Backends gráficos

| Backend | Cuándo | Cómo forzarlo |
|---|---|---|
| `d3d12` | Por defecto: cualquier placa DX11/DX12 con driver actual | `--gfx d3d12` |
| `vulkan` | Driver DirectX 12 roto pero Vulkan 1.3 OK (usa [vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) + `dxgi.dll` de [DXVK](https://github.com/doitsujin/dxvk), carpeta `vkd3d\`) | `--gfx vulkan` |
| `warp` | Sin placa útil, solo OpenGL, VM, escritorio remoto | `--gfx warp` |

También sirve la variable de entorno `FEDITOR_GFX`. El log (`GFX_BACKEND` / `GFX_DEVICE`) dice qué
backend y qué placa se usaron.

¿Encontraste un error? Abrí un [issue](https://github.com/FelixAyram/Feditor/issues) con el
`logfile.log` y, si hubo, el `crash_report.txt`.

## Compilar desde el código

Necesitás:

- Visual Studio 2022 (o Build Tools) con "Desarrollo de escritorio con C++".
- [CMake](https://cmake.org/) 3.24 o más nuevo.
- [vcpkg](https://github.com/microsoft/vcpkg) con `glfw3` e
  `imgui[glfw-binding,dx12-binding,win32-binding]` (x64-windows).
- [Rust](https://rustup.rs/) (cargo), para la simulación de vehículos de openOMSI.
- Para armar el `.zip` de un release: Python 3 con `pip install pyinstaller pygame moderngl Pillow`.

```bat
git clone --recursive https://github.com/FelixAyram/Feditor.git
cd Feditor
set VCPKG_ROOT=C:\ruta\a\vcpkg
build.bat
```

El ejecutable queda en `build\Release\omsi_f4_viewer.exe`. Para un release:
`build_tools.ps1` (congela el creador de splines y el empaquetador) y después `package_release.ps1`
(arma `dist\Feditor-<versión>.zip` con todo incluido).

## Licencia y créditos

Feditor es **software libre y gratuito** bajo la **GNU Affero General Public License v3.0**
(ver `LICENSE`). Usa openOMSI (AGPL-3.0) como submódulo; los componentes de terceros y sus licencias
están en `NOTICE.md`.

OMSI y OMSI 2 son marcas de MR Software / Aerosoft. Feditor es un proyecto independiente de la
comunidad: no está afiliado, aprobado ni patrocinado por ellos.

Hecho por **Felix** ([@FelixAyram](https://github.com/FelixAyram)). Si te sirve, dejale una ⭐ al
repositorio: ayuda a que más gente de la comunidad de OMSI lo encuentre.

## Enlaces

- **Página oficial / Official site:** <https://felixayram.github.io/Feditor/>
- **Descargas:** <https://github.com/FelixAyram/Feditor/releases>
- **Problemas:** <https://github.com/FelixAyram/Feditor/issues>
- **openOMSI:** <https://github.com/openOMSI-org/openOMSI>

---

## English

**Feditor is a free, open-source map editor and 3D viewer for OMSI 2** (*OMSI 2 map editor*,
*OMSI editor*, *OMSI 2 Karteneditor*). It opens OMSI 2 maps as they are (`.map` tiles, `.sli`
splines, `.sco` objects, terrain, entrypoints, traffic) with a modern **DirectX 12** renderer aiming
at **60 fps**, lets you edit and save them, walk around and edit on foot, drive a bus using the
[openOMSI](https://github.com/openOMSI-org/openOMSI) vehicle simulation, and **build maps together
with friends in co-op**.

> 💸 **Feditor is free and always will be.** No ads, no account, no paid edition. The only official
> download is on the [Releases](https://github.com/FelixAyram/Feditor/releases) page.

### Download

1. Get `Feditor-<version>.zip` from **[Releases](https://github.com/FelixAyram/Feditor/releases/latest)**.
2. Unzip it anywhere you can write to (e.g. `C:\Games\Feditor`).
3. Run **`omsi_f4_viewer.exe`**. Nothing else to install — the zip ships the Visual C++ runtime and
   the spline creator / map packer with their own Python.

Requirements: Windows 10/11 64-bit, any **DirectX 11-class or newer** GPU (the editor picks DirectX 12,
then Vulkan through vkd3d-proton, then WARP software rendering; force one with `--gfx d3d12|vulkan|warp`) and an **OMSI 2** installation (no OMSI files
are included). On first start pick your OMSI 2 folder (the one with `Omsi.exe`), then a map and an
entrypoint. The UI is available in **English and Spanish** and follows your Windows language.

### Highlights

- Full OMSI 2 map editing: objects, splines, tiles, terrain, entrypoints, paths, traffic lights,
  car parks; undo (**Ctrl+Z**) and save (**Ctrl+S**).
- Fast multithreaded streaming renderer: cascaded shadows, SMAA, fog, sky and time of day.
- **Co-op building**: several people edit the same map at once over LAN or the internet (automatic
  Cloudflare tunnel, no port forwarding). Side panel → **Session** → **Create room** → **Invite** →
  choose *Local network* or *Online* → copy the code; guests paste it in **Join with a code**.
- **Walk mode** (**F7**): WASD, Shift to run, Space to jump; in *walk-edit* the crosshair picks and
  carries objects (click / right-click to drop, **R** rotate, **Del** delete, **N** spawn, **E** shows
  the cursor).
- **Drive a bus** with openOMSI physics; stop (< 5 km/h) and press **F7** to get off, carry the bus like
  an object, **F7** next to it to get back in. AI traffic and parked cars included.
- **Map packer**: zips a map with all its dependencies, optionally excluding the base game and Steam DLC
  content.
- **Auto-update**: asks on start when a new version is out, then downloads, installs and restarts.

### FAQ

- **Is it free?** Yes — free, open source (AGPL-3.0), no ads.
- **Do I need OMSI 2?** Yes, Feditor reads your OMSI 2 installation.
- **Do edited maps still work in OMSI 2?** Yes, it saves the original OMSI 2 file formats.
- **Is it related to openOMSI?** It uses openOMSI's vehicle simulation to drive buses; they are
  separate projects.

Bug reports: [Issues](https://github.com/FelixAyram/Feditor/issues) (attach `logfile.log`, and
`crash_report.txt` if there is one). OMSI and OMSI 2 are trademarks of MR Software / Aerosoft;
Feditor is an independent community project, not affiliated with or endorsed by them.

<!-- keywords: OMSI 2 map editor, OMSI editor, OMSI 2 editor, editor de mapas OMSI 2, editor OMSI gratis,
OMSI 2 Karteneditor, OMSI Map Editor free, openOMSI, OMSI bus simulator, OMSI 2 tools, OMSI modding,
OMSI 2 mapping, splines sli, sco objects, Feditor -->
