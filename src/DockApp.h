#pragma once

#include "DockConfig.h"
#include "InstalledApps.h"
#include "Renderer.h"
#include "TypeSafeClient.h"
#include "WindowCatalog.h"
#include "SystemTray.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct ITaskbarList2;

class DockApp {
public:
    explicit DockApp(HINSTANCE instance);
    ~DockApp();

    DockApp(const DockApp&) = delete;
    DockApp& operator=(const DockApp&) = delete;

    int Run();

private:
    enum class VisibilityState {
        Hidden,
        Showing,
        Visible,
        Hiding,
    };

    struct DisplayApp {
        PinnedApp app;
        HWND runningWindow = nullptr;
        int persistentPinIndex = -1;
    };

    struct RefreshSnapshot {
        WindowCatalog windows;
        std::vector<DisplayApp> displayApps;
        std::vector<std::wstring> iconTargets;
        std::vector<std::vector<uint8_t>> iconPixels;
        std::vector<std::wstring> missingIconTargets;
        std::vector<std::vector<uint8_t>> missingIconPixels;
        bool layoutChanged = false;
        bool runningChanged = false;
    };

    struct LaunchTarget {
        std::string id;
        PinnedApp app;
        HWND runningWindow = nullptr;
        bool isStart = false;
        std::wstring shortcutName;
        std::wstring executableName;
        std::wstring executablePath;
        std::wstring aumid;
        std::wstring description;
        std::wstring productName;
        std::wstring publisher;
        std::wstring startMenuFolder;
        std::wstring comment;
        std::wstring windowTitle;
    };

    struct LaunchReply {
        UINT generation = 0;
        LaunchJudgment judgment;
        std::vector<LaunchTarget> targets;
    };

    struct BoostReply {
        std::wstring status;
        bool finished = true;
    };

    struct UpdateReply {
        std::wstring status;
        bool finished = true;
        bool readyToInstall = false;
        bool isSetup = true;
        std::wstring path;
        std::string version;
    };

    struct PinIconResult {
        UINT generation = 0;
        std::vector<std::wstring> targets;
        std::vector<std::vector<uint8_t>> pixels;
    };

    static constexpr UINT kPointerMessage = WM_APP + 1;
    static constexpr UINT kRenderMessage = WM_APP + 2;
    static constexpr UINT kRefreshApplyMessage = WM_APP + 5;
    static constexpr UINT kLaunchResultMessage = WM_APP + 6;
    static constexpr UINT kOpenStartMenuMessage = WM_APP + 7;
    static constexpr UINT kOverflowPaintMessage = WM_APP + 8;
    static constexpr UINT kOverflowWheelMessage = WM_APP + 9;
    static constexpr UINT kBoostResultMessage = WM_APP + 11;
    static constexpr UINT kPinIconMessage = WM_APP + 10;
    static constexpr UINT kUpdateResultMessage = WM_APP + 12;
    static constexpr UINT kSettingsPaintMessage = WM_APP + 13;
    static constexpr UINT_PTR kRefreshTimerId = 1;
    static constexpr UINT_PTR kDeferredRefreshTimerId = 2;
    static constexpr UINT_PTR kConfigSaveTimerId = 3;
    static constexpr UINT_PTR kBackdropTimerId = 4;
    static constexpr UINT_PTR kStartMenuTimerId = 5;
    static constexpr UINT_PTR kTaskbarMonitorTimerId = 6;
    static constexpr UINT_PTR kTrayTimerId = 7;
    static constexpr UINT_PTR kUpdateTimerId = 8;
    static constexpr UINT kUpdateIntervalMs = 6U * 60U * 60U * 1000U;
    static constexpr UINT kUpdateInitialDelayMs = 15000;
    static constexpr UINT kDeferredRefreshDelayMs = 400;
    static constexpr UINT kBackdropIntervalMs = 8;
    static constexpr UINT kTaskbarMonitorIntervalMs = 100;
    static constexpr UINT kTaskbarMonitorSlowIntervalMs = 1000;
    static constexpr int kTaskbarMonitorCalmPasses = 5;
    static constexpr UINT kTrayIntervalMs = 1000;
    static constexpr UINT kContextOpen = 1;
    static constexpr UINT kContextOpenLocation = 2;
    static constexpr UINT kContextClose = 3;
    static constexpr UINT kContextPin = 4;
    static constexpr UINT kContextUnpin = 5;
    static constexpr UINT kContextPinForeground = 6;
    static constexpr UINT kContextToggleBounds = 7;
    static constexpr UINT kContextEndTask = 8;

