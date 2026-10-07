#include "DockApp.h"

#include "QuickSettingsPanel.h"
#include "SystemTemps.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmreg.h>
#include <endpointvolume.h>
#include <iphlpapi.h>
#include <mmdeviceapi.h>
#include <powrprof.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wlanapi.h>
#include <audioclient.h>
#include <mutex>

#pragma warning(push)
#pragma warning(disable : 4458 4996 26495)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#pragma warning(pop)


#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dock_detail {
void CompositePremul(uint8_t* dest, int destWidth, int destHeight, int destX, int destY,
    const uint8_t* source, int sourceWidth, int sourceHeight);
void DrawFlyoutText(uint8_t* dest, int destWidth, int destHeight, RECT bounds, HFONT font,
    const std::wstring& text, UINT format, uint8_t gray, int underlineFrom = -1);
void FillCirclePremul(uint8_t* dest, int destWidth, int destHeight, float cx, float cy,
    float radius, float alpha, bool useInk = false);
void FillPillColorPremul(uint8_t* dest, int destWidth, int destHeight, float cxLeft,
    float cxRight, float cy, float radius, float alpha, uint8_t blue, uint8_t green,
    uint8_t red);
void FillSquircleColorPremul(uint8_t* dest, int destWidth, int destHeight, RECT bounds,
    float radius, float alpha, uint8_t blue, uint8_t green, uint8_t red);
void FillSquirclePremul(uint8_t* dest, int destWidth, int destHeight, RECT bounds, float radius,
    float alpha);
float ContentSquircleRadius(const RECT& bounds, float scale) noexcept;
void RemapPremulInkColor(std::vector<uint8_t>& pixels, uint8_t r, uint8_t g, uint8_t b);
void SetFlyoutChromeInk(uint8_t r, uint8_t g, uint8_t b) noexcept;
extern uint8_t g_flyoutInkR;
extern uint8_t g_flyoutInkG;
extern uint8_t g_flyoutInkB;
std::wstring NotifyIconTitle(const TrayNotifyIcon& icon);
std::wstring NotifyIconStatus(const TrayNotifyIcon& icon);
}  // namespace dock_detail
using namespace dock_detail;

namespace {

constexpr uint8_t kBlueB = 255;
constexpr uint8_t kBlueG = 132;
constexpr uint8_t kBlueR = 47;
constexpr uint8_t kInkDarkR = 33;
constexpr uint8_t kInkDarkG = 34;
constexpr uint8_t kInkDarkB = 36;
constexpr uint8_t kInkLightR = 245;
constexpr uint8_t kInkLightG = 245;
constexpr uint8_t kInkLightB = 247;

constexpr int kLinkWifi = 0;
constexpr int kLinkNetwork = 1;
constexpr int kLinkVpn = 2;
constexpr int kLinkSound = 3;
constexpr int kLinkPower = 4;
constexpr int kLinkTaskbar = 5;
constexpr int kLinkBluetooth = 7;
constexpr int kLinkDisplay = 8;
constexpr int kLinkNight = 9;
constexpr int kLinkMixer = 10;

constexpr int kToggleWifi = 0;
constexpr int kToggleMute = 1;
constexpr int kToggleMic = 2;
constexpr int kToggleUsb = 3;
constexpr int kToggleAirplane = 4;
constexpr int kToggleHdr = 5;

const GUID kUsbSubgroup = {
    0x2a737441, 0x1930, 0x4402, {0x8d, 0x77, 0xb2, 0xbe, 0xbb, 0xa3, 0x08, 0xa3}};
const GUID kUsbSuspend = {
    0x48e6b7a6, 0x50f5, 0x4782, {0xa5, 0xd4, 0x53, 0xbb, 0x8f, 0x07, 0xe2, 0x26}};

// Windows keeps the Ultimate Performance source scheme hidden until it is
// duplicated. The duplicate receives a normal, enumerable scheme GUID.
const GUID kUltimatePowerSchemeSource = {
    0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};

struct DeviceShareMode;

MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

const PROPERTYKEY kDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

const CLSID kPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

std::wstring Lower(std::wstring text) {
    for (wchar_t& ch : text) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return text;
}

bool Contains(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

std::wstring FormatLinkSpeed(ULONG64 bits) {
    if (bits >= 1000000000ULL) {
        wchar_t text[32] = {};
        swprintf_s(text, L"%.1f Gbps", static_cast<double>(bits) / 1000000000.0);
        return text;
    }
    if (bits >= 1000000ULL) {
        return std::to_wstring(static_cast<unsigned long long>(bits / 1000000ULL)) + L" Mbps";
    }
    if (bits == 0) {
        return L"Unknown";
    }
    return std::to_wstring(static_cast<unsigned long long>(bits)) + L" bps";
}

std::wstring Ipv4Text(const SOCKET_ADDRESS& address) {
    if (address.lpSockaddr == nullptr || address.lpSockaddr->sa_family != AF_INET) {
        return {};
    }
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address.lpSockaddr);
    const auto* bytes = reinterpret_cast<const unsigned char*>(&ipv4->sin_addr);
    return std::to_wstring(bytes[0]) + L"." + std::to_wstring(bytes[1]) + L"." +
        std::to_wstring(bytes[2]) + L"." + std::to_wstring(bytes[3]);
}

bool LooksVirtual(const std::wstring& description) {
    const std::wstring text = Lower(description);
    return Contains(text, L"virtual") || Contains(text, L"hyper-v") || Contains(text, L"vmware") ||
        Contains(text, L"virtualbox") || Contains(text, L"bluetooth") || Contains(text, L"wan miniport") ||
        Contains(text, L"loopback");
}

bool LooksLikeVpn(const std::wstring& description, IFTYPE type) {
    if (type == IF_TYPE_PPP || type == IF_TYPE_TUNNEL) {
        return true;
    }
    const std::wstring text = Lower(description);
    return Contains(text, L"vpn") || Contains(text, L"wireguard") || Contains(text, L"wintun") ||
        Contains(text, L"tap-windows") || Contains(text, L"nordlynx") || Contains(text, L"mullvad") ||
        Contains(text, L"openvpn");
}

std::wstring SsidText(const DOT11_SSID& ssid) {
    if (ssid.uSSIDLength == 0 || ssid.uSSIDLength > DOT11_SSID_MAX_LENGTH) {
        return {};
    }
    const int length = static_cast<int>(ssid.uSSIDLength);
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(ssid.ucSSID), length, nullptr, 0);
    const UINT codePage = needed > 0 ? CP_UTF8 : CP_ACP;
    const int wide = needed > 0
        ? needed
        : MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(ssid.ucSSID), length, nullptr, 0);
    if (wide <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(codePage, 0, reinterpret_cast<const char*>(ssid.ucSSID), length, text.data(),
        wide);
    return text;
}

const std::vector<uint8_t>& CachedGlyph(SystemTray& tray, wchar_t symbol, UINT extent, uint8_t red,
    uint8_t green, uint8_t blue) {
    struct Entry {
        wchar_t symbol = 0;
        UINT extent = 0;
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
        std::vector<uint8_t> pixels;
    };
    static std::vector<Entry> cache;
    for (const Entry& entry : cache) {
        if (entry.symbol == symbol && entry.extent == extent && entry.red == red &&
            entry.green == green && entry.blue == blue) {
            return entry.pixels;
        }
    }
    Entry created;
    created.symbol = symbol;
    created.extent = extent;
    created.red = red;
    created.green = green;
    created.blue = blue;
    created.pixels = tray.RasterizeSymbol(symbol, extent);
    RemapPremulInkColor(created.pixels, red, green, blue);
    cache.push_back(std::move(created));
    return cache.back().pixels;
}

float TrackLevel(const RECT& track, LONG x) {
    const LONG inset = std::max(6L, (track.bottom - track.top) / 2L);
    const LONG left = track.left + inset;
    const LONG right = std::max(left + 1L, track.right - inset);
    const float span = static_cast<float>(right - left);
    return std::clamp(static_cast<float>(x - left) / span, 0.0F, 1.0F);
}

bool SetDefaultAudioDevice(const std::wstring& id) {
    if (id.empty()) {
        return false;
    }
    IPolicyConfig* policy = nullptr;
    if (FAILED(CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&policy))) ||
        policy == nullptr) {
        return false;
    }
    const HRESULT console = policy->SetDefaultEndpoint(id.c_str(), eConsole);
    const HRESULT multimedia = policy->SetDefaultEndpoint(id.c_str(), eMultimedia);
    const HRESULT communications = policy->SetDefaultEndpoint(id.c_str(), eCommunications);
    policy->Release();
    return SUCCEEDED(console) || SUCCEEDED(multimedia) || SUCCEEDED(communications);
}

bool WithEndpoint(EDataFlow flow, const auto& visitor) {
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator))) ||
        enumerator == nullptr) {
        return false;
    }
    IMMDevice* device = nullptr;
    const bool opened = SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eMultimedia, &device)) &&
        device != nullptr;
    enumerator->Release();
    if (!opened) {
        return false;
    }
    const bool result = visitor(device);
    device->Release();
    return result;
}

std::wstring DeviceName(IMMDevice* device) {
    if (device == nullptr) {
        return {};
    }
    IPropertyStore* store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || store == nullptr) {
        return {};
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(store->GetValue(kDeviceFriendlyName, &value)) && value.vt == VT_LPWSTR &&
        value.pwszVal != nullptr) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    store->Release();
    return name;
}

std::wstring DeviceId(IMMDevice* device) {
    if (device == nullptr) {
        return {};
    }
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || id == nullptr) {
        return {};
    }
    std::wstring text = id;
    CoTaskMemFree(id);
    return text;
}

void CollectEndpoints(EDataFlow flow, std::vector<QsNamedId>& devices, std::wstring& defaultId,
    std::wstring& defaultName) {
    devices.clear();
    defaultId.clear();
    defaultName.clear();
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator))) ||
        enumerator == nullptr) {
        return;
    }
    IMMDevice* current = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eMultimedia, &current)) &&
        current != nullptr) {
        defaultId = DeviceId(current);
        defaultName = DeviceName(current);
        current->Release();
    }
    IMMDeviceCollection* collection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) &&
        collection != nullptr) {
        UINT count = 0;
        collection->GetCount(&count);
        const UINT limit = std::min<UINT>(count, 6U);
        for (UINT index = 0; index < limit; ++index) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(index, &device)) || device == nullptr) {
                continue;
            }
            QsNamedId entry;
            entry.id = DeviceId(device);
            entry.name = DeviceName(device);
            device->Release();
            if (!entry.name.empty()) {
                devices.push_back(std::move(entry));
            }
        }
        collection->Release();
    }
    enumerator->Release();
}

bool SetWifiRadio(bool enabled) {
    HANDLE client = nullptr;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS || client == nullptr) {
        return false;
    }
    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    bool wrote = false;
    if (WlanEnumInterfaces(client, nullptr, &interfaces) == ERROR_SUCCESS && interfaces != nullptr) {
        for (DWORD index = 0; index < interfaces->dwNumberOfItems; ++index) {
            WLAN_PHY_RADIO_STATE state{};
            state.dwPhyIndex = 0;
            state.dot11SoftwareRadioState =
                enabled ? dot11_radio_state_on : dot11_radio_state_off;
            if (WlanSetInterface(client, &interfaces->InterfaceInfo[index].InterfaceGuid,
                    wlan_intf_opcode_radio_state, sizeof(state), &state, nullptr) == ERROR_SUCCESS) {
                wrote = true;
            }
        }
        WlanFreeMemory(interfaces);
    }
    WlanCloseHandle(client, nullptr);
    return wrote;
}

bool ConnectWifiProfile(const GUID& interfaceId, const std::wstring& profile) {
    if (profile.empty()) {
        return false;
    }
    HANDLE client = nullptr;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS || client == nullptr) {
        return false;
    }
    WLAN_CONNECTION_PARAMETERS parameters{};
    parameters.wlanConnectionMode = wlan_connection_mode_profile;
    parameters.strProfile = profile.c_str();
    parameters.dot11BssType = dot11_BSS_type_any;
    parameters.pDot11Ssid = nullptr;
    parameters.dwFlags = 0;
    const DWORD result = WlanConnect(client, &interfaceId, &parameters, nullptr);
    WlanCloseHandle(client, nullptr);
    return result == ERROR_SUCCESS;
}

void QueryWifiRadio(QuickSettingsCache& cache) {
    cache.wifiRadioOn = false;
    cache.haveWifiInterface = false;
    HANDLE client = nullptr;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS || client == nullptr) {
        return;
    }
    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    if (WlanEnumInterfaces(client, nullptr, &interfaces) == ERROR_SUCCESS && interfaces != nullptr) {
        for (DWORD index = 0; index < interfaces->dwNumberOfItems; ++index) {
            const WLAN_INTERFACE_INFO& info = interfaces->InterfaceInfo[index];
            if (info.isState == wlan_interface_state_not_ready) {
                continue;
            }
            cache.haveWifiInterface = true;
            cache.wifiInterface = info.InterfaceGuid;
            DWORD size = 0;
            PWLAN_RADIO_STATE radio = nullptr;
            if (WlanQueryInterface(client, &info.InterfaceGuid, wlan_intf_opcode_radio_state, nullptr,
                    &size, reinterpret_cast<PVOID*>(&radio), nullptr) == ERROR_SUCCESS &&
                radio != nullptr) {
                if (radio->dwNumberOfPhys > 0) {
                    cache.wifiRadioOn =
                        radio->PhyRadioState[0].dot11SoftwareRadioState == dot11_radio_state_on;
                }
                WlanFreeMemory(radio);
            } else {
                cache.wifiRadioOn = true;
            }
            break;
        }
        WlanFreeMemory(interfaces);
    }
    WlanCloseHandle(client, nullptr);
}

void QueryWifiNetworks(QuickSettingsCache& cache) {
    cache.wifi.clear();
    if (!cache.haveWifiInterface || !cache.wifiRadioOn) {
        return;
    }
    HANDLE client = nullptr;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS || client == nullptr) {
        return;
    }
    PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
    if (WlanGetAvailableNetworkList(client, &cache.wifiInterface, 0, nullptr, &list) == ERROR_SUCCESS &&
        list != nullptr) {
        for (DWORD index = 0; index < list->dwNumberOfItems && cache.wifi.size() < 6; ++index) {
            const WLAN_AVAILABLE_NETWORK& network = list->Network[index];
            QsWifiNetwork entry;
            entry.name = SsidText(network.dot11Ssid);
            if (entry.name.empty() && network.strProfileName[0] != 0) {
                entry.name = network.strProfileName;
            }
            if (entry.name.empty()) {
                continue;
            }
            bool duplicate = false;
            for (const QsWifiNetwork& existing : cache.wifi) {
                if (existing.name == entry.name) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            const ULONG quality = network.wlanSignalQuality;
            entry.bars = quality >= 80 ? 3 : quality >= 50 ? 2 : quality >= 20 ? 1 : 0;
            entry.connected = (network.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
            entry.secure = network.bSecurityEnabled != FALSE;
            entry.hasProfile = (network.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) != 0 ||
                network.strProfileName[0] != 0;
            cache.wifi.push_back(std::move(entry));
        }
        WlanFreeMemory(list);
    }
    WlanCloseHandle(client, nullptr);
    std::stable_sort(cache.wifi.begin(), cache.wifi.end(),
        [](const QsWifiNetwork& left, const QsWifiNetwork& right) {
            return left.connected && !right.connected;
        });
}

void QueryAdapters(QuickSettingsCache& cache) {
    cache.ethernetUp = false;
    cache.ethernetStatus = L"Not connected";
    cache.ethernetSpeed = L"Unavailable";
    cache.ethernetIpv4 = L"Unavailable";
    cache.ethernetAdapter = L"No adapter";
    cache.vpn.clear();
    ULONG size = 16U * 1024U;
    std::vector<unsigned char> buffer(size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
        nullptr, addresses, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
            nullptr, addresses, &size);
    }
    if (result != ERROR_SUCCESS) {
        return;
    }
    const IP_ADAPTER_ADDRESSES* ethernet = nullptr;
    for (const IP_ADAPTER_ADDRESSES* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        const std::wstring description = adapter->Description != nullptr ? adapter->Description : L"";
        const std::wstring friendly = adapter->FriendlyName != nullptr ? adapter->FriendlyName : description;
        if (LooksLikeVpn(description + L" " + friendly, adapter->IfType)) {
            if (cache.vpn.size() < 4) {
                QsVpnEntry entry;
                entry.name = friendly.empty() ? L"VPN" : friendly;
                entry.connected = adapter->OperStatus == IfOperStatusUp;
                cache.vpn.push_back(std::move(entry));
            }
            continue;
        }
        if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD || LooksVirtual(description)) {
            continue;
        }
        if (ethernet == nullptr ||
            (adapter->OperStatus == IfOperStatusUp && ethernet->OperStatus != IfOperStatusUp)) {
            ethernet = adapter;
        }
    }
    if (ethernet == nullptr) {
        return;
    }
    cache.ethernetUp = ethernet->OperStatus == IfOperStatusUp;
    cache.ethernetStatus = cache.ethernetUp ? L"Connected" : L"Disconnected";
    cache.ethernetSpeed = FormatLinkSpeed(ethernet->TransmitLinkSpeed);
    cache.ethernetAdapter =
        ethernet->Description != nullptr ? ethernet->Description : L"Ethernet";
    if (ethernet->FirstUnicastAddress != nullptr) {
        const std::wstring ip = Ipv4Text(ethernet->FirstUnicastAddress->Address);
        if (!ip.empty()) {
            cache.ethernetIpv4 = ip;
        }
    }
}

const wchar_t* EnergyModeLabel(int mode) noexcept {
    if (mode == 0) {
        return L"High power";
    }
    if (mode == 2) {
        return L"Low power";
    }
    return L"Automatic";
}

bool IsEnergyNoiseProcess(const std::wstring& exeLower) {
    // Hosts and session infrastructure, not apps a person would recognize
    // in an "using energy" list. User processes (browsers, games, Dock) stay.
    static const wchar_t* kNoise[] = {
        L"system",
        L"idle",
        L"registry",
        L"secure system",
        L"memory compression",
        L"smss.exe",
        L"csrss.exe",
        L"wininit.exe",
        L"winlogon.exe",
        L"services.exe",
        L"lsass.exe",
        L"svchost.exe",
        L"fontdrvhost.exe",
        L"dwm.exe",
        L"sihost.exe",
        L"taskhostw.exe",
        L"runtimebroker.exe",
        L"searchhost.exe",
        L"startmenuexperiencehost.exe",
        L"shellexperiencehost.exe",
        L"textinputhost.exe",
        L"widgetservice.exe",
        L"widgets.exe",
        L"securityhealthservice.exe",
        L"securityhealthsystray.exe",
        L"msmpeng.exe",
        L"nissrv.exe",
        L"conhost.exe",
        L"dllhost.exe",
        L"ctfmon.exe",
        L"applicationframehost.exe",
        L"backgroundtaskhost.exe",
        L"wlanext.exe",
        L"spoolsv.exe",
        L"searchindexer.exe",
        L"audiodg.exe",
    };
    for (const wchar_t* name : kNoise) {
        if (exeLower == name) {
            return true;
        }
    }
    return false;
}

std::wstring PrettyProcessName(std::wstring name) {
    if (name.size() > 4 && Lower(name.substr(name.size() - 4)) == L".exe") {
        name.resize(name.size() - 4);
    }
    if (!name.empty()) {
        name[0] = static_cast<wchar_t>(towupper(name[0]));
    }
    return name;
}

struct EnergyCpuSample {
    DWORD pid = 0;
    unsigned long long cpu = 0;
    std::wstring name;
};

struct EnergyBaseline {
    ULONGLONG tick = 0;
    std::vector<EnergyCpuSample> samples;
};

EnergyBaseline g_energyBaseline;

struct EnergyBridge {
    std::mutex mutex;
    bool inFlight = false;
    bool haveResult = false;
    QuickSettingsCache result{};
    HWND notifyHwnd = nullptr;
    UINT notifyMsg = 0;
};

EnergyBridge g_energy;

struct NetworkBridge {
    std::mutex mutex;
    bool inFlight = false;
    bool haveResult = false;
    QuickSettingsCache result{};
    HWND notifyHwnd = nullptr;
    UINT notifyMsg = 0;
    QuickSettingsPage page = QuickSettingsPage::Home;
};

