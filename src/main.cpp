#include "DockApp.h"
#include "FileSearch.h"
#include "SystemTemps.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <ShlObj.h>
#include <Shellapi.h>

#include <cstdlib>
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

// Minidump to %LOCALAPPDATA%\Hoverdock\Crashes so a crash (access
// violation, std::terminate) leaves evidence instead of a silent exit.
void WriteCrashDump(EXCEPTION_POINTERS* exception) noexcept {
    static volatile LONG writing = 0;
    if (InterlockedExchange(&writing, 1) != 0) {
        return;
    }
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) || base == nullptr) {
        return;
    }
    wchar_t directory[MAX_PATH]{};
    swprintf_s(directory, L"%s\\Hoverdock", base);
    CoTaskMemFree(base);
    CreateDirectoryW(directory, nullptr);
    wcscat_s(directory, L"\\Crashes");
    CreateDirectoryW(directory, nullptr);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t path[MAX_PATH]{};
    swprintf_s(path, L"%s\\Dock-%s-%04u%02u%02u-%02u%02u%02u-%lu.dmp", directory, L"" DOCK_VERSION_STRING,
        static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth), static_cast<unsigned>(now.wDay),
        static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond),
        GetCurrentProcessId());
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = exception;
    info.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
        static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
        exception != nullptr ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* exception) {
    WriteCrashDump(exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashDumps() noexcept {
    SetUnhandledExceptionFilter(&CrashFilter);
    std::set_terminate([] {
        WriteCrashDump(nullptr);
        std::abort();
    });
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    InstallCrashDumps();
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
        // Search: elevated MFT reader (run by its scheduled task) and its
        // one-time registration (run via UAC from the Search panel).
        if (HasArg(argc, argv, L"--mft-dump")) {
            LocalFree(argv);
            return RunMftDumpHelper();
        }
        if (argc >= 3 && _wcsicmp(argv[1], L"--register-mft-task") == 0) {
            const std::wstring sid = argv[2];
            LocalFree(argv);
            return RegisterMftTask(sid.c_str());
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