    enum class TrayFlyoutHitKind : uint8_t {
        None,
        Settings,
        Wifi,
        Sound,
        Brightness,
        Boost,
        ClearAll,
        NotificationCenter,
        NotifyIcon,
    };

    struct TrayFlyoutHit {
        TrayFlyoutHitKind kind = TrayFlyoutHitKind::None;
        int index = -1;
        RECT bounds{};
    };

    enum class SettingsHitKind : uint8_t {
        None,
        Startup,
        Updates,
        CheckNow,
        Close,
    };

    struct SettingsHit {
        SettingsHitKind kind = SettingsHitKind::None;
        RECT bounds{};
    };

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK InputWindowProcedure(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam);
    static LRESULT CALLBACK HoverLabelWindowProcedure(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam);
    static LRESULT CALLBACK DragGhostWindowProcedure(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam);
    static LRESULT CALLBACK LaunchPromptWindowProcedure(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam);
    static LRESULT CALLBACK LaunchEditProcedure(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam);
    static LRESULT CALLBACK OverflowWindowProcedure(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam);
    static LRESULT CALLBACK DockSettingsProcedure(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam);
    static LRESULT CALLBACK MouseHook(int code, WPARAM wParam, LPARAM lParam);
    static BOOL CALLBACK FindTaskbarWindow(HWND window, LPARAM data);