NetworkBridge g_network;
std::atomic<bool> g_captureOpenInFlight{false};

// Two toolhelp snapshots. The first open of Energy has no delta yet and is
// labeled pending; the follow-up paint (about a second later) reports real
// CPU share. Snapshot failure is an explicit stub, not a fake app list.
void QueryEnergyApps(QuickSettingsCache& cache) {
    cache.energyApps.clear();
    cache.energyLive = false;
    cache.energyPending = false;
    cache.energyNote.clear();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        cache.energyNote = L"Energy usage unavailable";
        return;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::vector<EnergyCpuSample> current;
    const DWORD self = GetCurrentProcessId();
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == self) {
                continue;
            }
            const std::wstring exeLower = Lower(entry.szExeFile);
            if (IsEnergyNoiseProcess(exeLower)) {
                continue;
            }
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process == nullptr) {
                continue;
            }
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
                ULARGE_INTEGER kernelTime{};
                ULARGE_INTEGER userTime{};
                kernelTime.LowPart = kernel.dwLowDateTime;
                kernelTime.HighPart = kernel.dwHighDateTime;
                userTime.LowPart = user.dwLowDateTime;
                userTime.HighPart = user.dwHighDateTime;
                EnergyCpuSample sample;
                sample.pid = entry.th32ProcessID;
                sample.cpu = kernelTime.QuadPart + userTime.QuadPart;
                sample.name = PrettyProcessName(entry.szExeFile);
                current.push_back(std::move(sample));
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (current.empty() && g_energyBaseline.samples.empty()) {
        cache.energyNote = L"Energy usage unavailable";
        return;
    }

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed = g_energyBaseline.tick == 0 ? 0 : now - g_energyBaseline.tick;
    const bool haveBaseline = !g_energyBaseline.samples.empty() && g_energyBaseline.tick != 0;
    const bool tooSoon = haveBaseline && elapsed < 250ULL;
    const bool tooStale = haveBaseline && elapsed > 8000ULL;
    const bool usable = haveBaseline && !tooSoon && !tooStale;
    if (!usable) {
        // First sample or a stale baseline: seed/re-seed. A too-soon follow-up
        // (paint -> RefreshQuickSettingsCache -> RequestEnergyAppsAsync) must NOT
        // reset the tick - that kept the Power page stuck on Measuring forever
        // whenever ToolHelp finished under 250 ms.
        if (!haveBaseline || tooStale) {
            g_energyBaseline.tick = now;
            g_energyBaseline.samples = std::move(current);
        }
        cache.energyPending = true;
        cache.energyNote = L"Measuring energy use\u2026";
        return;
    }

    std::unordered_map<DWORD, unsigned long long> previous;
    previous.reserve(g_energyBaseline.samples.size());
    for (const EnergyCpuSample& sample : g_energyBaseline.samples) {
        previous[sample.pid] = sample.cpu;
    }
    const DWORD cores = (std::max)(static_cast<DWORD>(1), GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    const double wall = static_cast<double>(elapsed) * 10000.0 * static_cast<double>(cores);
    struct Ranked {
        std::wstring name;
        double percent = 0.0;
    };
    std::vector<Ranked> ranked;
    for (const EnergyCpuSample& sample : current) {
        const auto found = previous.find(sample.pid);
        if (found == previous.end() || sample.cpu < found->second || wall <= 0.0) {
            continue;
        }
        const double percent =
            (static_cast<double>(sample.cpu - found->second) / wall) * 100.0;
        if (percent >= 1.0) {
            ranked.push_back(Ranked{sample.name, percent});
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& left, const Ranked& right) {
        return left.percent > right.percent;
    });
    // Prefer clearly busy apps. If nothing clears 2%, keep the >=1% tail so a
    // mildly busy machine still lists something real.
    std::vector<Ranked> significant;
    for (const Ranked& row : ranked) {
        if (row.percent >= 2.0) {
            significant.push_back(row);
        }
    }
    if (significant.empty()) {
        significant = ranked;
    }
    if (significant.size() > 3) {
        significant.resize(3);
    }
    for (const Ranked& row : significant) {
        QsEnergyApp app;
        app.name = row.name;
        app.cpuPercent = std::clamp(static_cast<int>(std::lround(row.percent)), 1, 100);
        cache.energyApps.push_back(std::move(app));
    }
    g_energyBaseline.tick = now;
    g_energyBaseline.samples = std::move(current);
    cache.energyLive = true;
    if (cache.energyApps.empty()) {
        cache.energyNote = L"No apps using significant energy";
    }
}

std::wstring PowerSchemeFriendlyName(const GUID& scheme) {
    DWORD bytes = 0;
    if (PowerReadFriendlyName(nullptr, &scheme, nullptr, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t)) {
        return {};
    }
    std::wstring name(bytes / sizeof(wchar_t), L'\0');
    if (PowerReadFriendlyName(nullptr, &scheme, nullptr, nullptr,
            reinterpret_cast<UCHAR*>(name.data()), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    name.resize(wcslen(name.c_str()));
    return name;
}

bool EnumeratePowerScheme(DWORD index, GUID& scheme) {
    DWORD bytes = sizeof(scheme);
    return PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, static_cast<UCHAR>(index),
        reinterpret_cast<UCHAR*>(&scheme), &bytes) == ERROR_SUCCESS;
}

bool FindUltimatePowerScheme(GUID& scheme) {
    for (DWORD index = 0; index <= 255; ++index) {
        GUID candidate{};
        if (!EnumeratePowerScheme(index, candidate)) {
            break;
        }
        if (IsEqualGUID(candidate, kUltimatePowerSchemeSource) ||
            Lower(PowerSchemeFriendlyName(candidate)) == L"ultimate performance") {
            scheme = candidate;
            return true;
        }
    }
    return false;
}

bool DuplicateUltimatePowerScheme() {
    wchar_t commandLine[] =
        L"powercfg.exe /duplicatescheme e9a42b02-d5df-448d-aa00-03f14749eb61";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, commandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            nullptr, &startup, &process)) {
        return false;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, 10000);
    DWORD exitCode = ERROR_PROCESS_ABORTED;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && exitCode == ERROR_SUCCESS;
}

bool EnsureUltimatePowerScheme(GUID& scheme) {
    if (FindUltimatePowerScheme(scheme)) {
        return true;
    }
    return DuplicateUltimatePowerScheme() && FindUltimatePowerScheme(scheme);
}

void QueryPower(QuickSettingsCache& cache) {
    cache.powerName = L"Balanced";
    cache.powerMode = 1;
    cache.usbKnown = false;
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || scheme == nullptr) {
        return;
    }
    const std::wstring name = PowerSchemeFriendlyName(*scheme);
    if (IsEqualGUID(*scheme, GUID_MAX_POWER_SAVINGS)) {
        cache.powerMode = 2;
    } else if (IsEqualGUID(*scheme, GUID_MIN_POWER_SAVINGS) ||
        Lower(name) == L"ultimate performance") {
        cache.powerMode = 0;
    } else {
        cache.powerMode = 1;
    }
    if (!name.empty()) {
        cache.powerName = name;
    }
    DWORD usb = 0;
    if (PowerReadACValueIndex(nullptr, scheme, &kUsbSubgroup, &kUsbSuspend, &usb) == ERROR_SUCCESS) {
        cache.usbKnown = true;
        cache.usbSuspend = usb != 0;
    }
    LocalFree(scheme);
}

bool SetPowerMode(int mode) {
    GUID ultimate{};
    const GUID* scheme = &GUID_TYPICAL_POWER_SAVINGS;
    if (mode == 0) {
        if (!EnsureUltimatePowerScheme(ultimate)) {
            return false;
        }
        scheme = &ultimate;
    } else if (mode == 2) {
        scheme = &GUID_MAX_POWER_SAVINGS;
    }
    return PowerSetActiveScheme(nullptr, scheme) == ERROR_SUCCESS;
}

bool SetUsbSuspend(bool enabled) {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || scheme == nullptr) {
        return false;
    }
    const DWORD value = enabled ? 1U : 0U;
    const DWORD ac = PowerWriteACValueIndex(nullptr, scheme, &kUsbSubgroup, &kUsbSuspend, value);
    const DWORD dc = PowerWriteDCValueIndex(nullptr, scheme, &kUsbSubgroup, &kUsbSuspend, value);
    const DWORD apply = PowerSetActiveScheme(nullptr, scheme);
    LocalFree(scheme);
    return ac == ERROR_SUCCESS || dc == ERROR_SUCCESS || apply == ERROR_SUCCESS;
}

void QueryHdr(QuickSettingsCache& cache) {
    cache.hdrSupported = false;
    cache.hdrOn = false;
    UINT pathCount = 0;
    UINT modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS ||
        pathCount == 0) {
        return;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(),
            nullptr) != ERROR_SUCCESS) {
        return;
    }
    for (UINT index = 0; index < pathCount; ++index) {
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
        color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        color.header.size = sizeof(color);
        color.header.adapterId = paths[index].targetInfo.adapterId;
        color.header.id = paths[index].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS) {
            continue;
        }
        if (color.advancedColorSupported != 0) {
            cache.hdrSupported = true;
            cache.hdrOn = color.advancedColorEnabled != 0;
            return;
        }
    }
}

bool SetHdrEnabled(bool enabled) {
    UINT pathCount = 0;
    UINT modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS ||
        pathCount == 0) {
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(),
            nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool wrote = false;
    for (UINT index = 0; index < pathCount; ++index) {
        DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE state{};
        state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
        state.header.size = sizeof(state);
        state.header.adapterId = paths[index].targetInfo.adapterId;
        state.header.id = paths[index].targetInfo.id;
        state.enableAdvancedColor = enabled ? 1U : 0U;
        if (DisplayConfigSetDeviceInfo(&state.header) == ERROR_SUCCESS) {
            wrote = true;
        }
    }
    return wrote;
}

void SendMediaKey(WORD key);

bool NightLightEnabled(const BYTE* data, DWORD size) noexcept {
    if (data == nullptr || size < 24) {
        return false;
    }
    // CloudStore envelope, then an inner Bond struct that also starts with
    // 43 42 01 00. Compact-binary field 0 is the two bytes 10 00 and is
    // present only while Night Light is actually on (manual or schedule).
    // Byte 18 is the inner payload length, not the switch: on current
    // Windows 11 that length is 0x12 when the manual-transition field is
    // absent, so the old 0x15/0x19 test reported an enabled light as Off.
    DWORD payload = size;
    for (DWORD index = 1; index + 4 <= size; ++index) {
        if (data[index] == 0x43 && data[index + 1] == 0x42 && data[index + 2] == 0x01 &&
            data[index + 3] == 0x00) {
            payload = index + 4;
            break;
        }
    }
    if (payload + 1 >= size) {
        return false;
    }
    return data[payload] == 0x10 && data[payload + 1] == 0x00;
}

std::wstring DescribeCaptureError(HRESULT hr) {
    if (hr == E_ACCESSDENIED || hr == E_ACCESSDENIED) {
        return L"Microphone blocked";
    }
    switch (hr) {
    case E_ACCESSDENIED:
        return L"Microphone blocked";
    case AUDCLNT_E_DEVICE_IN_USE:
        return L"Microphone in use";
    case AUDCLNT_E_DEVICE_INVALIDATED:
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
        return L"Microphone unavailable";
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return L"Audio service stopped";
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return L"Unsupported microphone";
    default:
        break;
    }
    if (HRESULT_CODE(hr) == ERROR_ACCESS_DENIED) {
        return L"Microphone blocked";
    }
    return L"Microphone unavailable";
}

bool FormatIsFloat(const WAVEFORMATEX* format) noexcept {
    if (format == nullptr) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return false;
    }
    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    const GUID kFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    return IsEqualGUID(extensible->SubFormat, kFloat) != FALSE;
}

float PeakOfFrames(const BYTE* data, UINT32 frames, const WAVEFORMATEX* format) noexcept {
    if (data == nullptr || format == nullptr || frames == 0 || format->nChannels <= 0) {
        return 0.0F;
    }
    const int channels = format->nChannels;
    float peak = 0.0F;
    if (FormatIsFloat(format)) {
        const auto* samples = reinterpret_cast<const float*>(data);
        const size_t count = static_cast<size_t>(frames) * static_cast<size_t>(channels);
        for (size_t index = 0; index < count; ++index) {
            peak = std::max(peak, std::fabs(samples[index]));
        }
        return std::clamp(peak, 0.0F, 1.0F);
    }
    if (format->wBitsPerSample == 16) {
        const auto* samples = reinterpret_cast<const int16_t*>(data);
        const size_t count = static_cast<size_t>(frames) * static_cast<size_t>(channels);
        for (size_t index = 0; index < count; ++index) {
            peak = std::max(peak, std::fabs(static_cast<float>(samples[index])) / 32768.0F);
        }
        return std::clamp(peak, 0.0F, 1.0F);
    }
    if (format->wBitsPerSample == 32) {
        const auto* samples = reinterpret_cast<const int32_t*>(data);
        const size_t count = static_cast<size_t>(frames) * static_cast<size_t>(channels);
        for (size_t index = 0; index < count; ++index) {
            peak = std::max(peak, std::fabs(static_cast<float>(samples[index])) / 2147483648.0F);
        }
        return std::clamp(peak, 0.0F, 1.0F);
    }
    return 0.0F;
}

struct CaptureMeter {
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* format = nullptr;
    std::wstring deviceId;
    std::wstring note;
    float level = 0.0F;
    ULONGLONG retryAt = 0;
    bool started = false;
    // WASAPI drain runs on a ~25 Hz background pump so the UI thread never
    // touches GetBuffer while meters twitch.
    std::mutex mutex;
    std::atomic<bool> pumpRun{false};
    std::thread pumpThread;
    std::atomic<float> publishedLevel{0.0F};
    std::wstring publishedNote;
    // Serialises the pumpThread lifecycle. StopPump/StartPump are reached
    // concurrently from the UI thread (StopQuickSettingsCapture on prewarm /
    // hide), the network cache worker (QueryCapture -> Sample) and the
    // capture-open worker (EnsureQuickSettingsCaptureAsync). Unsynchronised
    // join()/move-assign on the same std::thread made join() throw inside a
    // noexcept function -> std::terminate -> abort (0xc0000409 in ucrtbase at
    // launch, 1.1.67). Lock order: never take this while holding `mutex`; the
    // pump thread itself never takes it, so joining under it cannot deadlock.
    std::mutex pumpControlMutex;

    void StopPumpLocked() noexcept {
        pumpRun.store(false, std::memory_order_release);
        if (!pumpThread.joinable()) {
            return;
        }
        try {
            if (pumpThread.get_id() == std::this_thread::get_id()) {
                pumpThread.detach();
            } else {
                pumpThread.join();
            }
        } catch (...) {
            // Never let a join failure escape a noexcept path; drop the handle
            // so a later StartPump cannot move-assign over a joinable thread.
            try {
                if (pumpThread.joinable()) {
                    pumpThread.detach();
                }
            } catch (...) {
            }
        }
    }

    void StopPump() noexcept {
        std::lock_guard<std::mutex> control(pumpControlMutex);
        StopPumpLocked();
    }

    void ReleaseResources() noexcept {
        if (client != nullptr && started) {
            client->Stop();
        }
        started = false;
        if (capture != nullptr) {
            capture->Release();
            capture = nullptr;
        }
        if (client != nullptr) {
            client->Release();
            client = nullptr;
        }
        if (format != nullptr) {
            CoTaskMemFree(format);
            format = nullptr;
        }
        deviceId.clear();
    }

    void Close() noexcept {
        StopPump();
        std::lock_guard<std::mutex> lock(mutex);
        ReleaseResources();
        level = 0.0F;
        publishedLevel.store(0.0F, std::memory_order_relaxed);
        publishedNote.clear();
    }

    void StartPump() {
        std::lock_guard<std::mutex> control(pumpControlMutex);
        if (pumpRun.load(std::memory_order_acquire) && pumpThread.joinable()) {
            return;
        }
        StopPumpLocked();
        pumpRun.store(true, std::memory_order_release);
        pumpThread = std::thread([this] {
            while (pumpRun.load(std::memory_order_acquire)) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (client != nullptr && capture != nullptr) {
                        const float instant = DrainUnlocked();
                        if (consumed) {
                            if (instant >= level) {
                                level = instant;
                            } else {
                                level = std::max(instant, level * 0.55F);
                                if (level < 0.01F) {
                                    level = 0.0F;
                                }
                            }
                        }
                        if (note.empty()) {
                            publishedLevel.store(std::clamp(level, 0.0F, 1.0F), std::memory_order_relaxed);
                            publishedNote.clear();
                        } else {
                            publishedLevel.store(0.0F, std::memory_order_relaxed);
                            publishedNote = note;
                        }
                    }
                }
                Sleep(40);  // ~25 Hz
            }
        });
    }

    bool Open(IMMDevice* device) {
        // Caller holds mutex (pump stopped). Tear down without StopPump.
        ReleaseResources();
        note.clear();
        if (device == nullptr) {
            note = L"No microphone";
            retryAt = GetTickCount64() + 2000ULL;
            return false;
        }
        const std::wstring id = DeviceId(device);
        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&client));
        if (FAILED(hr) || client == nullptr) {
            note = DescribeCaptureError(hr);
            retryAt = GetTickCount64() + 2000ULL;
            ReleaseResources();
            return false;
        }
        WAVEFORMATEX desired{};
        desired.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        desired.nChannels = 2;
        desired.nSamplesPerSec = 48000;
        desired.wBitsPerSample = 32;
        desired.nBlockAlign = 8;
        desired.nAvgBytesPerSec = 48000 * 8;
        const DWORD convertFlags =
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, convertFlags, 2000000, 0, &desired,
            nullptr);
        if (SUCCEEDED(hr)) {
            format = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
            if (format == nullptr) {
                note = L"Microphone unavailable";
                ReleaseResources();
                return false;
            }
            *format = desired;
        } else {
            client->Release();
            client = nullptr;
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&client));
            if (FAILED(hr) || client == nullptr) {
                note = DescribeCaptureError(hr);
                retryAt = GetTickCount64() + 2000ULL;
                return false;
            }
            hr = client->GetMixFormat(&format);
            if (FAILED(hr) || format == nullptr) {
                note = L"Unsupported microphone";
                retryAt = GetTickCount64() + 2000ULL;
                ReleaseResources();
                return false;
            }
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, format, nullptr);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED && format->nSamplesPerSec > 0) {
                UINT32 frames = 0;
                client->GetBufferSize(&frames);
                client->Release();
                client = nullptr;
                hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(&client));
                const REFERENCE_TIME aligned = static_cast<REFERENCE_TIME>(
                    (10000000.0 * static_cast<double>(frames) / format->nSamplesPerSec) + 0.5);
                if (SUCCEEDED(hr) && client != nullptr) {
                    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, aligned, 0, format, nullptr);
                }
            }
        }
        if (FAILED(hr) || client == nullptr) {
            note = DescribeCaptureError(hr);
            retryAt = GetTickCount64() + 2000ULL;
            ReleaseResources();
            return false;
        }
        hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
        if (FAILED(hr) || capture == nullptr) {
            note = DescribeCaptureError(hr);
            retryAt = GetTickCount64() + 2000ULL;
            ReleaseResources();
            return false;
        }
        hr = client->Start();
        if (FAILED(hr)) {
            note = DescribeCaptureError(hr);
            retryAt = GetTickCount64() + 2000ULL;
            ReleaseResources();
            return false;
        }
        started = true;
        deviceId = id;
        note.clear();
        level = 0.0F;
        return true;
    }

    bool consumed = false;

    float DrainUnlocked() noexcept {
        consumed = false;
        if (capture == nullptr || format == nullptr) {
            return 0.0F;
        }
        float peak = 0.0F;
        for (;;) {
            UINT32 packet = 0;
            const HRESULT next = capture->GetNextPacketSize(&packet);
            if (FAILED(next)) {
                note = L"Microphone unavailable";
                retryAt = GetTickCount64() + 500ULL;
                ReleaseResources();
                level = 0.0F;
                publishedLevel.store(0.0F, std::memory_order_relaxed);
                publishedNote = note;
                break;
            }
            if (packet == 0) {
                break;
            }
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
                break;
            }
            consumed = true;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
                peak = std::max(peak, PeakOfFrames(data, frames, format));
            }
            capture->ReleaseBuffer(frames);
        }
        return peak;
    }

    float Drain() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        return DrainUnlocked();
    }

    void ReadPublished(QuickSettingsCache& cache) {
        cache.inputPeak = publishedLevel.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mutex);
        cache.inputNote = publishedNote;
        if (!publishedNote.empty()) {
            cache.inputPeak = 0.0F;
        }
    }

    void Sample(QuickSettingsCache& cache, IMMDevice* device) {
        const ULONGLONG now = GetTickCount64();
        const std::wstring id = device != nullptr ? DeviceId(device) : std::wstring();
        if (device == nullptr) {
            Close();
            {
                std::lock_guard<std::mutex> lock(mutex);
                note = L"No microphone";
                publishedNote = note;
            }
            publishedLevel.store(0.0F, std::memory_order_relaxed);
            cache.inputPeak = 0.0F;
            cache.inputNote = L"No microphone";
            return;
        }
        bool needOpen = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            needOpen = client == nullptr || deviceId != id;
            if (needOpen && now < retryAt && deviceId.empty() && !note.empty()) {
                cache.inputPeak = 0.0F;
                cache.inputNote = note;
                return;
            }
        }
        if (needOpen) {
            StopPump();
            std::lock_guard<std::mutex> lock(mutex);
            if (!Open(device)) {
                publishedNote = note;
                publishedLevel.store(0.0F, std::memory_order_relaxed);
                cache.inputPeak = 0.0F;
                cache.inputNote = note;
                return;
            }
            publishedNote.clear();
        }
        StartPump();
        ReadPublished(cache);
    }
};

