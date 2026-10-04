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
    Nearby,
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
    int nearby = 0;
};
