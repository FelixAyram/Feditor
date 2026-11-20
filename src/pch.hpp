#pragma once

// Shared prefix for MSVC precompiled headers. Included before each .cpp via CMake,
// so windows.h / STL / DX12 are parsed once per target instead of once per file.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <d3d12.h>