CaptureMeter g_captureMeter;

using MediaManager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using MediaSession = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using MediaStatus = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;

struct MediaBridge {
    std::mutex mutex;
    MediaManager manager{nullptr};
    MediaSession session{nullptr};
    bool requestPending = false;
    bool propsPending = false;
    std::wstring title;
    std::wstring artist;
    bool have = false;
    bool playing = false;
    float progress = 0.0F;
    ULONGLONG lastSessionQuery = 0;
    ULONGLONG lastProgressQuery = 0;
};

MediaBridge g_media;

std::wstring WideFromHstring(const winrt::hstring& value) {
    return std::wstring(value.c_str());
}

void EnsureWinRt() {
    static bool ready = false;
    if (ready) {
        return;
    }
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        ready = true;
    } catch (const winrt::hresult_error&) {
        ready = true;
    }
}

void EnsureMediaManager() {
    EnsureWinRt();
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(g_media.mutex);
        start = !g_media.manager && !g_media.requestPending;
        if (start) {
            g_media.requestPending = true;
        }
    }
    if (!start) {
        return;
    }
    try {
        auto operation = MediaManager::RequestAsync();
        operation.Completed([](auto const& async, winrt::Windows::Foundation::AsyncStatus status) {
            MediaManager created{nullptr};
            if (status == winrt::Windows::Foundation::AsyncStatus::Completed) {
                try {
                    created = async.GetResults();
                } catch (const winrt::hresult_error&) {
                    created = nullptr;
                }
            }
            std::lock_guard<std::mutex> lock(g_media.mutex);
            g_media.manager = created;
            g_media.requestPending = false;
        });
    } catch (const winrt::hresult_error&) {
        std::lock_guard<std::mutex> lock(g_media.mutex);
        g_media.requestPending = false;
    }
}

MediaSession PickMediaSession(const MediaManager& manager) {
    MediaSession current{nullptr};
    MediaSession playing{nullptr};
    MediaSession paused{nullptr};
    try {
        current = manager.GetCurrentSession();
    } catch (const winrt::hresult_error&) {
        current = nullptr;
    }
    auto playingOf = [](const MediaSession& session) {
        try {
            return session.GetPlaybackInfo().PlaybackStatus();
        } catch (const winrt::hresult_error&) {
            return MediaStatus::Closed;
        }
    };
    if (current && playingOf(current) == MediaStatus::Playing) {
        return current;
    }
    try {
        for (const MediaSession& session : manager.GetSessions()) {
            const MediaStatus status = playingOf(session);
            if (status == MediaStatus::Playing && !playing) {
                playing = session;
            } else if (status == MediaStatus::Paused && !paused) {
                paused = session;
            }
        }
    } catch (const winrt::hresult_error&) {
    }
    if (playing) {
        return playing;
    }
    if (current) {
        return current;
    }
    return paused;
}

void RequestMediaProperties(const MediaSession& session) {
    {
        std::lock_guard<std::mutex> lock(g_media.mutex);
        if (g_media.propsPending) {
            return;
        }
        g_media.propsPending = true;
    }
    try {
        auto operation = session.TryGetMediaPropertiesAsync();
        operation.Completed([](auto const& async, winrt::Windows::Foundation::AsyncStatus status) {
            std::wstring title;
            std::wstring artist;
            bool have = false;
            if (status == winrt::Windows::Foundation::AsyncStatus::Completed) {
                try {
                    auto props = async.GetResults();
                    title = WideFromHstring(props.Title());
                    artist = WideFromHstring(props.Artist());
                    if (artist.empty()) {
                        artist = WideFromHstring(props.AlbumArtist());
                    }
                    have = !title.empty() || !artist.empty();
                } catch (const winrt::hresult_error&) {
                    have = false;
                }
            }
            std::lock_guard<std::mutex> lock(g_media.mutex);
            if (have) {
                g_media.title = std::move(title);
                g_media.artist = std::move(artist);
            }
            g_media.propsPending = false;
        });
    } catch (const winrt::hresult_error&) {
        std::lock_guard<std::mutex> lock(g_media.mutex);
        g_media.propsPending = false;
    }
}

float ReadMediaProgress(const MediaSession& session) noexcept {
    try {
        const auto timeline = session.GetTimelineProperties();
        const auto start = timeline.StartTime().count();
        const auto end = timeline.EndTime().count();
        const auto position = timeline.Position().count();
        const auto span = end - start;
        if (span <= 0) {
            return 0.0F;
        }
        return std::clamp(static_cast<float>(position - start) / static_cast<float>(span), 0.0F, 1.0F);
    } catch (const winrt::hresult_error&) {
        return 0.0F;
    }
}

void PublishMedia(QuickSettingsCache& cache) {
    std::lock_guard<std::mutex> lock(g_media.mutex);
    cache.mediaTitle = g_media.title;
    cache.mediaArtist = g_media.artist;
    cache.mediaHave = g_media.have;
    cache.mediaPlaying = g_media.playing;
    cache.mediaProgress = g_media.progress;
}

void QueryMediaSession() {
    EnsureMediaManager();
    MediaManager manager{nullptr};
    {
        std::lock_guard<std::mutex> lock(g_media.mutex);
        manager = g_media.manager;
    }
    if (!manager) {
        return;
    }
    try {
        MediaSession session = PickMediaSession(manager);
        if (!session) {
            std::lock_guard<std::mutex> lock(g_media.mutex);
            g_media.session = nullptr;
            g_media.have = false;
            g_media.playing = false;
            g_media.progress = 0.0F;
            g_media.title.clear();
            g_media.artist.clear();
        } else {
            const MediaStatus status = session.GetPlaybackInfo().PlaybackStatus();
            const bool playing = status == MediaStatus::Playing;
            std::wstring source;
            try {
                source = WideFromHstring(session.SourceAppUserModelId());
            } catch (const winrt::hresult_error&) {
                source.clear();
            }
            {
                std::lock_guard<std::mutex> lock(g_media.mutex);
                g_media.session = session;
                g_media.playing = playing;
                g_media.have = true;
                if (g_media.title.empty() && !source.empty()) {
                    g_media.artist = source;
                }
            }
            RequestMediaProperties(session);
        }
    } catch (const winrt::hresult_error&) {
    }
}

void QueryMediaProgress() {
    MediaSession session{nullptr};
    {
        std::lock_guard<std::mutex> lock(g_media.mutex);
        session = g_media.session;
    }
    if (!session) {
        return;
    }
    const float progress = ReadMediaProgress(session);
    std::lock_guard<std::mutex> lock(g_media.mutex);
    g_media.progress = progress;
}

void QueryMedia(QuickSettingsCache& cache, bool forceSession = false, bool forceProgress = false) {
    const ULONGLONG now = GetTickCount64();
    bool doSession = forceSession;
    bool doProgress = forceProgress;
    {
        std::lock_guard<std::mutex> lock(g_media.mutex);
        if (!doSession && (g_media.lastSessionQuery == 0 || now - g_media.lastSessionQuery >= 1000ULL)) {
            doSession = true;
        }
        if (!doProgress && (g_media.lastProgressQuery == 0 || now - g_media.lastProgressQuery >= 200ULL)) {
            doProgress = true;
        }
    }
    if (doSession) {
        QueryMediaSession();
        std::lock_guard<std::mutex> lock(g_media.mutex);
        g_media.lastSessionQuery = now;
    }
    if (doProgress) {
        QueryMediaProgress();
        std::lock_guard<std::mutex> lock(g_media.mutex);
        g_media.lastProgressQuery = now;
    }
    PublishMedia(cache);
}

void ControlMediaSession(int command) {
    MediaSession session{nullptr};
    {
        std::lock_guard<std::mutex> lock(g_media.mutex);
        session = g_media.session;
    }
    if (!session) {
        if (command == 0) {
            SendMediaKey(VK_MEDIA_PREV_TRACK);
        } else if (command == 2) {
            SendMediaKey(VK_MEDIA_NEXT_TRACK);
        } else {
            SendMediaKey(VK_MEDIA_PLAY_PAUSE);
        }
        return;
    }
    try {
        if (command == 0) {
            session.TrySkipPreviousAsync();
        } else if (command == 2) {
            session.TrySkipNextAsync();
        } else {
            session.TryTogglePlayPauseAsync();
        }
    } catch (const winrt::hresult_error&) {
        if (command == 0) {
            SendMediaKey(VK_MEDIA_PREV_TRACK);
        } else if (command == 2) {
            SendMediaKey(VK_MEDIA_NEXT_TRACK);
        } else {
            SendMediaKey(VK_MEDIA_PLAY_PAUSE);
        }
    }
}

int MeterBucket(float peak) noexcept {
    const float shaped = std::pow(std::clamp(peak, 0.0F, 1.0F), 0.45F);
    return static_cast<int>(std::lround(shaped * 32.0F));
}

void QueryNightLight(QuickSettingsCache& cache) {
    cache.nightKnown = false;
    cache.nightLight = false;
    HKEY key = nullptr;
    const wchar_t* path =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current\\"
        L"default$windows.data.bluelightreduction.bluelightreductionstate\\"
        L"windows.data.bluelightreduction.bluelightreductionstate";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return;
    }
    DWORD size = 0;
    DWORD type = 0;
    if (RegQueryValueExW(key, L"Data", nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        type == REG_BINARY && size > 22) {
        std::vector<BYTE> data(size);
        if (RegQueryValueExW(key, L"Data", nullptr, &type, data.data(), &size) == ERROR_SUCCESS &&
            size > 18) {
            cache.nightKnown = true;
            cache.nightLight = NightLightEnabled(data.data(), size);
        }
    }
    RegCloseKey(key);
}

void QueryCapture(QuickSettingsCache& cache) {
    cache.inputPeak = 0.0F;
    cache.inputGain = 1.0F;
    cache.inputMuted = false;
    cache.inputNote.clear();
    const bool opened = WithEndpoint(eCapture, [&](IMMDevice* device) {
        cache.inputName = DeviceName(device);
        IAudioEndpointVolume* volume = nullptr;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&volume))) &&
            volume != nullptr) {
            float level = 1.0F;
            BOOL muted = FALSE;
            if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&level))) {
                cache.inputGain = std::clamp(level, 0.0F, 1.0F);
            }
            if (SUCCEEDED(volume->GetMute(&muted))) {
                cache.inputMuted = muted != FALSE;
            }
            volume->Release();
        }
        g_captureMeter.Sample(cache, device);
        return true;
    });
    if (!opened) {
        g_captureMeter.Close();
        cache.inputPeak = 0.0F;
        cache.inputNote = L"No microphone";
    }
}

void FillQsNetworkCache(QuickSettingsCache& cache, QuickSettingsPage page) {
    // Heavy WLAN / IP helper / MMDevice / WASAPI / power / HDR / Night Light.
    // Runs only on the network worker - never on the UI thread.
    QueryWifiRadio(cache);
    if (page == QuickSettingsPage::Wifi) {
        QueryWifiNetworks(cache);
    } else {
        cache.wifi.clear();
    }
    QueryAdapters(cache);
    CollectEndpoints(eRender, cache.renderDevices, cache.renderDefaultId, cache.outputName);
    CollectEndpoints(eCapture, cache.captureDevices, cache.captureDefaultId, cache.inputName);
    QueryCapture(cache);
    QueryPower(cache);
    QueryHdr(cache);
    QueryNightLight(cache);
    const ULONGLONG now = GetTickCount64();
    cache.stampNetwork = now;
    cache.stampNight = now;
    cache.stamp = now;
}

bool SetCaptureMuted(bool muted) {
    return WithEndpoint(eCapture, [&](IMMDevice* device) {
        IAudioEndpointVolume* volume = nullptr;
        if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&volume))) ||
            volume == nullptr) {
            return false;
        }
        const HRESULT result = volume->SetMute(muted ? TRUE : FALSE, nullptr);
        volume->Release();
        return SUCCEEDED(result);
    });
}

bool SetCaptureGain(float level) {
    level = std::clamp(level, 0.0F, 1.0F);
    return WithEndpoint(eCapture, [&](IMMDevice* device) {
        IAudioEndpointVolume* volume = nullptr;
        if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&volume))) ||
            volume == nullptr) {
            return false;
        }
        const HRESULT result = volume->SetMasterVolumeLevelScalar(level, nullptr);
        if (SUCCEEDED(result)) {
            volume->SetMute(FALSE, nullptr);
        }
        volume->Release();
        return SUCCEEDED(result);
    });
}

void SendMediaKey(WORD key) {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = key;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = key;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}


void FillHGradientPill(uint8_t* dest, int destWidth, int destHeight, float x0, float x1, float cy,
    float radius, float alpha, uint8_t b0, uint8_t g0, uint8_t r0, uint8_t b1, uint8_t g1,
    uint8_t r1) {
    if (dest == nullptr || alpha <= 0.0F || x1 <= x0 + 0.5F || radius <= 0.5F) {
        return;
    }
    // Include the full end-cap discs (x0/x1 are stadium segment centers).
    // Bounding only to x0..x1 clipped the semicircles and left square ends.
    const int left = std::max(0, static_cast<int>(std::floor(x0 - radius - 2.0F)));
    const int right = std::min(destWidth, static_cast<int>(std::ceil(x1 + radius + 2.0F)));
    const int top = std::max(0, static_cast<int>(std::floor(cy - radius - 2.0F)));
    const int bottom = std::min(destHeight, static_cast<int>(std::ceil(cy + radius + 2.0F)));
    const float span = std::max(1.0F, x1 - x0);
    constexpr float aa = 1.2F;
    for (int y = top; y < bottom; ++y) {
        const float dy = std::fabs(static_cast<float>(y) + 0.5F - cy);
        for (int x = left; x < right; ++x) {
            const float px = static_cast<float>(x) + 0.5F;
            float dx = 0.0F;
            if (px < x0) {
                dx = x0 - px;
            } else if (px > x1) {
                dx = px - x1;
            }
            const float sd = (dx > 0.0F ? std::sqrt(dx * dx + dy * dy) : dy) - radius;
            const float coverage = 1.0F - std::clamp(sd / aa + 0.5F, 0.0F, 1.0F);
            if (coverage <= 0.0F) {
                continue;
            }
            const float t = std::clamp((px - x0) / span, 0.0F, 1.0F);
            const float srcA = coverage * alpha;
            const auto channel = [&](uint8_t from, uint8_t to) {
                return static_cast<uint8_t>(std::lround(
                    ((1.0F - t) * static_cast<float>(from) + t * static_cast<float>(to)) * srcA));
            };
            uint8_t pixel[4] = {channel(b0, b1), channel(g0, g1), channel(r0, r1),
                static_cast<uint8_t>(std::lround(255.0F * srcA))};
            CompositePremul(dest, destWidth, destHeight, x, y, pixel, 1, 1);
        }
    }
}

void FillSoftDisc(uint8_t* dest, int destWidth, int destHeight, float cx, float cy, float radius,
    float alpha, uint8_t blue, uint8_t green, uint8_t red) {
    if (dest == nullptr || alpha <= 0.0F || radius <= 0.5F) {
        return;
    }
    const int left = std::max(0, static_cast<int>(std::floor(cx - radius - 2.0F)));
    const int top = std::max(0, static_cast<int>(std::floor(cy - radius - 2.0F)));
    const int right = std::min(destWidth, static_cast<int>(std::ceil(cx + radius + 2.0F)));
    const int bottom = std::min(destHeight, static_cast<int>(std::ceil(cy + radius + 2.0F)));
    constexpr float aa = 1.25F;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - cx;
            const float dy = static_cast<float>(y) + 0.5F - cy;
            const float sd = std::sqrt(dx * dx + dy * dy) - radius;
            const float coverage = 1.0F - std::clamp(sd / aa + 0.5F, 0.0F, 1.0F);
            if (coverage <= 0.0F) {
                continue;
            }
            const float srcA = coverage * alpha;
            uint8_t pixel[4] = {
                static_cast<uint8_t>(std::lround(static_cast<float>(blue) * srcA)),
                static_cast<uint8_t>(std::lround(static_cast<float>(green) * srcA)),
                static_cast<uint8_t>(std::lround(static_cast<float>(red) * srcA)),
                static_cast<uint8_t>(std::lround(255.0F * srcA)),
            };
            CompositePremul(dest, destWidth, destHeight, x, y, pixel, 1, 1);
        }
    }
}