    LRESULT HandleRendererMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleInputMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void CreateOverlayWindow();
    void CreateHoverLabelWindow();
    void DestroyHoverLabelWindow();
    void RebuildLayout(bool reloadIcons);
    void UpdateInputRegion();
    void PositionOverlayWindows();
    void UpdateHoverLabel();
    void HideHoverLabel() noexcept;
    void DestroyHoverLabelFont() noexcept;
    [[nodiscard]] HFONT HoverLabelFont();
    void LoadIconTextures();
    void AssignIconTextureIndices();
    void EnsureTrayIcons();
    void RefreshTray(bool forceLayout);
    void OpenTraySlot(TraySlot slot);
    void ToggleOverflowPopup();
    void CloseOverflowPopup() noexcept;
    void RebuildOverflowPopup();
    void PaintOverflowPopup();
    void QueueOverflowPaint();
    void DrainBrightnessWheel();
    void ScrollBrightness(int delta);
    void RefreshBrightnessAsync();
    void RunPerformanceBoost();
    void ApplyBoostResult(const std::wstring& status, bool finished);
    void OpenDockSettings();
    void ToggleDockSettings();
    void CloseDockSettings() noexcept;
    void DestroyDockSettings() noexcept;
    void PositionDockSettings();
    void PaintSettingsPopup();
    void QueueSettingsPaint();
    void RebuildSettingsPopup();
    void HandleSettingsClick(const SettingsHit& hit, UINT message);
    [[nodiscard]] int SettingsHitIndex(POINT point) const noexcept;
    [[nodiscard]] bool SettingsScreenOrigin(POINT& origin) const noexcept;
    void InvalidateSettingsGlass() noexcept;
    [[nodiscard]] bool SettingsGlassValid(POINT origin) const noexcept;
    void RefreshSettingsControls();
    void SetUpdateStatus(const std::wstring& status);
    void CheckForUpdatesAsync(bool manual);
    void ApplyUpdateResult(const UpdateReply& reply);
    void StartUpdateTimer(UINT delayMs) noexcept;
    void StopUpdateTimer() noexcept;
    [[nodiscard]] bool IsDockSettingsOpen() const noexcept;
    [[nodiscard]] bool IsCursorOverSettings(POINT cursor) const noexcept;
    void EnsureOverflowGlyphs(UINT gearExtent, UINT tileExtent, UINT notifyExtent);
    void InvalidateOverflowGlass() noexcept;
    void EnsureOverflowFonts(float scale);
    void DestroyOverflowFonts() noexcept;
    [[nodiscard]] bool OverflowGlassValid(POINT origin) const noexcept;
    void PositionOverflowPopup();
    void DestroyOverflowPopup() noexcept;
    void HandleOverflowClick(const TrayFlyoutHit& hit, UINT message);
    [[nodiscard]] int OverflowHitIndex(POINT point) const noexcept;
    [[nodiscard]] bool OverflowScreenOrigin(POINT& origin, LONG& caretX) const noexcept;
    [[nodiscard]] bool IsOverflowOpen() const noexcept;
    [[nodiscard]] bool IsCursorOverOverflow(POINT cursor) const noexcept;
    [[nodiscard]] bool IsCursorWithinFlyoutZone(POINT cursor) const noexcept;
    [[nodiscard]] bool IsTrayRenderIndex(int index) const noexcept;
    [[nodiscard]] int AppSlotCount() const noexcept;
    void UpdatePrimaryMonitor();
    [[nodiscard]] bool AdoptMonitorForCursor(POINT cursor);
    [[nodiscard]] UINT HostDpi() const noexcept;
    void BeginShow();
    [[nodiscard]] bool CaptureLiveBackdrop();
    void BeginHide();
    void AdvanceAnimation();
    bool RenderFrame(bool allowBlockingGpuWait = true);
    void QueueRenderFrame(bool allowBlockingGpuWait = true);
    void StartBackdropTimer() noexcept;
    void StopBackdropTimer() noexcept;
    void HandlePointer(POINT cursor);
    void HandleContextMenu(POINT screenPoint);
    void ActivatePressedApp();
    bool OpenLaunchPrompt();
    void CloseLaunchPrompt(bool hideDockIfAway = true);
    void PositionLaunchPrompt();
    void SetLaunchPromptStatus(const std::wstring& text);
    void SubmitLaunchPrompt();
    void ApplyLaunchJudgment(UINT generation, const LaunchJudgment& judgment);
    [[nodiscard]] std::vector<LaunchTarget> CollectLaunchTargets(
        const std::vector<DisplayApp>& displayApps,
        const std::vector<RunningWindow>& runningWindows) const;
    [[nodiscard]] const LaunchTarget* FindLaunchTarget(const std::string& id) const noexcept;
    [[nodiscard]] LaunchCandidate MakeLaunchCandidate(const LaunchTarget& target) const;
    [[nodiscard]] std::string ExactLaunchId(const std::wstring& request,
        const std::vector<LaunchTarget>& targets) const;
    bool ActivateLaunchTarget(const LaunchTarget& target);
    [[nodiscard]] bool IsLaunchPromptOpen() const noexcept;
    void BeginDrag(POINT screenCursor);
    void UpdateDrag(POINT screenCursor);
    void FinishDrag(POINT screenCursor);
    void CancelDragWithSnapBack(POINT releaseCursor);
    void AdvanceDragSnapBack();
    void CompleteDrag();
    void ApplyDragPreviewLayout();
    void CacheLayoutSlotBounds();
    void UpdateDividerScaleDrag(POINT screenCursor);
    void EnsureDragGhostWindow();
    void UpdateDragGhostContent();
    void UpdateDragGhostPosition(POINT screenCursor);
    void SnapDragGhostToInsertionSlot();
    void BringDragGhostToFront();
    void FinishDropPresent(bool presented);
    void HideDragGhost() noexcept;
    void DestroyDragGhostWindow();
    void ClearPressState() noexcept;
    void RefreshRunningWindows(bool force = false);
    void BeginBackgroundRefresh(bool force);
    void ApplyBackgroundRefresh(UINT generation);
    void EnsureMissingPinIconsAsync();
    void ApplyPinIcons(UINT generation);
    void StartRefreshTimer() noexcept;
    void StopRefreshTimer() noexcept;
    void StartTrayTimer() noexcept;
    void StopTrayTimer() noexcept;
    void ScheduleConfigSave() noexcept;
    void ScheduleDeferredRefresh() noexcept;
    void CancelDeferredRefresh() noexcept;
    bool RebuildDisplayApps(bool* structureChanged = nullptr);
    [[nodiscard]] static RefreshSnapshot BuildDisplayAppsSnapshot(const WindowCatalog& windows,
        const std::vector<PinnedApp>& pins, const std::vector<DisplayApp>& currentApps);
    [[nodiscard]] static std::vector<std::wstring> IconCacheKeysFromDisplayApps(
        const std::vector<DisplayApp>& displayApps);
    [[nodiscard]] static int DividerIndexFromDisplayApps(const std::vector<DisplayApp>& displayApps);
    bool OpenStartMenuFromDock();
    void PrepareShellForStartMenu();
    void HideOverlayForShellFlyout();
    void RestoreOverlayAfterShellFlyout();
    void ReleaseShellFlyoutHold();
    void StopShellFlyoutWatch() noexcept;
    void HideTaskbar();
    void RestoreTaskbar();
    void SuppressNativeTaskbar();
    [[nodiscard]] bool MaintainNativeTaskbarSuppression();
    [[nodiscard]] bool ExpandPrimaryWorkArea(bool notify);
    [[nodiscard]] bool ExpandSecondaryMonitorWorkAreas();
    void RestoreDesktopWorkArea();
    void DisableMultiMonitorTaskbars();
    void RestoreMultiMonitorTaskbars();
    void NotifyExplorerTraySettings() noexcept;
    void EnsureFullscreenClaimWindows();
    void DestroyFullscreenClaimWindows() noexcept;
    void EnsureTaskbarList();
    [[nodiscard]] bool CollapseNativeTaskbarAppBar();
    void StartTaskbarMonitor() noexcept;
    void StopTaskbarMonitor() noexcept;
    void RegisterSystemResumeNotifications();
    void UnregisterSystemResumeNotifications() noexcept;
    LRESULT HandlePowerBroadcast(WPARAM wParam, LPARAM lParam);
    void LogInputMouse(UINT message, POINT screenPoint, int icon) const;
    void Log(const std::wstring& message) const;

