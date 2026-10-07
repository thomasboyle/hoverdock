#pragma once

#include "DockConfig.h"
#include "InstalledApps.h"
#include "Renderer.h"
#include "TypeSafeClient.h"
#include "LlamaServerClient.h"
#include "WindowCatalog.h"
#include "SystemTray.h"
#include "Bluetooth.h"
#include "QuickSettingsPanel.h"
#include "Weather.h"
#include "PerfProfiler.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct ITaskbarList2;
struct IDropTarget;
struct _ITEMIDLIST;
using TrashPidl = _ITEMIDLIST*;

class DockApp {
public:
    explicit DockApp(HINSTANCE instance);
    ~DockApp();

    DockApp(const DockApp&) = delete;
    DockApp& operator=(const DockApp&) = delete;

    int Run();

    friend class DockAppTrashDropTarget;

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
        // Agent-in-search: when set, `agent` carries the validated actions and
        // reply instead of `judgment`.
        bool agentMode = false;
        SearchAgentResult agent;
        // Latency log: which path resolved the query and worker time.
        std::wstring route;
        long long workerMs = 0;
    };

    struct AgentStatusReply {
        UINT generation = 0;
        std::wstring text;
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
    static constexpr UINT kCursorWatchSyncMessage = WM_APP + 14;
    static constexpr UINT kBeginShowDeferredMessage = WM_APP + 15;
    static constexpr UINT kLayoutApplyMessage = WM_APP + 16;
    static constexpr UINT kDeferredClickMessage = WM_APP + 17;
    static constexpr UINT kWeatherMessage = WM_APP + 22;
    static constexpr UINT kBluetoothMessage = WM_APP + 23;
    static constexpr UINT kAgentStatusMessage = WM_APP + 28;
    static constexpr UINT_PTR kWeatherTimerId = 10;
    static constexpr UINT_PTR kRefreshTimerId = 1;
    static constexpr UINT_PTR kDeferredRefreshTimerId = 2;
    static constexpr UINT_PTR kConfigSaveTimerId = 3;
    static constexpr UINT_PTR kBackdropTimerId = 4;
    static constexpr UINT_PTR kPerfOpenSettingsTimerId = 14;
    static constexpr UINT_PTR kStartMenuTimerId = 5;
    static constexpr UINT_PTR kTaskbarMonitorTimerId = 6;
    static constexpr UINT_PTR kTrayTimerId = 7;
    static constexpr UINT_PTR kUpdateTimerId = 8;
    static constexpr UINT_PTR kCursorWatchTimerId = 9;
    static constexpr UINT_PTR kGlintTimerId = 15;
    static constexpr UINT_PTR kBluetoothTimerId = 16;
    static constexpr UINT_PTR kEnergySampleTimerId = 17;
    static constexpr UINT_PTR kQsLiveTimerId = 18;
    static constexpr UINT_PTR kQsTempsTimerId = 19;
    // Pointer glint easing cadence (~60 Hz); the timer only runs while the
    // glint is still converging on the cursor, so an idle dock costs nothing.
    static constexpr UINT kGlintIntervalMs = 16;
    static constexpr UINT kUpdateIntervalMs = 6U * 60U * 60U * 1000U;
    static constexpr UINT kUpdateInitialDelayMs = 15000;
    static constexpr UINT kCursorWatchIntervalMs = 33;
    // 0 = no timer while Hidden with a healthy LL hook (event-driven via hook + FG WinEvent).
    static constexpr UINT kCursorWatchHiddenIntervalMs = 0;
    static constexpr UINT kDeferredRefreshDelayMs = 400;
    static constexpr UINT kBackdropIntervalMs = 8;
    // Idle DWM / unchanged-capture backoff. Moving wallpaper under the dock,
    // Quick Settings, or Dock Settings holds the 8 ms (~120 Hz) cadence.
    static constexpr UINT kBackdropIdleIntervalMs = 250;
    // Static desktop: duplication poll only, no BitBlt. Short enough that motion
    // is picked up and the fast cadence resumes within a frame or two.
    static constexpr UINT kBackdropRestingIntervalMs = 100;
    // Clean ticks required before leaving 120 Hz. One unchanged frame must not
    // drop a moving wallpaper to the resting poll.
    static constexpr UINT kBackdropIdleHysteresisTicks = 15;
    static constexpr UINT kBackdropRestingHysteresisTicks = 15;
    static constexpr UINT kTaskbarMonitorIntervalMs = 100;
    static constexpr UINT kTaskbarMonitorSlowIntervalMs = 5000;
    static constexpr int kTaskbarMonitorCalmPasses = 5;
    static constexpr UINT kTrayIntervalMs = 1000;
    // Dock face clock uses TIME_NOSECONDS. While Quick Settings is closed, StartTrayTimer
    // arms for the next minute boundary (not a fixed 30 s idle) so the face clock moves.
    static constexpr UINT kCursorWatchCalmIntervalMs = 250;
    static constexpr UINT kContextOpen = 1;
    static constexpr UINT kContextOpenLocation = 2;
    static constexpr UINT kContextClose = 3;
    static constexpr UINT kContextPin = 4;
    static constexpr UINT kContextUnpin = 5;
    static constexpr UINT kContextPinForeground = 6;
    static constexpr UINT kContextToggleBounds = 7;
    static constexpr UINT kContextEndTask = 8;
    static constexpr UINT kContextTrashOpen = 20;
    static constexpr UINT kContextTrashEmpty = 21;
    static constexpr UINT kContextTrashProperties = 22;
    static constexpr UINT kContextPaintMessage = WM_APP + 18;
    static constexpr UINT kTrashNotifyMessage = WM_APP + 19;
    static constexpr UINT kTrashRefreshMessage = WM_APP + 20;
    static constexpr UINT kTrashDropMessage = WM_APP + 21;
    static constexpr UINT kOverflowDismissMessage = WM_APP + 24;
    static constexpr UINT kQsEnergyResultMessage = WM_APP + 25;
    static constexpr UINT kQsTempsResultMessage = WM_APP + 26;
    static constexpr UINT kQsCacheResultMessage = WM_APP + 27;

    enum class TrayFlyoutHitKind : uint8_t {
        None,
        Settings,
        Wifi,
        Sound,
        Brightness,
        Boost,
        BluetoothRadio,
        BluetoothConnect,
        BluetoothPair,
        BluetoothDiscover,
        BluetoothSettings,
        NotifyIcon,
        Back,
        Ethernet,
        Vpn,
        Microphone,
        Display,
        Hdr,
        Power,
        Airplane,
        SystemTrayPage,
        VolumeSlider,
        BrightnessSlider,
        CaptureGain,
        Toggle,
        PowerMode,
        WifiNetwork,
        AudioOutput,
        AudioInput,
        VpnEntry,
        MoreSettings,
        MediaTransport,
        MediaSeek,
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
        RimLight,
        Lensing,
        Dispersion,
        Frost,
        Specular,
        DropShadow,
        DepthShade,
        LightPanels,
        CheckNow,
        PerfProfile,
        PerfLog,
        Close,
    };

    struct SettingsHit {
        SettingsHitKind kind = SettingsHitKind::None;
        RECT bounds{};
    };

    struct ContextItem {
        UINT command = 0;
        std::wstring label;
        wchar_t glyph = 0;
        bool separatorBefore = false;
        bool disabled = false;
    };

    struct ContextHit {
        UINT command = 0;
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
    static LRESULT CALLBACK ContextWindowProcedure(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam);
    static LRESULT CALLBACK DockSettingsProcedure(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam);
    static LRESULT CALLBACK MouseHook(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK OverflowDismissHook(int code, WPARAM wParam, LPARAM lParam);
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
    // GDI raster of a hover bubble: 32-bit BGRA bits with the displayed alpha
    // baked in. Pure function of (text, scale); UpdateHoverLabel caches results
    // so fast cursor waggles blit instead of re-running font/DC churn per icon.
    [[nodiscard]] bool RasterizeHoverLabel(const std::wstring& text, float scale, SIZE& labelSize,
        std::vector<uint8_t>& bits);
    struct HoverLabelBits {
        SIZE size{};
        std::vector<uint8_t> pixels;
    };
    std::unordered_map<std::wstring, HoverLabelBits> m_hoverLabelCache;
    void DestroyHoverLabelFont() noexcept;
    [[nodiscard]] HFONT HoverLabelFont();
    struct IconLoadRequest {
        std::vector<std::wstring> cacheKeys;
        std::vector<std::vector<std::wstring>> candidates;
        UINT extent = 0;
    };
    void LoadIconTextures();
    [[nodiscard]] IconLoadRequest PrepareIconLoad();
    void ApplyIconLoad(const IconLoadRequest& request,
        const std::vector<std::vector<uint8_t>>& pixels);
    void AssignIconTextureIndices();
    void EnsureTrayIcons();
    void RefreshTray(bool forceLayout);
    void OnWeatherUpdated();
    void ApplyBluetoothSnapshot();
    [[nodiscard]] bool IsWeatherRenderIndex(int index) const noexcept;
    void OpenTraySlot(TraySlot slot);
    void ToggleOverflowPopup();
    void CloseOverflowPopup() noexcept;
    void RebuildOverflowPopup();
    void PaintOverflowPopup();
    void RefreshQuickSettingsCache();
    void MeasureQuickSettings(float scale, LONG padding, LONG gearSize, LONG headerHeight,
        LONG& panelWidth, LONG& contentHeight);
    void PaintQuickSettings(uint8_t* pixels, int width, int height, HDC memory, float scale,
        LONG padding, LONG panelWidth, LONG gearSize, LONG headerHeight, HFONT titleFont,
        HFONT sectionFont, HFONT labelFont, HFONT statusFont);
    void LayoutQuickSettings(bool draw, uint8_t* pixels, int width, int height, HDC memory,
        float scale, LONG padding, LONG gearSize, LONG headerHeight, HFONT titleFont,
        HFONT sectionFont, HFONT labelFont, HFONT statusFont, LONG& panelWidth, LONG& contentBottom);
    void OpenQuickSettingsPage(QuickSettingsPage page);
    void CloseQuickSettingsPage();
    void OpenSettingsPage(const wchar_t* uri);
    void ProjectBrightness(int percent);
    void ApplyQuickSettingsSlider(TrayFlyoutHitKind kind, const RECT& track, LONG x);
    void AdjustCaptureGain(float delta);
    void ApplyQuickSettingsCommand(const TrayFlyoutHit& hit, UINT message);
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
    void PaintSettingsHoverFast();
    void ApplySettingsHoverHighlight(uint8_t* pixels, int width, int height,
        const SettingsHit& hit) const;
    void PresentSettingsLayer() noexcept;
    struct LayerPresentDib {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
        void* bits = nullptr;
        LONG width = 0;
        LONG height = 0;
        bool contentValid = false;
    };
    [[nodiscard]] bool EnsureLayerPresentDib(LayerPresentDib& slot, LONG width, LONG height) noexcept;
    void ReleaseLayerPresentDib(LayerPresentDib& slot) noexcept;
    bool PresentLayeredBits(HWND window, const POINT& origin, LONG width, LONG height,
        const uint8_t* pixels, size_t byteCount, LayerPresentDib& slot,
        const RECT* dirty = nullptr, bool preservePopupShadow = false) noexcept;
    void QueueSettingsPaint(bool hoverOnly = false);
    [[nodiscard]] UINT PackPopupGlassFxFlags(bool dockFace, bool popupShadow = false) const noexcept;
    [[nodiscard]] bool TryBakePopupGlass(POINT origin, LONG width, LONG height,
        uint8_t* pixels, size_t byteCount, bool dockFace, bool popupShadow = false);
    void RebuildSettingsPopup();
    void HandleSettingsClick(const SettingsHit& hit, UINT message);
    void ApplyFrostSliderAt(LONG clientX);
    [[nodiscard]] int SettingsHitIndex(POINT point) const noexcept;
    [[nodiscard]] bool SettingsScreenOrigin(POINT& origin) const noexcept;
    void InvalidateSettingsGlass() noexcept;
    [[nodiscard]] bool SettingsGlassValid(POINT origin) const noexcept;
    void RefreshSettingsControls();
    void SetUpdateStatus(const std::wstring& status);
    void CheckForUpdatesAsync(bool manual);
    void ApplyUpdateResult(const UpdateReply& reply);
    // Hands off to the registered install when it is newer than this running
    // copy ("installed X but the app still reports Y"). Returns an exit code
    // when startup must not continue.
    [[nodiscard]] std::optional<int> HandOffToNewerInstalledCopy();
    // Reconciles the last launched install with the running version: clears
    // the record once the update took effect, or spends one retry from a
    // small budget so a silent failed replace is re-offered promptly instead
    // of hitting the 24 h reinstall-guard silence.
    void ReconcileLastUpdate();
    void StartUpdateTimer(UINT delayMs) noexcept;
    void StopUpdateTimer() noexcept;
    [[nodiscard]] bool IsDockSettingsOpen() const noexcept;
    [[nodiscard]] bool IsCursorOverSettings(POINT cursor) const noexcept;
    void EnsureOverflowGlyphs(UINT gearExtent, UINT tileExtent);
    void InvalidateOverflowGlass() noexcept;
    void EnsureOverflowFonts(float scale);
    void DestroyOverflowFonts() noexcept;
    [[nodiscard]] bool OverflowGlassValid(POINT origin) const noexcept;
    void PositionOverflowPopup();
    void BeginOverflowShow();
    // Re-present the last Home frame without a full glass/content rebuild.
    [[nodiscard]] bool TryPresentOverflowFromCache() noexcept;
    // Create HWND + bake Home off-screen so the first chevron click is warm.
    void PrewarmOverflowPopup() noexcept;
    // Timers + async QS workers without a full PaintOverflowPopup.
    void ArmOverflowLiveWorkers() noexcept;
    void BeginOverflowHide(bool animate) noexcept;
    void FinishOverflowHide() noexcept;
    void PresentOverflowLayer() noexcept;
    void QueueOverflowPaint(bool hoverOnly = false);
    void PaintOverflowHoverFast();
    // Meter/scrub-only present from underlay via UpdateLayeredWindowIndirect.
    void PaintOverflowLiveFast();
    void PaintQsLiveOverlays(uint8_t* pixels, int width, int height, float scale,
        bool includeTemps = true) const;
    void ApplyOverflowHoverHighlight(uint8_t* pixels, int width, int height,
        const TrayFlyoutHit& hit) const;
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
    // Returns true when a live bake was submitted or a completed bake was applied.
    [[nodiscard]] bool TickLivePopupGlass();
    [[nodiscard]] bool ApplyLivePopupGlass(std::vector<uint8_t>& glassBits, SIZE size,
        std::vector<uint8_t>& cachedGlass, std::vector<uint8_t>& baseBits,
        std::vector<uint8_t>& presentBits, SIZE& presentSize);
    void BeginHide();
    void AdvanceAnimation();
    bool RenderFrame(bool allowBlockingGpuWait = true);
    void QueueRenderFrame(bool allowBlockingGpuWait = true);
    void OnBackdropTick();
    void StartBackdropTimer() noexcept;
    void StopBackdropTimer() noexcept;
    [[nodiscard]] bool ArmBackdropFastTimer() noexcept;
    void DisarmBackdropFastTimer() noexcept;
    void SyncBackdropTimerInterval(bool wantFast, bool wantResting = false) noexcept;
    void HandlePointer(POINT cursor);
    void UpdateGlintTarget(POINT cursor);
    void TickGlint();
    void StopGlint() noexcept;
    void HandleContextMenu(POINT screenPoint);
    [[nodiscard]] std::vector<ContextItem> BuildContextItems(int icon, DisplayApp& outApp,
        bool& outHasApp, bool& outIsSpecial) const;
    void ShowContextMenu(POINT screenPoint, int icon);
    void CloseContextMenu() noexcept;
    void DestroyContextMenu() noexcept;
    void PositionContextMenu();
    void PaintContextMenu();
    void PaintContextHoverFast();
    void QueueContextPaint(bool hoverOnly = false);
    void ExecuteContextCommand(UINT command);
    [[nodiscard]] int ContextHitIndex(POINT point) const noexcept;
    [[nodiscard]] bool ContextScreenOrigin(POINT& origin) const;
    [[nodiscard]] bool IsContextMenuOpen() const noexcept;
    [[nodiscard]] bool IsCursorOverContextMenu(POINT cursor) const noexcept;
    void InvalidateContextGlass() noexcept;
    [[nodiscard]] bool ContextGlassValid(POINT origin) const noexcept;
    void EnsureContextFonts(float scale);
    void DestroyContextFonts() noexcept;
    void ActivatePressedApp();
    bool OpenLaunchPrompt();
    void CloseLaunchPrompt(bool hideDockIfAway = true);
    void PositionLaunchPrompt();
    void SetLaunchPromptStatus(const std::wstring& text);
    void SetLaunchPromptAnswer(const std::wstring& text);
    void SubmitLaunchPrompt();
    void ApplyLaunchJudgment(UINT generation, const LaunchJudgment& judgment);
    void ApplyAgentResult(UINT generation, const SearchAgentResult& result);
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
    [[nodiscard]] std::wstring TrayIconsKey() const;
    [[nodiscard]] bool TrayIconsNeedApply() const;
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
    [[nodiscard]] UINT DesiredTrayIntervalMs() const noexcept;
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
    void UnmarkFullscreenClaims();
    void HideOverlayForShellFlyout();
    void RestoreOverlayAfterShellFlyout();
    void ReleaseShellFlyoutHold();
    void StopShellFlyoutWatch() noexcept;
    void HideTaskbar();
    void RestoreTaskbar();
    void SuppressNativeTaskbar();
    [[nodiscard]] bool MaintainNativeTaskbarSuppression();
    [[nodiscard]] bool ExpandPrimaryWorkArea(bool notify);
    void CollectTaskbarWindows(std::vector<HWND>& taskbars);
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
    void StartCursorWatch() noexcept;
    void StopCursorWatch() noexcept;
    void EnsureMouseHook() noexcept;
    void InstallOverflowDismissHook() noexcept;
    void RemoveOverflowDismissHook() noexcept;
    void DismissQuickSettings();
    void PumpCursorWatch();
    [[nodiscard]] UINT DesiredCursorWatchIntervalMs() const noexcept;
    void SyncCursorWatchInterval() noexcept;
    void RegisterForegroundWatch() noexcept;
    void UnregisterForegroundWatch() noexcept;
    static void CALLBACK ForegroundWinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
        LONG idObject, LONG idChild, DWORD idEventThread, DWORD dwmsEventTime);
    [[nodiscard]] bool IsElevatedForeground() const noexcept;
    void ApplyPinUnpinLayoutChange();
    void RemapInteractionAfterLayoutChange(const std::wstring& pressedTarget,
        const std::wstring& draggedTarget);
    void RegisterSystemResumeNotifications();
    void UnregisterSystemResumeNotifications() noexcept;
    LRESULT HandlePowerBroadcast(WPARAM wParam, LPARAM lParam);
    void LogInputMouse(UINT message, POINT screenPoint, int icon) const;
    void Log(const std::wstring& message) const;

    [[nodiscard]] bool IsCursorInBottomHotZone(POINT cursor) const noexcept;
    // True when the foreground app is borderless / monitor-covering fullscreen on
    // the monitor under `cursor` (typical game borderless). Used to suppress
    // edge-show so the dock does not interrupt gameplay.
    [[nodiscard]] bool IsForegroundBorderlessFullscreenAt(POINT cursor) const noexcept;
    [[nodiscard]] int IconAtScreenPoint(POINT cursor) const noexcept;
    [[nodiscard]] int DividerAtScreenPoint(POINT cursor) const noexcept;
    [[nodiscard]] int InsertionIndexForDrag(POINT cursor) const noexcept;
    [[nodiscard]] bool HasCrossedDragThreshold(POINT cursor) const noexcept;
    [[nodiscard]] bool IsCursorOverDock(POINT cursor) const noexcept;
    [[nodiscard]] bool ShouldPostPointerUpdate(POINT cursor) const noexcept;
    [[nodiscard]] bool NeedsHookPointerPost(POINT cursor) const noexcept;
    [[nodiscard]] bool IsDragActive() const noexcept;
    [[nodiscard]] bool IsPersistentDisplayIcon(int icon) const noexcept;
    [[nodiscard]] LONG CurrentY() const noexcept;
    [[nodiscard]] bool IsAnimating() const noexcept;
    [[nodiscard]] double SecondsSinceAnimationStarted() const noexcept;
    [[nodiscard]] static double QpcSeconds();
    [[nodiscard]] UINT IconPixelExtent() const noexcept;
    void ReloadIconsIfExtentChanged();
    // --- Trash / Recycle Bin (macOS-style, left of Quick Settings) ---
    [[nodiscard]] bool IsTrashRenderIndex(int icon) const noexcept;
    [[nodiscard]] int TrashRenderIndex() const noexcept;
    [[nodiscard]] bool IsPointOverTrash(POINT screen) const noexcept;
    void RefreshTrash(bool forceLayout);
    void EnsureTrashIcons();
    void UpdateTrashIconState(bool full);
    void OpenTrash();
    void EmptyTrashWithConfirm();
    void ShowTrashProperties();
    void RegisterTrashNotify();
    void UnregisterTrashNotify() noexcept;
    void RegisterTrashDropTarget();
    void RevokeTrashDropTarget() noexcept;
    void OnTrashDragEnter();
    void OnTrashDragOver(POINT screen);
    void OnTrashDragLeave();
    [[nodiscard]] bool OnTrashDrop(const std::vector<std::wstring>& paths);
    void HandleTrashDropResult(const std::wstring& error, bool moved);
    [[nodiscard]] std::wstring TrashHoverText() const;

    HINSTANCE m_instance = nullptr;
    HWND m_window = nullptr;
    HWND m_inputWindow = nullptr;
    HWND m_hoverLabelWindow = nullptr;
    HWND m_dragGhostWindow = nullptr;
    HWND m_launchPromptWindow = nullptr;
    HWND m_launchAnswer = nullptr;  // multi-line Q&A strip above the Search edit
    HWND m_launchEdit = nullptr;
    HWND m_launchStatus = nullptr;
    HHOOK m_mouseHook = nullptr;
    HHOOK m_overflowDismissHook = nullptr;
    HFONT m_hoverLabelFont = nullptr;
    UINT m_hoverLabelFontDpi = 0;
    int m_hoverLabelFontPx = 0;
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
    // Retained across monitor passes so a resting dock does not heap-allocate
    // a fresh window list, collapse list, or monitor list on every tick.
    std::vector<HWND> m_taskbarEnumScratch;
    std::vector<std::pair<HWND, RECT>> m_collapsedTaskbarScratch;
    std::vector<std::pair<HMONITOR, RECT>> m_monitorEnumScratch;
    UINT m_taskbarCreatedMessage = 0;
    // Agent/perf: PostMessage RegisterWindowMessage(L"Hoverdock.OpenPerfMenus").
    UINT m_openPerfMenusMessage = 0;
    UINT m_closePerfMenusMessage = 0;
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
    double m_pressedAtTime = 0.0;
    double m_lastPointerSampleAt = 0.0;
    // QPC time of the last actual cursor position change (not calm watch polls).
    // Backdrop capture + TickLivePopupGlass skip while this is recent and the
    // dock is open (or QS / Dock Settings / context) so BitBlt / Present cannot
    // stall WH_MOUSE_LL mid-move — including pointer motion over empty desktop.
    double m_lastPointerMotionAt = 0.0;
    // Pointer-reactive rim glint (dock-client px). The eased values feed
    // DockRenderState; targets follow the cursor while it is over the dock.
    float m_glintX = 0.0F;
    float m_glintY = 0.0F;
    float m_glintStrength = 0.0F;
    float m_glintTargetX = 0.0F;
    float m_glintTargetY = 0.0F;
    float m_glintTargetStrength = 0.0F;
    double m_glintLastTickAt = 0.0;
    bool m_glintTimerRunning = false;
    bool m_suppressDragUntilRelease = false;
    bool m_launchClickInProgress = false;
    std::wstring m_pressedTarget;
    std::wstring m_draggedTarget;
    bool m_taskbarHidden = false;
    int m_taskbarMonitorQuietPasses = 0;
    bool m_taskbarMonitorFast = true;
    bool m_cursorWatchArmed = false;
    bool m_cursorWatchTimerRunning = false;
    bool m_cursorWatchCalm = false;
    UINT m_cursorWatchAppliedMs = 0;
    UINT m_cursorWatchStationaryPumps = 0;
    HWINEVENTHOOK m_foregroundHook = nullptr;
    bool m_taskbarStateSaved = false;
    UINT m_savedTaskbarState = 0;
    RECT m_savedWorkArea{};
    bool m_workAreaSaved = false;
    bool m_workAreaExpanded = false;
    RECT m_lastObservedWorkArea{};
    bool m_hasLastObservedWorkArea = false;
    // OpenProcess/token query is kernel-heavy. Cache it so a 33 ms elevated
    // cursor watch (or a taskbar-monitor sync) does not fault every tick.
    // Invalidated on kCursorWatchSyncMessage when the foreground changes.
    mutable bool m_elevatedForegroundValid = false;
    mutable bool m_elevatedForegroundCached = false;
    mutable double m_elevatedForegroundCheckedAt = 0.0;
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
    double m_lastWindowRefresh = 0.0;
    int m_hoverLabelIcon = -1;
    int m_dividerIndex = -1;
    UINT m_showSessionId = 0;
    // Deferred GPU-backed layout apply (UpdateInputRegion + reposition +
    // swapchain Resize): set when RebuildLayout changes the size, cleared by the
    // kLayoutApplyMessage handler. Coalesces bursts (scale-drag) so only the
    // latest dims are applied, keeping RebuildLayout itself under 1ms.
    bool m_layoutApplyPending = false;
    // Tray glyph upload pending (clock raster + atlas re-upload with GPU waits):
    // set by RebuildLayout when the tray visual key changes, serviced by the
    // same kLayoutApplyMessage handler so layout math stays under 1ms.
    bool m_trayIconsApplyPending = false;
    // Last rect applied by PositionOverlayWindows: skips redundant DWM
    // SetWindowPos round-trips so steady calls cost microseconds.
    bool m_overlayPosValid = false;
    LONG m_overlayX = 0;
    LONG m_overlayY = 0;
    LONG m_overlayW = 0;
    LONG m_overlayH = 0;
    // Render HWND during a slide: fixed rect covering the travel, while the
    // swap chain offset follows m_currentY. Input HWND stays pill-sized.
    LONG m_overlayRenderY = 0;
    LONG m_overlayRenderH = 0;
    UINT m_loadedIconExtent = 0;
    UINT m_shellFlyoutAttempts = 0;
    bool m_shellFlyoutIsSearch = false;
    bool m_shellFlyoutHold = false;
    bool m_overlayHiddenForFlyout = false;
    HWND m_shellFlyoutWindow = nullptr;
    double m_shellFlyoutHoldUntil = 0.0;
    // Adaptive hold phase for the Win+N panel: false while waiting for it to
    // appear after the click, true once seen (hold then lasts until it closes
    // plus a short slide-out grace).
    bool m_shellFlyoutSeen = false;
    double m_suppressBackdropUntil = 0.0;
    UINT m_backdropTimerAppliedMs = 0;
    // High-resolution waitable timer for the 8 ms backdrop cadence. SetTimer
    // cannot fire faster than USER_TIMER_MINIMUM (10 ms), so it never reaches
    // 120 Hz. Armed only while the dock or an open plate is tracking motion.
    HANDLE m_backdropWaitable = nullptr;
    bool m_backdropFastArmed = false;
    UINT m_backdropIdleStreak = 0;
    bool m_backdropCaptureWasIdle = false;
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
    // Last submitted Search query (for last-goal replay memory).
    std::wstring m_launchRequest;
    WNDPROC m_launchEditPrevious = nullptr;
    InstalledAppCatalog m_installedApps;
    SystemTray m_tray;
    WeatherService m_weather;
    BluetoothService m_bluetooth;
    BluetoothSnapshot m_bluetoothSnapshot;
    HWND m_overflowWindow = nullptr;
    VisibilityState m_overflowVisibility = VisibilityState::Hidden;
    std::vector<TrayNotifyIcon> m_overflowIcons;
    bool m_overflowIconsLoaded = false;
    // When true, PresentOverflowLayer uploads pixels but does not SW_SHOWNA.
    bool m_overflowPresentSuppressShow = false;
    std::vector<TrayFlyoutHit> m_overflowHits;
    int m_overflowHover = -1;
    QuickSettingsPage m_qsPage = QuickSettingsPage::Home;
    QuickSettingsPage m_qsReturn = QuickSettingsPage::Home;
    QuickSettingsCache m_qsCache{};
    bool m_qsDragging = false;
    bool m_qsDragMoved = false;
    TrayFlyoutHitKind m_qsDragKind = TrayFlyoutHitKind::None;
    RECT m_qsDragBounds{};
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
    bool m_settingsHoverPaintOnly = false;
    std::vector<uint8_t> m_settingsBaseBits;
    std::vector<uint8_t> m_settingsPresentBits;
    LayerPresentDib m_settingsLayerDib{};
    ULONGLONG m_lastSettingsHoverPresentMs = 0;
    RECT m_settingsHoverDirty{};
    bool m_settingsHoverDirtyValid = false;
    SIZE m_settingsPresentSize{};
    bool m_frostSliderDragging = false;
    ULONGLONG m_frostSliderLastRenderMs = 0;
    // Last live rebake of open menu glass (Quick Settings / Dock Settings / context).
    ULONGLONG m_lastPopupGlassRefreshMs = 0;
    // After a confirmed unchanged capture, wait this long before BitBlt again.
    static constexpr ULONGLONG kLivePopupStaticIntervalMs = 250;
    bool m_livePopupGlassStatic = false;
    // Skip live menu BitBlt/GPU until dock backdrop reports a desktop change
    // (or kLivePopupForcedRefreshMs elapses). Keeps glass live over moving
    // wallpaper without 120 Hz rebakes on a static desktop.
    static constexpr ULONGLONG kLivePopupForcedRefreshMs = 8000;
    uint64_t m_livePopupBackdropSerial = 0;
    // Round-robin target for async menu glass rebakes (0=settings,1=overflow,2=context).
    int m_livePopupGlassTarget = 0;
    int m_livePopupGlassPending = -1;
    // Plate still owes a GPU bake after a dirty desktop frame (0 settings, 1 quick
    // settings, 2 context). Cleared when the bake is submitted or the pixels match.
    bool m_popupGlassDirty[3] = {};
    RECT m_frostSliderTrack{};
    SIZE m_settingsSize{};
    std::vector<uint8_t> m_settingsGlass;
    SIZE m_settingsGlassSize{};
    POINT m_settingsGlassOrigin{};
    float m_settingsGlassFrost = -1.0F;
    std::wstring m_updateStatus = L"Checking for updates...";
    std::atomic<bool> m_updateInFlight{false};
    std::atomic<bool> m_updateInstalling{false};
    PerfProfiler m_perfProfiler;
    std::wstring m_perfStatus;
    // Set only after a profile finishes writing. Clicking the path opens it.
    std::wstring m_perfSavedLog;
    double m_lastUpdateCheck = 0.0;
    std::wstring m_overflowGlyphKey;
    std::vector<uint8_t> m_overflowGlyphGear;
    // Idle gear blit rect (top-left + extent). Hover scales about this center.
    int m_overflowGearX = 0;
    int m_overflowGearY = 0;
    UINT m_overflowGearExtent = 0;
    std::vector<uint8_t> m_overflowGlyphWifi;
    std::vector<uint8_t> m_overflowGlyphSound;
    std::vector<uint8_t> m_overflowGlyphBrightness;
    std::vector<uint8_t> m_overflowGlyphBoost;
    // Quick Settings Boost tile status, UI thread only. Updated through
    // kBoostResultMessage so the worker never touches popup state.
    // Tile-sized: keep statuses short (~12 chars) so the narrow Boost
    // circle shows key info instead of an ellipsized cut-off.
    std::wstring m_boostStatus = L"Tap to boost";
    std::atomic<bool> m_boostInFlight{false};
    SIZE m_overflowSize{};
    LONG m_overflowCaretX = 0;
    std::vector<uint8_t> m_overflowBaseBits;
    // Static plate without live meter/scrub values; live present restores from here.
    std::vector<uint8_t> m_overflowUnderlayBits;
    std::vector<uint8_t> m_overflowPresentBits;
    RECT m_qsMeterRect{};
    RECT m_qsScrubRect{};
    bool m_qsMeterValid = false;
    bool m_qsScrubValid = false;
    // Home temperature tile value rects (live overlay) and the ink they were
    // laid out with, so a fast present matches the full paint exactly.
    RECT m_qsTempCpuRect{};
    RECT m_qsTempGpuRect{};
    bool m_qsTempsValid = false;
    // Set when a new reading arrives; the next fast present restores/redraws
    // the value rects. Mic-meter frames leave them untouched.
    bool m_qsTempsDirty = false;
    uint8_t m_qsTempsInkR = 0;
    uint8_t m_qsTempsInkG = 0;
    uint8_t m_qsTempsInkB = 0;
    // When true, LayoutQuickSettings paints meter/scrub tracks at empty levels and
    // records their rects for PaintOverflowLiveFast.
    bool m_qsPaintUnderlayPass = false;
    LayerPresentDib m_overflowLayerDib{};
    ULONGLONG m_lastOverflowHoverPresentMs = 0;
    RECT m_overflowHoverDirty{};
    bool m_overflowHoverDirtyValid = false;
    SIZE m_overflowPresentSize{};
    bool m_overflowHoverPaintOnly = false;
    std::vector<uint8_t> m_overflowGlass;
    SIZE m_overflowGlassSize{};
    POINT m_overflowGlassOrigin{};
    LONG m_overflowGlassCaretX = 0;
    float m_overflowGlassFrost = -1.0F;
    float m_overflowFontScale = 0.0F;
    HFONT m_overflowTitleFont = nullptr;
    HFONT m_overflowSectionFont = nullptr;
    HFONT m_overflowLabelFont = nullptr;
    HFONT m_overflowStatusFont = nullptr;
    HWND m_contextWindow = nullptr;
    std::vector<ContextItem> m_contextItems;
    std::vector<ContextHit> m_contextHits;
    std::vector<std::vector<uint8_t>> m_contextGlyphs;
    int m_contextHover = -1;
    bool m_contextPaintQueued = false;
    SIZE m_contextSize{};
    POINT m_contextOrigin{};
    POINT m_contextAnchor{};
    DisplayApp m_contextApp;
    bool m_contextHasApp = false;
    bool m_contextIsSpecial = false;
    std::vector<uint8_t> m_contextGlass;
    SIZE m_contextGlassSize{};
    POINT m_contextGlassOrigin{};
    float m_contextGlassFrost = -1.0F;
    std::vector<uint8_t> m_contextBaseBits;
    std::vector<uint8_t> m_contextPresentBits;
    LayerPresentDib m_contextLayerDib{};
    RECT m_contextHoverDirty{};
    bool m_contextHoverDirtyValid = false;
    SIZE m_contextPresentSize{};
    bool m_contextHoverPaintOnly = false;
    float m_contextFontScale = 0.0F;
    HFONT m_contextLabelFont = nullptr;
    // --- Trash / Recycle Bin state (UI thread only unless noted) ---
    struct TrashState {
        ULONGLONG itemCount = 0;
        ULONGLONG byteSize = 0;
        bool isEmpty = true;
    };
    TrashState m_trashState;
    bool m_trashFull = false;
    int m_trashIndex = -1;
    UINT m_loadedTrashExtent = 0;
    bool m_trashDragOver = false;
    bool m_trashDropInFlight = false;
    ULONG m_trashNotifyCookie = 0;
    TrashPidl m_trashNotifyPidl = nullptr;
    IDropTarget* m_trashDropTarget = nullptr;
    bool m_trashDropOnRenderer = false;
    bool m_trashDropOnInput = false;
    bool m_contextIsTrash = false;

    static DockApp* s_instance;
    bool m_shellFlyoutIsTray = false;
    std::wstring m_trayVisualKey;
};
