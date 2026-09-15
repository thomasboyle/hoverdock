#include "WindowCatalog.h"

#include <Shellapi.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iterator>

namespace {

bool EqualInsensitive(const std::wstring& left, const std::wstring& right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](wchar_t lhs, wchar_t rhs) {
            return std::towlower(lhs) == std::towlower(rhs);
        });
}

std::wstring FileNameWithoutExtension(const std::wstring& path) {
    const std::filesystem::path file(path);
    return file.stem().wstring();
}

}  // namespace

void WindowCatalog::Refresh() {
    m_windows.clear();
    EnumWindows(&WindowCatalog::EnumerateWindows, reinterpret_cast<LPARAM>(this));
}

bool WindowCatalog::IsRunning(const PinnedApp& app) const {
    return FindWindowFor(app) != nullptr;
}

HWND WindowCatalog::FindWindowFor(const PinnedApp& app) const {
    if (IsShellTarget(app.target)) {
        return nullptr;
    }

    const std::wstring target = NormalizedPath(app.target);
    for (const RunningWindow& window : m_windows) {
        if (EqualInsensitive(target, NormalizedPath(window.executablePath))) {
            return window.handle;
        }
    }
    return nullptr;
}

bool WindowCatalog::ActivateOrLaunch(const PinnedApp& app) const {
    if (const HWND window = FindWindowFor(app)) {
        if (IsIconic(window) != FALSE) {
            ShowWindowAsync(window, SW_RESTORE);
        }
        BringWindowToTop(window);
        if (SetForegroundWindow(window) != FALSE) {
            return true;
        }

        FLASHWINFO flash{sizeof(flash)};
        flash.hwnd = window;
        flash.dwFlags = FLASHW_TRAY;
        flash.uCount = 3;
        FlashWindowEx(&flash);
        return false;
    }

    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"open";
    launch.lpFile = app.target.c_str();
    launch.lpParameters = app.arguments.empty() ? nullptr : app.arguments.c_str();
    launch.lpDirectory = app.workingDirectory.empty() ? nullptr : app.workingDirectory.c_str();
    launch.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&launch) != FALSE;
}

bool WindowCatalog::Close(const PinnedApp& app) const {
    const HWND window = FindWindowFor(app);
    return window != nullptr && PostMessageW(window, WM_CLOSE, 0, 0) != FALSE;
}

bool WindowCatalog::OpenLocation(const PinnedApp& app) const {
    if (IsShellTarget(app.target)) {
        return false;
    }

    const std::wstring parameters = L"/select,\"" + app.target + L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe",
        parameters.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

bool WindowCatalog::AddForegroundApplication(std::vector<PinnedApp>& pins) const {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr || !IsApplicationWindow(foreground)) {
        return false;
    }

    const std::wstring path = ExecutablePath(foreground);
    if (path.empty()) {
        return false;
    }

    const std::wstring normalized = NormalizedPath(path);
    const bool alreadyPinned = std::ranges::any_of(pins, [&normalized](const PinnedApp& app) {
        return EqualInsensitive(WindowCatalog::NormalizedPath(app.target), normalized);
    });
    if (alreadyPinned) {
        return false;
    }

    PinnedApp app;
    app.name = WindowTitle(foreground);
    app.target = path;
    if (app.name.empty()) {
        app.name = FileNameWithoutExtension(path);
    }
    pins.push_back(std::move(app));
    return true;
}

BOOL CALLBACK WindowCatalog::EnumerateWindows(HWND window, LPARAM data) {
    auto* catalog = reinterpret_cast<WindowCatalog*>(data);
    if (!IsApplicationWindow(window)) {
        return TRUE;
    }

    const std::wstring path = ExecutablePath(window);
    if (path.empty()) {
        return TRUE;
    }

    catalog->m_windows.push_back({window, path, WindowTitle(window)});
    return TRUE;
}

bool WindowCatalog::IsApplicationWindow(HWND window) {
    if (IsWindowVisible(window) == FALSE || GetWindow(window, GW_OWNER) != nullptr) {
        return false;
    }

    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    return (style & WS_CHILD) == 0 && (extendedStyle & WS_EX_TOOLWINDOW) == 0;
}

std::wstring WindowCatalog::ExecutablePath(HWND window) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0) {
        return {};
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return {};
    }

    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const BOOL success = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    if (success == FALSE) {
        return {};
    }

    path.resize(length);
    return path;
}

std::wstring WindowCatalog::WindowTitle(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) {
        return {};
    }

    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, title.data(), length + 1);
    title.resize(static_cast<size_t>(length));
    return title;
}

std::wstring WindowCatalog::NormalizedPath(const std::wstring& path) {
    wchar_t fullPath[32768]{};
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(fullPath)),
        fullPath, nullptr);
    if (length == 0 || length >= std::size(fullPath)) {
        return path;
    }
    return fullPath;
}

bool WindowCatalog::IsShellTarget(const std::wstring& target) {
    return target.rfind(L"shell:", 0) == 0;
}
