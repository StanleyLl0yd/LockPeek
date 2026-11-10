#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "app.h"
#include "scanner.h"
#include <shellapi.h>
#include <string>
#pragma comment(lib, "shell32.lib")

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // No process elevation or persistent services by default.
    // Per-monitor v2 DPI awareness is available on supported Windows 10/11.
    using SetDpiContext = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto* setDpi = reinterpret_cast<SetDpiContext>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (!setDpi || !setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring path;
    bool worker = false;
    if (argv) {
        if (argc == 3 && std::wstring(argv[1]) == L"--scan-worker") {
            worker = true;
            path = argv[2];
        } else if (argc == 2) {
            path = argv[1];
        }
        LocalFree(argv);
    }
    if (worker) return lockpeek::RunScanWorker(path);
    return lockpeek::RunApplication(instance, path, show);
}
