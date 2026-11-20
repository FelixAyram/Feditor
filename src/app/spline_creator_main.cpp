#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "app/spline_creator_app.hpp"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    omsi::app::SplineCreatorCli cli{};
    if (!omsi::app::parse_spline_creator_cli(argc, argv, cli)) {
        LocalFree(argv);
        return 2;
    }
    LocalFree(argv);
    return omsi::app::run_spline_creator_app(cli);
}
