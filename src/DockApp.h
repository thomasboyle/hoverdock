#pragma once

#include "DockConfig.h"
#include "AiAnswerView.h"
#include "AiAttachments.h"
#include "GroqClient.h"
#include "InstalledApps.h"
#include "NotificationBadges.h"
#include "Renderer.h"
#include "TypeSafeClient.h"
#include "FileSearch.h"
#include "SearchHotkey.h"
#include "WindowCatalog.h"
#include "SystemTray.h"
#include "Bluetooth.h"
#include "QuickSettingsPanel.h"
#include "Weather.h"
#include "SpeedTest.h"
#include "PerfProfiler.h"
#include "PerfBoost.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
        // Pinned app alive with no visible window (minimised to the tray).
        bool runningInBackground = false;

        [[nodiscard]] bool IsRunning() const noexcept {
            return runningWindow != nullptr || runningInBackground;
        }
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
        // Catalog-event epoch observed by the worker. Apply clears the hidden
        // dirty flag only when no newer show/hide arrived during the enum.
        UINT catalogEpoch = 0;
        UINT refreshGeneration = 0;
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

    // Spotlight-style Search panel rows.
    struct SearchRow {
        // Link opens `path` (a URL); Ai asks the AI about the query.
        enum class Kind : uint8_t { App, File, Folder, Web, EnableMft, Info, Link, Ai };
        Kind kind = Kind::Info;
        std::wstring title;
        std::wstring subtitle;
        std::wstring path;
        size_t appIndex = 0;
        // Icon cache key + what to extract it from (empty: drawn glyph).
        std::wstring iconKey;
        std::wstring iconSource;
    };

    struct SearchAppsReply {
        std::vector<LaunchTarget> targets;
        std::vector<std::wstring> names;
    };

    // What the Search panel is showing: results, an AI answer, or a one-line
    // entry for a Dock Settings value (Groq API key / AI model).
    enum class SearchMode : uint8_t { Search, Ai, KeyEntry, ModelEntry };

    struct SearchIconReply {
        UINT extent = 0;
        std::vector<std::pair<std::wstring, std::vector<uint8_t>>> icons;
    };

    struct BoostReply {
        std::wstring status;
        bool finished = true;
        // Set on the finished reply: what the run did, for the Boost page.
        std::optional<BoostRunReport> report;
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
    static constexpr UINT kSearchResultMessage = WM_APP + 6;
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
    static constexpr UINT kSearchStatusMessage = WM_APP + 28;
    static constexpr UINT kSearchHotkeyMessage = WM_APP + 36;
    static constexpr UINT kSearchIconMessage = WM_APP + 37;
    static constexpr UINT kSearchAppsMessage = WM_APP + 38;
    static constexpr UINT kSearchMftAccessMessage = WM_APP + 39;
    // AI answer progress: wParam = AiSession id (state read from the session).
    static constexpr UINT kAiUpdateMessage = WM_APP + 40;
    // AI answer image decoded: lParam owns an AiImageReply.
    static constexpr UINT kAiImageMessage = WM_APP + 41;
    // Dock Settings AI rows: wParam = SearchMode to open Search in.
    static constexpr UINT kOpenSearchEntryMessage = WM_APP + 42;
    // AI image attachment prepared: lParam owns an AiAttachmentReply.
    static constexpr UINT kAiAttachmentMessage = WM_APP + 43;
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
    // Adaptive chrome ink crossfade (macOS menu bar black/white switch).
    static constexpr UINT_PTR kChromeInkTimerId = 22;
    static constexpr UINT_PTR kBluetoothTimerId = 16;
    static constexpr UINT_PTR kEnergySampleTimerId = 17;
    static constexpr UINT_PTR kQsLiveTimerId = 18;
    static constexpr UINT_PTR kQsTempsTimerId = 19;
    // Home Wi-Fi / Bluetooth tile arrow fade; runs only while an arrow is fading.
    static constexpr UINT_PTR kQsChevronFadeTimerId = 20;
    // Home module drag-to-rearrange animation frames (~60 Hz while moving).
    static constexpr UINT_PTR kQsReorderTimerId = 21;
    // Post-wake topmost re-assertion: Explorer/DWM reshuffle z-order for a
    // few seconds after resume/unlock, which can sink the dock under windows.
    static constexpr UINT_PTR kResumeZOrderTimerId = 23;
    static constexpr UINT kResumeZOrderIntervalMs = 1000;
    static constexpr int kResumeZOrderPasses = 4;
    static constexpr UINT kQsChevronFadeIntervalMs = 16;
    static constexpr float kQsChevronFadeMs = 140.0F;
    // Home tiles with a hover arrow: Wi-Fi, Bluetooth, Boost, Power, Sound,
    // Display, System Tray, Mic.
    static constexpr size_t kQsChevronTiles = 8;
    // Pointer glint easing cadence (~60 Hz); the timer only runs while the
    // glint is still converging on the cursor, so an idle dock costs nothing.
    static constexpr UINT kGlintIntervalMs = 16;
    static constexpr UINT kUpdateIntervalMs = 6U * 60U * 60U * 1000U;
    static constexpr UINT kUpdateInitialDelayMs = 15000;
    static constexpr UINT kCursorWatchIntervalMs = 33;
    // 0 = no timer while Hidden with a healthy LL hook (event-driven via hook + FG WinEvent).
    static constexpr UINT kCursorWatchHiddenIntervalMs = 0;
    static constexpr UINT kDeferredRefreshDelayMs = 400;
    // Used only when the host refresh rate cannot be read. While the dock is
    // visible the backdrop timer tracks that monitor (BackdropFrameIntervalMs).
    static constexpr UINT kBackdropIntervalMs = 8;
    static constexpr UINT kBackdropIdleIntervalMs = 250;
    static constexpr UINT kBackdropRestingIntervalMs = 100;
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
    static constexpr UINT kOpenDockSettingsMessage = WM_APP + 29;
    static constexpr UINT kHiddenCatalogMessage = WM_APP + 30;
    // Refresh-rate worker finished: wParam = Hz, lParam = 1 on success.
    static constexpr UINT kQsRefreshResultMessage = WM_APP + 31;
    // Tray icon worker finished: lParam owns a std::vector<TrayNotifyIcon>.
    static constexpr UINT kTrayIconsMessage = WM_APP + 32;
    // Notification badge set changed (NotificationBadges worker, no payload).
    static constexpr UINT kBadgeMessage = WM_APP + 33;
    // Speed test progress / result (SpeedTestService, ~5 Hz while running).
    static constexpr UINT kSpeedTestMessage = WM_APP + 34;
    // Foreground window changed: wParam = HWND, clears that app's toast badge.
    static constexpr UINT kBadgeSeenMessage = WM_APP + 35;

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
        // Home Bluetooth tile outside its glyph disc: opens the menu.
        BluetoothPage,
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
        // Home Wi-Fi tile glyph disc (radio toggle) and the rest of the tile (menu).
        WifiRadio,
        WifiPage,
        // Collapsible "Other Networks" / "Other Devices" rows on the drill-ins.
        WifiOthers,
        BluetoothOthers,
        // Home Boost tile outside its glyph disc: opens the last-run report.
        BoostPage,
        // Refresh Rate menu row; index is the rate in Hz.
        RefreshRate,
        // Home "Edit Controls" / "Done" capsule (macOS 26 edit mode).
        QsEditControls,
        // System Tray page row arrow: opens the item's menu (its right-click).
        TrayIconOptions,
        // Wi-Fi page "Run Speed Test" row.
        SpeedTest,
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
        HandsFree,
        LightPanels,
        CheckNow,
        PerfProfile,
        PerfLog,
        Close,
        QsColorSwatch,
        QsColorHue,
        QsColorSat,
        QsColorVal,
        QsColorReset,
        AiApiKey,
        AiModel,
    };

    struct SettingsHit {
        SettingsHitKind kind = SettingsHitKind::None;
        RECT bounds{};
        int index = -1;
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
    static LRESULT CALLBACK SearchWindowProcedure(HWND window, UINT message, WPARAM wParam,
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
    void ReassertOverlayZOrder();
    void ScheduleResumeZOrderReassert();
    void UpdateHoverLabel(bool forceGlassRefresh = false);
    void HideHoverLabel() noexcept;
    // Liquid-glass hover label: BakeGlassPanel plate + flyout ink glyphs.
    // screenOrigin is the layered-window top-left (label sits above the dock).
    // Height is uniform for every label; width follows text up to a max with
    // ellipsis. Not cached — glass samples the live backdrop at that origin.
    [[nodiscard]] bool RasterizeHoverLabel(const std::wstring& text, float scale,
        POINT screenOrigin, SIZE& labelSize, std::vector<uint8_t>& bits);
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
    // Notification badges (red unread counts on app icons).
    void OnBadgesUpdated();
    // Clears toast-count badges of the dock app owning `foreground`.
    void MarkBadgesSeenFor(HWND foreground);
    // Re-match badge sources to dock entries; queues the raster upload when the
    // visuals changed. Returns true when the visible badge set changed.
    bool RefreshBadgeAssignments();
    void AssignBadgeTextureIndices();
    void EnsureBadgeIcons();
    [[nodiscard]] std::wstring BadgeIconsKey() const;
    [[nodiscard]] bool BadgeIconsNeedApply() const;
    [[nodiscard]] static std::vector<uint8_t> RasterizeBadge(const std::wstring& text, LONG width,
        LONG height, UINT atlasExtent);
    [[nodiscard]] static RECT BadgeBounds(const RECT& iconBounds, LONG width, LONG height) noexcept;
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
    // Send the scrub-drag preview as one GSMTC seek (button up / lost capture).
    void CommitQuickSettingsMediaSeek();
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
    // allowCc=false keeps the plain dock material even while Dock Settings has CC shapes.
    [[nodiscard]] UINT PackPopupGlassFxFlags(bool dockFace, bool popupShadow = false,
        bool allowCc = true) const noexcept;
    // `alsoExclude` keeps another visible popup (Search) out of the sample.
    [[nodiscard]] bool TryBakePopupGlass(POINT origin, LONG width, LONG height,
        uint8_t* pixels, size_t byteCount, bool dockFace, bool popupShadow = false,
        HWND alsoExclude = nullptr);
    void RebuildSettingsPopup();
    void HandleSettingsClick(const SettingsHit& hit, UINT message);
    void ApplyFrostSliderAt(LONG clientX);
    // Quick Settings icon colour picker (Dock Settings). Drag maps x on the
    // active hue / saturation / value track; Set applies live to Quick Settings.
    void ApplyQsColorDragAt(LONG clientX);
    void SetQsIconColorLive(unsigned rgb);
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
    // macOS style arrow that fades in on the hovered Home Wi-Fi / Bluetooth tile.
    int QsChevronTarget() const noexcept;
    void KickQsChevronFade() noexcept;
    void StepQsChevronFade();
    void ResetQsChevronFade() noexcept;
    // Exact square the tile arrow glyph occupies (window px).
    RECT QsChevronGlyphRect(size_t tile) const noexcept;
    // Restore drawn arrows in the present frame from the arrow-free base. With
    // `within`, only arrows overlapping it are cleared. Returns a tile bitmask.
    unsigned ClearQsTileChevrons(RECT* dirty, const RECT* within = nullptr) noexcept;
    void PaintQsTileChevrons(uint8_t* pixels, int width, int height,
        unsigned tiles = (1U << kQsChevronTiles) - 1U);
    // Arrow-only present for fade frames (arrow glyph squares, nothing else).
    void PaintQsChevronFadeFast();
    // Signatures of what the live meter / scrub would draw; equal means no pixel change.
    void QsLiveSignatures(int64_t& meter, int64_t& scrub) const noexcept;
    // Now Playing time line ("1:23 / 3:45"); -1 when the media module is not laid out.
    int64_t QsMediaTimeSignature() const noexcept;
    // Meter/scrub-only present from underlay via UpdateLayeredWindowIndirect.
    void PaintOverflowLiveFast();
    void PaintQsLiveOverlays(uint8_t* pixels, int width, int height, float scale,
        bool includeTemps = true, bool includeBoost = true, bool includeBluetooth = true,
        bool includeMeter = true, bool includeScrub = true, bool includeTray = true,
        bool includeMediaTime = true) const;
    // "N items" for the Home System Tray tile ("..." until the first scan).
    [[nodiscard]] std::wstring TrayItemCountText() const;
    // Scan tray icons (UI Automation) on a worker; kTrayIconsMessage applies.
    void RequestTrayIconsAsync();
    void ApplyTrayIcons(std::vector<TrayNotifyIcon>&& icons);
    // Hover/dirty extent of a hit: tray option arrows light their whole row.
    [[nodiscard]] RECT OverflowHoverBounds(const TrayFlyoutHit& hit) const noexcept;
    // Hovered System Tray row (-1 none) and its fading row arrows.
    [[nodiscard]] int QsTrayArrowTarget() const noexcept;
    void PaintQsTrayArrows(uint8_t* pixels, int width, int height);
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
    void NoteHostRefreshHz() noexcept;
    [[nodiscard]] UINT BackdropFrameIntervalMs() const noexcept;
    void BeginShow();
    void CatchUpHiddenCatalog();
    void PumpCatalogMessages();
    void FlushPendingLayout();
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
    // Re-measure the desktop behind the tray + clock group and start the
    // black/white ink crossfade when it crosses the hysteresis band.
    void UpdateChromeInkTarget();
    void TickChromeInk();
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
    // Spotlight Search (DockSearch.cpp): glass panel centred above the dock,
    // Win+Space, MFT-backed file index + app catalogue.
    bool OpenSearch();
    void CloseSearch(bool hideDockIfAway = true);
    void ToggleSearch();
    [[nodiscard]] bool IsSearchOpen() const noexcept;
    void PositionSearch();
    void PaintSearch();
    void SearchTextChanged();
    void ApplySearchReply(std::unique_ptr<FileSearchReply> reply);
    void ApplySearchApps(std::unique_ptr<SearchAppsReply> reply);
    void ApplySearchIcons(std::unique_ptr<SearchIconReply> reply);
    void RefreshSearchApps();
    void RequestSearchIcons();
    void ActivateSearchRow(int index, bool reveal);
    void MoveSearchSelection(int delta);
    bool HandleSearchKey(WPARAM key);
    void HandleSearchChar(wchar_t character);
    [[nodiscard]] int SearchRowAt(POINT client) const noexcept;
    [[nodiscard]] bool SearchScreenOrigin(POINT& origin, SIZE& size, float& scale) const;
    void EnsureSearchFonts(float scale);
    void DestroySearchFonts() noexcept;
    void StartSearchServices();
    void StopSearchServices() noexcept;
    // Search panel glass: the dock's own material and corner (live-refreshed
    // by TickLivePopupGlass as target 3).
    [[nodiscard]] float DockCornerRadiusPx(float scale) const noexcept;
    // Dock face + macOS Spotlight material (wide frost, vibrancy, light/dark fill).
    [[nodiscard]] UINT SearchGlassFxFlags() const noexcept;
    static bool SystemUsesLightApps() noexcept;
    [[nodiscard]] bool BakeSearchGlass(POINT origin, SIZE size, uint8_t* pixels, size_t bytes);
    [[nodiscard]] bool SearchGlassLiveReady(POINT& origin, SIZE& size) const;
    void ApplySearchLiveGlass(std::vector<uint8_t>& glass);
    // AI answers in Search (Ctrl+Enter), DockSearch.cpp.
    void AskAi();
    void ApplyAiUpdate(uint64_t sessionId);
    void ApplyAiImage(std::unique_ptr<AiImageReply> image);
    void RequestAiImages();
    // Stops the answer and returns to plain Search state (no repaint/query).
    void ExitAiState() noexcept;
    void PaintSearchAi(uint8_t* pixels, int width, int height, float scale, LONG listTop);
    bool HandleAiClick(POINT client);
    [[nodiscard]] int AiHoverTarget(POINT client) const;
    [[nodiscard]] bool AiPointerIsLink(POINT client) const;
    void ScrollAi(int delta);
    void CopyAiAnswer();
    // Image attachments for the AI question (paste, picker), DockSearch.cpp.
    void AddAiAttachments(std::vector<AiAttachmentSource> sources);
    void ApplyAiAttachment(std::unique_ptr<AiAttachmentReply> reply);
    void RemoveAiAttachment(size_t index);
    void ClearAiAttachments();
    void PickAiAttachments();
    [[nodiscard]] LONG AttachStripHeight(float scale) const noexcept;
    void PaintAttachStrip(uint8_t* pixels, int width, int height, float scale, LONG top, LONG left);
    void OpenSearchEntry(SearchMode mode);
    void BuildSearchEntryRows();
    void CommitSearchEntry();
    void ExitSearchEntry(bool refresh);
    [[nodiscard]] std::vector<LaunchTarget> CollectLaunchTargets(
        const std::vector<DisplayApp>& displayApps,
        const std::vector<RunningWindow>& runningWindows) const;
    [[nodiscard]] LaunchCandidate MakeLaunchCandidate(const LaunchTarget& target) const;
    bool ActivateLaunchTarget(const LaunchTarget& target);
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
    static void CALLBACK CatalogWinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
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
    HWND m_searchWindow = nullptr;
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
    // Agent hooks (no synthetic input): RegisterWindowMessage
    // "Hoverdock.OpenQsPage" (wParam 0 = Home, 1 = Bluetooth) and
    // "Hoverdock.BluetoothTest" (only honored with HOVERDOCK_BT_DRYRUN=1;
    // wParam 1 = radio toggle, 2 = connect/disconnect paired[lParam],
    // 3 = scan start/stop, 4 = pair discovered[lParam]).
    UINT m_openQsPageMessage = 0;
    UINT m_bluetoothTestMessage = 0;
    void RepaintBluetoothStatus();
    // Boost status/report changed: live tile overlay on Home, full repaint on the Boost page.
    void RepaintBoostStatus();
    // Home module drag-to-rearrange. A press on a module arms a reorder; moving
    // past the drag threshold snapshots every module over a tile-free glass
    // backdrop, the module follows the cursor and the others spring into the
    // nearest gap-free layout. Release settles everything, then saves.
    void SyncQsModuleOrder();
    [[nodiscard]] int QsModuleAt(POINT point) const noexcept;
    [[nodiscard]] RECT QsModuleSlot(size_t module, const QsGridCell& cell) const noexcept;
    bool BeginQsReorder(int module, POINT cursor);
    void UpdateQsReorder(POINT cursor);
    void EndQsReorder(bool commit);
    void StepQsReorder();
    void PresentQsReorderFrame();
    void FinishQsReorder();
    // Overflow hide: drop reorder state (a released drag still saves).
    void AbortQsReorder() noexcept;
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
    // Bluetooth QS page: last UI interaction / last periodic refresh (QPC seconds).
    double m_lastBluetoothUiAt = 0.0;
    double m_lastBluetoothRefreshAt = 0.0;
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
    // Adaptive chrome ink: 0 = white ink, 1 = black ink. m_chromeInk eases
    // toward m_chromeInkTarget; unknown until the first backdrop sample.
    float m_chromeInk = 0.0F;
    float m_chromeInkTarget = 0.0F;
    bool m_chromeInkKnown = false;
    bool m_chromeInkTimerRunning = false;
    double m_chromeInkLastTickAt = 0.0;
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
    HWINEVENTHOOK m_catalogHook = nullptr;
    bool m_inBeginShow = false;
    std::atomic<bool> m_hiddenCatalogDirty{false};
    std::atomic<UINT> m_hiddenCatalogEpoch{0};
    UINT m_hostRefreshHz = 60;
    UINT m_backdropFastPeriodMs = 0;
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
    // Badge raster upload pending; serviced by the kLayoutApplyMessage handler.
    bool m_badgeIconsApplyPending = false;
    // Last rect applied by PositionOverlayWindows: skips redundant DWM
    // SetWindowPos round-trips so steady calls cost microseconds.
    bool m_overlayPosValid = false;
    int m_resumeZOrderPasses = 0;
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
    // High-resolution waitable timer paced to the host monitor refresh.
    // SetTimer cannot fire faster than USER_TIMER_MINIMUM (10 ms). Armed while
    // the dock is visible so the glass tracks that panel's refresh rate.
    HANDLE m_backdropWaitable = nullptr;
    bool m_backdropFastArmed = false;
    UINT m_backdropIdleStreak = 0;
    bool m_backdropCaptureWasIdle = false;
    bool m_dropPresentPending = false;
    std::atomic<bool> m_refreshInFlight{false};
    // Retry of pinned apps whose icon extraction failed (see BeginBackgroundRefresh).
    double m_lastIconRetry = 0.0;
    double m_iconRetryDelay = 4.0;
    std::atomic<UINT> m_refreshGeneration{0};
    std::mutex m_refreshSnapshotMutex;
    std::optional<RefreshSnapshot> m_refreshSnapshot;
    std::atomic<UINT> m_pinIconGeneration{0};
    std::mutex m_pinIconMutex;
    std::optional<PinIconResult> m_pinIconPending;
    // ---- Spotlight Search ----
    FileSearchService m_fileSearch;
    SearchHotkey m_searchHotkey;
    std::wstring m_searchText;
    size_t m_searchCaret = 0;
    bool m_searchSelectAll = false;
    bool m_searchCaretVisible = true;
    std::vector<SearchRow> m_searchRows;
    int m_searchSelected = 0;
    int m_searchScroll = 0;
    uint64_t m_searchRequestId = 0;
    std::vector<LaunchTarget> m_searchApps;
    bool m_searchAppsInFlight = false;
    std::unordered_map<std::wstring, std::vector<uint8_t>> m_searchIcons;
    std::unordered_set<std::wstring> m_searchIconsMissing;
    bool m_searchIconsInFlight = false;
    UINT m_searchIconExtent = 0;
    bool m_searchMftPrompting = false;
    bool m_searchClosing = false;
    // Last client mouse position: rows only follow real pointer movement.
    POINT m_searchMouse{-1, -1};
    // Glass plates per panel height (origin is fixed while open; rows grow down).
    std::unordered_map<LONG, std::vector<uint8_t>> m_searchGlass;
    POINT m_searchGlassOrigin{};
    POINT m_searchOrigin{};
    SIZE m_searchSize{};
    std::vector<uint8_t> m_searchBits;
    std::vector<RECT> m_searchRowRects;
    float m_searchFontScale = 0.0F;
    HFONT m_searchQueryFont = nullptr;
    HFONT m_searchTitleFont = nullptr;
    HFONT m_searchSubFont = nullptr;
    HFONT m_searchGlyphFont = nullptr;
    HFONT m_searchBigGlyphFont = nullptr;
    HFONT m_searchSmallGlyphFont = nullptr;
    LayerPresentDib m_searchLayerDib{};
    // ---- Search AI answers ----
    SearchMode m_searchMode = SearchMode::Search;
    std::wstring m_searchSavedText;
    bool m_aiAskAfterKey = false;
    std::unique_ptr<AiAnswerView> m_aiView;
    std::shared_ptr<AiSession> m_aiSession;
    uint64_t m_aiSessionCounter = 0;
    AiSnapshot m_aiState;
    std::wstring m_aiModel;
    bool m_aiNeedsKey = false;
    int m_aiScroll = 0;
    int m_aiSpinner = 0;
    bool m_aiShowSources = false;
    int m_aiSourcesScroll = 0;
    int m_aiHover = -1;
    RECT m_aiViewport{};
    RECT m_aiBadge{};
    RECT m_aiKeyButton{};
    std::vector<RECT> m_aiSourceRects;
    struct AiAttachment {
        uint64_t id = 0;
        std::wstring name;
        std::string dataUrl;  // empty while preparing
        std::vector<uint8_t> thumb;
        UINT thumbExtent = 0;
    };
    std::vector<AiAttachment> m_aiAttachments;
    uint64_t m_aiAttachmentCounter = 0;
    // Ctrl+Enter pressed while images were still being prepared.
    bool m_aiAskWhenReady = false;
    std::wstring m_aiAttachNotice;
    bool m_searchPickerOpen = false;
    // Windows "app mode" light theme, read when Search opens (Spotlight fill).
    bool m_searchLightTheme = false;
    RECT m_searchAttachButton{};
    std::vector<RECT> m_aiAttachRemoveRects;
    InstalledAppCatalog m_installedApps;
    SystemTray m_tray;
    WeatherService m_weather;
    SpeedTestService m_speedTest;
    NotificationBadges m_badges;
    struct DockBadge {
        size_t iconIndex = 0;
        std::wstring text;
        LONG width = 0;
        LONG height = 0;
        // Atlas key (one slot per visible badge, replaced in place).
        std::wstring textureKey;
        UINT textureIndex = 0;
        bool ready = false;
    };
    std::vector<NotificationBadges::Badge> m_badgeSources;
    std::vector<DockBadge> m_dockBadges;
    std::wstring m_badgeVisualKey;
    // (target, window) -> index into m_badgeSources, -1 for no badge. Cleared
    // when the sources change so RebuildLayout re-matches in O(entries).
    std::unordered_map<std::wstring, int> m_badgeMatchMemo;
    // m_iconRenderData plus badge instances; reused so frames do not allocate.
    std::vector<DockIconRenderData> m_frameIcons;
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
    // Live glass targets: 0 Dock Settings, 1 Quick Settings, 2 context menu, 3 Search.
    bool m_popupGlassDirty[4] = {};
    RECT m_frostSliderTrack{};
    // Dock Settings Control Centre glass shapes (window px) and picker state.
    std::vector<Renderer::CcShapeDesc> m_dsCcDescs;
    std::vector<Renderer::CcShapeDesc> m_qsCcDescs;
    float m_dsHue = 0.0F;
    float m_dsSat = 0.0F;
    float m_dsVal = 1.0F;
    bool m_dsHsvValid = false;
    SettingsHitKind m_dsDragKind = SettingsHitKind::None;
    RECT m_dsColorTracks[3]{};
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
    // Most recent finished boost run (UI thread) and when it arrived.
    std::optional<BoostRunReport> m_boostReport;
    ULONGLONG m_boostReportTick = 0;
    SIZE m_overflowSize{};
    LONG m_overflowCaretX = 0;
    std::vector<uint8_t> m_overflowBaseBits;
    // Static plate without live meter/scrub values; live present restores from here.
    std::vector<uint8_t> m_overflowUnderlayBits;
    std::vector<uint8_t> m_overflowPresentBits;
    RECT m_qsMeterRect{};
    RECT m_qsScrubRect{};
    bool m_qsMeterValid = false;
    // Now Playing live elements (valid with m_qsScrubValid): m_qsScrubRect is
    // the drawn scrub track, m_qsMediaLiveRect the scrub + transport rows a
    // live present restores, m_qsMediaTimeRect the time line beside the art,
    // and the transport hit rects' centres place the prev/play/next glyphs.
    bool m_qsScrubValid = false;
    RECT m_qsMediaLiveRect{};
    RECT m_qsMediaTimeRect{};
    RECT m_qsMediaTransportRect[3]{};
    // Home temperature tile value rects (live overlay) and the ink they were
    // laid out with, so a fast present matches the full paint exactly.
    RECT m_qsTempCpuRect{};
    RECT m_qsTempGpuRect{};
    bool m_qsTempsValid = false;
    // Set when a new reading arrives; the next fast present restores/redraws
    // the value rects. Mic-meter frames leave them untouched.
    bool m_qsTempsDirty = false;
    uint8_t m_qsMeterInkR = 33;
    uint8_t m_qsMeterInkG = 34;
    uint8_t m_qsMeterInkB = 36;
    uint8_t m_qsScrubInkR = 33;
    uint8_t m_qsScrubInkG = 34;
    uint8_t m_qsScrubInkB = 36;
    uint8_t m_qsTempsInkR = 0;
    uint8_t m_qsTempsInkG = 0;
    uint8_t m_qsTempsInkB = 0;
    uint8_t m_qsTempsGpuInkR = 0;
    uint8_t m_qsTempsGpuInkG = 0;
    uint8_t m_qsTempsGpuInkB = 0;
    // Boost tile status line lives outside the underlay (like temps) so
    // ApplyBoostResult can present via PaintOverflowLiveFast without a
    // ~100ms PaintOverflowPopup on the WH_MOUSE_LL thread.
    RECT m_qsBoostStatusRect{};
    bool m_qsBoostStatusValid = false;
    bool m_qsBoostStatusDirty = false;
    uint8_t m_qsBoostStatusInkR = 0;
    uint8_t m_qsBoostStatusInkG = 0;
    uint8_t m_qsBoostStatusInkB = 0;
    // Home Bluetooth tile status ("Off" / "On" / "Connected to X"): live
    // overlay like Boost so BT worker snapshots and the tile toggle present
    // one rect via PaintOverflowLiveFast instead of a full PaintQuickSettings.
    // Home tile chevrons (kQsChevronTiles order), composited only on hover
    // like the macOS Control Centre module arrow. Glyphs match the tile ink.
    RECT m_qsTileChevron[kQsChevronTiles]{};
    std::vector<uint8_t> m_qsTileChevronGlyph[kQsChevronTiles];
    UINT m_qsTileChevronExtent = 0;
    uint8_t m_qsTileChevronInk[kQsChevronTiles][3]{};
    float m_qsChevronAlpha[kQsChevronTiles]{};
    bool m_qsChevronDrawn[kQsChevronTiles]{};
    ULONGLONG m_qsChevronFadeLastMs = 0;
    std::vector<uint8_t> m_qsChevronScratch;
    std::vector<uint8_t> m_qsContentScratch;
    int64_t m_qsMeterPaintedSig = -1;
    int64_t m_qsScrubPaintedSig = -1;
    int64_t m_qsMediaTimePaintedSig = -1;
    // Home module order (dock.ini QuickSettingsLayout) and, from the last Home
    // layout, each module's window-px rect and the grid origin.
    QsModuleOrder m_qsOrder = DefaultQsModuleOrder();
    std::wstring m_qsOrderSource;
    bool m_qsOrderSynced = false;
    RECT m_qsModuleRect[kQsModuleCount]{};
    bool m_qsModuleRectsValid = false;
    LONG m_qsGridLeft = 0;
    LONG m_qsGridTop = 0;
    float m_qsGridScale = 1.0F;
    // macOS "Edit Controls" mode: modules drag instead of acting. Ctrl-drag
    // (the Windows stand-in for holding Command) works outside it too.
    bool m_qsEditMode = false;
    // Left press on a Home module, not yet a drag.
    bool m_qsPressArmed = false;
    POINT m_qsPressPoint{};
    int m_qsPressModule = -1;
    struct QsReorderSprite {
        // Border (px) around the tile in every plane, for the outline ring.
        static constexpr LONG kEdge = 2;
        // Module pixels premultiplied by shape coverage, that coverage, and
        // CcGlass's dark outline ring just outside the shape.
        std::vector<uint8_t> bits;
        std::vector<uint8_t> cover;
        std::vector<uint8_t> ring;
        LONG width = 0;
        LONG height = 0;
        float radius = 0.0F;
    };
    bool m_qsReorderActive = false;
    // Released (or cancelled): everything springs home, then FinishQsReorder.
    bool m_qsReorderDropped = false;
    // A full repaint was requested mid-drag; run it once the drag finishes.
    bool m_qsReorderPaintPending = false;
    int m_qsReorderModule = -1;
    int m_qsReorderRows = 0;
    POINT m_qsReorderGrab{};
    POINT m_qsReorderCursor{};
    QsModuleOrder m_qsReorderOrder{};
    QsModuleOrder m_qsReorderStartOrder{};
    float m_qsReorderPos[kQsModuleCount][2]{};
    float m_qsReorderVel[kQsModuleCount][2]{};
    float m_qsReorderLift = 0.0F;
    // Lifted tile scale (springs to ~1.06 while held, back to 1 on drop).
    float m_qsReorderScale = 1.0F;
    float m_qsReorderScaleVel = 0.0F;
    // Blurred coverage of the lifted tile, padded by m_qsReorderShadowPad.
    std::vector<uint8_t> m_qsReorderShadow;
    LONG m_qsReorderShadowPad = 0;
    QsReorderSprite m_qsReorderSprites[kQsModuleCount];
    std::vector<uint8_t> m_qsReorderBackdrop;
    RECT m_qsReorderBounds{};
    RECT m_qsReorderDirty{};
    ULONGLONG m_qsReorderLastMs = 0;
    // Refresh Rate menu: the rate being applied (0 = none) and a failure note.
    int m_qsRefreshPendingHz = 0;
    std::wstring m_qsRefreshNote;
    bool m_qsWifiOthersOpen = false;
    bool m_qsBtOthersOpen = false;
    RECT m_qsTrayStatusRect{};
    bool m_qsTrayStatusValid = false;
    bool m_qsTrayStatusDirty = false;
    uint8_t m_qsTrayStatusInkR = 0;
    uint8_t m_qsTrayStatusInkG = 0;
    uint8_t m_qsTrayStatusInkB = 0;
    std::atomic<bool> m_trayIconsInFlight{false};
    // Right-chevron for tray rows, in the page ink (built during layout).
    std::vector<uint8_t> m_qsTrayArrowGlyph;
    UINT m_qsTrayArrowExtent = 0;
    // Per-row arrow areas (window px) and fade alphas: the same hover fade as
    // the Home tile arrows, driven by kQsChevronFadeTimerId.
    std::vector<RECT> m_qsTrayArrowRects;
    std::vector<float> m_qsTrayArrowAlpha;
    std::vector<uint8_t> m_qsTrayArrowScratch;
    RECT m_qsBtStatusRect{};
    bool m_qsBtStatusValid = false;
    bool m_qsBtStatusDirty = false;
    uint8_t m_qsBtStatusInkR = 0;
    uint8_t m_qsBtStatusInkG = 0;
    uint8_t m_qsBtStatusInkB = 0;
    // When true, LayoutQuickSettings paints meter/scrub tracks at empty levels and
    // records their rects for PaintOverflowLiveFast.
    bool m_qsPaintUnderlayPass = false;
    // macOS 26 Control Centre glass shapes (content coords, shape 0 = panel).
    // Filled by every LayoutQuickSettings pass; baked by the renderer (DOCK_FX_CC).
    struct QsCcShape {
        RECT bounds{};
        float radius = 0.0F;
    };
    std::vector<QsCcShape> m_qsCcShapes;
    uint64_t m_overflowGlassCcHash = 0;
    uint64_t m_qsCcInkSig = 0;
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
