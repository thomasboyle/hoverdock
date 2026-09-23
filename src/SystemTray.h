#pragma once

#include <Windows.h>

#include <string>
#include <vector>

enum class TraySlot {
    Overflow,
    Network,
    Volume,
    Power,
    Clock,
};

enum class TrayNetworkKind {
    Disconnected,
    Wifi,
    Ethernet,
    Airplane,
};

struct TrayStatus {
    TrayNetworkKind network = TrayNetworkKind::Disconnected;
    int wifiBars = 0;
    bool wifiRadioOn = false;
    bool volumeMuted = false;
    float volumeLevel = 1.0F;
    bool hasBattery = false;
    bool batteryCharging = false;
    int batteryPercent = 100;
    int brightnessPercent = 100;
    bool brightnessAvailable = false;
    std::wstring networkName;
    std::wstring timeText;
    std::wstring dateText;
};

struct TrayNotifyIcon {
    HWND window = nullptr;
    UINT id = 0;
    GUID guid{};
    HICON icon = nullptr;
    std::wstring tip;
    std::wstring exeName;
    std::wstring executablePath;
    int preference = 0;

    TrayNotifyIcon() = default;
    TrayNotifyIcon(const TrayNotifyIcon&) = delete;
    TrayNotifyIcon& operator=(const TrayNotifyIcon&) = delete;
    TrayNotifyIcon(TrayNotifyIcon&& other) noexcept;
    TrayNotifyIcon& operator=(TrayNotifyIcon&& other) noexcept;
    ~TrayNotifyIcon();
};

// Dock text face: JetBrainsMono Nerd Font for labels and clock.
// Icons stay on Segoe Fluent/MDL2; this is text only (clock, labels, flyouts).
[[nodiscard]] const wchar_t* DockTextFontFace() noexcept;

class SystemTray {
public:
    [[nodiscard]] bool Refresh();
    // Cheap 1 Hz poll for the visible dock: clock + battery only. The dock face
    // shows Overflow, Power, and Clock; volume/network/brightness tiles live in
    // Quick Settings and are refreshed by the full Refresh() while the popup is
    // open (plus once synchronously on open). This keeps the steady visible-idle
    // tick to two cheap syscalls instead of WLAN/COM audio/power IPC every second.
    [[nodiscard]] bool RefreshForDock();
    [[nodiscard]] const TrayStatus& Status() const noexcept;

    [[nodiscard]] static const wchar_t* TargetForSlot(TraySlot slot) noexcept;
    [[nodiscard]] static const wchar_t* LabelForSlot(TraySlot slot, const TrayStatus& status) noexcept;
    [[nodiscard]] static bool SlotVisible(TraySlot slot, const TrayStatus& status) noexcept;

    [[nodiscard]] std::wstring NetworkStatusText() const;    [[nodiscard]] std::wstring VolumeStatusText() const;
    [[nodiscard]] std::wstring BatteryStatusText() const;
    [[nodiscard]] std::wstring BrightnessStatusText() const;

    [[nodiscard]] std::vector<uint8_t> RasterizeGlyph(TraySlot slot, UINT extent) const;
    [[nodiscard]] std::vector<uint8_t> RasterizeSymbol(wchar_t symbol, UINT extent) const;
    [[nodiscard]] std::vector<uint8_t> RasterizeClock(UINT atlasExtent, UINT displayWidth,
        UINT displayHeight, float layoutScale) const;
    [[nodiscard]] SIZE MeasureClock(float layoutScale) const;

    [[nodiscard]] std::vector<TrayNotifyIcon> EnumerateNotifyIcons() const;
    bool ToggleMute();
    bool AdjustVolume(float delta);
    bool AdjustBrightness(int deltaPercent);
    // Absolute set without re-reading first (worker threads only). The caller
    // owns freshness; used for debounced writes of an already-projected value.
    bool SetBrightnessLevel(int percent);
    // Live DDC read into the status for worker threads. Never call on the UI
    // thread: monitor round-trips routinely stall for seconds.
    bool RefreshBrightnessFromDdc();
    static bool OpenFlyout(TraySlot slot);
    static bool OpenNetworkPanel();
    static bool OpenSoundMixer();
    static bool OpenPowerSettings();
    static bool OpenWindowsSettings();
    static bool OpenDisplaySettings();
    static bool OpenDateTimeSettings();
    static bool OpenNotificationCenter();
    static bool InvokeNotifyIcon(const TrayNotifyIcon& icon, UINT mouseMessage);

private:
    TrayStatus m_status;
    // Last worker-confirmed DDC level, -1 when unknown. The 1 Hz UI poll
    // prefers this over power-scheme values (which don't track external
    // monitors) so it never clobbers freshly adjusted state with stale data.
    int m_ddcPercent = -1;
    // MeasureClock cache: GDI font creation + text measurement costs ~1ms per
    // call, and RebuildLayout invokes it every time. The result only depends on
    // the clock texts and the scale, so cache it (mutable: MeasureClock is
    // const and UI-thread confined like the rest of SystemTray).
    mutable std::wstring m_clockMeasureTime;
    mutable std::wstring m_clockMeasureDate;
    mutable float m_clockMeasureScale = -1.0F;
    mutable SIZE m_clockMeasureSize{};
};
