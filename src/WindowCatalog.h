#pragma once

#include "DockConfig.h"

#include <Windows.h>

#include <string>
#include <vector>

struct RunningWindow {
    HWND handle = nullptr;
    std::wstring executablePath;
    std::wstring title;
};

class WindowCatalog {
public:
    void Refresh();
    [[nodiscard]] bool IsRunning(const PinnedApp& app) const;
    [[nodiscard]] HWND FindWindowFor(const PinnedApp& app) const;
    [[nodiscard]] bool ActivateOrLaunch(const PinnedApp& app) const;
    [[nodiscard]] bool Close(const PinnedApp& app) const;
    [[nodiscard]] bool OpenLocation(const PinnedApp& app) const;
    [[nodiscard]] bool AddForegroundApplication(std::vector<PinnedApp>& pins) const;

private:
    static BOOL CALLBACK EnumerateWindows(HWND window, LPARAM data);
    static bool IsApplicationWindow(HWND window);
    static std::wstring ExecutablePath(HWND window);
    static std::wstring WindowTitle(HWND window);
    static std::wstring NormalizedPath(const std::wstring& path);
    static bool IsShellTarget(const std::wstring& target);

    std::vector<RunningWindow> m_windows;
};