void FillAlbumDisc(uint8_t* dest, int destWidth, int destHeight, float cx, float cy, float radius) {
    if (dest == nullptr || radius <= 1.0F) {
        return;
    }
    const int left = std::max(0, static_cast<int>(std::floor(cx - radius - 2.0F)));
    const int top = std::max(0, static_cast<int>(std::floor(cy - radius - 2.0F)));
    const int right = std::min(destWidth, static_cast<int>(std::ceil(cx + radius + 2.0F)));
    const int bottom = std::min(destHeight, static_cast<int>(std::ceil(cy + radius + 2.0F)));
    constexpr float aa = 1.2F;
    const float diameter = std::max(1.0F, radius * 2.0F);
    for (int y = top; y < bottom; ++y) {
        const float py = static_cast<float>(y) + 0.5F;
        const float v = std::clamp((py - (cy - radius)) / diameter, 0.0F, 1.0F);
        for (int x = left; x < right; ++x) {
            const float px = static_cast<float>(x) + 0.5F;
            const float dx = px - cx;
            const float dy = py - cy;
            const float dist = std::sqrt(dx * dx + dy * dy);
            const float coverage = 1.0F - std::clamp((dist - radius) / aa + 0.5F, 0.0F, 1.0F);
            if (coverage <= 0.0F) {
                continue;
            }
            const float u = std::clamp((px - (cx - radius)) / diameter, 0.0F, 1.0F);
            // Stand-in for the mock's circular sunset: navy sky, warm horizon,
            // ridge line, and a dark reflection. Not the source photo.
            float red = 0.0F;
            float green = 0.0F;
            float blue = 0.0F;
            if (v < 0.22F) {
                const float t = v / 0.22F;
                red = 18.0F + t * 40.0F;
                green = 16.0F + t * 10.0F;
                blue = 48.0F + t * 36.0F;
            } else if (v < 0.48F) {
                const float t = (v - 0.22F) / 0.26F;
                red = 58.0F + t * 150.0F;
                green = 26.0F + t * 40.0F;
                blue = 84.0F + t * 40.0F;
            } else if (v < 0.62F) {
                const float t = (v - 0.48F) / 0.14F;
                red = 208.0F + t * 40.0F;
                green = 66.0F + t * 70.0F;
                blue = 124.0F - t * 70.0F;
            } else {
                red = 36.0F;
                green = 22.0F;
                blue = 32.0F;
            }
            if (v < 0.28F) {
                const unsigned h = static_cast<unsigned>(x * 374761393 + y * 668265263);
                const unsigned n = (h ^ (h >> 13)) * 1274126177U;
                if ((n & 1023U) > 1008U) {
                    red = green = blue = 230.0F;
                }
            }
            const float sunDx = u - 0.50F;
            const float sunDy = (v - 0.575F) * 1.15F;
            const float sun = std::sqrt(sunDx * sunDx + sunDy * sunDy);
            if (sun < 0.055F) {
                red = 255.0F;
                green = 214.0F;
                blue = 150.0F;
            } else if (sun < 0.16F && v < 0.70F) {
                const float g = 1.0F - (sun - 0.055F) / 0.105F;
                red = red * (1.0F - g) + 255.0F * g;
                green = green * (1.0F - g) + 140.0F * g;
                blue = blue * (1.0F - g) + 70.0F * g;
            }
            const float ridge = 0.60F + 0.040F * std::sin(u * 10.5F) + 0.028F * std::sin(u * 18.0F + 1.1F);
            if (v > ridge && v < 0.74F) {
                const float depth = std::clamp((v - ridge) / 0.08F, 0.0F, 1.0F);
                red = 28.0F + (1.0F - depth) * 50.0F;
                green = 18.0F + (1.0F - depth) * 16.0F;
                blue = 36.0F + (1.0F - depth) * 20.0F;
            }
            if (v >= 0.74F) {
                const float t = (v - 0.74F) / 0.26F;
                red = 70.0F - t * 48.0F;
                green = 28.0F - t * 16.0F;
                blue = 48.0F - t * 28.0F;
                if (std::fabs(u - 0.50F) < 0.015F + (1.0F - t) * 0.01F) {
                    red = 220.0F;
                    green = 130.0F;
                    blue = 80.0F;
                }
            }
            const float edge = std::clamp(dist / radius, 0.0F, 1.0F);
            const float shade = 1.0F - 0.18F * edge * edge;
            const float srcA = coverage;
            uint8_t pixel[4] = {
                static_cast<uint8_t>(std::lround(std::clamp(blue * shade, 0.0F, 255.0F) * srcA)),
                static_cast<uint8_t>(std::lround(std::clamp(green * shade, 0.0F, 255.0F) * srcA)),
                static_cast<uint8_t>(std::lround(std::clamp(red * shade, 0.0F, 255.0F) * srcA)),
                static_cast<uint8_t>(std::lround(255.0F * srcA)),
            };
            CompositePremul(dest, destWidth, destHeight, x, y, pixel, 1, 1);
        }
    }
}

// Outline thermometer (stem capsule + bulb) with a filled bulb and mercury
// column, stroked to match the Fluent glyphs on the neighbouring tiles. No
// Segoe Fluent/MDL2 thermometer glyph exists, so it is drawn from an SDF.
void FillThermometerPremul(uint8_t* dest, int destWidth, int destHeight, float cx, float top,
    float extent, float stroke, uint8_t red, uint8_t green, uint8_t blue) {
    if (dest == nullptr || extent < 4.0F) {
        return;
    }
    const float bulbR = extent * 0.25F;
    const float stemR = extent * 0.14F;
    const float bulbCy = top + extent - bulbR;
    const float stemTop = top + stemR;
    const float half = stroke * 0.5F;
    const float dotR = std::max(1.0F, bulbR - stroke * 1.7F);
    const float columnR = std::max(0.7F, stroke * 0.55F);
    const float columnTop = top + extent * 0.38F;
    const int left = static_cast<int>(std::floor(cx - bulbR - 2.0F));
    const int right = static_cast<int>(std::ceil(cx + bulbR + 2.0F));
    const int y0 = static_cast<int>(std::floor(top - 2.0F));
    const int y1 = static_cast<int>(std::ceil(top + extent + 2.0F));
    const int boxW = right - left;
    const int boxH = y1 - y0;
    if (boxW <= 0 || boxH <= 0) {
        return;
    }
    std::vector<uint8_t> glyph(static_cast<size_t>(boxW) * static_cast<size_t>(boxH) * 4U, 0);
    for (int y = 0; y < boxH; ++y) {
        for (int x = 0; x < boxW; ++x) {
            const float px = static_cast<float>(left + x) + 0.5F;
            const float py = static_cast<float>(y0 + y) + 0.5F;
            const float dx = px - cx;
            const float stemY = std::clamp(py, stemTop, bulbCy);
            const float outline = std::min(std::hypot(dx, py - stemY) - stemR, std::hypot(dx, py - bulbCy) - bulbR);
            const float colY = std::clamp(py, columnTop, bulbCy);
            const float fill = std::min(std::hypot(dx, py - bulbCy) - dotR, std::hypot(dx, py - colY) - columnR);
            const float ring = std::clamp(half + 0.5F - std::fabs(outline), 0.0F, 1.0F);
            const float core = std::clamp(0.5F - fill, 0.0F, 1.0F);
            const float coverage = std::max(ring, core);
            if (coverage <= 0.0F) {
                continue;
            }
            uint8_t* out = glyph.data() + (static_cast<size_t>(y) * static_cast<size_t>(boxW) + static_cast<size_t>(x)) * 4U;
            out[0] = static_cast<uint8_t>(std::lround(static_cast<float>(blue) * coverage));
            out[1] = static_cast<uint8_t>(std::lround(static_cast<float>(green) * coverage));
            out[2] = static_cast<uint8_t>(std::lround(static_cast<float>(red) * coverage));
            out[3] = static_cast<uint8_t>(std::lround(255.0F * coverage));
        }
    }
    CompositePremul(dest, destWidth, destHeight, left, y0, glyph.data(), boxW, boxH);
}

std::wstring TempPowerText(int celsius, int watts) {
    // Two lines when both values exist so the narrow CPU/GPU columns can show
    // the full reading (single-line "XX°C · YYW" clipped on the home row).
    if (celsius < 0 && watts < 0) {
        return std::wstring(L"-");
    }
    std::wstring out;
    if (celsius >= 0) {
        out += std::to_wstring(celsius);
        out += L"°C";
    }
    if (watts >= 0) {
        if (!out.empty()) {
            out += L'\n';
        }
        out += std::to_wstring(watts);
        out += L'W';
    }
    return out;
}

}  // namespace

namespace {

void BlitIcon(uint8_t* pixels, int width, int height, HDC memory, HICON icon, int x, int y,
    int extent) {
    if (pixels == nullptr || memory == nullptr || icon == nullptr || extent <= 0) {
        return;
    }
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = extent;
    header.bV5Height = -extent;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000U;
    header.bV5GreenMask = 0x0000ff00U;
    header.bV5BlueMask = 0x000000ffU;
    header.bV5AlphaMask = 0xff000000U;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, reinterpret_cast<const BITMAPINFO*>(&header),
        DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        return;
    }
    HDC iconDc = CreateCompatibleDC(memory);
    if (iconDc == nullptr) {
        DeleteObject(bitmap);
        return;
    }
    HGDIOBJ previous = SelectObject(iconDc, bitmap);
    std::memset(bits, 0, static_cast<size_t>(extent) * static_cast<size_t>(extent) * 4U);
    DrawIconEx(iconDc, 0, 0, icon, extent, extent, 0, nullptr, DI_NORMAL);
    auto* iconPixels = static_cast<uint8_t*>(bits);
    const size_t count = static_cast<size_t>(extent) * static_cast<size_t>(extent);
    for (size_t index = 0; index < count; ++index) {
        uint8_t* sample = iconPixels + index * 4U;
        if ((sample[0] | sample[1] | sample[2]) != 0 && sample[3] == 0) {
            sample[3] = 255;
        }
    }
    CompositePremul(pixels, width, height, x, y, iconPixels, extent, extent);
    SelectObject(iconDc, previous);
    DeleteDC(iconDc);
    DeleteObject(bitmap);
}

}  // namespace

void SeekQuickSettingsMedia(float level) noexcept {
    level = std::clamp(level, 0.0F, 1.0F);
    MediaSession session{nullptr};
    {
        std::lock_guard<std::mutex> lock(g_media.mutex);
        session = g_media.session;
        g_media.progress = level;
    }
    if (!session) {
        return;
    }
    try {
        const auto timeline = session.GetTimelineProperties();
        const auto start = timeline.StartTime().count();
        const auto end = timeline.EndTime().count();
        const auto span = end - start;
        if (span <= 0) {
            return;
        }
        const auto target = start + static_cast<int64_t>(std::llround(static_cast<double>(span) * level));
        session.TryChangePlaybackPositionAsync(target);
    } catch (const winrt::hresult_error&) {
    }
}

void StopQuickSettingsCapture() noexcept {
    g_captureMeter.Close();
    g_captureMeter.retryAt = 0;
}

void EnsureQuickSettingsCaptureAsync() noexcept {
    bool needOpen = false;
    {
        std::lock_guard<std::mutex> lock(g_captureMeter.mutex);
        needOpen = g_captureMeter.client == nullptr || g_captureMeter.capture == nullptr;
        if (needOpen && GetTickCount64() < g_captureMeter.retryAt && !g_captureMeter.note.empty() &&
            g_captureMeter.deviceId.empty()) {
            return;
        }
    }
    if (!needOpen) {
        return;
    }
    if (g_captureOpenInFlight.exchange(true)) {
        return;
    }
    std::thread([] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        QuickSettingsCache local{};
        const bool opened = WithEndpoint(eCapture, [&](IMMDevice* device) {
            g_captureMeter.Sample(local, device);
            return true;
        });
        if (!opened) {
            g_captureMeter.Close();
            std::lock_guard<std::mutex> lock(g_captureMeter.mutex);
            g_captureMeter.note = L"No microphone";
            g_captureMeter.publishedNote = g_captureMeter.note;
            g_captureMeter.publishedLevel.store(0.0F, std::memory_order_relaxed);
        }
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
        g_captureOpenInFlight.store(false);
    }).detach();
}

void RequestQuickSettingsCacheAsync(HWND notifyHwnd, UINT notifyMsg, QuickSettingsPage page) {
    if (notifyHwnd == nullptr || notifyMsg == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_network.mutex);
        g_network.notifyHwnd = notifyHwnd;
        g_network.notifyMsg = notifyMsg;
        g_network.page = page;
        if (g_network.inFlight) {
            return;
        }
        g_network.inFlight = true;
    }
    std::thread([] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        QuickSettingsPage page = QuickSettingsPage::Home;
        {
            std::lock_guard<std::mutex> lock(g_network.mutex);
            page = g_network.page;
        }
        QuickSettingsCache local{};
        FillQsNetworkCache(local, page);
        HWND hwnd = nullptr;
        UINT msg = 0;
        {
            std::lock_guard<std::mutex> lock(g_network.mutex);
            g_network.result = std::move(local);
            g_network.haveResult = true;
            g_network.inFlight = false;
            hwnd = g_network.notifyHwnd;
            msg = g_network.notifyMsg;
        }
        if (hwnd != nullptr && msg != 0) {
            PostMessageW(hwnd, msg, 0, 0);
        }
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }).detach();
}

bool ApplyQuickSettingsCacheResult(QuickSettingsCache& cache) {
    std::lock_guard<std::mutex> lock(g_network.mutex);
    if (!g_network.haveResult) {
        return false;
    }
    g_network.haveResult = false;
    const QuickSettingsCache& r = g_network.result;
    bool changed = cache.wifiRadioOn != r.wifiRadioOn || cache.haveWifiInterface != r.haveWifiInterface ||
        cache.ethernetUp != r.ethernetUp || cache.ethernetStatus != r.ethernetStatus ||
        cache.ethernetSpeed != r.ethernetSpeed || cache.ethernetIpv4 != r.ethernetIpv4 ||
        cache.ethernetAdapter != r.ethernetAdapter || cache.outputName != r.outputName ||
        cache.inputName != r.inputName || cache.inputGain != r.inputGain ||
        cache.inputMuted != r.inputMuted || cache.inputNote != r.inputNote ||
        cache.powerName != r.powerName || cache.powerMode != r.powerMode ||
        cache.usbSuspend != r.usbSuspend || cache.usbKnown != r.usbKnown ||
        cache.hdrOn != r.hdrOn || cache.hdrSupported != r.hdrSupported ||
        cache.nightLight != r.nightLight || cache.nightKnown != r.nightKnown ||
        cache.wifi.size() != r.wifi.size() || cache.vpn.size() != r.vpn.size() ||
        cache.renderDevices.size() != r.renderDevices.size() ||
        cache.captureDevices.size() != r.captureDevices.size();
    cache.wifiRadioOn = r.wifiRadioOn;
    cache.haveWifiInterface = r.haveWifiInterface;
    cache.wifiInterface = r.wifiInterface;
    cache.wifi = r.wifi;
    cache.ethernetUp = r.ethernetUp;
    cache.ethernetStatus = r.ethernetStatus;
    cache.ethernetSpeed = r.ethernetSpeed;
    cache.ethernetIpv4 = r.ethernetIpv4;
    cache.ethernetAdapter = r.ethernetAdapter;
    cache.vpn = r.vpn;
    cache.outputName = r.outputName;
    cache.inputName = r.inputName;
    cache.renderDefaultId = r.renderDefaultId;
    cache.captureDefaultId = r.captureDefaultId;
    cache.renderDevices = r.renderDevices;
    cache.captureDevices = r.captureDevices;
    cache.inputPeak = r.inputPeak;
    cache.inputGain = r.inputGain;
    cache.inputMuted = r.inputMuted;
    cache.inputNote = r.inputNote;
    cache.powerName = r.powerName;
    cache.powerMode = r.powerMode;
    cache.usbSuspend = r.usbSuspend;
    cache.usbKnown = r.usbKnown;
    cache.hdrOn = r.hdrOn;
    cache.hdrSupported = r.hdrSupported;
    cache.nightLight = r.nightLight;
    cache.nightKnown = r.nightKnown;
    cache.stampNetwork = r.stampNetwork;
    cache.stampNight = r.stampNight;
    cache.stamp = r.stamp;
    return changed;
}

void RequestEnergyAppsAsync(HWND notifyHwnd, UINT notifyMsg) {
    if (notifyHwnd == nullptr || notifyMsg == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_energy.mutex);
        g_energy.notifyHwnd = notifyHwnd;
        g_energy.notifyMsg = notifyMsg;
        if (g_energy.inFlight) {
            return;
        }
        // Paint-driven re-entry can land within a few ms of the previous sample.
        // Starting another ToolHelp then would either reset the baseline (stuck
        // Measuring) or publish an empty pending snapshot over live rows.
        if (!g_energyBaseline.samples.empty() && g_energyBaseline.tick != 0) {
            const ULONGLONG since = GetTickCount64() - g_energyBaseline.tick;
            if (since < 250ULL) {
                return;
            }
        }
        g_energy.inFlight = true;
    }
    std::thread([] {
        QuickSettingsCache local{};
        QueryEnergyApps(local);
        HWND hwnd = nullptr;
        UINT msg = 0;
        {
            std::lock_guard<std::mutex> lock(g_energy.mutex);
            g_energy.result.energyApps = std::move(local.energyApps);
            g_energy.result.energyLive = local.energyLive;
            g_energy.result.energyPending = local.energyPending;
            g_energy.result.energyNote = std::move(local.energyNote);
            g_energy.haveResult = true;
            g_energy.inFlight = false;
            hwnd = g_energy.notifyHwnd;
            msg = g_energy.notifyMsg;
        }
        if (hwnd != nullptr && msg != 0) {
            PostMessageW(hwnd, msg, 0, 0);
        }
    }).detach();
}

bool ApplyEnergyAppsResult(QuickSettingsCache& cache) {
    std::lock_guard<std::mutex> lock(g_energy.mutex);
    if (!g_energy.haveResult) {
        return false;
    }
    g_energy.haveResult = false;
    bool changed = cache.energyLive != g_energy.result.energyLive ||
        cache.energyPending != g_energy.result.energyPending ||
        cache.energyNote != g_energy.result.energyNote ||
        cache.energyApps.size() != g_energy.result.energyApps.size();
    if (!changed) {
        for (size_t i = 0; i < cache.energyApps.size(); ++i) {
            if (cache.energyApps[i].name != g_energy.result.energyApps[i].name ||
                cache.energyApps[i].cpuPercent != g_energy.result.energyApps[i].cpuPercent) {
                changed = true;
                break;
            }
        }
    }
    cache.energyApps = g_energy.result.energyApps;
    cache.energyLive = g_energy.result.energyLive;
    cache.energyPending = g_energy.result.energyPending;
    cache.energyNote = g_energy.result.energyNote;
    return changed;
}

void RequestQuickSettingsTemps(HWND notifyHwnd, UINT notifyMsg) noexcept {
    RequestSystemTemps(notifyHwnd, notifyMsg);
}

bool ApplyQuickSettingsTemps(QuickSettingsCache& cache) {
    // A reading older than this (panel reopened after a long idle) is shown as
    // unavailable until the worker's first fresh poll lands (~0.1-0.5 s).
    constexpr ULONGLONG kStaleMs = 60000;
    SystemTempsReading reading;
    int cpu = -1;
    int gpu = -1;
    int cpuW = -1;
    int gpuW = -1;
    if (LatestSystemTemps(reading) && GetTickCount64() - reading.stamp <= kStaleMs) {
        cpu = reading.cpuC;
        gpu = reading.gpuC;
        cpuW = reading.cpuW;
        gpuW = reading.gpuW;
    }
    const bool changed = cpu != cache.cpuTempC || gpu != cache.gpuTempC ||
        cpuW != cache.cpuWatts || gpuW != cache.gpuWatts;
    cache.cpuTempC = cpu;
    cache.gpuTempC = gpu;
    cache.cpuWatts = cpuW;
    cache.gpuWatts = gpuW;
    return changed;
}

QsLiveChange RefreshQuickSettingsLive(QuickSettingsCache& cache) {
    const int beforeBucket = MeterBucket(cache.inputPeak);
    const std::wstring beforeTitle = cache.mediaTitle;
    const std::wstring beforeArtist = cache.mediaArtist;
    const bool beforeHave = cache.mediaHave;
    const bool beforePlaying = cache.mediaPlaying;
    const int beforeProgress = static_cast<int>(std::lround(cache.mediaProgress * 48.0F));
    const std::wstring beforeNote = cache.inputNote;

    // Mic peak comes from the ~25 Hz background pump; UI only publishes it.
    // WASAPI Open/Initialize stays on EnsureQuickSettingsCaptureAsync (worker).
    bool pumpAlive = false;
    {
        std::lock_guard<std::mutex> lock(g_captureMeter.mutex);
        pumpAlive = g_captureMeter.client != nullptr && g_captureMeter.capture != nullptr;
    }
    if (!pumpAlive) {
        EnsureQuickSettingsCaptureAsync();
    }
    g_captureMeter.ReadPublished(cache);
    if (!pumpAlive && cache.inputNote.empty()) {
        cache.inputPeak = 0.0F;
    }

    // GSMTC: session ~1 Hz, scrub/progress ~5 Hz.
    QueryMedia(cache, false, false);

    const bool labels = cache.mediaTitle != beforeTitle || cache.mediaArtist != beforeArtist ||
        cache.mediaHave != beforeHave || cache.mediaPlaying != beforePlaying ||
        cache.inputNote != beforeNote;
    const bool meters = MeterBucket(cache.inputPeak) != beforeBucket ||
        static_cast<int>(std::lround(cache.mediaProgress * 48.0F)) != beforeProgress;
    if (labels) {
        return QsLiveChange::Labels;
    }
    if (meters) {
        return QsLiveChange::MetersOnly;
    }
    return QsLiveChange::None;
}

