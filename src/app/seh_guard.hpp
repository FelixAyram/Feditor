#pragma once
// Guard SEH (access violation, etc.) para que el viewer no muera en silencio.
// Uso: seh_invoke(+[](void* p){ ... }, ctx);
// El cuerpo __try vive en seh_invoke.cpp (no inline: /EHsc + objetos C++ en el TU).

#include "app/crash_report.hpp"

int seh_invoke(void (*fn)(void*), void* arg);

inline void omsi_install_seh_filter() { omsi::crash_report::install(); }
