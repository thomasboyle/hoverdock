#include "DockApp.h"
#include "SystemTemps.h"

#include <Windows.h>
#include <Shellapi.h>

#include <exception>
#include <string>

namespace {

bool HasArg(int argc, LPWSTR* argv, const wchar_t* flag) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        if ((HasArg(argc, argv, L"--cpu-sensor") || HasArg(argc, argv, L"--run"))) {
            LocalFree(argv);
            return RunCpuSensorWorker();
        }
        if (HasArg(argc, argv, L"--install-cpu-sensor")) {
            LocalFree(argv);
            return InstallCpuSensorTask();
        }
        LocalFree(argv);
    }

    try {
        DockApp app(instance);
        return app.Run();
    } catch (const std::exception& exception) {
        const std::string narrow(exception.what());
        const std::wstring message(narrow.begin(), narrow.end());
        MessageBoxW(nullptr, message.c_str(), L"Liquid Glass Dock", MB_ICONERROR | MB_OK);
        return 1;
    }
}