void DockApp::RefreshQuickSettingsCache() {
    const ULONGLONG now = GetTickCount64();
    // WLAN / adapters / endpoints / WASAPI / power / HDR / Night Light: worker.
    // First paint keeps whatever is already cached (possibly stale).
    const bool networkDue =
        m_qsCache.stampNetwork == 0 || now - m_qsCache.stampNetwork >= 2500ULL;
    const bool nightDue = m_qsCache.stampNight == 0 || now - m_qsCache.stampNight >= 8000ULL;
    if ((networkDue || nightDue) && m_overflowWindow != nullptr) {
        RequestQuickSettingsCacheAsync(m_overflowWindow, kQsCacheResultMessage, m_qsPage);
    }
    if (m_qsPage == QuickSettingsPage::Power) {
        if (!m_qsCache.energyLive && !m_qsCache.energyPending) {
            m_qsCache.energyPending = true;
            m_qsCache.energyNote = L"Measuring energy use\u2026";
            // First entry only: the 500 ms timer + ArmOverflowLiveWorkers own the
            // steady sample cadence. Re-requesting from every paint was racing the
            // 250 ms baseline window and left the list empty on Measuring.
            if (m_overflowWindow != nullptr) {
                RequestEnergyAppsAsync(m_overflowWindow, kQsEnergyResultMessage);
            }
        }
    } else {
        m_qsCache.energyApps.clear();
        m_qsCache.energyLive = false;
        m_qsCache.energyPending = false;
        m_qsCache.energyNote.clear();
    }
    if (m_qsPage == QuickSettingsPage::Home) {
        // Sensors are read on the temperature worker; this only copies the
        // last published values and re-arms its ~500 ms poll.
        ApplyQuickSettingsTemps(m_qsCache);
        if (m_overflowWindow != nullptr) {
            RequestQuickSettingsTemps(m_overflowWindow, kQsTempsResultMessage);
        }
    }
    // Publish last GSMTC snapshot only - session/scrub polls stay on the live
    // timer so opening never blocks on WinRT media IPC.
    PublishMedia(m_qsCache);
    EnsureQuickSettingsCaptureAsync();
    m_qsCache.stamp = now;
}

void DockApp::OpenQuickSettingsPage(QuickSettingsPage page) {
    if (page == m_qsPage) {
        return;
    }
    const QuickSettingsPage previous = m_qsPage;
    m_qsReturn = m_qsPage;
    m_qsPage = page;
    m_overflowHover = -1;
    m_overflowHoverDirtyValid = false;
    m_qsDragging = false;
    // Expire TTLs so the worker refreshes page-specific data (wifi list, etc.)
    // after this paint - never run WLAN/COM/UIA on the UI thread here.
    m_qsCache.stampNetwork = 0;
    m_qsCache.stampNight = 0;
    if (page == QuickSettingsPage::SystemTray && !m_overflowIconsLoaded) {
        m_overflowIcons = m_tray.EnumerateNotifyIcons();
        m_overflowIconsLoaded = true;
    }
    if (page == QuickSettingsPage::Bluetooth) {
        m_lastBluetoothUiAt = QpcSeconds();
        m_lastBluetoothRefreshAt = m_lastBluetoothUiAt;
        m_bluetooth.RequestRefresh(true);
        if (m_window != nullptr) {
            SetTimer(m_window, kBluetoothTimerId, 4000, nullptr);
        }
    } else if (previous == QuickSettingsPage::Bluetooth && m_window != nullptr &&
        !m_bluetoothSnapshot.discovering && !m_bluetooth.IsPairing()) {
        KillTimer(m_window, kBluetoothTimerId);
    }
    // Page content changed; overflow size/origin/frost usually unchanged, so
    // keep the warm frosted glass buffer. Sync PaintOverflowPopup on this
    // thread (LL mouse hook) freezes the cursor - coalesce via the queue.
    QueueOverflowPaint();
}

void DockApp::CloseQuickSettingsPage() {
    QuickSettingsPage back = m_qsReturn;
    if (back == m_qsPage) {
        back = QuickSettingsPage::Home;
    }
    const QuickSettingsPage leaving = m_qsPage;
    m_qsReturn = QuickSettingsPage::Home;
    m_qsPage = back;
    m_overflowHover = -1;
    m_overflowHoverDirtyValid = false;
    m_qsDragging = false;
    if (leaving == QuickSettingsPage::Bluetooth && back != QuickSettingsPage::Bluetooth &&
        m_window != nullptr && !m_bluetoothSnapshot.discovering && !m_bluetooth.IsPairing()) {
        KillTimer(m_window, kBluetoothTimerId);
    } else if (back == QuickSettingsPage::Bluetooth) {
        m_lastBluetoothUiAt = QpcSeconds();
        m_bluetooth.RequestRefresh(true);
        if (m_window != nullptr) {
            SetTimer(m_window, kBluetoothTimerId, 4000, nullptr);
        }
    }
    // Same as OpenQuickSettingsPage: reuse glass; do not sync-paint on the
    // hook thread.
    QueueOverflowPaint();
}

void DockApp::ProjectBrightness(int percent) {
    percent = std::clamp(percent, 0, 100);
    m_brightnessTarget.store(percent);
    if (!m_brightnessAdjustInFlight.exchange(true)) {
        std::thread([this] { DrainBrightnessWheel(); }).detach();
    }
}

void DockApp::OpenSettingsPage(const wchar_t* uri) {
    CloseOverflowPopup();
    if (m_window != nullptr) {
        SetForegroundWindow(m_window);
    }
    ShellExecuteW(m_window, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
}

void DockApp::ApplyQuickSettingsSlider(TrayFlyoutHitKind kind, const RECT& track, LONG x) {
    const float level = TrackLevel(track, x);
    if (kind == TrayFlyoutHitKind::VolumeSlider) {
        if (m_tray.SetVolumeLevel(level)) {
            EnsureTrayIcons();
            QueueOverflowPaint();
            QueueRenderFrame();
        }
        return;
    }
    if (kind == TrayFlyoutHitKind::BrightnessSlider) {
        if (m_brightnessTarget.load() < 0 && !m_tray.Status().brightnessAvailable) {
            OpenSettingsPage(L"ms-settings:display");
            return;
        }
        ProjectBrightness(static_cast<int>(std::lround(level * 100.0F)));
        QueueOverflowPaint();
        return;
    }
    if (kind == TrayFlyoutHitKind::CaptureGain) {
        if (SetCaptureGain(level)) {
            m_qsCache.inputGain = level;
            m_qsCache.inputMuted = false;
            m_qsCache.stamp = GetTickCount64();
            QueueOverflowPaint();
        }
        return;
    }
    if (kind == TrayFlyoutHitKind::MediaSeek) {
        SeekQuickSettingsMedia(level);
        m_qsCache.mediaProgress = level;
        QueueOverflowPaint();
    }
}

void DockApp::MeasureQuickSettings(float scale, LONG padding, LONG gearSize, LONG headerHeight,
    LONG& panelWidth, LONG& contentHeight) {
    LayoutQuickSettings(false, nullptr, 0, 0, nullptr, scale, padding, gearSize, headerHeight,
        nullptr, nullptr, nullptr, nullptr, panelWidth, contentHeight);
}

void DockApp::PaintQuickSettings(uint8_t* pixels, int width, int height, HDC memory, float scale,
    LONG padding, LONG panelWidth, LONG gearSize, LONG headerHeight, HFONT titleFont,
    HFONT sectionFont, HFONT labelFont, HFONT statusFont) {
    // The window is deliberately larger than the content plate. Keeping this
    // transparent margin outside the squircle lets the layered window carry a
    // soft shadow/rim while the wallpaper remains visible in every corner.
    const LONG shadowMargin = std::max(1L, std::lround(DOCK_SHADOW_MARGIN_PT * scale));
    const int contentWidth = width - static_cast<int>(shadowMargin * 2L);
    const int contentHeight = height - static_cast<int>(shadowMargin * 2L);
    if (pixels == nullptr || contentWidth <= 0 || contentHeight <= 0) {
        return;
    }
    const size_t bytes = static_cast<size_t>(contentWidth) * static_cast<size_t>(contentHeight) * 4U;
    std::vector<uint8_t> content(bytes, 0);
    LONG widthOut = panelWidth;
    LONG heightOut = 0;
    LayoutQuickSettings(true, content.data(), contentWidth, contentHeight, memory, scale, padding,
        gearSize, headerHeight, titleFont, sectionFont, labelFont, statusFont, widthOut, heightOut);
    CompositePremul(pixels, width, height, static_cast<int>(shadowMargin), static_cast<int>(shadowMargin),
        content.data(), contentWidth, contentHeight);
    for (TrayFlyoutHit& hit : m_overflowHits) {
        OffsetRect(&hit.bounds, shadowMargin, shadowMargin);
    }
    if (m_qsMeterValid) {
        OffsetRect(&m_qsMeterRect, shadowMargin, shadowMargin);
    }
    if (m_qsScrubValid) {
        OffsetRect(&m_qsScrubRect, shadowMargin, shadowMargin);
    }
    if (m_qsTempsValid) {
        OffsetRect(&m_qsTempCpuRect, shadowMargin, shadowMargin);
        OffsetRect(&m_qsTempGpuRect, shadowMargin, shadowMargin);
    }
    if (m_qsBoostStatusValid) {
        OffsetRect(&m_qsBoostStatusRect, shadowMargin, shadowMargin);
    }
    m_overflowGearX += static_cast<int>(shadowMargin);
    m_overflowGearY += static_cast<int>(shadowMargin);
}

void DockApp::PaintQsLiveOverlays(uint8_t* pixels, int width, int height, float scale,
    bool includeTemps) const {
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }
    const bool light = m_config.LightPanels();
    const uint8_t inkR = light ? kInkDarkR : kInkLightR;
    const uint8_t inkG = light ? kInkDarkG : kInkLightG;
    const uint8_t inkB = light ? kInkDarkB : kInkLightB;
    if (m_qsMeterValid) {
        const RECT& meter = m_qsMeterRect;
        if (m_qsPage == QuickSettingsPage::Microphone) {
            const int segments = 16;
            const int lit = m_qsCache.inputMuted
                ? 0
                : static_cast<int>(std::lround(std::pow(std::clamp(m_qsCache.inputPeak, 0.0F, 1.0F), 0.45F) *
                      static_cast<float>(segments)));
            const LONG segGap = 3;
            const LONG segW =
                std::max(3L, (meter.right - meter.left - 24 - segGap * (segments - 1)) / segments);
            for (int index = 0; index < segments; ++index) {
                const LONG left = meter.left + 12 + index * (segW + segGap);
                const RECT seg{left, meter.top + 8, left + segW, meter.bottom - 8};
                FillSquircleColorPremul(pixels, width, height, seg, 2.0F, index < lit ? 0.95F : 0.25F,
                    index < lit ? kBlueB : inkB, index < lit ? kBlueG : inkG, index < lit ? kBlueR : inkR);
            }
        } else {
            constexpr int segments = 12;
            const int lit = m_qsCache.inputMuted
                ? 0
                : static_cast<int>(std::lround(std::pow(std::clamp(m_qsCache.inputPeak, 0.0F, 1.0F), 0.45F) *
                      static_cast<float>(segments)));
            const LONG barH = std::max(1L, meter.bottom - meter.top);
            const LONG segGap = std::max(2L, std::lround(3.0F * scale));
            const LONG segW = std::max(4L, (meter.right - meter.left - segGap * (segments - 1)) / segments);
            const float segRadius = static_cast<float>(barH) * 0.5F;
            for (int index = 0; index < segments; ++index) {
                const LONG left = meter.left + index * (segW + segGap);
                const float cxL = static_cast<float>(left) + segRadius;
                const float cxR = static_cast<float>(left + segW) - segRadius;
                const float cy = 0.5F * static_cast<float>(meter.top + meter.bottom);
                if (index < lit) {
                    FillHGradientPill(pixels, width, height, cxL, std::max(cxL + 1.0F, cxR), cy, segRadius,
                        1.0F, kBlueB, kBlueG, kBlueR, kBlueB, kBlueG, kBlueR);
                } else {
                    FillHGradientPill(pixels, width, height, cxL, std::max(cxL + 1.0F, cxR), cy, segRadius,
                        1.0F, 198, 198, 202, 188, 188, 192);
                }
            }
        }
    }
    if (m_qsScrubValid && m_qsPage == QuickSettingsPage::Home) {
        const RECT& track = m_qsScrubRect;
        const float level = m_qsCache.mediaHave ? std::clamp(m_qsCache.mediaProgress, 0.0F, 1.0F) : 0.0F;
        const float radius = std::max(4.0F, static_cast<float>(track.bottom - track.top) * 0.5F);
        const float cy = 0.5F * static_cast<float>(track.top + track.bottom);
        const float left = static_cast<float>(track.left) + radius;
        const float right = std::max(left + 1.0F, static_cast<float>(track.right) - radius);
        const float fill = left + (right - left) * level;
        const float shadeY = cy + std::max(1.5F, 2.0F * scale);
        FillHGradientPill(pixels, width, height, left, right, shadeY, radius, light ? 0.16F : 0.22F,
            180, 170, 190, 170, 160, 184);
        FillHGradientPill(pixels, width, height, left, right, cy, radius, light ? 0.40F : 0.34F,
            228, 220, 236, 210, 204, 226);
        if (level > 0.01F) {
            FillHGradientPill(pixels, width, height, left, std::max(left + radius, fill), cy, radius,
                1.0F, 250, 140, 64, 245, 90, 176);
        }
        const float thumb = radius + std::max(3.5F, 5.0F * scale);
        FillSoftDisc(pixels, width, height, fill, cy, thumb + std::max(4.0F, 5.5F * scale), 0.55F,
            255, 150, 210);
        FillSoftDisc(pixels, width, height, fill, cy, thumb, 1.0F, 245, 120, 210);
        FillSoftDisc(pixels, width, height, fill, cy, thumb * 0.62F, 1.0F, 255, 236, 252);
    }
    if (includeTemps && m_qsTempsValid && m_qsPage == QuickSettingsPage::Home &&
        m_overflowLabelFont != nullptr) {
        // Values live outside the underlay so a new reading presents only
        // these two rects (PaintOverflowLiveFast) instead of a full rebuild.
        const uint8_t savedR = g_flyoutInkR;
        const uint8_t savedG = g_flyoutInkG;
        const uint8_t savedB = g_flyoutInkB;
        SetFlyoutChromeInk(m_qsTempsInkR, m_qsTempsInkG, m_qsTempsInkB);
        HFONT valueFont =
            m_overflowStatusFont != nullptr ? m_overflowStatusFont : m_overflowLabelFont;
        constexpr UINT format = DT_LEFT | DT_TOP | DT_WORDBREAK;
        DrawFlyoutText(pixels, width, height, m_qsTempCpuRect, valueFont,
            TempPowerText(m_qsCache.cpuTempC, m_qsCache.cpuWatts), format, 255);
        DrawFlyoutText(pixels, width, height, m_qsTempGpuRect, valueFont,
            TempPowerText(m_qsCache.gpuTempC, m_qsCache.gpuWatts), format, 255);
        SetFlyoutChromeInk(savedR, savedG, savedB);
    }
    if (m_qsBoostStatusValid && m_qsPage == QuickSettingsPage::Home &&
        !m_boostStatus.empty()) {
        HFONT statusFont =
            m_overflowStatusFont != nullptr ? m_overflowStatusFont : m_overflowLabelFont;
        if (statusFont != nullptr) {
            const uint8_t savedR = g_flyoutInkR;
            const uint8_t savedG = g_flyoutInkG;
            const uint8_t savedB = g_flyoutInkB;
            SetFlyoutChromeInk(m_qsBoostStatusInkR, m_qsBoostStatusInkG, m_qsBoostStatusInkB);
            DrawFlyoutText(pixels, width, height, m_qsBoostStatusRect, statusFont,
                m_boostStatus, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 196);
            SetFlyoutChromeInk(savedR, savedG, savedB);
        }
    }
}

