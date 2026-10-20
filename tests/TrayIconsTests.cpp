// Windows integration test for system tray detection (SystemTray::EnumerateNotifyIcons).
// A child process adds a real notification icon owned by an ordinary hidden
// window (no Electron/Chrome/Qt host class, and with the taskbar hidden its
// XAML button may never be realized); the dock must still list it with its
// executable, tooltip, icon and id.
//   TrayIconsTests.exe          run the test
//   TrayIconsTests.exe --probe  (internal) owns the probe notification icon
#include "SystemTray.h"

#include <Windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr UINT kProbeId = 7;
constexpr wchar_t kProbeTip[] = L"Hoverdock tray probe";
constexpr wchar_t kProbeClass[] = L"HoverdockTrayProbeWindow";
int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

int Probe(bool hidden) {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kProbeClass;
    RegisterClassExW(&windowClass);
    HWND window = CreateWindowExW(0, kProbeClass, L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
        windowClass.hInstance, nullptr);
    if (window == nullptr) {
        return 2;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kProbeId;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | (hidden ? NIF_STATE : 0U);
    data.uCallbackMessage = WM_APP + 9;
    data.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    if (hidden) {
        data.dwState = NIS_HIDDEN;
        data.dwStateMask = NIS_HIDDEN;
    }
    wcscpy_s(data.szTip, kProbeTip);
    if (Shell_NotifyIconW(NIM_ADD, &data) == FALSE) {
        return 3;
    }
    const ULONGLONG deadline = GetTickCount64() + 60000;
    MSG message{};
    while (GetTickCount64() < deadline) {
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            if (message.message == WM_QUIT || (message.hwnd == window && message.message == WM_CLOSE)) {
                break;
            }
            DispatchMessageW(&message);
            continue;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
    }
    Shell_NotifyIconW(NIM_DELETE, &data);
    DestroyWindow(window);
    return 0;
}

HANDLE StartProbe() {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = std::wstring(L"\"") + self + L"\" --probe";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) ==
        FALSE) {
        return nullptr;
    }
    CloseHandle(process.hThread);
    return process.hProcess;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::wstring(argv[1]) == L"--probe") {
        return Probe(false);
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HANDLE probe = StartProbe();
    CHECK(probe != nullptr);
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);

    SystemTray tray;
    const TrayNotifyIcon* found = nullptr;
    std::vector<TrayNotifyIcon> icons;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (found == nullptr && GetTickCount64() < deadline) {
        Sleep(250);
        icons = tray.EnumerateNotifyIcons();
        for (const TrayNotifyIcon& icon : icons) {
            if (icon.tip == kProbeTip) {
                found = &icon;
            }
        }
    }
    std::printf("tray items: %zu\n", icons.size());
    for (const TrayNotifyIcon& icon : icons) {
        std::printf("  '%ls' exe=%ls id=%u window=%p icon=%d shellButton=%d\n", icon.tip.c_str(),
            icon.executablePath.c_str(), icon.id, static_cast<void*>(icon.window), icon.icon != nullptr ? 1 : 0,
            icon.shellButton ? 1 : 0);
    }
    CHECK(found != nullptr);
    if (found != nullptr) {
        CHECK(found->id == kProbeId);
        CHECK(found->icon != nullptr);
        CHECK(found->window != nullptr && IsWindow(found->window) != FALSE);
        CHECK(_wcsicmp(found->executablePath.c_str(), self) == 0);
        PostMessageW(found->window, WM_CLOSE, 0, 0);
    }
    if (probe != nullptr) {
        if (WaitForSingleObject(probe, 10000) != WAIT_OBJECT_0) {
            TerminateProcess(probe, 0);
        }
        CloseHandle(probe);
    }
    // Gone once its owner removed it.
    bool stillListed = false;
    for (const TrayNotifyIcon& icon : tray.EnumerateNotifyIcons()) {
        stillListed = stillListed || icon.tip == kProbeTip;
    }
    CHECK(!stillListed);
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    if (g_failures == 0) {
        std::printf("Tray icon tests: all passed\n");
        return 0;
    }
    std::printf("Tray icon tests: %d failure(s)\n", g_failures);
    return 1;
}
