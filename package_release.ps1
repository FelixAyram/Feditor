# Packs build\Release into dist\Feditor-<version>.zip for a GitHub release (the updater downloads the first
# .zip asset of the latest release and unpacks it over the install folder, so the files sit at the zip's root).
param([string]$BuildDir = "build\Release")
$ErrorActionPreference = "Stop"
$ver = (Select-String -Path "include\omsi\editor_version.hpp" -Pattern 'kEditorVersion = "([^"]+)"').Matches[0].Groups[1].Value
$stage = Join-Path $env:TEMP "feditor_stage_$ver"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
# the program, its DLLs and data folders - never logs, crash dumps or a user's settings
Get-ChildItem $BuildDir -File | Where-Object {
    ($_.Extension -in ".exe", ".dll") -and ($_.Name -notlike "*.locked_*")
} | Copy-Item -Destination $stage
foreach ($d in "assets", "tools", "shaders", "third_party") {
    $p = Join-Path $BuildDir $d
    if (Test-Path $p) { Copy-Item $p -Destination (Join-Path $stage $d) -Recurse }
}
# the source tree's assets too (editor sounds, UI fonts): the build only deploys the spline tool icons
Copy-Item "assets\*" -Destination (Join-Path $stage "assets") -Recurse -Force
# Nothing else to install: the Visual C++ runtime next to the exe (app-local, Microsoft's redistributable files) ...
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory | Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "Visual C++ redist folder not found" }
foreach ($f in "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll") {
    Copy-Item (Join-Path $crt.FullName $f) -Destination $stage
}
# ... and the Python tools frozen with their own Python (build_tools.ps1): no Python on the user's PC
$pyi = "build\pyi\dist"
if (-not (Test-Path "$pyi\spline_creator\spline_creator.exe") -or -not (Test-Path "$pyi\f4_pack_scan\f4_pack_scan.exe")) {
    throw "run build_tools.ps1 first (frozen spline creator / pack scanner missing)"
}
New-Item -ItemType Directory -Path (Join-Path $stage "tools\map_dependency_scanner") -Force | Out-Null
Copy-Item "$pyi\spline_creator" -Destination (Join-Path $stage "tools\spline_creator") -Recurse
Copy-Item "$pyi\f4_pack_scan" -Destination (Join-Path $stage "tools\map_dependency_scanner\f4_pack_scan") -Recurse
Copy-Item LICENSE, NOTICE.md, README.md -Destination $stage
New-Item -ItemType Directory -Path dist -Force | Out-Null
$zip = "dist\Feditor-$ver.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Host "Release package: $zip"