void DockApp::LayoutQuickSettings(bool draw, uint8_t* pixels, int width, int height, HDC memory,
    float scale, LONG padding, LONG gearSize, LONG headerHeight, HFONT titleFont, HFONT sectionFont,
    HFONT labelFont, HFONT statusFont, LONG& panelWidth, LONG& contentBottom) {
    if (draw) {
        m_qsMeterValid = false;
        m_qsScrubValid = false;
        m_qsTempsValid = false;
        m_qsBoostStatusValid = false;
        m_qsMeterRect = {};
        m_qsScrubRect = {};
        m_qsTempCpuRect = {};
        m_qsTempGpuRect = {};
        m_qsBoostStatusRect = {};
    }
    const bool home = m_qsPage == QuickSettingsPage::Home;
    panelWidth = std::max(320L, std::lround((home ? 600.0F : 380.0F) * scale));
    const LONG gap = std::max(8L, std::lround(10.0F * scale));
    const LONG sliderH = std::max(22L, std::lround(28.0F * scale));
    const LONG listRow = std::max(52L, std::lround(58.0F * scale));
    const LONG sectionH = std::max(22L, std::lround(26.0F * scale));
    const bool light = m_config.LightPanels();
    const uint8_t inkR = light ? kInkDarkR : kInkLightR;
    const uint8_t inkG = light ? kInkDarkG : kInkLightG;
    const uint8_t inkB = light ? kInkDarkB : kInkLightB;
    LONG y = padding;

    auto push = [&](TrayFlyoutHitKind kind, RECT bounds, int index = -1) {
        if (!draw) {
            return;
        }
        TrayFlyoutHit hit;
        hit.kind = kind;
        hit.index = index;
        hit.bounds = bounds;
        m_overflowHits.push_back(hit);
    };
    auto restoreInk = [&]() {
        SetFlyoutChromeInk(inkR, inkG, inkB);
    };
    // Bind flyout ink before any DrawFlyoutText: LightPanels white tiles need
    // dark ink, dark tiles need chrome ink. Without this, leftover chrome ink
    // from dock glyphs paints white-on-white QS labels when LightPanels is on.
    if (draw) {
        restoreInk();
    }
    auto text = [&](RECT bounds, HFONT font, const std::wstring& value, UINT format, uint8_t alpha) {
        if (!draw || value.empty() || font == nullptr) {
            return;
        }
        DrawFlyoutText(pixels, width, height, bounds, font, value, format, alpha);
    };
    auto icon = [&](LONG left, LONG top, wchar_t symbol, UINT extent, uint8_t red, uint8_t green,
                    uint8_t blue) {
        if (!draw || extent == 0) {
            return;
        }
        const std::vector<uint8_t>& glyph = CachedGlyph(m_tray, symbol, extent, red, green, blue);
        if (!glyph.empty()) {
            CompositePremul(pixels, width, height, static_cast<int>(left), static_cast<int>(top),
                glyph.data(), static_cast<int>(extent), static_cast<int>(extent));
        }
    };
    auto card = [&](RECT bounds, bool active) {
        if (!draw) {
            return;
        }
        const float radius = ContentSquircleRadius(bounds, scale);
        if (active) {
            FillSquircleColorPremul(pixels, width, height, bounds, radius, 0.96F, kBlueB, kBlueG,
                kBlueR);
        } else if (light) {
            FillSquircleColorPremul(pixels, width, height, bounds, radius, 0.72F, 255, 255, 255);
        } else {
            FillSquircleColorPremul(pixels, width, height, bounds, radius, 0.42F, 32, 34, 40);
        }
    };
    auto slider = [&](RECT track, float level) {
        if (!draw) {
            return;
        }
        level = std::clamp(level, 0.0F, 1.0F);
        const float radius = std::max(3.0F, static_cast<float>(track.bottom - track.top) * 0.5F);
        const float cy = 0.5F * static_cast<float>(track.top + track.bottom);
        const float left = static_cast<float>(track.left) + radius;
        const float right = std::max(left + 1.0F, static_cast<float>(track.right) - radius);
        FillPillColorPremul(pixels, width, height, left, right, cy, radius, light ? 0.28F : 0.35F,
            light ? 176 : 70, light ? 182 : 74, light ? 190 : 82);
        const float fill = left + (right - left) * level;
        if (level > 0.015F) {
            FillPillColorPremul(pixels, width, height, left, std::max(left, fill), cy, radius, 0.98F,
                kBlueB, kBlueG, kBlueR);
        }
        FillCirclePremul(pixels, width, height, fill, cy, radius + std::max(2.0F, 3.0F * scale),
            0.98F, false);
    };
    auto radio = [&](float cx, float cy, bool selected) {
        if (!draw) {
            return;
        }
        const float radius = std::max(7.0F, 8.0F * scale);
        FillPillColorPremul(pixels, width, height, cx, cx, cy, radius, 0.50F, inkB, inkG, inkR);
        FillPillColorPremul(pixels, width, height, cx, cx, cy, radius - 1.8F, 0.96F,
            light ? 255 : 32, light ? 255 : 34, light ? 255 : 40);
        if (selected) {
            FillPillColorPremul(pixels, width, height, cx, cx, cy, radius * 0.46F, 1.0F, kBlueB,
                kBlueG, kBlueR);
        }
    };
    auto toggleRect = [&](LONG centerY, LONG right, bool enabled) {
        const LONG switchW = std::max(40L, std::lround(44.0F * scale));
        const LONG switchH = std::max(22L, std::lround(24.0F * scale));
        const LONG left = right - switchW;
        const LONG top = centerY - switchH / 2L;
        if (draw) {
            const float radius = static_cast<float>(switchH) * 0.5F;
            const float cxL = static_cast<float>(left) + radius;
            const float cxR = static_cast<float>(right) - radius;
            const float cy = static_cast<float>(top) + radius;
            if (enabled) {
                FillPillColorPremul(pixels, width, height, cxL, cxR, cy, radius, 0.96F, kBlueB,
                    kBlueG, kBlueR);
            } else {
                FillPillColorPremul(pixels, width, height, cxL, cxR, cy, radius, 0.45F, inkB, inkG,
                    inkR);
            }
            FillCirclePremul(pixels, width, height, enabled ? cxR : cxL, cy,
                radius - std::max(2.0F, 2.0F * scale), 0.98F, false);
        }
        return RECT{left - 8, top - 6, right + 6, top + switchH + 6};
    };
    auto beginHeader = [&](const wchar_t* title, bool gear, int toggleIndex, bool toggleOn) {
        if (m_qsPage != QuickSettingsPage::Home) {
            const RECT back{padding, y, padding + headerHeight, y + headerHeight};
            text(back, titleFont, L"\u2190", DT_CENTER | DT_VCENTER | DT_SINGLELINE, 255);
            push(TrayFlyoutHitKind::Back, back);
        }
        LONG rightLimit = panelWidth - padding;
        if (gear) {
            const UINT gearExtent = static_cast<UINT>(std::max(1L, gearSize));
            const RECT gearRect{panelWidth - padding - gearSize, y + (headerHeight - gearSize) / 2L,
                panelWidth - padding, y + (headerHeight + gearSize) / 2L};
            if (draw) {
                EnsureOverflowGlyphs(gearExtent, gearExtent);
                m_overflowGearX = static_cast<int>(gearRect.left);
                m_overflowGearY = static_cast<int>(gearRect.top);
                m_overflowGearExtent = gearExtent;
                if (!m_overflowGlyphGear.empty()) {
                    CompositePremul(pixels, width, height, m_overflowGearX, m_overflowGearY,
                        m_overflowGlyphGear.data(), static_cast<int>(gearExtent),
                        static_cast<int>(gearExtent));
                }
            }
            push(TrayFlyoutHitKind::Settings, {gearRect.left - 8, y, panelWidth - padding + 4, y + headerHeight});
            rightLimit = gearRect.left - 8;
        } else if (draw) {
            m_overflowGearExtent = 0;
        }
        if (toggleIndex >= 0) {
            const RECT toggle = toggleRect(y + headerHeight / 2L, panelWidth - padding, toggleOn);
            push(TrayFlyoutHitKind::Toggle, toggle, toggleIndex);
            rightLimit = toggle.left - 4;
        }
        const LONG titleLeft = m_qsPage == QuickSettingsPage::Home ? padding : padding + headerHeight;
        text({titleLeft, y, rightLimit, y + headerHeight}, titleFont, title,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
        y += headerHeight;
    };
    auto section = [&](const wchar_t* label) {
        text({padding, y, panelWidth - padding, y + sectionH}, sectionFont, label,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE, 170);
        y += sectionH;
    };
    auto footer = [&](const wchar_t* label, int link) {
        const RECT row{padding, y, panelWidth - padding, y + listRow};
        if (draw) {
            SetFlyoutChromeInk(kBlueR, kBlueG, kBlueB);
            text(row, labelFont, std::wstring(label) + L"    \u203A",
                DT_LEFT | DT_VCENTER | DT_SINGLELINE, 255);
            restoreInk();
        }
        push(TrayFlyoutHitKind::MoreSettings, row, link);
        y += listRow;
    };

    const TrayStatus& tray = m_tray.Status();
    const int projected = m_brightnessTarget.load();
    const float volume = tray.volumeMuted ? 0.0F : tray.volumeLevel;
    const float brightness = projected >= 0
        ? static_cast<float>(projected) / 100.0F
        : (tray.brightnessAvailable ? static_cast<float>(tray.brightnessPercent) / 100.0F : 0.0F);
    const std::wstring outputName = m_qsCache.outputName.empty() ? L"Speakers" : m_qsCache.outputName;
    const std::wstring inputName = m_qsCache.inputName.empty() ? L"Microphone" : m_qsCache.inputName;

    if (home) {
        beginHeader(L"Quick Settings", true, -1, false);
        const LONG homeGap = std::max(8L, std::lround(12.0F * scale));
        y += homeGap;
        const LONG inner = panelWidth - padding * 2L;
        const LONG tileGap = homeGap;
        const LONG homeTileH = std::max(96L, std::lround(110.0F * scale));
        const LONG homeCardH = std::max(112L, std::lround(124.0F * scale));
        const LONG homeSmallH = std::max(74L, std::lround(86.0F * scale));
        const LONG homeMediaH = std::max(78L, std::lround(90.0F * scale));
        const LONG tileCount = 5L;
        const LONG tileGaps = tileGap * (tileCount - 1L);
        const LONG tileBase = (inner - tileGaps) / tileCount;
        const LONG tileExtra = (inner - tileGaps) - tileBase * tileCount;
        LONG tileWidths[5] = {};
        for (LONG tileIndex = 0; tileIndex < tileCount; ++tileIndex) {
            tileWidths[tileIndex] = tileBase + (tileIndex < tileExtra ? 1L : 0L);
        }
        const UINT tileIcon = static_cast<UINT>(std::max(16L, std::lround(18.0F * scale)));
        const LONG inset = std::max(10L, std::lround(14.0F * scale));
        auto homeCard = [&](RECT bounds) {
            if (!draw) {
                return;
            }
            // Mock tiles (1072px export of a ~600px panel) use ~8pt corners,
            // a hairline cool rim, a 2px top inset, and a ~8pt soft drop.
            const float shortest = static_cast<float>(std::min(bounds.right - bounds.left,
                bounds.bottom - bounds.top));
            const float radius = std::min(shortest * 0.22F, std::max(7.0F, 8.0F * scale));
            const int blur = std::max(4, static_cast<int>(std::lround(7.0F * scale)));
            for (int layer = blur; layer >= 1; --layer) {
                const float t = static_cast<float>(layer) / static_cast<float>(blur);
                RECT shadow = bounds;
                const LONG expand = std::max(0L, std::lround(2.4F * scale * t));
                InflateRect(&shadow, expand, 0);
                const LONG drop = std::max(1L, std::lround((0.6F + 6.5F * t) * scale));
                shadow.top += std::max(1L, drop / 5L);
                shadow.bottom += drop;
                const float falloff = (1.0F - t) * (1.0F - t);
                const float alpha = (light ? 0.085F : 0.18F) * falloff;
                if (alpha < 0.012F) {
                    continue;
                }
                FillSquircleColorPremul(pixels, width, height, shadow,
                    radius + static_cast<float>(expand), alpha, light ? 132 : 0, light ? 136 : 0,
                    light ? 146 : 0);
            }
            FillSquircleColorPremul(pixels, width, height, bounds, radius, light ? 0.92F : 0.72F,
                light ? 226 : 40, light ? 228 : 42, light ? 234 : 48);
            RECT face = bounds;
            const LONG border = std::max(1L, std::lround(1.0F * scale));
            InflateRect(&face, -border, -border);
            face.top += border;
            if (face.right > face.left + 4 && face.bottom > face.top + 4) {
                FillSquircleColorPremul(pixels, width, height, face,
                    std::max(6.0F, radius - static_cast<float>(border)), light ? 0.985F : 0.94F,
                    light ? 250 : 30, light ? 250 : 32, light ? 252 : 36);
            }
        };
        auto tile = [&](RECT bounds, wchar_t symbol, const wchar_t* title, const std::wstring& subtitle,
                        TrayFlyoutHitKind kind, bool chevron = true) {
            homeCard(bounds);
            icon(bounds.left + inset, bounds.top + inset, symbol, tileIcon, inkR, inkG, inkB);
            const LONG titleTop = bounds.top + inset + static_cast<LONG>(tileIcon) +
                std::max(2L, std::lround(3.0F * scale));
            const LONG statusBand = std::max(18L, std::lround(22.0F * scale));
            text({bounds.left + inset, titleTop, bounds.right - std::max(8L, inset - 2),
                     bounds.bottom - statusBand},
                labelFont, title, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS, 255);
            const LONG chevW = chevron ? std::max(12L, std::lround(14.0F * scale)) : 0L;
            const LONG statusBottom = bounds.bottom - std::max(8L, std::lround(10.0F * scale));
            const LONG statusTop = statusBottom - std::max(16L, std::lround(18.0F * scale));
            const RECT statusBounds{bounds.left + inset, statusTop,
                bounds.right - inset - chevW, statusBottom};
            // Boost status is a live overlay (PaintQsLiveOverlays) so worker
            // status posts never force a full PaintOverflowPopup.
            if (kind == TrayFlyoutHitKind::Boost) {
                m_qsBoostStatusRect = statusBounds;
                m_qsBoostStatusValid = true;
                m_qsBoostStatusInkR = inkR;
                m_qsBoostStatusInkG = inkG;
                m_qsBoostStatusInkB = inkB;
            } else {
                text(statusBounds, statusFont, subtitle,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 196);
            }
            if (chevron) {
                text({bounds.right - inset - chevW, statusTop, bounds.right - std::max(6L, std::lround(8.0F * scale)),
                         statusBottom},
                    statusFont, L"\u203A", DT_RIGHT | DT_VCENTER | DT_SINGLELINE, 168);
            }
            push(kind, bounds);
        };
        auto gradientSlider = [&](RECT track, float level, bool media) {
            if (!draw) {
                return;
            }
            level = std::clamp(level, 0.0F, 1.0F);
            const float radius = std::max(4.0F, static_cast<float>(track.bottom - track.top) * 0.5F);
            const float cy = 0.5F * static_cast<float>(track.top + track.bottom);
            const float left = static_cast<float>(track.left) + radius;
            const float right = std::max(left + 1.0F, static_cast<float>(track.right) - radius);
            const float fill = left + (right - left) * level;
            const float shadeY = cy + std::max(1.5F, 2.0F * scale);
            if (media) {
                FillHGradientPill(pixels, width, height, left, right, shadeY, radius, light ? 0.16F : 0.22F,
                    180, 170, 190, 170, 160, 184);
                FillHGradientPill(pixels, width, height, left, right, cy, radius, light ? 0.40F : 0.34F,
                    228, 220, 236, 210, 204, 226);
            } else {
                FillHGradientPill(pixels, width, height, left, right, shadeY, radius, light ? 0.14F : 0.22F,
                    176, 180, 190, 168, 172, 182);
                FillHGradientPill(pixels, width, height, left, right, cy, radius, 1.0F, 236, 236, 238,
                    226, 226, 230);
            }
            if (level > 0.01F) {
                if (media) {
                    FillHGradientPill(pixels, width, height, left, std::max(left + radius, fill), cy, radius,
                        1.0F, 250, 140, 64, 245, 90, 176);
                } else {
                    FillHGradientPill(pixels, width, height, left, std::max(left + radius, fill), cy, radius,
                        1.0F, 255, 168, 72, 255, 118, 36);
                }
            }
            // Mock volume thumb is a hair larger than the track, with a soft
            // contact shade. The media thumb is a larger saturated orb.
            const float thumb = radius + (media ? std::max(3.5F, 5.0F * scale)
                                                : std::max(2.0F, 2.5F * scale));
            if (media) {
                FillSoftDisc(pixels, width, height, fill, cy, thumb + std::max(4.0F, 5.5F * scale), 0.55F,
                    255, 150, 210);
                FillSoftDisc(pixels, width, height, fill, cy, thumb, 1.0F, 245, 120, 210);
                FillSoftDisc(pixels, width, height, fill, cy, thumb * 0.62F, 1.0F, 255, 236, 252);
            } else {
                FillSoftDisc(pixels, width, height, fill, cy + std::max(1.0F, 1.6F * scale),
                    thumb + std::max(1.5F, 2.0F * scale), 0.22F, 186, 196, 210);
                FillCirclePremul(pixels, width, height, fill, cy, thumb, 0.99F, false);
                FillSoftDisc(pixels, width, height, fill - thumb * 0.18F, cy, thumb * 0.72F, 0.35F,
                    255, 255, 255);
            }
        };

        LONG x = padding;
        const bool wifiOn = tray.network == TrayNetworkKind::Wifi;
        std::wstring wifiLabel = L"Off";
        if (wifiOn) {
            wifiLabel = tray.networkName.empty() ? L"Connected" : tray.networkName;
        } else if (m_qsCache.wifiRadioOn) {
            // "Not connected" ellipsizes to "Not…" in this tile. The mock is
            // the word "Not" with clear space before the chevron.
            wifiLabel = L"Not";
        }
        tile({x, y, x + tileWidths[0], y + homeTileH}, L'\uE701', L"Wi-Fi", wifiLabel,
            TrayFlyoutHitKind::Wifi);
        x += tileWidths[0] + tileGap;
        tile({x, y, x + tileWidths[1], y + homeTileH}, L'\uE839', L"Ethernet",
            m_qsCache.ethernetUp ? L"Connected" : L"Off", TrayFlyoutHitKind::Ethernet);
        x += tileWidths[1] + tileGap;
        tile({x, y, x + tileWidths[2], y + homeTileH}, L'\uE945', L"Boost", m_boostStatus,
            TrayFlyoutHitKind::Boost, false);
        x += tileWidths[2] + tileGap;
        tile({x, y, x + tileWidths[3], y + homeTileH}, L'\uE7F4', L"System Tray",
            (m_overflowIconsLoaded && m_overflowIcons.empty()) ? L"No icons" : L"Hidden",
            TrayFlyoutHitKind::SystemTrayPage);
        x += tileWidths[3] + tileGap;
        std::wstring vpnLabel = L"Off";
        for (const QsVpnEntry& entry : m_qsCache.vpn) {
            if (entry.connected) {
                vpnLabel = entry.name.empty() ? L"Connected" : entry.name;
                break;
            }
        }
        tile({x, y, x + tileWidths[4], y + homeTileH}, L'\uE72E', L"VPN", vpnLabel, TrayFlyoutHitKind::Vpn);
        y += homeTileH + homeGap;

        // Mock measures about 58/42, not a literal 2:1. Sound is the wide card.
        const LONG soundW = (inner - homeGap) * 58L / 100L;
        const RECT sound{padding, y, padding + soundW, y + homeCardH};
        const RECT mic{sound.right + homeGap, y, panelWidth - padding, y + homeCardH};
        const LONG percentW = std::max(36L, std::lround(44.0F * scale));
        const LONG sliderTrackH = std::max(20L, std::lround(28.0F * scale));
        const RECT volumeTrack{sound.left + inset, sound.bottom - inset - sliderTrackH,
            sound.right - inset - percentW, sound.bottom - inset};
        push(TrayFlyoutHitKind::VolumeSlider, volumeTrack);
        homeCard(sound);
        icon(sound.left + inset, sound.top + inset, tray.volumeMuted ? L'\uE74F' : L'\uE767', tileIcon,
            inkR, inkG, inkB);
        const LONG titleLeft = sound.left + inset + static_cast<LONG>(tileIcon) + std::max(6L, std::lround(8.0F * scale));
        text({titleLeft, sound.top + inset - 2, sound.right - 28, sound.top + inset + static_cast<LONG>(tileIcon)},
            labelFont, L"Sound", DT_LEFT | DT_VCENTER | DT_SINGLELINE, 255);
        text({titleLeft, sound.top + inset + static_cast<LONG>(tileIcon) - 2, sound.right - inset,
                 volumeTrack.top - 4},
            statusFont, outputName, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 180);
        text({sound.right - 26, sound.top + inset - 2, sound.right - 8, sound.top + inset + static_cast<LONG>(tileIcon)},
            statusFont, L"\u203A", DT_CENTER | DT_VCENTER | DT_SINGLELINE, 168);
        gradientSlider(volumeTrack, volume, false);
        text({volumeTrack.right + 4, volumeTrack.top, sound.right - 8, volumeTrack.bottom}, statusFont,
            std::to_wstring(static_cast<int>(std::lround(volume * 100.0F))) + L"%",
            DT_LEFT | DT_VCENTER | DT_SINGLELINE, 170);
        push(TrayFlyoutHitKind::Sound, sound);

        homeCard(mic);
        icon(mic.left + inset, mic.top + inset, L'\uE720', tileIcon, inkR, inkG, inkB);
        const LONG micTitleLeft = mic.left + inset + static_cast<LONG>(tileIcon) + std::max(6L, std::lround(8.0F * scale));
        text({micTitleLeft, mic.top + inset - 2, mic.right - 28, mic.top + inset + static_cast<LONG>(tileIcon)},
            labelFont, L"Microphone", DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
        const std::wstring micStatus = m_qsCache.inputNote.empty() ? inputName : m_qsCache.inputNote;
        text({micTitleLeft, mic.top + inset + static_cast<LONG>(tileIcon) - 2, mic.right - inset, mic.bottom - 36},
            statusFont, micStatus, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 180);
        text({mic.right - 26, mic.top + inset - 2, mic.right - 8, mic.top + inset + static_cast<LONG>(tileIcon)},
            statusFont, L"\u203A", DT_CENTER | DT_VCENTER | DT_SINGLELINE, 168);
        const LONG barH = std::max(6L, std::lround(8.0F * scale));
        const RECT meter{mic.left + inset, mic.bottom - inset - barH, mic.right - inset, mic.bottom - inset};
        if (draw) {
            m_qsMeterRect = meter;
            m_qsMeterValid = true;
            constexpr int segments = 12;
            const float peak = m_qsPaintUnderlayPass ? 0.0F : m_qsCache.inputPeak;
            const int lit = m_qsCache.inputMuted || m_qsPaintUnderlayPass
                ? 0
                : static_cast<int>(std::lround(std::pow(std::clamp(peak, 0.0F, 1.0F), 0.45F) *
                      static_cast<float>(segments)));
            const LONG segGap = std::max(2L, std::lround(3.0F * scale));
            const LONG segW = std::max(4L, (meter.right - meter.left - segGap * (segments - 1)) / segments);
            const float segRadius = static_cast<float>(barH) * 0.5F;
            for (int index = 0; index < segments; ++index) {
                const LONG left = meter.left + index * (segW + segGap);
                const float cxL = static_cast<float>(left) + segRadius;
                const float cxR = static_cast<float>(left + segW) - segRadius;
                const float cy = 0.5F * static_cast<float>(meter.top + meter.bottom);
                if (index < lit) {
                    FillHGradientPill(pixels, width, height, cxL, std::max(cxL + 1.0F, cxR), cy, segRadius,
                        1.0F, kBlueB, kBlueG, kBlueR, kBlueB, kBlueG, kBlueR);
                } else {
                    FillHGradientPill(pixels, width, height, cxL, std::max(cxL + 1.0F, cxR), cy, segRadius,
                        1.0F, 198, 198, 202, 188, 188, 192);
                }
            }
        }
        push(TrayFlyoutHitKind::Microphone, mic);
        y += homeCardH + homeGap;

        const LONG cell = (inner - homeGap * 2L) / 3L;
        auto smallTile = [&](RECT bounds, wchar_t symbol, const wchar_t* title, const std::wstring& subtitle,
                             TrayFlyoutHitKind kind) {
            homeCard(bounds);
            icon(bounds.left + inset, bounds.top + std::max(8L, std::lround(10.0F * scale)), symbol, tileIcon,
                inkR, inkG, inkB);
            const LONG titleTop = bounds.top + std::max(8L, std::lround(10.0F * scale)) +
                static_cast<LONG>(tileIcon) + 2;
            text({bounds.left + inset, titleTop, bounds.right - 8, titleTop + std::max(16L, std::lround(20.0F * scale))},
                labelFont, title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
            const LONG statusBottom = bounds.bottom - std::max(8L, std::lround(10.0F * scale));
            const LONG statusTop = statusBottom - std::max(16L, std::lround(18.0F * scale));
            const LONG chevW = std::max(12L, std::lround(14.0F * scale));
            text({bounds.left + inset, statusTop, bounds.right - inset - chevW, statusBottom}, statusFont,
                subtitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 196);
            text({bounds.right - inset - chevW, statusTop, bounds.right - std::max(6L, std::lround(8.0F * scale)),
                     statusBottom},
                statusFont, L"\u203A", DT_RIGHT | DT_VCENTER | DT_SINGLELINE, 168);
            push(kind, bounds);
        };
        std::wstring displayStatus = L"On";
        if (m_qsCache.nightKnown && m_qsCache.nightLight) {
            displayStatus = L"Night light";
        }
        x = padding;
        // Temp tile needs more than 1/3 width so two-line values fit; Display
        // and Power shrink slightly to fund it.
        {
            const LONG tempsW = std::max(cell + std::max(8L, homeGap / 2L), (inner * 40L) / 100L);
            const LONG sideW = std::max(1L, (inner - tempsW - homeGap * 2L) / 2L);
            smallTile({x, y, x + sideW, y + homeSmallH}, L'\uE706', L"Display", displayStatus,
                TrayFlyoutHitKind::Display);
            x += sideW + homeGap;
            smallTile({x, y, x + sideW, y + homeSmallH}, L'\uE708', L"Power", EnergyModeLabel(m_qsCache.powerMode),
                TrayFlyoutHitKind::Power);
            x += sideW + homeGap;
        }
        {
            // Thermometer + CPU / GPU label-over-value columns. Values are two
            // lines (temp then watts) in the smaller status font so neither
            // column clips. Read-only, no hit/hover/chevron.
            const RECT temps{x, y, panelWidth - padding, y + homeSmallH};
            homeCard(temps);
            const float glyphH = static_cast<float>(tileIcon) * 1.15F;
            const float glyphCx = static_cast<float>(temps.left + inset) + static_cast<float>(tileIcon) * 0.45F;
            const float glyphTop = static_cast<float>(temps.top + temps.bottom) * 0.5F - glyphH * 0.5F;
            if (draw) {
                FillThermometerPremul(pixels, width, height, glyphCx, glyphTop, glyphH,
                    std::max(1.0F, 1.05F * scale), inkR, inkG, inkB);
            }
            const LONG columnsLeft = temps.left + inset + static_cast<LONG>(tileIcon) +
                std::max(6L, std::lround(8.0F * scale));
            const LONG columnsRight = temps.right - std::max(4L, inset / 2L);
            const LONG columnW = std::max(1L, (columnsRight - columnsLeft) / 2L);
            const LONG labelH = std::max(16L, std::lround(18.0F * scale));
            const LONG valueH = std::max(30L, std::lround(34.0F * scale));
            const LONG blockTop = (temps.top + temps.bottom) / 2L - (labelH + valueH) / 2L;
            const RECT cpuLabel{columnsLeft, blockTop, columnsLeft + columnW, blockTop + labelH};
            const RECT gpuLabel{columnsLeft + columnW, blockTop, columnsRight, blockTop + labelH};
            const RECT cpuValue{cpuLabel.left, cpuLabel.bottom, cpuLabel.right, cpuLabel.bottom + valueH};
            const RECT gpuValue{gpuLabel.left, gpuLabel.bottom, gpuLabel.right, gpuLabel.bottom + valueH};
            constexpr UINT labelFormat = DT_LEFT | DT_VCENTER | DT_SINGLELINE;
            // Wrap/newline so temp and watts paint as two lines inside the value rect.
            constexpr UINT valueFormat = DT_LEFT | DT_TOP | DT_WORDBREAK;
            text(cpuLabel, labelFont, L"CPU", labelFormat, 255);
            text(gpuLabel, labelFont, L"GPU", labelFormat, 255);
            if (draw) {
                m_qsTempCpuRect = cpuValue;
                m_qsTempGpuRect = gpuValue;
                m_qsTempsValid = true;
                m_qsTempsInkR = g_flyoutInkR;
                m_qsTempsInkG = g_flyoutInkG;
                m_qsTempsInkB = g_flyoutInkB;
            }
            // Values are a live overlay (see PaintQsLiveOverlays); only an
            // overlay-less paint draws them into the plate. Prefer the smaller
            // status font so two lines fit the column width.
            HFONT valueFont = statusFont != nullptr ? statusFont : labelFont;
            if (!m_qsPaintUnderlayPass) {
                text(cpuValue, valueFont, TempPowerText(m_qsCache.cpuTempC, m_qsCache.cpuWatts), valueFormat, 255);
                text(gpuValue, valueFont, TempPowerText(m_qsCache.gpuTempC, m_qsCache.gpuWatts), valueFormat, 255);
            }
        }
        y += homeSmallH + homeGap;

        const RECT media{padding, y, panelWidth - padding, y + homeMediaH};
        homeCard(media);
        const LONG art = std::max(44L, std::min(homeMediaH - std::max(16L, std::lround(20.0F * scale)),
                          std::lround(56.0F * scale)));
        const LONG artLeft = media.left + std::max(12L, std::lround(14.0F * scale));
        const LONG artTop = media.top + (homeMediaH - art) / 2L;
        if (draw) {
            FillAlbumDisc(pixels, width, height, static_cast<float>(artLeft) + static_cast<float>(art) * 0.5F,
                static_cast<float>(artTop) + static_cast<float>(art) * 0.5F,
                static_cast<float>(art) * 0.5F);
        }
        const LONG scrubW = std::max(108L, std::lround(132.0F * scale));
        const LONG scrubH = std::max(7L, std::lround(9.0F * scale));
        const LONG scrubRight = media.right - std::max(16L, std::lround(18.0F * scale));
        const LONG scrubLeft = scrubRight - scrubW;
        const LONG midY = media.top + homeMediaH / 2L;
        const RECT scrub{scrubLeft, midY - scrubH / 2L, scrubRight, midY + (scrubH - scrubH / 2L)};
        const LONG transport = std::max(28L, std::lround(32.0F * scale));
        const LONG transportGap = std::max(2L, std::lround(4.0F * scale));
        const LONG nextRight = scrubLeft - std::max(12L, std::lround(16.0F * scale));
        const RECT next{nextRight - transport, midY - transport / 2L, nextRight, midY + transport / 2L};
        const RECT play{next.left - transportGap - transport, next.top, next.left - transportGap, next.bottom};
        const RECT prev{play.left - transportGap - transport, next.top, play.left - transportGap, next.bottom};
        push(TrayFlyoutHitKind::MediaSeek, scrub);
        push(TrayFlyoutHitKind::MediaTransport, prev, 0);
        push(TrayFlyoutHitKind::MediaTransport, play, 1);
        push(TrayFlyoutHitKind::MediaTransport, next, 2);
        const LONG textLeft = artLeft + art + std::max(10L, std::lround(12.0F * scale));
        const std::wstring mediaTitle = m_qsCache.mediaHave && !m_qsCache.mediaTitle.empty()
            ? m_qsCache.mediaTitle
            : (m_qsCache.mediaHave ? L"Now playing" : L"Nothing playing");
        const std::wstring mediaArtist = m_qsCache.mediaHave
            ? (m_qsCache.mediaArtist.empty() ? (m_qsCache.mediaPlaying ? L"Playing" : L"Paused")
                                             : m_qsCache.mediaArtist)
            : L"System media";
        text({textLeft, media.top + homeMediaH / 2L - std::max(20L, std::lround(22.0F * scale)), prev.left - 8,
                 media.top + homeMediaH / 2L},
            titleFont, mediaTitle, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
        text({textLeft, media.top + homeMediaH / 2L, prev.left - 8,
                 media.top + homeMediaH / 2L + std::max(18L, std::lround(20.0F * scale))},
            statusFont, mediaArtist, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 180);
        const UINT transportIcon = static_cast<UINT>(std::max(14L, std::lround(16.0F * scale)));
        icon(prev.left + (transport - static_cast<LONG>(transportIcon)) / 2L,
            prev.top + (transport - static_cast<LONG>(transportIcon)) / 2L, L'\uE892', transportIcon, inkR, inkG,
            inkB);
        icon(play.left + (transport - static_cast<LONG>(transportIcon)) / 2L,
            play.top + (transport - static_cast<LONG>(transportIcon)) / 2L,
            m_qsCache.mediaPlaying ? L'\uE769' : L'\uE768', transportIcon, inkR, inkG, inkB);
        icon(next.left + (transport - static_cast<LONG>(transportIcon)) / 2L,
            next.top + (transport - static_cast<LONG>(transportIcon)) / 2L, L'\uE893', transportIcon, inkR, inkG,
            inkB);
        if (draw) {
            m_qsScrubRect = scrub;
            m_qsScrubValid = true;
        }
        const float scrubLevel = (!m_qsPaintUnderlayPass && m_qsCache.mediaHave) ? m_qsCache.mediaProgress : 0.0F;
        gradientSlider(scrub, scrubLevel, true);
        y += homeMediaH + padding;
        contentBottom = y;
        return;
    }

    auto infoRow = [&](RECT row, const wchar_t* title, const std::wstring& value) {
        text({row.left + 14, row.top + 6, row.right - 14, row.top + 26}, labelFont, title,
            DT_LEFT | DT_BOTTOM | DT_SINGLELINE, 255);
        text({row.left + 14, row.top + 26, row.right - 14, row.bottom - 4}, statusFont, value,
            DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 180);
    };
    auto navRow = [&](RECT row, wchar_t symbol, const std::wstring& title, const std::wstring& subtitle,
                      TrayFlyoutHitKind kind, int index) {
        const UINT extent = 18;
        icon(row.left + 12, row.top + (listRow - 18) / 2L, symbol, extent, inkR, inkG, inkB);
        text({row.left + 40, row.top + 6, row.right - 28, row.top + 28}, labelFont, title,
            DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
        text({row.left + 40, row.top + 28, row.right - 28, row.bottom - 4}, statusFont, subtitle,
            DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 170);
        text({row.right - 24, row.top, row.right - 8, row.bottom}, statusFont, L"\u203A",
            DT_CENTER | DT_VCENTER | DT_SINGLELINE, 160);
        push(kind, row, index);
    };

    if (m_qsPage == QuickSettingsPage::Wifi) {
        beginHeader(L"Wi-Fi", false, kToggleWifi, m_qsCache.wifiRadioOn);
        y += gap;
        if (!m_qsCache.wifiRadioOn) {
            card({padding, y, panelWidth - padding, y + listRow}, false);
            text({padding, y, panelWidth - padding, y + listRow}, labelFont, L"Wi-Fi is off",
                DT_CENTER | DT_VCENTER | DT_SINGLELINE, 200);
            y += listRow + gap;
        } else if (m_qsCache.wifi.empty()) {
            card({padding, y, panelWidth - padding, y + listRow}, false);
            text({padding, y, panelWidth - padding, y + listRow}, labelFont, L"No networks found",
                DT_CENTER | DT_VCENTER | DT_SINGLELINE, 200);
            y += listRow + gap;
        } else {
            const LONG rows = static_cast<LONG>(m_qsCache.wifi.size());
            const RECT group{padding, y, panelWidth - padding, y + rows * listRow};
            card(group, false);
            for (LONG index = 0; index < rows; ++index) {
                const QsWifiNetwork& network = m_qsCache.wifi[static_cast<size_t>(index)];
                const RECT row{group.left, group.top + index * listRow, group.right,
                    group.top + (index + 1) * listRow};
                std::wstring subtitle = network.connected ? L"Connected" : (network.secure ? L"Secured" : L"Open");
                navRow(row, L'\uE701', network.name, subtitle, TrayFlyoutHitKind::WifiNetwork,
                    static_cast<int>(index));
            }
            y += rows * listRow + gap;
        }
        footer(L"More Wi-Fi settings", kLinkWifi);
    } else if (m_qsPage == QuickSettingsPage::Ethernet) {
        beginHeader(L"Ethernet", false, -1, false);
        y += 4;
        const std::wstring summary = m_qsCache.ethernetUp
            ? (m_qsCache.ethernetStatus + L"  \u00B7  " + m_qsCache.ethernetSpeed)
            : m_qsCache.ethernetStatus;
        text({padding, y, panelWidth - padding, y + sectionH}, statusFont, summary,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 180);
        y += sectionH + 6;
        const RECT group{padding, y, panelWidth - padding, y + listRow * 4L};
        card(group, false);
        const wchar_t* labels[4] = {L"Status", L"Link speed", L"IPv4", L"Adapter"};
        const std::wstring values[4] = {m_qsCache.ethernetStatus, m_qsCache.ethernetSpeed,
            m_qsCache.ethernetIpv4, m_qsCache.ethernetAdapter};
        for (LONG index = 0; index < 4; ++index) {
            infoRow({group.left, group.top + index * listRow, group.right, group.top + (index + 1) * listRow},
                labels[index], values[index]);
        }
        y += listRow * 4L + gap;
        footer(L"More network settings", kLinkNetwork);
    } else if (m_qsPage == QuickSettingsPage::Vpn) {
        beginHeader(L"VPN", false, -1, false);
        y += gap;
        if (m_qsCache.vpn.empty()) {
            card({padding, y, panelWidth - padding, y + listRow}, false);
            text({padding, y, panelWidth - padding, y + listRow}, labelFont, L"No VPN connected",
                DT_CENTER | DT_VCENTER | DT_SINGLELINE, 200);
            y += listRow + gap;
        } else {
            const LONG rows = static_cast<LONG>(m_qsCache.vpn.size());
            const RECT group{padding, y, panelWidth - padding, y + rows * listRow};
            card(group, false);
            for (LONG index = 0; index < rows; ++index) {
                const QsVpnEntry& entry = m_qsCache.vpn[static_cast<size_t>(index)];
                navRow({group.left, group.top + index * listRow, group.right,
                           group.top + (index + 1) * listRow},
                    L'\uE72E', entry.name, entry.connected ? L"Connected" : L"Not connected",
                    TrayFlyoutHitKind::VpnEntry, static_cast<int>(index));
            }
            y += rows * listRow + gap;
        }
        footer(L"More VPN settings", kLinkVpn);
    } else if (m_qsPage == QuickSettingsPage::Sound) {
        beginHeader(L"Sound", false, -1, false);
        y += gap;
        const RECT track{padding + 28, y, panelWidth - padding - 8, y + sliderH};
        push(TrayFlyoutHitKind::VolumeSlider, track);
        icon(padding, y - 2, tray.volumeMuted ? L'\uE74F' : L'\uE767', 18, inkR, inkG, inkB);
        slider(track, volume);
        y += sliderH + gap;
        section(L"Output device");
        const LONG rows = std::max(1L, static_cast<LONG>(m_qsCache.renderDevices.size()));
        const RECT group{padding, y, panelWidth - padding, y + rows * listRow};
        card(group, false);
        if (m_qsCache.renderDevices.empty()) {
            infoRow(group, outputName.c_str(), L"Default output");
        } else {
            for (LONG index = 0; index < rows; ++index) {
                const QsNamedId& device = m_qsCache.renderDevices[static_cast<size_t>(index)];
                const RECT row{group.left, group.top + index * listRow, group.right,
                    group.top + (index + 1) * listRow};
                const bool selected = device.id == m_qsCache.renderDefaultId;
                radio(static_cast<float>(row.left + 22), static_cast<float>(row.top + listRow / 2L),
                    selected);
                text({row.left + 40, row.top + 8, row.right - 12, row.top + 30}, labelFont, device.name,
                    DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
                text({row.left + 40, row.top + 30, row.right - 12, row.bottom - 6}, statusFont,
                    selected ? L"Default" : L"Available", DT_LEFT | DT_TOP | DT_SINGLELINE, 170);
                push(TrayFlyoutHitKind::AudioOutput, row, static_cast<int>(index));
            }
        }
        y += rows * listRow + gap;
        const RECT mute{padding, y, panelWidth - padding, y + listRow};
        card(mute, false);
        text({mute.left + 14, mute.top, mute.right - 70, mute.bottom}, labelFont, L"Mute",
            DT_LEFT | DT_VCENTER | DT_SINGLELINE, 255);
        const RECT muteToggle = toggleRect(mute.top + listRow / 2L, mute.right - 12, tray.volumeMuted);
        push(TrayFlyoutHitKind::Toggle, muteToggle, kToggleMute);
        y += listRow + gap;
        const RECT apps{padding, y, panelWidth - padding, y + listRow};
        card(apps, false);
        navRow(apps, L'\uE767', L"Per-app volume", L"Open the volume mixer", TrayFlyoutHitKind::MoreSettings,
            kLinkMixer);
        y += listRow + gap;
        footer(L"More sound settings", kLinkSound);
    } else if (m_qsPage == QuickSettingsPage::Microphone) {
        beginHeader(L"Microphone", false, -1, false);
        y += gap;
        section(L"Input level");
        const RECT level{padding, y, panelWidth - padding, y + 28};
        card(level, false);
        if (draw && !m_qsCache.inputNote.empty()) {
            text(level, statusFont, m_qsCache.inputNote, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS,
                220);
        } else if (draw) {
            m_qsMeterRect = level;
            m_qsMeterValid = true;
            const int segments = 16;
            const float peak = m_qsPaintUnderlayPass ? 0.0F : m_qsCache.inputPeak;
            const int lit = m_qsCache.inputMuted || m_qsPaintUnderlayPass
                ? 0
                : static_cast<int>(std::lround(std::pow(std::clamp(peak, 0.0F, 1.0F), 0.45F) * static_cast<float>(segments)));
            const LONG segGap = 3;
            const LONG segW =
                std::max(3L, (level.right - level.left - 24 - segGap * (segments - 1)) / segments);
            for (int index = 0; index < segments; ++index) {
                const LONG left = level.left + 12 + index * (segW + segGap);
                const RECT seg{left, level.top + 8, left + segW, level.bottom - 8};
                FillSquircleColorPremul(pixels, width, height, seg, 2.0F, index < lit ? 0.95F : 0.25F,
                    index < lit ? kBlueB : inkB, index < lit ? kBlueG : inkG, index < lit ? kBlueR : inkR);
            }
        }
        y += 28 + gap;
        section(L"Gain");
        const RECT gain{padding, y, panelWidth - padding, y + sliderH};
        push(TrayFlyoutHitKind::CaptureGain, gain);
        slider(gain, m_qsCache.inputGain);
        y += sliderH + gap;
        section(L"Input device");
        const LONG rows = std::max(1L, static_cast<LONG>(m_qsCache.captureDevices.size()));
        const RECT group{padding, y, panelWidth - padding, y + rows * listRow};
        card(group, false);
        if (m_qsCache.captureDevices.empty()) {
            infoRow(group, inputName.c_str(), L"Default input");
        } else {
            for (LONG index = 0; index < rows; ++index) {
                const QsNamedId& device = m_qsCache.captureDevices[static_cast<size_t>(index)];
                const RECT row{group.left, group.top + index * listRow, group.right,
                    group.top + (index + 1) * listRow};
                const bool selected = device.id == m_qsCache.captureDefaultId;
                radio(static_cast<float>(row.left + 22), static_cast<float>(row.top + listRow / 2L),
                    selected);
                text({row.left + 40, row.top + 8, row.right - 12, row.top + 30}, labelFont, device.name,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
                push(TrayFlyoutHitKind::AudioInput, row, static_cast<int>(index));
            }
        }
        y += rows * listRow + gap;
        const RECT mute{padding, y, panelWidth - padding, y + listRow};
        card(mute, false);
        text({mute.left + 14, mute.top, mute.right - 70, mute.bottom}, labelFont, L"Mute microphone",
            DT_LEFT | DT_VCENTER | DT_SINGLELINE, 255);
        push(TrayFlyoutHitKind::Toggle, toggleRect(mute.top + listRow / 2L, mute.right - 12, m_qsCache.inputMuted),
            kToggleMic);
        y += listRow + gap;
        footer(L"More sound settings", kLinkSound);
    } else if (m_qsPage == QuickSettingsPage::Power) {
        beginHeader(L"Power", false, -1, false);
        y += gap;
        section(L"Energy mode");
        const RECT group{padding, y, panelWidth - padding, y + listRow * 3L};
        card(group, false);
        const wchar_t* modes[3] = {L"High power", L"Automatic", L"Low power"};
        for (int index = 0; index < 3; ++index) {
            const RECT row{group.left, group.top + index * listRow, group.right,
                group.top + (index + 1) * listRow};
            radio(static_cast<float>(row.left + 22), static_cast<float>(row.top + listRow / 2L),
                m_qsCache.powerMode == index);
            text({row.left + 40, row.top, row.right - 12, row.bottom}, labelFont, modes[index],
                DT_LEFT | DT_VCENTER | DT_SINGLELINE, 255);
            push(TrayFlyoutHitKind::PowerMode, row, index);
        }
        y += listRow * 3L + gap;
        if (m_qsCache.usbKnown) {
            const RECT usb{padding, y, panelWidth - padding, y + listRow};
            card(usb, false);
            text({usb.left + 14, usb.top, usb.right - 70, usb.bottom}, labelFont, L"USB selective suspend",
                DT_LEFT | DT_VCENTER | DT_SINGLELINE, 255);
            push(TrayFlyoutHitKind::Toggle,
                toggleRect(usb.top + listRow / 2L, usb.right - 12, m_qsCache.usbSuspend), kToggleUsb);
            y += listRow + gap;
        }
        section(L"Using significant energy");
        const size_t energyCount = m_qsCache.energyLive
            ? std::min(m_qsCache.energyApps.size(), static_cast<size_t>(3))
            : 0;
        if (energyCount == 0) {
            const std::wstring note = m_qsCache.energyNote.empty()
                ? (m_qsCache.energyPending ? L"Measuring energy use\u2026" : L"Energy usage unavailable")
                : m_qsCache.energyNote;
            const RECT row{padding, y, panelWidth - padding, y + listRow};
            card(row, false);
            text({row.left + 14, row.top, row.right - 14, row.bottom}, labelFont, note,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 200);
            y += listRow + gap;
        } else {
            const RECT energyGroup{padding, y, panelWidth - padding,
                y + static_cast<LONG>(energyCount) * listRow};
            card(energyGroup, false);
            for (size_t index = 0; index < energyCount; ++index) {
                const QsEnergyApp& energyApp = m_qsCache.energyApps[index];
                const RECT row{energyGroup.left,
                    energyGroup.top + static_cast<LONG>(index) * listRow, energyGroup.right,
                    energyGroup.top + static_cast<LONG>(index + 1) * listRow};
                const std::wstring usage = std::to_wstring(energyApp.cpuPercent) + L"% CPU";
                text({row.left + 14, row.top, row.right - 92, row.bottom}, labelFont, energyApp.name,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
                text({row.right - 88, row.top, row.right - 12, row.bottom}, statusFont, usage,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE, 180);
            }
            y += static_cast<LONG>(energyCount) * listRow + gap;
        }
        footer(L"More power settings", kLinkPower);

    } else if (m_qsPage == QuickSettingsPage::SystemTray) {
        beginHeader(L"System Tray", false, -1, false);
        y += 2;
        text({padding, y, panelWidth - padding, y + sectionH}, statusFont, L"Icons and right-click actions",
            DT_LEFT | DT_VCENTER | DT_SINGLELINE, 170);
        y += sectionH;
        const size_t shown = std::min(m_overflowIcons.size(), static_cast<size_t>(8));
        if (shown == 0) {
            card({padding, y, panelWidth - padding, y + listRow}, false);
            text({padding, y, panelWidth - padding, y + listRow}, labelFont, L"No tray icons",
                DT_CENTER | DT_VCENTER | DT_SINGLELINE, 200);
            y += listRow + gap;
        } else {
            const RECT group{padding, y, panelWidth - padding, y + static_cast<LONG>(shown) * listRow};
            card(group, false);
            for (size_t index = 0; index < shown; ++index) {
                const RECT row{group.left, group.top + static_cast<LONG>(index) * listRow, group.right,
                    group.top + static_cast<LONG>(index + 1) * listRow};
                std::wstring title = NotifyIconTitle(m_overflowIcons[index]);
                const size_t paren = title.find(L" (");
                if (paren != std::wstring::npos && paren > 0) {
                    title.resize(paren);
                }
                std::wstring subtitle = NotifyIconStatus(m_overflowIcons[index]);
                if (subtitle.empty()) {
                    subtitle = L"Click to open";
                }
                if (draw) {
                    const LONG iconLeft = row.left + 12;
                    const LONG iconTop = row.top + (listRow - 18) / 2L;
                    if (m_overflowIcons[index].icon != nullptr) {
                        BlitIcon(pixels, width, height, memory, m_overflowIcons[index].icon,
                            static_cast<int>(iconLeft), static_cast<int>(iconTop), 18);
                    } else {
                        // No grey placeholder plate - use a Fluent app glyph instead.
                        icon(iconLeft, iconTop, static_cast<wchar_t>(0xE8A5), 18, inkR, inkG, inkB);
                    }
                }
                text({row.left + 40, row.top + 6, row.right - 16, row.top + 28}, labelFont, title,
                    DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
                text({row.left + 40, row.top + 28, row.right - 16, row.bottom - 4}, statusFont, subtitle,
                    DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 170);
                push(TrayFlyoutHitKind::NotifyIcon, row, static_cast<int>(index));
            }
            y += static_cast<LONG>(shown) * listRow + gap;
        }
        footer(L"Manage tray icons", kLinkTaskbar);
    } else if (m_qsPage == QuickSettingsPage::Airplane) {
        const bool airplane = !m_qsCache.wifiRadioOn &&
            (!m_bluetoothSnapshot.radioPresent || !m_bluetoothSnapshot.radioOn);
        beginHeader(L"Airplane mode", false, kToggleAirplane, airplane);
        y += 4;
        text({padding, y, panelWidth - padding, y + sectionH}, statusFont, L"Turns off Wi-Fi and Bluetooth.",
            DT_LEFT | DT_VCENTER | DT_SINGLELINE, 180);
        y += sectionH + 6;
        const RECT group{padding, y, panelWidth - padding, y + listRow * 2L};
        card(group, false);
        navRow({group.left, group.top, group.right, group.top + listRow}, L'\uE701', L"Wi-Fi",
            m_qsCache.wifiRadioOn ? L"On" : L"Off", TrayFlyoutHitKind::Wifi, -1);
        navRow({group.left, group.top + listRow, group.right, group.bottom}, L'\uE702', L"Bluetooth",
            m_bluetoothSnapshot.radioOn ? L"On" : L"Off", TrayFlyoutHitKind::MoreSettings, 11);
        y += listRow * 2L + gap;
        footer(L"More network settings", kLinkNetwork);
    } else if (m_qsPage == QuickSettingsPage::Bluetooth) {
        beginHeader(L"Bluetooth", false, -1, false);
        if (m_bluetoothSnapshot.ready && m_bluetoothSnapshot.radioPresent) {
            const RECT toggle = toggleRect(padding + headerHeight / 2L, panelWidth - padding,
                m_bluetoothSnapshot.radioOn);
            push(TrayFlyoutHitKind::BluetoothRadio, toggle);
        }
        y += gap;
        if (!m_bluetoothSnapshot.ready) {
            text({padding, y, panelWidth - padding, y + listRow}, labelFont, L"Looking for devices...",
                DT_CENTER | DT_VCENTER | DT_SINGLELINE, 200);
            y += listRow;
        } else if (!m_bluetoothSnapshot.radioPresent || !m_bluetoothSnapshot.radioOn) {
            card({padding, y, panelWidth - padding, y + listRow}, false);
            text({padding, y, panelWidth - padding, y + listRow}, labelFont,
                m_bluetoothSnapshot.radioPresent ? L"Bluetooth is off" : L"Bluetooth is unavailable",
                DT_CENTER | DT_VCENTER | DT_SINGLELINE, 200);
            y += listRow + gap;
        } else {
            const size_t paired = std::min(m_bluetoothSnapshot.paired.size(), static_cast<size_t>(4));
            const size_t found = std::min(m_bluetoothSnapshot.discovered.size(), static_cast<size_t>(3));
            const LONG rows = static_cast<LONG>(paired + found + 1);
            const RECT group{padding, y, panelWidth - padding, y + rows * listRow};
            card(group, false);
            LONG rowIndex = 0;
            for (size_t index = 0; index < paired; ++index, ++rowIndex) {
                const BluetoothDeviceInfo& device = m_bluetoothSnapshot.paired[index];
                navRow({group.left, group.top + rowIndex * listRow, group.right,
                           group.top + (rowIndex + 1) * listRow},
                    L'\uE702', device.name, device.connected ? L"Connected" : device.status,
                    TrayFlyoutHitKind::BluetoothConnect, static_cast<int>(index));
            }
            for (size_t index = 0; index < found; ++index, ++rowIndex) {
                const BluetoothDeviceInfo& device = m_bluetoothSnapshot.discovered[index];
                navRow({group.left, group.top + rowIndex * listRow, group.right,
                           group.top + (rowIndex + 1) * listRow},
                    L'\uE702', device.name, device.busy ? L"Pairing..." : L"Tap to pair",
                    TrayFlyoutHitKind::BluetoothPair, static_cast<int>(index));
            }
            navRow({group.left, group.top + rowIndex * listRow, group.right, group.bottom}, L'\uE710',
                m_bluetoothSnapshot.discovering ? L"Stop searching" : L"Pair new device",
                m_bluetoothSnapshot.discovering ? L"Looking nearby" : L"Headphones, speakers, and more",
                TrayFlyoutHitKind::BluetoothDiscover, -1);
            y += rows * listRow + gap;
        }
        footer(L"More Bluetooth settings", kLinkBluetooth);
    } else {
        beginHeader(L"Display", false, -1, false);
        y += gap;
        const RECT brightRow{padding, y, panelWidth - padding, y + listRow};
        card(brightRow, false);
        icon(brightRow.left + 14, brightRow.top + (listRow - 18) / 2L, L'\uE706', 18, inkR, inkG, inkB);
        text({brightRow.left + 42, brightRow.top + 4, brightRow.right - 14, brightRow.top + 24}, labelFont,
            L"Brightness", DT_LEFT | DT_BOTTOM | DT_SINGLELINE, 255);
        const RECT brightTrack{brightRow.left + 42, brightRow.bottom - 8 - sliderH, brightRow.right - 16,
            brightRow.bottom - 8};
        push(TrayFlyoutHitKind::BrightnessSlider, brightTrack);
        slider(brightTrack, brightness);
        y += listRow + gap;
        const RECT night{padding, y, panelWidth - padding, y + listRow};
        card(night, false);
        navRow(night, L'\uE706', L"Night light",
            m_qsCache.nightKnown ? (m_qsCache.nightLight ? L"On" : L"Off") : L"Open settings",
            TrayFlyoutHitKind::MoreSettings, kLinkNight);
        y += listRow + gap;
        const RECT hdr{padding, y, panelWidth - padding, y + listRow};
        card(hdr, false);
        text({hdr.left + 14, hdr.top, hdr.right - 70, hdr.bottom}, labelFont, L"HDR",
            DT_LEFT | DT_VCENTER | DT_SINGLELINE, 255);
        text({hdr.left + 70, hdr.top, hdr.right - 78, hdr.bottom}, statusFont,
            !m_qsCache.hdrSupported ? L"Unavailable" : (m_qsCache.hdrOn ? L"On" : L"Off"),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE, 180);
        if (m_qsCache.hdrSupported) {
            push(TrayFlyoutHitKind::Toggle,
                toggleRect(hdr.top + listRow / 2L, hdr.right - 12, m_qsCache.hdrOn), kToggleHdr);
        }
        y += listRow + gap;
        footer(L"More display settings", kLinkDisplay);
    }
    y += padding;
    contentBottom = y;
}

void DockApp::ApplyQuickSettingsCommand(const TrayFlyoutHit& hit, UINT message) {
    const bool alternate = message == WM_RBUTTONUP;
    auto openUri = [&](const wchar_t* uri) { OpenSettingsPage(uri); };
    auto navigate = [&](QuickSettingsPage page, const wchar_t* uri) {
        if (alternate) {
            openUri(uri);
        } else {
            OpenQuickSettingsPage(page);
        }
    };
    switch (hit.kind) {
    case TrayFlyoutHitKind::None:
    case TrayFlyoutHitKind::Settings:
    case TrayFlyoutHitKind::Wifi:
    case TrayFlyoutHitKind::Sound:
    case TrayFlyoutHitKind::Brightness:
    case TrayFlyoutHitKind::Boost:
    case TrayFlyoutHitKind::BluetoothRadio:
    case TrayFlyoutHitKind::BluetoothConnect:
    case TrayFlyoutHitKind::BluetoothPair:
    case TrayFlyoutHitKind::BluetoothDiscover:
    case TrayFlyoutHitKind::BluetoothSettings:
    case TrayFlyoutHitKind::NotifyIcon:
    case TrayFlyoutHitKind::VolumeSlider:
    case TrayFlyoutHitKind::BrightnessSlider:
    case TrayFlyoutHitKind::CaptureGain:
    case TrayFlyoutHitKind::MediaSeek:
        break;
    case TrayFlyoutHitKind::Back:
        CloseQuickSettingsPage();
        break;
    case TrayFlyoutHitKind::Ethernet:
        navigate(QuickSettingsPage::Ethernet, L"ms-settings:network-ethernet");
        break;
    case TrayFlyoutHitKind::Vpn:
        navigate(QuickSettingsPage::Vpn, L"ms-settings:network-vpn");
        break;
    case TrayFlyoutHitKind::Microphone:
        navigate(QuickSettingsPage::Microphone, L"ms-settings:sound");
        break;
    case TrayFlyoutHitKind::Display:
        navigate(QuickSettingsPage::Display, L"ms-settings:display");
        break;
    case TrayFlyoutHitKind::Hdr:
        navigate(QuickSettingsPage::Display, L"ms-settings:display");
        break;
    case TrayFlyoutHitKind::Power:
        navigate(QuickSettingsPage::Power, L"ms-settings:powersleep");
        break;
    case TrayFlyoutHitKind::Airplane:
        navigate(QuickSettingsPage::Airplane, L"ms-settings:network-airplanemode");
        break;
    case TrayFlyoutHitKind::SystemTrayPage:
        if (alternate) {
            openUri(L"ms-settings:taskbar");
        } else {
            OpenQuickSettingsPage(QuickSettingsPage::SystemTray);
        }
        break;
    case TrayFlyoutHitKind::Toggle:
        switch (hit.index) {
        case kToggleWifi:
            SetWifiRadio(!m_qsCache.wifiRadioOn);
            static_cast<void>(m_tray.Refresh());
            m_qsCache.stamp = 0;
            EnsureTrayIcons();
            QueueOverflowPaint();
            QueueRenderFrame();
            break;
        case kToggleMute:
            if (!m_tray.ToggleMute()) {
                Log(L"Volume mute did not change.");
                break;
            }
            EnsureTrayIcons();
            QueueOverflowPaint();
            QueueRenderFrame();
            break;
        case kToggleMic:
            SetCaptureMuted(!m_qsCache.inputMuted);
            m_qsCache.stamp = 0;
            QueueOverflowPaint();
            break;
        case kToggleUsb:
            SetUsbSuspend(!m_qsCache.usbSuspend);
            m_qsCache.stamp = 0;
            QueueOverflowPaint();
            break;
        case kToggleAirplane: {
            const bool radiosOn = !m_qsCache.wifiRadioOn &&
                (!m_bluetoothSnapshot.radioPresent || !m_bluetoothSnapshot.radioOn);
            SetWifiRadio(radiosOn);
            if (m_bluetoothSnapshot.radioPresent) {
                m_bluetoothSnapshot.radioOn = radiosOn;
                m_bluetoothSnapshot.discovering = radiosOn ? m_bluetoothSnapshot.discovering : false;
                if (!radiosOn) {
                    m_bluetoothSnapshot.discovered.clear();
                }
                m_bluetooth.SetRadioEnabled(radiosOn);
            }
            static_cast<void>(m_tray.Refresh());
            m_qsCache.stamp = 0;
            EnsureTrayIcons();
            QueueOverflowPaint();
            QueueRenderFrame();
            break;
        }
        case kToggleHdr:
            SetHdrEnabled(!m_qsCache.hdrOn);
            m_qsCache.stamp = 0;
            QueueOverflowPaint();
            break;
        default:
            break;
        }
        break;
    case TrayFlyoutHitKind::PowerMode:
        if (SetPowerMode(hit.index)) {
            m_qsCache.powerMode = hit.index;
            m_qsCache.stamp = 0;
            QueueOverflowPaint();
        }
        break;
    case TrayFlyoutHitKind::WifiNetwork:
        if (hit.index >= 0 && static_cast<size_t>(hit.index) < m_qsCache.wifi.size()) {
            const QsWifiNetwork& network = m_qsCache.wifi[static_cast<size_t>(hit.index)];
            if (!network.connected && network.hasProfile && m_qsCache.haveWifiInterface &&
                ConnectWifiProfile(m_qsCache.wifiInterface, network.name)) {
                static_cast<void>(m_tray.Refresh());
                m_qsCache.stamp = 0;
                EnsureTrayIcons();
                QueueOverflowPaint();
                QueueRenderFrame();
            } else if (!network.connected) {
                openUri(L"ms-settings:network-wifi");
            }
        }
        break;
    case TrayFlyoutHitKind::AudioOutput:
        if (hit.index >= 0 && static_cast<size_t>(hit.index) < m_qsCache.renderDevices.size()) {
            const QsNamedId& device = m_qsCache.renderDevices[static_cast<size_t>(hit.index)];
            if (!SetDefaultAudioDevice(device.id)) {
                openUri(L"ms-settings:sound");
                break;
            }
            static_cast<void>(m_tray.Refresh());
            m_qsCache.stamp = 0;
            EnsureTrayIcons();
            QueueOverflowPaint();
            QueueRenderFrame();
        }
        break;
    case TrayFlyoutHitKind::AudioInput:
        if (hit.index >= 0 && static_cast<size_t>(hit.index) < m_qsCache.captureDevices.size()) {
            const QsNamedId& device = m_qsCache.captureDevices[static_cast<size_t>(hit.index)];
            if (!SetDefaultAudioDevice(device.id)) {
                openUri(L"ms-settings:sound");
                break;
            }
            m_qsCache.stamp = 0;
            QueueOverflowPaint();
        }
        break;
    case TrayFlyoutHitKind::VpnEntry:
        openUri(L"ms-settings:network-vpn");
        break;
    case TrayFlyoutHitKind::MoreSettings:
        switch (hit.index) {
        case kLinkWifi:
            openUri(L"ms-settings:network-wifi");
            break;
        case kLinkNetwork:
            openUri(L"ms-settings:network-status");
            break;
        case kLinkVpn:
            openUri(L"ms-settings:network-vpn");
            break;
        case kLinkSound:
            openUri(L"ms-settings:sound");
            break;
        case kLinkPower:
            openUri(L"ms-settings:powersleep");
            break;
        case kLinkTaskbar:
            openUri(L"ms-settings:taskbar");
            break;
        case kLinkBluetooth:
            openUri(L"ms-settings:bluetooth");
            break;
        case kLinkDisplay:
            openUri(L"ms-settings:display");
            break;
        case kLinkNight:
            openUri(L"ms-settings:nightlight");
            break;
        case kLinkMixer:
            CloseOverflowPopup();
            if (!SystemTray::OpenSoundMixer()) {
                Log(L"Volume mixer did not open.");
            }
            break;
        case 11:
            OpenQuickSettingsPage(QuickSettingsPage::Bluetooth);
            break;
        default:
            break;
        }
        break;
    case TrayFlyoutHitKind::MediaTransport:
        ControlMediaSession(hit.index == 0 ? 0 : (hit.index == 2 ? 2 : 1));
        m_qsCache.stamp = 0;
        QueueOverflowPaint();
        break;
    }
}

void DockApp::AdjustCaptureGain(float delta) {
    const float next = std::clamp(m_qsCache.inputGain + delta, 0.0F, 1.0F);
    if (!SetCaptureGain(next)) {
        return;
    }
    m_qsCache.inputGain = next;
    if (delta > 0.0F) {
        m_qsCache.inputMuted = false;
    }
    m_qsCache.stamp = GetTickCount64();
    QueueOverflowPaint();
}
