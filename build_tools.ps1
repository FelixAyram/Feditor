# Freezes the editor's Python tools with PyInstaller so the release needs no Python on the user's PC:
#   build\pyi\dist\spline_creator\spline_creator.exe   (Herramientas > Creador de splines)
#   build\pyi\dist\f4_pack_scan\f4_pack_scan.exe       (Archivo > Empaquetar mapa)
# Needs (only on the machine that builds the release): py -3 -m pip install pyinstaller pygame moderngl Pillow
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$common = @("--noconfirm", "--clean", "--onedir", "--distpath", "build\pyi\dist", "--workpath", "build\pyi\work",
            "--specpath", "build\pyi")
py -3 -m PyInstaller @common --windowed --name spline_creator --paths tools\spline_creator `
    --collect-all moderngl --collect-all glcontext tools\spline_creator\app.py
if ($LASTEXITCODE -ne 0) { throw "PyInstaller spline_creator failed" }
py -3 -m PyInstaller @common --console --name f4_pack_scan --paths tools\map_dependency_scanner `
    --hidden-import map_dependency_scanner --hidden-import sim_streetsign_engine tools\map_dependency_scanner\f4_pack_scan.py
if ($LASTEXITCODE -ne 0) { throw "PyInstaller f4_pack_scan failed" }
Write-Host "Frozen tools: build\pyi\dist"
