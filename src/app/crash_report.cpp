#include "app/crash_report.hpp"

#include "omsi/logger.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <dbghelp.h>
#  include <eh.h>
#endif

namespace omsi::crash_report {
namespace {

std::atomic<int> g_abnormal_exit{0};
std::atomic<int> g_installed{0};
#if defined(_WIN32)
DWORD g_main_tid = 0; // thread that called install() (the editor main thread)
#endif

constexpr std::size_t k_max_breadcrumbs = 128;
std::mutex g_breadcrumb_mu;
std::deque<std::string> g_breadcrumbs;

#if defined(_WIN32)

bool exe_directory(char* out, DWORD out_chars) {
    const DWORD n = GetModuleFileNameA(nullptr, out, out_chars);
    if (n == 0 || n >= out_chars) return false;
    char* slash = out;
    for (char* p = out; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    *slash = '\0';
    return true;
}

void append_crash_report_line(const char* line) {
    char dir[MAX_PATH]{};
    if (!exe_directory(dir, MAX_PATH)) return;
    char path[MAX_PATH]{};
    std::snprintf(path, sizeof(path), "%s\\crash_report.txt", dir);
    HANDLE hf = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return;
    DWORD ignored = 0;
    WriteFile(hf, line, static_cast<DWORD>(std::strlen(line)), &ignored, nullptr);
    const char nl[] = "\r\n";
    WriteFile(hf, nl, 2, &ignored, nullptr);
    CloseHandle(hf);
}

void log_fatal(const char* stderr_line, const char* log_line = nullptr) {
    g_abnormal_exit.store(1);
    std::fprintf(stderr, "%s\n", stderr_line);
    std::fflush(stderr);
    append_crash_report_line(stderr_line);
    try {
        if (log_line)
            omsi::log::error(log_line);
        else
            omsi::log::error(stderr_line);
        omsi::log::flush();
    } catch (...) {
    }
}

const char* seh_code_label(const DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case 0xC0000409u: return "STACK_BUFFER_OVERRUN (/GS fast-fail)";
    case 0xC00000FDu: return "STACK_OVERFLOW";
    case 0xC0000017u: return "OUT_OF_MEMORY";
    case 0xC000001Du: return "ILLEGAL_INSTRUCTION";
    case 0xC0000094u: return "INT_DIVIDE_BY_ZERO";
    case 0xC0000096u: return "PRIVILEGED_INSTRUCTION";
    case 0xE06D7363u: return "CPP_EH_EXCEPTION";
    default: return "";
    }
}

void write_minidump(EXCEPTION_POINTERS* ep) {
    char dir[MAX_PATH]{};
    if (!exe_directory(dir, MAX_PATH)) return;
    char dump_path[MAX_PATH]{};
    std::snprintf(dump_path, sizeof(dump_path), "%s\\omsi_f4_viewer_crash_%lu_%lu.dmp", dir,
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long>(GetTickCount()));
    HANDLE hf = CreateFileA(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
    if (hf == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    const BOOL ok =
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hf,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo |
                                                     MiniDumpWithIndirectlyReferencedMemory |
                                                     MiniDumpWithFullMemoryInfo),
                          ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(hf);
    if (ok) {
        char msg[512];
        std::snprintf(msg, sizeof(msg), "minidump: %s", dump_path);
        log_fatal(msg, msg);
    }
}

void dump_breadcrumbs() {
    char dir[MAX_PATH]{};
    if (!exe_directory(dir, MAX_PATH)) return;
    char path[MAX_PATH]{};
    std::snprintf(path, sizeof(path), "%s\\crash_breadcrumbs_%lu_%lu.txt", dir,
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long>(GetTickCount()));
    HANDLE hf = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return;
    std::deque<std::string> copy;
    {
        std::lock_guard<std::mutex> lock(g_breadcrumb_mu);
        copy = g_breadcrumbs;
    }
    DWORD ignored = 0;
    const char hdr[] = "=== crash breadcrumbs (oldest first) ===\r\n";
    WriteFile(hf, hdr, static_cast<DWORD>(sizeof(hdr) - 1), &ignored, nullptr);
    for (const auto& line : copy) {
        WriteFile(hf, line.data(), static_cast<DWORD>(line.size()), &ignored, nullptr);
        const char nl[] = "\r\n";
        WriteFile(hf, nl, 2, &ignored, nullptr);
    }
    CloseHandle(hf);
    char msg[512];
    std::snprintf(msg, sizeof(msg), "breadcrumbs: %s (n=%zu)", path, copy.size());
    std::fprintf(stderr, "%s\n", msg);
    std::fflush(stderr);
    append_crash_report_line(msg);
}

// Symbolized call stack of the faulting thread (function + file:line from the PDB next to the exe),
// appended to crash_report.txt. Lets a crash be diagnosed without opening the minidump.
void write_symbolized_stack(EXCEPTION_POINTERS* ep, const char* tag) {
    if (!ep || !ep->ContextRecord) return;
    static std::atomic<int> s_busy{0};
    if (s_busy.exchange(1)) return;
    char dir[MAX_PATH]{};
    exe_directory(dir, MAX_PATH);
    const HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    const BOOL inited = SymInitialize(proc, dir[0] ? dir : nullptr, TRUE);
    char head[160];
    std::snprintf(head, sizeof(head), "--- stack (%s) ---", tag ? tag : "");
    append_crash_report_line(head);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 48; ++i) {
        if (i > 0 && !StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx, nullptr,
                                  SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (sf.AddrPC.Offset == 0) break;
        char line[600];
        const DWORD64 pc = sf.AddrPC.Offset;
        const DWORD64 mod = inited ? SymGetModuleBase64(proc, pc) : 0;
        char symbuf[sizeof(SYMBOL_INFO) + 300]{};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 299;
        DWORD64 disp = 0;
        if (inited && SymFromAddr(proc, pc, &disp, sym)) {
            IMAGEHLP_LINE64 il{};
            il.SizeOfStruct = sizeof(il);
            DWORD ldisp = 0;
            if (SymGetLineFromAddr64(proc, pc, &ldisp, &il))
                std::snprintf(line, sizeof(line), "  #%d %s+0x%llx (%s:%lu) rva=0x%llx", i, sym->Name,
                              static_cast<unsigned long long>(disp), il.FileName,
                              static_cast<unsigned long>(il.LineNumber),
                              static_cast<unsigned long long>(pc - mod));
            else
                std::snprintf(line, sizeof(line), "  #%d %s+0x%llx rva=0x%llx", i, sym->Name,
                              static_cast<unsigned long long>(disp), static_cast<unsigned long long>(pc - mod));
        } else {
            std::snprintf(line, sizeof(line), "  #%d 0x%llx (no symbol) rva=0x%llx", i,
                          static_cast<unsigned long long>(pc), static_cast<unsigned long long>(pc - mod));
        }
        append_crash_report_line(line);
    }
    if (inited) SymCleanup(proc);
    s_busy.store(0);
}

// MSVC C++ exception (0xE06D7363): x64 ThrowInfo is image-relative. Log every catchable type name and, when one of
// them is std::exception, its what(). Before this the report only said "uncaught C++ exception".
void describe_cpp_exception(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || ep->ExceptionRecord->NumberParameters < 4) return;
    const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
    const auto* obj = reinterpret_cast<const void*>(info[1]);
    const auto* throw_info = reinterpret_cast<const std::uint8_t*>(info[2]);
    const auto base = static_cast<std::uintptr_t>(info[3]);
    if (!throw_info || !base) return;
    char line[700]{};
    std::size_t len = 0;
    bool is_std_exception = false;
    __try {
        const auto cta_rva = *reinterpret_cast<const std::int32_t*>(throw_info + 12);
        const auto* cta = reinterpret_cast<const std::int32_t*>(base + static_cast<std::uint32_t>(cta_rva));
        const int n = (std::min)(cta[0], 8);
        len = static_cast<std::size_t>(std::snprintf(line, sizeof(line), "C++ exception types:"));
        for (int i = 0; i < n && len < sizeof(line) - 80; ++i) {
            const auto* ct = reinterpret_cast<const std::uint8_t*>(base + static_cast<std::uint32_t>(cta[1 + i]));
            const auto type_rva = *reinterpret_cast<const std::int32_t*>(ct + 4);
            const char* name = reinterpret_cast<const char*>(base + static_cast<std::uint32_t>(type_rva) + 16);
            if (std::strstr(name, "exception@std@@")) is_std_exception = true;
            len += static_cast<std::size_t>(std::snprintf(line + len, sizeof(line) - len, " %.100s", name));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(line + len, sizeof(line) - len, " <unreadable throw info>");
    }
    append_crash_report_line(line);
    if (is_std_exception && obj) {
        char what_line[600]{};
        __try {
            std::snprintf(what_line, sizeof(what_line), "C++ exception what(): %.500s",
                          static_cast<const std::exception*>(obj)->what());
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::snprintf(what_line, sizeof(what_line), "C++ exception what(): <unreadable>");
        }
        append_crash_report_line(what_line);
        omsi::log::error(what_line);
    }
}

LONG WINAPI unhandled_seh_filter(EXCEPTION_POINTERS* ep) {
    const DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0u;
    static volatile long s_cpp_eh_count = 0;
    if (code == 0xE06D7363u) {
        g_abnormal_exit.store(1);
        const long n = InterlockedIncrement(&s_cpp_eh_count);
        if (n <= 3) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "UNHANDLED C++ EH 0xE06D7363 #%ld (uncaught C++ exception — often "
                          "std::execution::par or noexcept violation)",
                          n);
            if (ep && ep->ExceptionRecord && ep->ExceptionRecord->NumberParameters >= 2) {
                const ULONG_PTR thrown_obj = ep->ExceptionRecord->ExceptionInformation[1];
                char extra[64];
                std::snprintf(extra, sizeof(extra), " thrown_obj=%p", reinterpret_cast<void*>(thrown_obj));
                std::strncat(msg, extra, sizeof(msg) - std::strlen(msg) - 1);
            }
            log_fatal(msg, msg);
            describe_cpp_exception(ep);
            write_symbolized_stack(ep, "cpp_eh");
            dump_breadcrumbs();
            write_minidump(ep);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void* fault_addr = nullptr;
    const char* av_rw = "";
    if (ep && ep->ExceptionRecord && code == EXCEPTION_ACCESS_VIOLATION) {
        const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
        av_rw = info[0] ? "WRITE" : "READ";
        fault_addr = reinterpret_cast<void*>(info[1]);
    }

    const char* label = seh_code_label(code);
    char msg[512];
    if (label[0] != '\0') {
        std::snprintf(msg, sizeof(msg), "FATAL SEH code=0x%08lX (%s)%s%s", static_cast<unsigned long>(code),
                      label, av_rw[0] ? " AV=" : "", av_rw[0] ? av_rw : "");
    } else {
        std::snprintf(msg, sizeof(msg), "FATAL SEH code=0x%08lX%s%s", static_cast<unsigned long>(code),
                      av_rw[0] ? " AV=" : "", av_rw[0] ? av_rw : "");
    }
    if (fault_addr) {
        char extra[64];
        std::snprintf(extra, sizeof(extra), " addr=%p", fault_addr);
        std::strncat(msg, extra, sizeof(msg) - std::strlen(msg) - 1);
    }
    {
        // Which thread faulted: breadcrumbs only describe the main loop, so a fault on a worker thread
        // looked like it came from "post_render". Also report the faulting RIP for the PDB lookup.
        char extra[128];
        const DWORD tid = GetCurrentThreadId();
        std::snprintf(extra, sizeof(extra), " tid=%lu %s rip=%p", static_cast<unsigned long>(tid),
                      tid == g_main_tid ? "MAIN_THREAD" : "WORKER_THREAD",
                      (ep && ep->ContextRecord) ? reinterpret_cast<void*>(ep->ContextRecord->Rip) : nullptr);
        std::strncat(msg, extra, sizeof(msg) - std::strlen(msg) - 1);
    }
    log_fatal(msg, msg);
    write_symbolized_stack(ep, "fatal");
    dump_breadcrumbs();
    write_minidump(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

void on_terminate() noexcept {
    g_abnormal_exit.store(1);
    const char* detail = "std::terminate (no active std::exception_ptr)";
    if (const auto eptr = std::current_exception()) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            static thread_local char buf[512];
            std::snprintf(buf, sizeof(buf), "std::terminate: std::exception: %s", e.what());
            detail = buf;
        } catch (...) {
            detail = "std::terminate: unknown exception type";
        }
    }
    log_fatal(detail, detail);
    dump_breadcrumbs();
    std::abort();
}

void on_invalid_parameter(const wchar_t* expression, const wchar_t* function, const wchar_t* file,
                          unsigned int line, uintptr_t /*reserved*/) {
    char msg[768];
    std::snprintf(msg, sizeof(msg), "CRT invalid_parameter expr=%ls func=%ls file=%ls line=%u",
                  expression ? expression : L"?", function ? function : L"?", file ? file : L"?", line);
    log_fatal(msg, msg);
    std::abort();
}

void on_purecall() {
    log_fatal("CRT pure virtual function call", "CRT pure virtual function call");
    std::abort();
}

void console_hold_on_exit() {
    if (g_abnormal_exit.load()) return;
    if (GetConsoleWindow() == nullptr) return;
    std::fprintf(stderr, "\n[omsi_f4_viewer] fin. Enter para cerrar la consola...\n");
    std::fflush(stderr);
    try {
        omsi::log::flush();
    } catch (...) {
    }
    (void)std::getchar();
}

#endif // _WIN32

} // namespace

void note(std::string_view label) {
    if (label.empty()) return;
    std::lock_guard<std::mutex> lock(g_breadcrumb_mu);
    g_breadcrumbs.push_back(std::string(label));
    while (g_breadcrumbs.size() > k_max_breadcrumbs) g_breadcrumbs.pop_front();
}

void install() {
    if (g_installed.exchange(1)) return;
#if defined(_WIN32)
    g_main_tid = GetCurrentThreadId();
    SetUnhandledExceptionFilter(unhandled_seh_filter);
    std::set_terminate(on_terminate);
    _set_invalid_parameter_handler(on_invalid_parameter);
    _set_purecall_handler(on_purecall);
    std::atexit(console_hold_on_exit);
    append_crash_report_line("crash_report: hooks installed");
    note("crash_report installed");
#endif
}

bool abnormal_exit() { return g_abnormal_exit.load() != 0; }

void* open_current_thread() {
#if defined(_WIN32)
    return OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                      GetCurrentThreadId());
#else
    return nullptr;
#endif
}

void sample_thread_stack(void* thread, char* out, std::size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
#if defined(_WIN32) && defined(_M_X64)
    if (!thread) return;
    HANDLE h = static_cast<HANDLE>(thread);
    DWORD64 pcs[40];
    int n = 0;
    // nothing that allocates or locks while the thread is suspended (it may hold the heap lock)
    if (SuspendThread(h) == static_cast<DWORD>(-1)) return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(h, &ctx)) {
        while (n < 40 && ctx.Rip) {
            pcs[n++] = ctx.Rip;
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
            if (!fe) { // leaf function: return address on top of the stack
                if (!ctx.Rsp) break;
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            } else {
                void* handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fe, &ctx, &handler_data, &establisher,
                                 nullptr);
            }
        }
    }
    ResumeThread(h);
    const HMODULE exe = GetModuleHandleW(nullptr);
    std::size_t used = 0;
    for (int i = 0; i < n && used + 48 < cap; ++i) {
        HMODULE mod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(pcs[i]), &mod);
        char name[64] = "?";
        if (mod == exe) {
            std::snprintf(name, sizeof(name), "x");
        } else if (mod) {
            char path[MAX_PATH];
            if (GetModuleFileNameA(mod, path, MAX_PATH)) {
                const char* b = path;
                for (const char* p = path; *p; ++p)
                    if (*p == '\\' || *p == '/') b = p + 1;
                std::snprintf(name, sizeof(name), "%.40s", b);
            }
        }
        const unsigned long long off =
            mod ? static_cast<unsigned long long>(pcs[i] - reinterpret_cast<DWORD64>(mod)) : pcs[i];
        const int w = std::snprintf(out + used, cap - used, "%s%s+%llx", i ? " " : "", name, off);
        if (w <= 0) break;
        used += static_cast<std::size_t>(w);
    }
#else
    (void)thread;
#endif
}

void log_stack(void* exception_pointers, const char* tag) {
#if defined(_WIN32)
    write_symbolized_stack(static_cast<EXCEPTION_POINTERS*>(exception_pointers), tag);
#else
    (void)exception_pointers;
    (void)tag;
#endif
}

} // namespace omsi::crash_report
