#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Quick Settings pages. Home matches the control-center mock; the other pages
// are the drill-ins opened from its tiles.
enum class QuickSettingsPage : uint8_t {
    Home = 0,
    Wifi,
    Vpn,
    Sound,
    Microphone,
    Power,
    SystemTray,
    Airplane,
    Bluetooth,
    Display,
    Boost,
};

// Home Control Centre modules, in default grid order. The user can drag them
// into a different order (dock.ini QuickSettingsLayout).
enum class QsModule : uint8_t {
    Wifi = 0,
    Bluetooth,
    Boost,
    Power,
    SystemTray,
    Vpn,
    Microphone,
    Sound,
    Display,
    Temps,
    Media,
};
inline constexpr size_t kQsModuleCount = 11;
using QsModuleOrder = std::array<uint8_t, kQsModuleCount>;
struct QsGridCell {
    int col = 0;
    int row = 0;
};
[[nodiscard]] QsModuleOrder DefaultQsModuleOrder() noexcept;
// Grid footprint of a module in 4-column cells.
[[nodiscard]] int QsModuleCols(size_t module) noexcept;
[[nodiscard]] int QsModuleRows(size_t module) noexcept;
// First-fit flow of `order` into the 4-column grid. False if order is not a
// permutation of the modules; rows receives the grid height in cells.
bool PackQsModules(const QsModuleOrder& order, QsGridCell (&cells)[kQsModuleCount], int& rows) noexcept;
// Comma-separated module names ("wifi,bluetooth,...") for dock.ini.
[[nodiscard]] std::wstring FormatQsModuleOrder(const QsModuleOrder& order);
// Parse dock.ini text. Unknown names are ignored, missing modules keep their
// default relative order; false when the result does not tile the grid.
[[nodiscard]] bool ParseQsModuleOrder(const std::wstring& text, QsModuleOrder& order);

struct QsNamedId {
    std::wstring id;
    std::wstring name;
};

struct QsWifiNetwork {
    std::wstring name;
    int bars = 0;
    bool connected = false;
    bool secure = false;
    bool hasProfile = false;
};

struct QsVpnEntry {
    std::wstring name;
    bool connected = false;
};

// One app row on the Energy page. cpuPercent is this process's share of
// machine CPU between two samples (a stand-in for significant energy use).
struct QsEnergyApp {
    std::wstring name;
    int cpuPercent = 0;
};

// Snapshot filled on the UI thread while Quick Settings is open. Queries are
// cached for a short interval so slider drags do not redo WLAN/COM work.
struct QuickSettingsCache {
    ULONGLONG stamp = 0;
    // Independent TTLs so heavy queries are not all tied to the 800 ms stamp.
    ULONGLONG stampNetwork = 0;  // wifi / adapters / endpoints / power / HDR
    ULONGLONG stampNight = 0;    // Night Light CloudStore
    bool wifiRadioOn = true;
    bool haveWifiInterface = false;
    GUID wifiInterface{};
    std::vector<QsWifiNetwork> wifi;
    std::vector<QsVpnEntry> vpn;
    std::wstring outputName;
    std::wstring inputName;
    std::wstring renderDefaultId;
    std::wstring captureDefaultId;
    std::vector<QsNamedId> renderDevices;
    std::vector<QsNamedId> captureDevices;
    float inputPeak = 0.0F;
    float inputGain = 1.0F;
    bool inputMuted = false;
    std::wstring powerName;
    int powerMode = 1;
    std::vector<QsEnergyApp> energyApps;
    bool energyLive = false;
    // True while the first CPU sample has no baseline yet. The popup timer
    // paints again so the list can fill without blocking the UI thread.
    bool energyPending = false;
    std::wstring energyNote;
    bool usbSuspend = false;
    bool usbKnown = false;
    bool hdrOn = false;
    bool hdrSupported = false;
    bool nightLight = false;
    bool nightKnown = false;
    // Windows app/system theme (Personalize AppsUseLightTheme == 0).
    bool darkMode = false;
    bool darkKnown = false;
    // Primary display refresh rate and the rates it supports at the current
    // resolution, ascending. 0 / empty when unknown.
    int refreshHz = 0;
    std::vector<int> refreshRates;
    // Home temperature tile. Whole degrees C / watts; -1 shows an em dash.
    int cpuTempC = -1;
    int gpuTempC = -1;
    int cpuWatts = -1;
    int gpuWatts = -1;
    // Set when WASAPI capture cannot be opened (permission, missing device).
    std::wstring inputNote;
    std::wstring mediaTitle;
    std::wstring mediaArtist;
    bool mediaHave = false;
    bool mediaPlaying = false;
    // 0..1 along the GSMTC timeline. 0 when the session has no duration.
    float mediaProgress = 0.0F;
};

