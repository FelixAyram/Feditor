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
Copy-Item LICENSE, NOTICE.md, README.md -Destination $stage
New-Item -ItemType Directory -Path dist -Force | Out-Null
$zip = "dist\Feditor-$ver.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Write-Host "Release package: $zip"
