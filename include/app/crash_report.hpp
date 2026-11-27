#pragma once

#include <cstddef>
#include <string_view>

namespace omsi::crash_report {

// Install Windows + C++20 fatal-error hooks (call once, as early as possible in main).
// Writes to stderr, logfile (best effort), crash_report.txt, crash_breadcrumbs_*.txt,
// and omsi_f4_viewer_crash_*.dmp next to the .exe.
void install();

// Ring buffer of recent labels (safe from any thread). Dumped automatically on fatal exit.
void note(std::string_view label);

[[nodiscard]] bool abnormal_exit();

// Appends a symbolized stack (from EXCEPTION_POINTERS*) to crash_report.txt. Used by SEH nets that survive.
void log_stack(void* exception_pointers, const char* tag);

// A handle to the calling thread usable from another thread (suspend / context), or nullptr.
[[nodiscard]] void* open_current_thread();
// Suspends `thread`, walks its stack (x64 unwind data), resumes it, and writes the frames as
// "exe+rva" / "module.dll+off" separated by spaces. Empty on failure.
void sample_thread_stack(void* thread, char* out, std::size_t cap);

} // namespace omsi::crash_report