// Release the Quick Settings capture client so the microphone-in-use indicator
// drops when the panel closes. Also stops the background peak pump.
void StopQuickSettingsCapture() noexcept;
// Live-meter / media poll result. MetersOnly = peak or scrub moved (partial
// present). Labels = title/artist/note/playing changed (full rebuild).
enum class QsLiveChange : uint8_t {
    None = 0,
    MetersOnly = 1,
    Labels = 2,
};
// Read the background mic peak and rate-limited GSMTC state into cache.
QsLiveChange RefreshQuickSettingsLive(QuickSettingsCache& cache);
// Seek the current GSMTC session. level is 0..1 across its timeline.
void SeekQuickSettingsMedia(float level) noexcept;
// Kick a ToolHelp energy sample on a worker (~500 ms cadence). Posts notifyMsg to
// notifyHwnd when a fresh result is ready; never blocks the UI thread.
void RequestEnergyAppsAsync(HWND notifyHwnd, UINT notifyMsg);
// Copy the latest worker energy snapshot into cache. True when values changed.
bool ApplyEnergyAppsResult(QuickSettingsCache& cache);
// Keep the background CPU/GPU temperature worker polling (~500 ms). Posts
// notifyMsg to notifyHwnd when a displayed value changes; never blocks.
void RequestQuickSettingsTemps(HWND notifyHwnd, UINT notifyMsg) noexcept;
// Copy the latest worker temperatures into cache. True when a value changed.
bool ApplyQuickSettingsTemps(QuickSettingsCache& cache);
// Fill wifi/adapters/endpoints/power/HDR/night/capture on a worker. Posts
// notifyMsg when ready; never blocks the UI thread. First paint should use
// whatever is already in cache (possibly stale).
void RequestQuickSettingsCacheAsync(HWND notifyHwnd, UINT notifyMsg, QuickSettingsPage page);
// Merge the worker network/audio/power snapshot into cache. True on change.
bool ApplyQuickSettingsCacheResult(QuickSettingsCache& cache);
// Open the mic capture client on a worker if the peak pump is not running.
void EnsureQuickSettingsCaptureAsync() noexcept;

// Shared macOS 26 Control Centre controls + accent, used by Quick Settings and
// Dock Settings (defined in QuickSettingsPanel.cpp).
namespace dock_detail {
// Quick Settings accent / icon colour (BGR order in draw calls). Default #98A869,
// overridden by dock.ini QuickSettingsIconColor.
extern uint8_t g_qsAccentR;
extern uint8_t g_qsAccentG;
extern uint8_t g_qsAccentB;
void SetQsAccent(unsigned rgb) noexcept;
// Dark or light glyph ink that stays legible on top of the given accent fill.
void CcOnAccentInk(uint8_t accentR, uint8_t accentG, uint8_t accentB, uint8_t& r, uint8_t& g,
    uint8_t& b) noexcept;
void CcDrawSwitch(uint8_t* pixels, int width, int height, float cxLeft, float cxRight, float cy,
    float radius, float scale, bool on, uint8_t accentR, uint8_t accentG, uint8_t accentB,
    uint8_t inkR, uint8_t inkG, uint8_t inkB);
void CcDrawSlider(uint8_t* pixels, int width, int height, RECT track, float level, float scale,
    uint8_t accentR, uint8_t accentG, uint8_t accentB, uint8_t inkR, uint8_t inkG, uint8_t inkB);
}  // namespace dock_detail
