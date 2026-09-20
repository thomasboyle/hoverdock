#pragma once

#include "DockConfig.h"

#include <Windows.h>

#include <mutex>
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
    // Every live window matching the pin, topmost first. Fresh z-order walk
    // (not the handle-sorted snapshot) so a just-opened second window is
    // seen immediately. Powers macOS-style activation: the whole app comes
    // forward, not a single window.
    [[nodiscard]] std::vector<HWND> FindWindowsFor(const PinnedApp& app) const;
    [[nodiscard]] bool ActivateOrLaunch(const PinnedApp& app, HWND preferredWindow = nullptr) const;
    // Fast, UI-thread safe: brings all of the app's windows forward
    // macOS-style (relative z-order preserved, frontmost focused) without
    // launching. Returns true when a window was found and activation was
    // attempted — including when the foreground grab itself is denied, so
    // callers never launch a duplicate for a running app.
    [[nodiscard]] bool TryActivate(const PinnedApp& app, HWND preferredWindow = nullptr) const;
    // Thread-safe fire-and-forget launcher. Must NOT touch WindowCatalog instance
    // state so DockApp can call it from a worker thread without stalling the
    // WH_MOUSE_LL hook thread (which shares the UI thread — a stalled UI thread
    // freezes the system cursor until ShellExecute returns, e.g. heavy Electron
    // apps like Grok). Uses SEE_MASK_ASYNCOK so the shell returns immediately.
    [[nodiscard]] static bool LaunchApp(const PinnedApp& app);
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
    // PID-validated executable path: OpenProcess + QueryFullProcessImageName
    // costs ~30-100us per window, dominating Refresh. Cache by PID, validated
    // against process creation time so recycled PIDs never serve stale paths.
    // Shared across catalog instances (background refreshes use throwaway
    // catalogs); mutex-guarded, uncontended in practice.
    struct ProcessPathEntry {
        std::wstring path;
        ULONGLONG creationTime = 0;
    };
    static std::wstring CachedExecutablePath(DWORD processId);
    // Normalized-path cache: GetLongPathName hits FS metadata (~10-50us) per
    // call, and TargetsMatch normalizes both sides on every comparison
    // (RemapInteraction does dozens per pin change). Pure function of the
    // string; bounded shared cache, same threading as above. Stale only if a
    // pinned target is renamed mid-session (mismatch until restart).
    static std::wstring CachedNormalizedPathStatic(const std::wstring& path);
    // AUMID cache: SHGetPropertyStoreForWindow is COM (~100us+) per window per
    // refresh. A window's AUMID never changes during its lifetime, so cache by
    // HWND validated by live PID (+TID): a recycled handle with a different
    // owner fails validation and is re-queried.
    struct AumidEntry {
        DWORD processId = 0;
        DWORD threadId = 0;
        std::wstring aumid;
    };
    static std::wstring CachedAppUserModelId(HWND window);
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
    bool m_pinMatchingNeedsAumid = false;
    bool m_enrichedAumid = false;
    static std::unordered_map<DWORD, ProcessPathEntry> s_processPathCache;
    static std::mutex s_processPathMutex;
    static std::unordered_map<std::wstring, std::wstring> s_normalizedPathCache;
    static std::mutex s_normalizedPathMutex;
    static std::unordered_map<HWND, AumidEntry> s_aumidCache;
    static std::mutex s_aumidMutex;
    // Display-name cache: DisplayNameForApp reads version resources from disk
    // and COM shell names (~0.5-3ms), and the hover path calls it on EVERY
    // icon change — fast cursor waggles burned a third of a core on it. The
    // name is stable per (target, pin name, running exe), so cache it.
    static std::wstring DisplayNameForAppSlow(const PinnedApp& app, HWND runningWindow);
    static std::unordered_map<std::wstring, std::wstring> s_displayNameCache;
    static std::mutex s_displayNameMutex;
};