    [[nodiscard]] bool IsCursorInBottomHotZone(POINT cursor) const noexcept;
    [[nodiscard]] int IconAtScreenPoint(POINT cursor) const noexcept;
    [[nodiscard]] int DividerAtScreenPoint(POINT cursor) const noexcept;
    [[nodiscard]] int InsertionIndexForDrag(POINT cursor) const noexcept;
    [[nodiscard]] bool HasCrossedDragThreshold(POINT cursor) const noexcept;
    [[nodiscard]] bool IsCursorOverDock(POINT cursor) const noexcept;
    [[nodiscard]] bool ShouldPostPointerUpdate(POINT cursor) const noexcept;
    [[nodiscard]] bool IsDragActive() const noexcept;
    [[nodiscard]] bool IsPersistentDisplayIcon(int icon) const noexcept;
    [[nodiscard]] LONG CurrentY() const noexcept;
    [[nodiscard]] bool IsAnimating() const noexcept;
    [[nodiscard]] double SecondsSinceAnimationStarted() const noexcept;
    [[nodiscard]] static double QpcSeconds();
    [[nodiscard]] UINT IconPixelExtent() const noexcept;
    void ReloadIconsIfExtentChanged();

    HINSTANCE m_instance = nullptr;
    HWND m_window = nullptr;
    HWND m_inputWindow = nullptr;
    HWND m_hoverLabelWindow = nullptr;
    HWND m_dragGhostWindow = nullptr;
    HWND m_launchPromptWindow = nullptr;
    HWND m_launchEdit = nullptr;
    HWND m_launchStatus = nullptr;
    HHOOK m_mouseHook = nullptr;
    HFONT m_hoverLabelFont = nullptr;
    UINT m_hoverLabelFontDpi = 0;
    HBITMAP m_dragGhostBitmap = nullptr;
    HDC m_dragGhostMemoryDc = nullptr;
    HGDIOBJ m_dragGhostPreviousBitmap = nullptr;
    SIZE m_dragGhostSize{};
    HANDLE m_singleInstanceMutex = nullptr;
    RECT m_primaryBounds{};
    HMONITOR m_hostMonitor = nullptr;
    RECT m_hostBounds{};
    POINT m_lastCursor{};
    std::vector<HWND> m_hiddenTaskbars;
    std::vector<std::pair<HWND, RECT>> m_hiddenTaskbarRects;
    std::vector<std::pair<HWND, RECT>> m_collapsedTaskbars;
    UINT m_taskbarCreatedMessage = 0;
    HPOWERNOTIFY m_suspendNotify = nullptr;
    HPOWERNOTIFY m_monitorNotify = nullptr;
    bool m_sessionNotifyRegistered = false;
    DockConfig m_config;
    WindowCatalog m_windows;
    Renderer m_renderer;
    std::vector<DisplayApp> m_displayApps;
    std::vector<DockIconRenderData> m_iconRenderData;
    VisibilityState m_visibility = VisibilityState::Hidden;
    double m_animationStartedAt = 0.0;
    LONG m_animationFromY = 0;
    LONG m_animationToY = 0;
    LONG m_windowX = 0;
    LONG m_visibleY = 0;
    LONG m_hiddenY = 0;
    LONG m_currentY = 0;
    UINT m_dockWidth = 1;
    UINT m_dockHeight = 1;
    int m_hoveredIcon = -1;
    int m_hoveredDivider = -1;
    int m_pressedIcon = -1;
    int m_draggedIcon = -1;
    int m_dragInsertion = -1;
    int m_dragOriginIndex = -1;
    RECT m_dragOriginBounds{};
    POINT m_dragGrabOffset{};
    POINT m_dragSnapFrom{};
    POINT m_dragSnapTo{};
    double m_dragSnapStartedAt = 0.0;
    double m_lastDragPresentAt = 0.0;
    bool m_dragSnapAnimating = false;
    bool m_scalingDivider = false;
    float m_dockScale = 1.0F;
    float m_scaleDragStartValue = 1.0F;
    LONG m_scaleDragStartY = 0;
    std::vector<RECT> m_layoutSlotBounds;
    POINT m_pressedAt{};
    bool m_taskbarHidden = false;
    int m_taskbarMonitorQuietPasses = 0;
    bool m_taskbarMonitorFast = true;
    bool m_taskbarStateSaved = false;
    UINT m_savedTaskbarState = 0;
    RECT m_savedWorkArea{};
    bool m_workAreaSaved = false;
    bool m_workAreaExpanded = false;
    bool m_multiMonTaskbarSaved = false;
    bool m_multiMonTaskbarHadValue = false;
    DWORD m_savedMultiMonTaskbar = 1;
    struct FullscreenClaim {
        HMONITOR monitor = nullptr;
        RECT bounds{};
        HWND window = nullptr;
    };
    std::vector<FullscreenClaim> m_fullscreenClaims;
    ITaskbarList2* m_taskbarList2 = nullptr;
    HRESULT m_comResult = E_FAIL;
    bool m_rendererInitialized = false;
    bool m_renderQueued = false;
    bool m_renderAllowBlockingGpuWait = true;
    bool m_backdropCaptureRequiresHide = true;
    double m_lastWindowRefresh = 0.0;
    int m_hoverLabelIcon = -1;
    int m_dividerIndex = -1;
    UINT m_showSessionId = 0;
    UINT m_loadedIconExtent = 0;
    UINT m_shellFlyoutAttempts = 0;
    bool m_shellFlyoutIsSearch = false;
    bool m_shellFlyoutHold = false;
    bool m_overlayHiddenForFlyout = false;
    HWND m_shellFlyoutWindow = nullptr;
    double m_shellFlyoutHoldUntil = 0.0;
    double m_suppressBackdropUntil = 0.0;
    bool m_dropPresentPending = false;
    std::atomic<bool> m_refreshInFlight{false};
    std::atomic<UINT> m_refreshGeneration{0};
    std::mutex m_refreshSnapshotMutex;
    std::optional<RefreshSnapshot> m_refreshSnapshot;
    std::atomic<UINT> m_pinIconGeneration{0};
    std::mutex m_pinIconMutex;
    std::optional<PinIconResult> m_pinIconPending;
    std::vector<LaunchTarget> m_launchTargets;
    std::atomic<UINT> m_launchGeneration{0};
    std::atomic<bool> m_launchInFlight{false};
    WNDPROC m_launchEditPrevious = nullptr;
    InstalledAppCatalog m_installedApps;
    SystemTray m_tray;
    HWND m_overflowWindow = nullptr;
    std::vector<TrayNotifyIcon> m_overflowIcons;
    std::vector<TrayFlyoutHit> m_overflowHits;
    int m_overflowHover = -1;
    bool m_overflowPaintQueued = false;
    int m_flyoutWheelAccum = 0;
    // Optimistic brightness UI: the displayed level. -1 follows live status;
    // non-negative values are user-projected (instant fill/%) while the worker
    // writes the monitor in the background, then control returns to status.
    std::atomic<int> m_brightnessTarget{-1};
    int m_brightnessWheelRemainder = 0;
    std::atomic<bool> m_brightnessAdjustInFlight{false};
    std::atomic<bool> m_brightnessRefreshInFlight{false};
    HWND m_settingsWindow = nullptr;
    std::vector<SettingsHit> m_settingsHits;
    int m_settingsHover = -1;
    bool m_settingsPaintQueued = false;
    SIZE m_settingsSize{};
    std::vector<uint8_t> m_settingsGlass;
    SIZE m_settingsGlassSize{};
    POINT m_settingsGlassOrigin{};
    std::wstring m_updateStatus = L"Checking for updates...";
    std::atomic<bool> m_updateInFlight{false};
    std::atomic<bool> m_updateInstalling{false};
    double m_lastUpdateCheck = 0.0;
    std::wstring m_overflowGlyphKey;
    std::vector<uint8_t> m_overflowGlyphGear;
    std::vector<uint8_t> m_overflowGlyphWifi;
    std::vector<uint8_t> m_overflowGlyphSound;
    std::vector<uint8_t> m_overflowGlyphBrightness;
    std::vector<uint8_t> m_overflowGlyphBell;
    std::vector<uint8_t> m_overflowGlyphBoost;
    // Quick Settings Boost tile status, UI thread only. Updated through
    // kBoostResultMessage so the worker never touches popup state.
    // Tile-sized: keep statuses short (~12 chars) so the narrow Boost
    // circle shows key info instead of an ellipsized cut-off.
    std::wstring m_boostStatus = L"Tap to boost";
    std::atomic<bool> m_boostInFlight{false};
    SIZE m_overflowSize{};
    LONG m_overflowCaretX = 0;
    std::vector<uint8_t> m_overflowGlass;
    SIZE m_overflowGlassSize{};
    POINT m_overflowGlassOrigin{};
    LONG m_overflowGlassCaretX = 0;
    float m_overflowFontScale = 0.0F;
    HFONT m_overflowTitleFont = nullptr;
    HFONT m_overflowSectionFont = nullptr;
    HFONT m_overflowLabelFont = nullptr;
    HFONT m_overflowStatusFont = nullptr;

    static DockApp* s_instance;
    bool m_shellFlyoutIsTray = false;
    std::wstring m_trayVisualKey;
};
