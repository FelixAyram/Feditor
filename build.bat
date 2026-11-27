@echo off
rem Builds Feditor (Release) into build\Release.
rem Needs: Visual Studio 2022 C++ tools, CMake 3.24+, vcpkg (VCPKG_ROOT) with glfw3 and imgui, Rust (cargo).
setlocal
if "%VCPKG_ROOT%"=="" (
    echo Set VCPKG_ROOT to your vcpkg folder first, e.g.  set VCPKG_ROOT=C:\vcpkg
    exit /b 1
)
if not exist third_party\openOMSI\Cargo.toml (
    echo Fetching the openOMSI submodule...
    git submodule update --init --recursive || exit /b 1
)
rem Feditor patches on top of the pinned openOMSI commit (AI traffic, timetable, key aliases); skipped when already applied
for %%p in (ffi\openomsi_patches\*.patch) do (
    git -C third_party\openOMSI apply --check "%%~fp" 2>nul && git -C third_party\openOMSI apply "%%~fp"
)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows || exit /b 1
cmake --build build --config Release --target omsi_f4_viewer -- /m || exit /b 1
echo.
echo Done: build\Release\omsi_f4_viewer.exe
