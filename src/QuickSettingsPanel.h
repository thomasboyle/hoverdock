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
    bool usbSuspend = false;
    bool usbKnown = false;
    bool hdrOn = false;
    bool hdrSupported = false;
    bool nightLight = false;
    bool nightKnown = false;
    int nearby = 0;
};
