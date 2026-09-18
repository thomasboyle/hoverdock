#pragma once

#include "DockConfig.h"

#include <Windows.h>

#include <string>
#include <unordered_map>
#include <vector>

struct RunningWindow {
    HWND handle = nullptr;
    std::wstring executablePath;
    std::wstring normalizedPath;
    std::wstring appUserModelId;
    std::wstring title;
    std::wstring executableName;
    DWORD processId = 0;
};

class WindowCatalog {
public:
    [[nodiscard]] bool Refresh();
    void RebuildPinProfiles(const std::vector<PinnedApp>& pins);
    [[nodiscard]] const std::vector<RunningWindow>& RunningWindows() const noexcept;
    [[nodiscard]] bool IsRunning(const PinnedApp& app) const;
    [[nodiscard]] HWND FindWindowFor(const PinnedApp& app) const;
    [[nodiscard]] bool ActivateOrLaunch(const PinnedApp& app, HWND preferredWindow = nullptr) const;
    [[nodiscard]] bool Close(const PinnedApp& app, HWND preferredWindow = nullptr) const;
    [[nodiscard]] bool EndTask(const PinnedApp& app, HWND preferredWindow = nullptr) const;
    [[nodiscard]] bool OpenLocation(const PinnedApp& app) const;
    [[nodiscard]] bool AddForegroundApplication(std::vector<PinnedApp>& pins) const;
    [[nodiscard]] bool MatchesPin(const PinnedApp& pin, const RunningWindow& window) const;
    [[nodiscard]] bool MatchesAnyPin(const RunningWindow& window) const;
    [[nodiscard]] static bool TargetsMatch(const std::wstring& left, const std::wstring& right);
    [[nodiscard]] static std::wstring ResolveLauncherProcessPath(const PinnedApp& app);
    [[nodiscard]] static std::wstring IconCacheKey(const PinnedApp& app);
    [[nodiscard]] static std::vector<std::wstring> IconResolutionCandidates(const PinnedApp& app,
        HWND runningWindow = nullptr);
    [[nodiscard]] static std::wstring DisplayNameForApp(const PinnedApp& app,
        HWND runningWindow = nullptr);

private:
    struct PinMatchProfile {
        std::wstring sourceTarget;
        std::vector<std::wstring> normalizedPaths;
        std::vector<std::wstring> executableNames;
        std::wstring appsFolderAumid;
        std::wstring packageFamily;
    };

    static BOOL CALLBACK EnumerateWindows(HWND window, LPARAM data);
    static bool IsApplicationWindow(HWND window);
    static std::wstring ExecutablePath(HWND window);
    static std::wstring WindowTitle(HWND window);
    static std::wstring NormalizedPath(const std::wstring& path);
    static bool IsShellTarget(const std::wstring& target);

    [[nodiscard]] const PinMatchProfile* ProfileForPin(const PinnedApp& pin) const noexcept;
    [[nodiscard]] bool MatchWindowAgainstProfile(const PinMatchProfile& profile,
        const RunningWindow& window) const noexcept;
    void EnrichWindows();
    [[nodiscard]] static bool WindowsSnapshotEqual(const std::vector<RunningWindow>& left,
        const std::vector<RunningWindow>& right) noexcept;
    [[nodiscard]] std::wstring CachedNormalizedPath(const std::wstring& path);

    std::vector<RunningWindow> m_windows;
    std::vector<PinMatchProfile> m_pinProfiles;
    std::vector<PinnedApp> m_cachedPinProfileSources;
    std::unordered_map<std::wstring, std::wstring> m_normalizedPathCache;
    bool m_pinMatchingNeedsAumid = false;
    bool m_enrichedAumid = false;
};
