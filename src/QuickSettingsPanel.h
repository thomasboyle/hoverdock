#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

// Quick Settings pages. Home matches the control-center mock; the other pages
// are the drill-ins opened from its tiles.
enum class QuickSettingsPage : uint8_t {
    Home = 0,
    Wifi,
    Ethernet,
    Vpn,
    Sound,
    Microphone,
    Power,
    SystemTray,
    Airplane,
    Bluetooth,
    Display,
};

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
    bool ethernetUp = false;
    std::wstring ethernetStatus;
    std::wstring ethernetSpeed;
    std::wstring ethernetIpv4;
    std::wstring ethernetAdapter;
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
// Kick a ToolHelp energy sample on a worker (~2 s cadence). Posts notifyMsg to
// notifyHwnd when a fresh result is ready; never blocks the UI thread.
void RequestEnergyAppsAsync(HWND notifyHwnd, UINT notifyMsg);
// Copy the latest worker energy snapshot into cache. True when values changed.
bool ApplyEnergyAppsResult(QuickSettingsCache& cache);
// Keep the background CPU/GPU temperature worker polling (~2 s). Posts
// notifyMsg to notifyHwnd when a displayed value changes; never blocks.
void RequestQuickSettingsTemps(HWND notifyHwnd, UINT notifyMsg) noexcept;
// Copy the latest worker temperatures into cache. True when a value changed.
bool ApplyQuickSettingsTemps(QuickSettingsCache& cache);
