#include "app/seh_guard.hpp"

#include <cstdio>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#if defined(_WIN32)
static int seh_filter(EXCEPTION_POINTERS* ep) {
    omsi::crash_report::log_stack(ep, "seh_invoke (survived)");
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int seh_invoke(void (*fn)(void*), void* arg) {
#if defined(_WIN32)
    __try {
        fn(arg);
        return 0;
    } __except (seh_filter(GetExceptionInformation())) {
        const unsigned code = static_cast<unsigned>(GetExceptionCode());
        std::fprintf(stderr, "[rails] SEH caught code=0x%08X — operacion abortada, app sigue\n",
                     code);
        std::fflush(stderr);
        return static_cast<int>(code);
    }
#else
    fn(arg);
    return 0;
#endif
}
