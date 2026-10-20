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
#include <pdh.h>
#include <pdhmsg.h>
#include <mutex>

#pragma warning(push)
#pragma warning(disable : 4458 4996 26495)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#pragma warning(pop)


#include <atomic>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <limits>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
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
void FillRectColorPremul(uint8_t* dest, int destWidth, int destHeight, RECT bounds, float alpha,
    uint8_t blue, uint8_t green, uint8_t red);
float ContentSquircleRadius(const RECT& bounds, float scale) noexcept;
void RemapPremulInkColor(std::vector<uint8_t>& pixels, uint8_t r, uint8_t g, uint8_t b);
void SetFlyoutChromeInk(uint8_t r, uint8_t g, uint8_t b) noexcept;
extern bool g_flyoutTextHalo;
extern uint8_t g_flyoutInkR;
extern uint8_t g_flyoutInkG;
extern uint8_t g_flyoutInkB;
std::wstring NotifyIconTitle(const TrayNotifyIcon& icon);
std::wstring NotifyIconStatus(const TrayNotifyIcon& icon);
}  // namespace dock_detail
using namespace dock_detail;

namespace {

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
// Opens the in-panel Bluetooth drill-in (Airplane page row).
constexpr int kLinkBluetoothPage = 11;
constexpr int kLinkStartupApps = 12;
constexpr int kLinkDisplayAdvanced = 13;

constexpr int kToggleWifi = 0;
constexpr int kToggleMute = 1;
constexpr int kToggleMic = 2;
constexpr int kToggleAirplane = 4;
constexpr int kToggleHdr = 5;
constexpr int kToggleDarkMode = 6;
constexpr int kToggleRefreshRate = 7;

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

bool LooksLikeVpn(const std::wstring& description, IFTYPE type) {
    if (type == IF_TYPE_PPP || type == IF_TYPE_TUNNEL) {
        return true;
    }
    const std::wstring text = Lower(description);
    return Contains(text, L"vpn") || Contains(text, L"wireguard") || Contains(text, L"wintun") ||
        Contains(text, L"tap-windows") || Contains(text, L"nordlynx") || Contains(text, L"mullvad") ||
        Contains(text, L"openvpn");
}

// GDI advance width of a single line in the given flyout font. Used to pick
// the longest Bluetooth tile status that fits before falling back to an
// ellipsized short form. Cheap (no layout object); only the live overlay calls it.
LONG MeasureFlyoutLine(HFONT font, const std::wstring& text) {
    if (font == nullptr || text.empty()) {
        return 0;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr) {
        return LONG_MAX;
    }
    HGDIOBJ previous = SelectObject(dc, font);
    SIZE size{};
    const BOOL ok = GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, previous);
    DeleteDC(dc);
    return ok != FALSE ? size.cx : LONG_MAX;
}

// Character-granular end ellipsis that always shows the "…" glyph. The
// DirectWrite path trims at word granularity, which can leave a long single
// token (device names like "WH-1000XM5") clipped with no ellipsis at all.
std::wstring FitFlyoutLine(HFONT font, const std::wstring& text, LONG width) {
    if (font == nullptr || text.empty() || width <= 0) {
        return text;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr) {
        return text;
    }
    HGDIOBJ previous = SelectObject(dc, font);
    auto measure = [&](const std::wstring& value) -> LONG {
        SIZE size{};
        return GetTextExtentPoint32W(dc, value.c_str(), static_cast<int>(value.size()), &size) != FALSE
            ? size.cx
            : LONG_MAX;
    };
    std::wstring result = text;
    if (measure(text) > width) {
        const std::wstring ellipsis = L"\u2026";
        auto head = [&](size_t count) {
            if (count > 0 && count < text.size() && IS_HIGH_SURROGATE(text[count - 1])) {
                --count;
            }
            std::wstring value = text.substr(0, count);
            while (!value.empty() && (value.back() == L' ' || value.back() == L',' || value.back() == L'-' ||
                                         value.back() == L'\u00B7')) {
                value.pop_back();
            }
            return value;
        };
        size_t low = 0;
        size_t high = text.size();
        while (low < high) {
            const size_t mid = (low + high + 1) / 2;
            if (measure(head(mid) + ellipsis) <= width) {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        result = head(low) + ellipsis;
    }
    SelectObject(dc, previous);
    DeleteDC(dc);
    return result;
}

// Home tile status candidates, longest first. The overlay draws the first
// one that fits the status band; the last one is drawn with DT_END_ELLIPSIS.
std::vector<std::wstring> BluetoothTileStatusCandidates(const BluetoothSnapshot& bt) {
    if (!bt.ready) {
        return {L"\u2026"};
    }
    if (!bt.radioPresent) {
        return {L"Unavailable"};
    }
    if (!bt.radioOn) {
        return {L"Off"};
    }
    // Home tile shows the radio state only; the drill-in names connected devices.
    return {L"On"};
}

// "Connected to <device> +N" for the drill-in summary, or empty when none is.
std::wstring BluetoothConnectedSummary(const BluetoothSnapshot& bt) {
    const BluetoothDeviceInfo* first = nullptr;
    int connected = 0;
    for (const BluetoothDeviceInfo& device : bt.paired) {
        if (device.connected) {
            if (first == nullptr) {
                first = &device;
            }
            ++connected;
        }
    }
    if (first == nullptr) {
        return {};
    }
    const std::wstring name = first->name.empty() ? L"device" : first->name;
    const std::wstring more = connected > 1 ? (L" +" + std::to_wstring(connected - 1)) : L"";
    return L"Connected to " + name + more;
}

// Default flow: CPU (2x1) and Media (2x2) share rows 5-6 with GPU (2x1)
// filling the slot under CPU, so the grid still tiles 7 full rows.
constexpr int kQsModuleW[kQsModuleCount] = {2, 2, 2, 2, 2, 2, 4, 4, 2, 2, 2};
constexpr int kQsModuleH[kQsModuleCount] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1};
constexpr const wchar_t* kQsModuleNames[kQsModuleCount] = {L"wifi", L"bluetooth", L"boost", L"power", L"tray",
    L"mic", L"sound", L"display", L"temps", L"media", L"gpu"};
constexpr int kQsGridCols = 4;
// The default order tiles 28 cells into 7 full rows; reorders must too.
constexpr int kQsGridRows = 7;

// One Home grid cell span in panel px: 63pt cells on a 76pt pitch.
RECT QsGridCellRect(LONG left, LONG top, float scale, int col, int row, int w, int h) noexcept {
    const float x0 = static_cast<float>(col) * 76.0F;
    const float x1 = x0 + static_cast<float>(w) * 76.0F - 13.0F;
    const float y0 = static_cast<float>(row) * 76.0F;
    const float y1 = y0 + static_cast<float>(h) * 76.0F - 13.0F;
    return RECT{left + static_cast<LONG>(std::lround(x0 * scale)), top + static_cast<LONG>(std::lround(y0 * scale)),
        left + static_cast<LONG>(std::lround(x1 * scale)), top + static_cast<LONG>(std::lround(y1 * scale))};
}

// Corner radius each module's glass shape is baked with (see LayoutQuickSettings).
float QsModuleRadius(size_t module, const RECT& bounds, float scale) noexcept {
    if (kQsModuleW[module] == 1) {
        return 0.5F * static_cast<float>(bounds.right - bounds.left);
    }
    if (kQsModuleH[module] == 1) {
        return 0.5F * static_cast<float>(bounds.bottom - bounds.top);
    }
    return static_cast<float>(std::lround(30.0F * scale));
}

// One damped-spring step, parameterised like SwiftUI/UIKit springs: response
// is the undamped period in seconds, damping the damping fraction.
void StepSpring(float& pos, float& vel, float target, float response, float damping, float dt) noexcept {
    const float omega = 2.0F * 3.14159265F / std::max(response, 0.01F);
    constexpr float kStep = 1.0F / 240.0F;
    for (float remaining = dt; remaining > 0.0F; remaining -= kStep) {
        const float h = std::min(kStep, remaining);
        vel += (omega * omega * (target - pos) - 2.0F * damping * omega * vel) * h;
        pos += vel * h;
    }
}

// Three box passes ~ Gaussian blur of a single-channel plane, in place.
void BlurAlphaPlane(std::vector<uint8_t>& plane, LONG width, LONG height, int radius) {
    if (radius <= 0 || width <= 0 || height <= 0) {
        return;
    }
    std::vector<uint8_t> scratch(plane.size());
    auto pass = [&](const uint8_t* src, uint8_t* dst, LONG count, LONG stride, LONG lines, LONG lineStride) {
        const int window = radius * 2 + 1;
        for (LONG line = 0; line < lines; ++line) {
            const uint8_t* in = src + static_cast<size_t>(line) * static_cast<size_t>(lineStride);
            uint8_t* out = dst + static_cast<size_t>(line) * static_cast<size_t>(lineStride);
            int sum = 0;
            for (int k = -radius; k <= radius; ++k) {
                const LONG at = std::clamp(static_cast<LONG>(k), 0L, count - 1);
                sum += in[static_cast<size_t>(at) * static_cast<size_t>(stride)];
            }
            for (LONG i = 0; i < count; ++i) {
                out[static_cast<size_t>(i) * static_cast<size_t>(stride)] = static_cast<uint8_t>(sum / window);
                const LONG drop = std::clamp(i - radius, 0L, count - 1);
                const LONG add = std::clamp(i + radius + 1, 0L, count - 1);
                sum += in[static_cast<size_t>(add) * static_cast<size_t>(stride)] -
                    in[static_cast<size_t>(drop) * static_cast<size_t>(stride)];
            }
        }
    };
    for (int iteration = 0; iteration < 3; ++iteration) {
        pass(plane.data(), scratch.data(), width, 1, height, width);
        pass(scratch.data(), plane.data(), height, width, width, 1);
    }
}

// Bilinear tap of a single- or four-channel plane; zero outside.
inline float SamplePlane(const uint8_t* plane, LONG width, LONG height, size_t channels, size_t channel, float u,
    float v) noexcept {
    const float fx = std::floor(u);
    const float fy = std::floor(v);
    const LONG x0 = static_cast<LONG>(fx);
    const LONG y0 = static_cast<LONG>(fy);
    const float tx = u - fx;
    const float ty = v - fy;
    auto at = [&](LONG x, LONG y) -> float {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return 0.0F;
        }
        return static_cast<float>(
            plane[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * channels + channel]);
    };
    const float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
    const float bottom = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
    return top + (bottom - top) * ty;
}

// Boost report app name without the ".exe" suffix, with a "(N)" process count.
std::wstring BoostAppLabel(const BoostReportItem& item) {
    std::wstring name = item.name;
    if (name.size() > 4U) {
        std::wstring tail = name.substr(name.size() - 4U);
        for (wchar_t& ch : tail) {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
        if (tail == L".exe") {
            name.resize(name.size() - 4U);
        }
    }
    if (item.count > 1) {
        name += L" (" + std::to_wstring(item.count) + L")";
    }
    return name;
}

std::wstring BoostAgeText(ULONGLONG elapsedMs) {
    const ULONGLONG minutes = elapsedMs / 60000ULL;
    if (minutes < 1ULL) {
        return L"Last run just now";
    }
    if (minutes < 60ULL) {
        return L"Last run " + std::to_wstring(minutes) + L" min ago";
    }
    if (minutes < 1440ULL) {
        return L"Last run " + std::to_wstring(minutes / 60ULL) + L" h ago";
    }
    return L"Last run " + std::to_wstring(minutes / 1440ULL) + L" d ago";
}

std::wstring BoostDurationText(uint64_t ms) {
    if (ms >= 60000ULL) {
        return std::to_wstring(ms / 60000ULL) + L" min " + std::to_wstring((ms % 60000ULL) / 1000ULL) + L" s";
    }
    wchar_t buffer[24]{};
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%.1f s", static_cast<double>(ms) / 1000.0);
    return buffer;
}

// One line for the drill-in header summary.
std::wstring BluetoothPageSummary(const BluetoothSnapshot& bt) {
    if (!bt.hint.empty()) {
        return bt.hint;
    }
    if (!bt.ready) {
        return L"Checking Bluetooth\u2026";
    }
    if (!bt.radioPresent) {
        return L"No Bluetooth radio found";
    }
    if (!bt.radioOn) {
        return L"Off";
    }
    const std::wstring connected = BluetoothConnectedSummary(bt);
    return connected.empty() ? L"On \u00B7 not connected" : connected;
}

// Device-type glyph for Bluetooth rows, guessed from the friendly name since
// the snapshot carries no class-of-device.
wchar_t BluetoothDeviceGlyph(const std::wstring& name) {
    std::wstring lower = name;
    for (wchar_t& ch : lower) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    auto has = [&](const wchar_t* needle) { return lower.find(needle) != std::wstring::npos; };
    if (has(L"mouse") || has(L"trackpad") || has(L"touchpad")) {
        return L'\uE962';
    }
    if (has(L"keyboard")) {
        return L'\uE765';
    }
    if (has(L"airpods") || has(L"buds") || has(L"headphone") || has(L"headset") || has(L"earphone") ||
        has(L"wh-") || has(L"wf-")) {
        return L'\uE7F6';
    }
    if (has(L"speaker") || has(L"soundbar") || has(L"boom")) {
        return L'\uE7F5';
    }
    if (has(L"controller") || has(L"gamepad") || has(L"xbox") || has(L"dualsense") || has(L"dualshock")) {
        return L'\uE7FC';
    }
    if (has(L"phone") || has(L"iphone") || has(L"pixel") || has(L"galaxy")) {
        return L'\uE8EA';
    }
    return L'\uE702';
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

bool SetWifiRadio(bool enabled);

// WlanSetInterface can block for tens of ms; run it on a worker so the UI
// thread (which also services the low-level mouse hook) never waits on it.
// Rapid toggles collapse to the latest requested state, applied in order.
void SetWifiRadioAsync(bool enabled, HWND notifyHwnd, UINT notifyMsg, QuickSettingsPage page) {
    static std::atomic<int> desired{-1};
    static std::atomic<bool> busy{false};
    desired.store(enabled ? 1 : 0);
    if (busy.exchange(true)) {
        return;
    }
    std::thread([notifyHwnd, notifyMsg, page] {
        for (;;) {
            const int want = desired.exchange(-1);
            if (want < 0) {
                busy.store(false);
                // A request that raced the store above restarts the loop.
                if (desired.load() >= 0 && !busy.exchange(true)) {
                    continue;
                }
                break;
            }
            static_cast<void>(SetWifiRadio(want == 1));
        }
        // Real radio state + network list replace the optimistic flip.
        RequestQuickSettingsCacheAsync(notifyHwnd, notifyMsg, page);
    }).detach();
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
    // VPN list only. The Ethernet tile/page were replaced by Bluetooth, so the
    // wired-adapter summary (speed / IPv4 / adapter) is no longer collected.
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
    // Kernel + user time (100 ns) and read + write bytes since process start.
    unsigned long long cpu = 0;
    unsigned long long io = 0;
    // Display name; also the per-app grouping key (Chrome's many processes).
    std::wstring name;
    std::wstring path;
};

struct EnergyBaseline {
    ULONGLONG tick = 0;
    std::vector<EnergyCpuSample> samples;
};

EnergyBaseline g_energyBaseline;

// Score is roughly "% of the machine": CPU share + 0.4 x busiest GPU engine +
// 0.6 x disk MB/s (capped). Bands follow Task Manager's Power usage column:
// Very low < 1.5, Low < 4, Moderate < 9, High < 20, Very high >= 20.
constexpr double kEnergyHighScore = 9.0;
constexpr double kEnergyVeryHighScore = 20.0;
// Hysteresis: a listed app stays until it has been below these for the hold.
constexpr double kEnergyLeaveScore = 6.0;
constexpr double kEnergyVeryHighLeaveScore = 16.0;
constexpr ULONGLONG kEnergyHoldMs = 6000;
constexpr size_t kEnergyMaxRows = 5;

// Per-app smoothed score and list membership, kept across samples so the
// Power page list changes only when an app clearly starts or stops.
struct EnergyTrack {
    double score = 0.0;
    bool listed = false;
    bool veryHigh = false;
    ULONGLONG listedAt = 0;
    ULONGLONG belowSince = 0;
    ULONGLONG seenAt = 0;
    std::wstring path;
};

std::unordered_map<std::wstring, EnergyTrack> g_energyTracks;

// "\GPU Engine(*)\Utilization Percentage", the counters Task Manager's GPU
// column reads. Only the energy worker touches it (one sample in flight).
struct GpuEngineCounters {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    bool failed = false;
    bool primed = false;
};

GpuEngineCounters g_gpuEngines;

// Busiest GPU engine (%) per pid since the previous call. A rate counter: the
// first collect only primes it and reports nothing.
void SampleGpuEngines(std::unordered_map<DWORD, double>& busiest) {
    GpuEngineCounters& gpu = g_gpuEngines;
    if (gpu.failed) {
        return;
    }
    if (gpu.query == nullptr) {
        if (PdhOpenQueryW(nullptr, 0, &gpu.query) != ERROR_SUCCESS) {
            gpu.query = nullptr;
            gpu.failed = true;
            return;
        }
        if (PdhAddEnglishCounterW(gpu.query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpu.counter) !=
            ERROR_SUCCESS) {
            PdhCloseQuery(gpu.query);
            gpu.query = nullptr;
            gpu.failed = true;
            return;
        }
    }
    if (PdhCollectQueryData(gpu.query) != ERROR_SUCCESS) {
        return;
    }
    if (!gpu.primed) {
        gpu.primed = true;
        return;
    }
    DWORD bytes = 0;
    DWORD count = 0;
    if (PdhGetFormattedCounterArrayW(gpu.counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr) !=
            static_cast<PDH_STATUS>(PDH_MORE_DATA) ||
        bytes == 0) {
        return;
    }
    std::vector<uint8_t> buffer(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(gpu.counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items) !=
        ERROR_SUCCESS) {
        return;
    }
    for (DWORD index = 0; index < count; ++index) {
        const PDH_FMT_COUNTERVALUE& value = items[index].FmtValue;
        if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) {
            continue;
        }
        // Instances look like "pid_1234_luid_0x..._phys_0_eng_0_engtype_3D".
        const wchar_t* tag = items[index].szName != nullptr ? std::wcsstr(items[index].szName, L"pid_") : nullptr;
        if (tag == nullptr) {
            continue;
        }
        const DWORD pid = static_cast<DWORD>(std::wcstoul(tag + 4, nullptr, 10));
        double& peak = busiest[pid];
        peak = (std::max)(peak, value.doubleValue);
    }
}

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

// Two samples ~2 s apart (the first open seeds a baseline and re-samples
// after ~0.7 s). Per app: CPU share, busiest GPU engine and disk I/O give a
// Task Manager style power score; apps above Moderate are listed, smoothed
// and held so rows neither flicker nor reorder. Snapshot failure is an
// explicit note, not a fake app list.
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
                IO_COUNTERS io{};
                if (GetProcessIoCounters(process, &io)) {
                    sample.io = io.ReadTransferCount + io.WriteTransferCount;
                }
                wchar_t imagePath[MAX_PATH]{};
                DWORD imageChars = MAX_PATH;
                if (QueryFullProcessImageNameW(process, 0, imagePath, &imageChars)) {
                    sample.path.assign(imagePath, imageChars);
                }
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
        // must NOT reset the tick - that kept the Power page stuck on
        // Measuring forever whenever ToolHelp finished under 250 ms.
        if (!haveBaseline || tooStale) {
            g_energyBaseline.tick = now;
            g_energyBaseline.samples = std::move(current);
            g_energyTracks.clear();
            std::unordered_map<DWORD, double> discard;
            SampleGpuEngines(discard);
        }
        cache.energyPending = true;
        cache.energyNote = L"Measuring energy use\u2026";
        return;
    }

    std::unordered_map<DWORD, double> gpuByPid;
    SampleGpuEngines(gpuByPid);
    std::unordered_map<DWORD, const EnergyCpuSample*> previous;
    previous.reserve(g_energyBaseline.samples.size());
    for (const EnergyCpuSample& sample : g_energyBaseline.samples) {
        previous[sample.pid] = &sample;
    }
    const DWORD cores = (std::max)(static_cast<DWORD>(1), GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    const double wall = static_cast<double>(elapsed) * 10000.0 * static_cast<double>(cores);
    const double seconds = static_cast<double>(elapsed) / 1000.0;
    struct AppLoad {
        double cpu = 0.0;
        double gpu = 0.0;
        double ioBytes = 0.0;
        std::wstring path;
    };
    std::unordered_map<std::wstring, AppLoad> apps;
    for (const EnergyCpuSample& sample : current) {
        AppLoad& load = apps[sample.name];
        if (load.path.empty()) {
            load.path = sample.path;
        }
        const auto found = previous.find(sample.pid);
        if (found != previous.end()) {
            const EnergyCpuSample& before = *found->second;
            if (sample.cpu >= before.cpu && wall > 0.0) {
                load.cpu += static_cast<double>(sample.cpu - before.cpu) / wall * 100.0;
            }
            if (sample.io >= before.io) {
                load.ioBytes += static_cast<double>(sample.io - before.io);
            }
        }
        const auto gpu = gpuByPid.find(sample.pid);
        if (gpu != gpuByPid.end()) {
            load.gpu += gpu->second;
        }
    }
    for (const auto& [name, load] : apps) {
        const double diskMBps = load.ioBytes / (std::max)(0.25, seconds) / 1048576.0;
        const double raw = load.cpu + 0.4 * (std::min)(load.gpu, 100.0) + (std::min)(15.0, 0.6 * diskMBps);
        EnergyTrack& track = g_energyTracks[name];
        // ~4 s time constant at the 2 s cadence; a new app starts at its value.
        track.score = track.seenAt == 0 ? raw : 0.5 * track.score + 0.5 * raw;
        track.seenAt = now;
        if (!load.path.empty()) {
            track.path = load.path;
        }
    }
    std::vector<std::pair<const std::wstring*, const EnergyTrack*>> listed;
    for (auto it = g_energyTracks.begin(); it != g_energyTracks.end();) {
        EnergyTrack& track = it->second;
        if (track.seenAt != now) {
            // Exited: decay so a listed app still leaves through the hold.
            track.score *= 0.5;
            if (!track.listed) {
                it = g_energyTracks.erase(it);
                continue;
            }
        }
        if (!track.listed) {
            if (track.score >= kEnergyHighScore) {
                track.listed = true;
                track.listedAt = now;
                track.belowSince = 0;
                track.veryHigh = track.score >= kEnergyVeryHighScore;
            }
        } else {
            if (track.score >= kEnergyLeaveScore) {
                track.belowSince = 0;
            } else if (track.belowSince == 0) {
                track.belowSince = now;
            } else if (now - track.belowSince >= kEnergyHoldMs && now - track.listedAt >= kEnergyHoldMs) {
                track.listed = false;
            }
            track.veryHigh = track.veryHigh ? track.score >= kEnergyVeryHighLeaveScore
                                            : track.score >= kEnergyVeryHighScore;
        }
        if (track.listed) {
            listed.emplace_back(&it->first, &track);
        } else if (track.seenAt != now) {
            it = g_energyTracks.erase(it);
            continue;
        }
        ++it;
    }
    // Stable order: first listed stays on top, like the macOS menu.
    std::sort(listed.begin(), listed.end(), [](const auto& left, const auto& right) {
        if (left.second->listedAt != right.second->listedAt) {
            return left.second->listedAt < right.second->listedAt;
        }
        return *left.first < *right.first;
    });
    for (const auto& [name, track] : listed) {
        if (cache.energyApps.size() >= kEnergyMaxRows) {
            break;
        }
        QsEnergyApp app;
        app.name = *name;
        app.path = track->path;
        app.level = track->veryHigh ? QsPowerLevel::VeryHigh : QsPowerLevel::High;
        cache.energyApps.push_back(std::move(app));
    }
    g_energyBaseline.tick = now;
    g_energyBaseline.samples = std::move(current);
    cache.energyLive = true;
    if (cache.energyApps.empty()) {
        cache.energyNote = L"No Apps Using Significant Energy";
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

constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

void QueryDarkMode(QuickSettingsCache& cache) {
    DWORD light = 1;
    DWORD size = sizeof(light);
    cache.darkKnown = RegGetValueW(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", RRF_RT_REG_DWORD,
                          nullptr, &light, &size) == ERROR_SUCCESS;
    cache.darkMode = cache.darkKnown && light == 0;
}

// Apps and system (taskbar/Start) follow together, like the Settings toggle.
// The WM_SETTINGCHANGE broadcast can stall on hung windows: workers only.
bool SetDarkMode(bool dark) {
    const DWORD light = dark ? 0U : 1U;
    const LSTATUS apps = RegSetKeyValueW(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", REG_DWORD,
        &light, sizeof(light));
    const LSTATUS system = RegSetKeyValueW(HKEY_CURRENT_USER, kPersonalizeKey, L"SystemUsesLightTheme",
        REG_DWORD, &light, sizeof(light));
    if (apps != ERROR_SUCCESS && system != ERROR_SUCCESS) {
        return false;
    }
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"),
        SMTO_ABORTIFHUNG, 1000, &ignored);
    return true;
}

// Friendly name ("DELL U2723QE") of the monitor behind a GDI device name.
std::wstring MonitorFriendlyName(const std::wstring& device) {
    UINT pathCount = 0;
    UINT modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS ||
        pathCount == 0) {
        return {};
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(),
            nullptr) != ERROR_SUCCESS) {
        return {};
    }
    for (UINT index = 0; index < pathCount; ++index) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[index].sourceInfo.adapterId;
        source.header.id = paths[index].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            _wcsicmp(source.viewGdiDeviceName, device.c_str()) != 0) {
            continue;
        }
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = paths[index].targetInfo.adapterId;
        target.header.id = paths[index].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
            return target.monitorFriendlyDeviceName;
        }
    }
    return {};
}

// Rates for the monitor `anchor` sits on (the dock's / Quick Settings'),
// limited to modes matching the current resolution, depth and scan type.
void QueryRefreshRate(QuickSettingsCache& cache, HWND anchor) {
    cache.refreshHz = 0;
    cache.refreshRates.clear();
    cache.refreshDevice.clear();
    cache.refreshMonitor.clear();
    cache.refreshWidth = 0;
    cache.refreshHeight = 0;
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    const HMONITOR handle = anchor != nullptr ? MonitorFromWindow(anchor, MONITOR_DEFAULTTOPRIMARY)
                                              : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (handle == nullptr || GetMonitorInfoW(handle, &monitor) == FALSE) {
        return;
    }
    const std::wstring device = monitor.szDevice;
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &current) == FALSE) {
        return;
    }
    cache.refreshDevice = device;
    cache.refreshMonitor = MonitorFriendlyName(device);
    cache.refreshWidth = static_cast<int>(current.dmPelsWidth);
    cache.refreshHeight = static_cast<int>(current.dmPelsHeight);
    // 0 and 1 mean "hardware default", not a real rate.
    if (current.dmDisplayFrequency > 1U) {
        cache.refreshHz = static_cast<int>(current.dmDisplayFrequency);
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD index = 0; EnumDisplaySettingsW(device.c_str(), index, &mode) != FALSE; ++index) {
        if (mode.dmPelsWidth != current.dmPelsWidth || mode.dmPelsHeight != current.dmPelsHeight ||
            mode.dmBitsPerPel != current.dmBitsPerPel ||
            (mode.dmDisplayFlags & DM_INTERLACED) != (current.dmDisplayFlags & DM_INTERLACED) ||
            mode.dmDisplayFrequency <= 1U) {
            continue;
        }
        const int hz = static_cast<int>(mode.dmDisplayFrequency);
        if (std::find(cache.refreshRates.begin(), cache.refreshRates.end(), hz) == cache.refreshRates.end()) {
            cache.refreshRates.push_back(hz);
        }
    }
    if (cache.refreshHz > 0 &&
        std::find(cache.refreshRates.begin(), cache.refreshRates.end(), cache.refreshHz) == cache.refreshRates.end()) {
        cache.refreshRates.push_back(cache.refreshHz);
    }
    // Fastest first, like the macOS display menu.
    std::sort(cache.refreshRates.rbegin(), cache.refreshRates.rend());
}

// Switch `device` to `hz` at its current resolution. The mode is validated
// with CDS_TEST first so an unsupported rate never blanks the screen. Blocks
// while the display retrains: workers only.
bool SetRefreshRate(const std::wstring& device, int hz) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (device.empty() || hz <= 1 || EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &mode) == FALSE) {
        return false;
    }
    mode.dmDisplayFrequency = static_cast<DWORD>(hz);
    mode.dmFields = DM_DISPLAYFREQUENCY;
    if (ChangeDisplaySettingsExW(device.c_str(), &mode, nullptr, CDS_TEST, nullptr) != DISP_CHANGE_SUCCESSFUL) {
        return false;
    }
    return ChangeDisplaySettingsExW(device.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY, nullptr) ==
        DISP_CHANGE_SUCCESSFUL;
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

// GSMTC runs on one MTA worker that owns every WinRT object. Session, playback,
// metadata and timeline events wake it; it publishes a plain snapshot that the
// UI thread copies without any cross-process call. Positions are anchored to a
// GetTickCount64 stamp and extrapolated on the UI side, because Spotify and
// browsers only report the timeline on play/pause/seek.
constexpr unsigned kMediaDirtySessions = 1U;
constexpr unsigned kMediaDirtyPlayback = 2U;
constexpr unsigned kMediaDirtyProps = 4U;
constexpr unsigned kMediaDirtyTimeline = 8U;
constexpr unsigned kMediaDirtyAll = 15U;
constexpr int64_t kMediaTicksPerMs = 10000;
constexpr int64_t kMediaTicksPerSecond = 10000000;
constexpr ULONGLONG kMediaPollMs = 1000;
constexpr ULONGLONG kMediaPropsPollMs = 4000;
constexpr ULONGLONG kMediaDemandMs = 3000;
constexpr ULONGLONG kMediaHoldMs = 2500;
constexpr int kMediaArtPx = 192;

enum class MediaCommand : uint8_t {
    Previous = 0,
    TogglePlay = 1,
    Next = 2,
    Seek = 3,
};

struct MediaRequest {
    MediaCommand command = MediaCommand::TogglePlay;
    int64_t position = 0;
};

struct MediaState {
    bool have = false;
    bool playing = false;
    bool canPrev = false;
    bool canNext = false;
    bool canPlayPause = false;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring app;
    // Position (100 ns ticks from the timeline start) as of anchorMs.
    int64_t positionTicks = 0;
    int64_t durationTicks = 0;
    ULONGLONG anchorMs = 0;
    double rate = 1.0;
    uint64_t artGen = 0;
    std::shared_ptr<const std::vector<uint8_t>> art;
};

struct MediaBridge {
    std::mutex mutex;
    std::condition_variable wake;
    bool started = false;
    // RequestAsync failed (no GSMTC): transport falls back to media keys.
    bool unavailable = false;
    bool sleeping = false;
    unsigned dirty = 0;
    std::vector<MediaRequest> requests;
    ULONGLONG demandMs = 0;
    uint64_t gen = 1;
    MediaState state;
    // Optimistic UI: a click shows its result at once and the worker's
    // snapshots cannot revert it until the app confirms or the hold expires.
    ULONGLONG playHoldUntil = 0;
    bool playHoldValue = false;
    ULONGLONG positionHoldUntil = 0;
    bool positionHoldSkip = false;
};

// Leaked on purpose: the detached worker may still touch it during exit.
MediaBridge& Media() {
    static MediaBridge* bridge = new MediaBridge();
    return *bridge;
}

std::wstring WideFromHstring(const winrt::hstring& value) {
    return std::wstring(value.c_str());
}

int64_t MediaPositionAt(const MediaState& state, ULONGLONG nowMs) noexcept {
    int64_t position = state.positionTicks;
    if (state.playing && nowMs > state.anchorMs) {
        position += static_cast<int64_t>(
            static_cast<double>(nowMs - state.anchorMs) * static_cast<double>(kMediaTicksPerMs) * state.rate);
    }
    if (state.durationTicks > 0) {
        position = std::min(position, state.durationTicks);
    }
    return std::max<int64_t>(position, 0);
}

void WakeMedia(unsigned bits) {
    MediaBridge& media = Media();
    {
        std::lock_guard<std::mutex> lock(media.mutex);
        media.dirty |= bits;
    }
    media.wake.notify_one();
}

// Bounded, null-safe wait for a WinRT async call. A player that stalls
// mid-toggle must not wedge the worker (and every later click) forever.
template <typename Async>
auto MediaAwait(const Async& operation, uint32_t limitMs) -> decltype(operation.GetResults()) {
    using Result = decltype(operation.GetResults());
    bool completed = false;
    if (operation) {
        completed = operation.wait_for(std::chrono::milliseconds(limitMs)) ==
            winrt::Windows::Foundation::AsyncStatus::Completed;
        if (!completed) {
            try {
                operation.Cancel();
            } catch (...) {
            }
        }
    }
    if (!completed) {
        if constexpr (std::is_same_v<Result, bool>) {
            return false;
        } else {
            return Result{nullptr};
        }
    }
    return operation.GetResults();
}

// Short app label from the session AUMID ("Spotify.exe", "MSEdge",
// "308046B0AF4A39CB" for Firefox, "Publisher.App_hash!App" for packaged apps).
std::wstring MediaAppName(const std::wstring& aumid) {
    if (aumid.empty()) {
        return {};
    }
    const std::wstring lower = Lower(aumid);
    struct Known {
        const wchar_t* needle;
        const wchar_t* name;
    };
    static constexpr Known kKnown[] = {
        {L"spotify", L"Spotify"},
        {L"msedge", L"Edge"},
        {L"chrome", L"Chrome"},
        {L"firefox", L"Firefox"},
        {L"308046b0af4a39cb", L"Firefox"},
        {L"brave", L"Brave"},
        {L"opera", L"Opera"},
        {L"vivaldi", L"Vivaldi"},
        {L"thebrowsercompany.arc", L"Arc"},
        {L"zunemusic", L"Media Player"},
        {L"zunevideo", L"Films & TV"},
        {L"applemusic", L"Apple Music"},
        {L"itunes", L"iTunes"},
        {L"amazonmusic", L"Amazon Music"},
        {L"tidal", L"TIDAL"},
        {L"deezer", L"Deezer"},
        {L"vlc", L"VLC"},
        {L"foobar2000", L"foobar2000"},
        {L"musicbee", L"MusicBee"},
        {L"aimp", L"AIMP"},
        {L"discord", L"Discord"},
    };
    for (const Known& known : kKnown) {
        if (lower.find(known.needle) != std::wstring::npos) {
            return known.name;
        }
    }
    std::wstring name = aumid;
    const size_t bang = name.find(L'!');
    if (bang != std::wstring::npos) {
        name.resize(bang);
    }
    const size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        name.erase(0, slash + 1);
    }
    if (name.size() > 4 && Lower(name.substr(name.size() - 4)) == L".exe") {
        name.resize(name.size() - 4);
    }
    const size_t family = name.find(L'_');
    if (family != std::wstring::npos && family > 0) {
        name.resize(family);
    }
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot + 1 < name.size()) {
        name.erase(0, dot + 1);
    }
    if (!name.empty()) {
        name[0] = static_cast<wchar_t>(std::towupper(name[0]));
    }
    return name;
}

std::wstring MediaAumid(const MediaSession& session) {
    try {
        return WideFromHstring(session.SourceAppUserModelId());
    } catch (...) {
        return {};
    }
}

// GSMTC getters can return null objects while a player tears its session
// down (Spotify / Chromium on rapid play-pause). Calling through a null
// projection is an access violation that catch (...) cannot stop, so every
// WinRT result below is null-checked before use.
MediaStatus MediaStatusOf(const MediaSession& session) {
    try {
        if (!session) {
            return MediaStatus::Closed;
        }
        const auto info = session.GetPlaybackInfo();
        return info ? info.PlaybackStatus() : MediaStatus::Closed;
    } catch (...) {
        return MediaStatus::Closed;
    }
}

// Playing beats paused; the last shown app is sticky so pausing Spotify does
// not flip the module to an idle browser tab (and back on the next poll).
MediaSession PickMediaSession(const MediaManager& manager, const std::wstring& sticky) {
    MediaSession current{nullptr};
    try {
        current = manager.GetCurrentSession();
    } catch (...) {
        current = nullptr;
    }
    const std::wstring currentId = current ? MediaAumid(current) : std::wstring{};
    if (current && MediaStatusOf(current) == MediaStatus::Playing &&
        (sticky.empty() || currentId == sticky)) {
        return current;
    }
    MediaSession stickyAny{nullptr};
    MediaSession stickyPlaying{nullptr};
    MediaSession playing{nullptr};
    MediaSession paused{nullptr};
    MediaSession first{nullptr};
    try {
        const auto sessions = manager.GetSessions();
        if (!sessions) {
            return current;
        }
        for (const MediaSession& session : sessions) {
            if (!session) {
                continue;
            }
            const MediaStatus status = MediaStatusOf(session);
            if (status == MediaStatus::Closed) {
                continue;
            }
            const bool isSticky = !sticky.empty() && MediaAumid(session) == sticky;
            if (!first) {
                first = session;
            }
            if (status == MediaStatus::Playing) {
                if (isSticky && !stickyPlaying) {
                    stickyPlaying = session;
                }
                if (!playing) {
                    playing = session;
                }
            } else if (status == MediaStatus::Paused && !paused) {
                paused = session;
            }
            if (isSticky && !stickyAny) {
                stickyAny = session;
            }
        }
    } catch (...) {
    }
    if (stickyPlaying) {
        return stickyPlaying;
    }
    if (current && MediaStatusOf(current) == MediaStatus::Playing) {
        return current;
    }
    if (playing) {
        return playing;
    }
    if (stickyAny) {
        return stickyAny;
    }
    if (current) {
        return current;
    }
    return paused ? paused : first;
}

struct MediaHook {
    MediaSession session{nullptr};
    winrt::event_token playback{};
    winrt::event_token props{};
    winrt::event_token timeline{};
};

void UnhookMediaSessions(std::vector<MediaHook>& hooks) {
    for (MediaHook& hook : hooks) {
        try {
            if (hook.playback.value != 0) {
                hook.session.PlaybackInfoChanged(hook.playback);
            }
            if (hook.props.value != 0) {
                hook.session.MediaPropertiesChanged(hook.props);
            }
            if (hook.timeline.value != 0) {
                hook.session.TimelinePropertiesChanged(hook.timeline);
            }
        } catch (...) {
        }
    }
    hooks.clear();
}

void HookMediaSessions(const MediaManager& manager, std::vector<MediaHook>& hooks) {
    UnhookMediaSessions(hooks);
    try {
        const auto sessions = manager.GetSessions();
        if (!sessions) {
            return;
        }
        for (const MediaSession& session : sessions) {
            if (!session) {
                continue;
            }
            MediaHook hook;
            hook.session = session;
            try {
                hook.playback = session.PlaybackInfoChanged(
                    [](auto&&, auto&&) { WakeMedia(kMediaDirtyPlayback); });
                hook.props = session.MediaPropertiesChanged(
                    [](auto&&, auto&&) { WakeMedia(kMediaDirtyProps); });
                hook.timeline = session.TimelinePropertiesChanged(
                    [](auto&&, auto&&) { WakeMedia(kMediaDirtyTimeline); });
            } catch (...) {
            }
            hooks.push_back(std::move(hook));
        }
    } catch (...) {
    }
}

// Album art, centre-cropped to a square (YouTube frames are 16:9) and decoded
// to premultiplied BGRA at kMediaArtPx. The UI box-filters it to the module.
std::shared_ptr<const std::vector<uint8_t>> DecodeMediaArt(
    const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& reference) {
    namespace imaging = winrt::Windows::Graphics::Imaging;
    if (!reference) {
        return nullptr;
    }
    const auto stream = MediaAwait(reference.OpenReadAsync(), 3000);
    if (!stream) {
        return nullptr;
    }
    const imaging::BitmapDecoder decoder = MediaAwait(imaging::BitmapDecoder::CreateAsync(stream), 3000);
    if (!decoder) {
        return nullptr;
    }
    const uint32_t sourceW = decoder.OrientedPixelWidth();
    const uint32_t sourceH = decoder.OrientedPixelHeight();
    if (sourceW == 0 || sourceH == 0) {
        return nullptr;
    }
    constexpr uint32_t target = static_cast<uint32_t>(kMediaArtPx);
    const double fit = static_cast<double>(target) / static_cast<double>(std::min(sourceW, sourceH));
    const uint32_t scaledW = std::max(target, static_cast<uint32_t>(std::lround(sourceW * fit)));
    const uint32_t scaledH = std::max(target, static_cast<uint32_t>(std::lround(sourceH * fit)));
    imaging::BitmapTransform transform;
    transform.ScaledWidth(scaledW);
    transform.ScaledHeight(scaledH);
    transform.InterpolationMode(imaging::BitmapInterpolationMode::Fant);
    transform.Bounds(imaging::BitmapBounds{(scaledW - target) / 2U, (scaledH - target) / 2U, target, target});
    const imaging::PixelDataProvider data = MediaAwait(
        decoder.GetPixelDataAsync(imaging::BitmapPixelFormat::Bgra8, imaging::BitmapAlphaMode::Premultiplied,
            transform, imaging::ExifOrientationMode::RespectExifOrientation,
            imaging::ColorManagementMode::ColorManageToSRgb),
        3000);
    if (!data) {
        return nullptr;
    }
    const winrt::com_array<uint8_t> bytes = data.DetachPixelData();
    if (bytes.size() != static_cast<size_t>(target) * static_cast<size_t>(target) * 4U) {
        return nullptr;
    }
    return std::make_shared<std::vector<uint8_t>>(bytes.begin(), bytes.end());
}

// Worker-only counter; a new value tells the UI to re-raster the art.
uint64_t NextMediaArtGen() noexcept {
    static uint64_t counter = 0;
    return ++counter;
}

// Worker-side read of the chosen session.
struct MediaRead {
    MediaState state;
    int64_t startTicks = 0;
    bool canToggle = false;
    // Apps that move Position without LastUpdatedTime would be extrapolated
    // twice; detect that once per session and stop compensating.
    int64_t lastRawPosition = -1;
    int64_t lastStamp = -1;
    bool staleStamp = false;
    std::wstring artKey;
    bool artLoaded = false;
};

void ReadMediaPlayback(const MediaSession& session, MediaRead& read) {
    const auto info = session.GetPlaybackInfo();
    if (!info) {
        read.state.have = false;
        read.state.playing = false;
        return;
    }
    const MediaStatus status = info.PlaybackStatus();
    // Changing = track switch in progress; keep the last state so the glyph
    // does not flicker between tracks.
    read.state.playing = status == MediaStatus::Playing || (status == MediaStatus::Changing && read.state.playing);
    read.state.have = status != MediaStatus::Closed;
    const auto controls = info.Controls();
    if (controls) {
        read.state.canPrev = controls.IsPreviousEnabled();
        read.state.canNext = controls.IsNextEnabled();
        read.canToggle = controls.IsPlayPauseToggleEnabled();
        read.state.canPlayPause = read.canToggle || controls.IsPlayEnabled() || controls.IsPauseEnabled();
    }
    double rate = 1.0;
    try {
        const auto reported = info.PlaybackRate();
        if (reported) {
            rate = reported.Value();
        }
    } catch (...) {
    }
    read.state.rate = (rate > 0.0 && rate <= 16.0) ? rate : 1.0;
}

void ReadMediaTimeline(const MediaSession& session, MediaRead& read) {
    const auto timeline = session.GetTimelineProperties();
    if (!timeline) {
        return;
    }
    int64_t start = timeline.StartTime().count();
    int64_t end = timeline.EndTime().count();
    if (end <= start) {
        start = std::min(start, timeline.MinSeekTime().count());
        end = timeline.MaxSeekTime().count();
    }
    const int64_t raw = timeline.Position().count();
    const int64_t stamp = timeline.LastUpdatedTime().time_since_epoch().count();
    if (read.lastRawPosition >= 0 && raw != read.lastRawPosition && stamp == read.lastStamp) {
        read.staleStamp = true;
    }
    read.lastRawPosition = raw;
    read.lastStamp = stamp;
    int64_t position = raw - start;
    if (read.state.playing && !read.staleStamp && stamp > 0) {
        const int64_t age = (winrt::clock::now() - timeline.LastUpdatedTime()).count();
        if (age > 0 && age < 3600LL * kMediaTicksPerSecond) {
            position += static_cast<int64_t>(static_cast<double>(age) * read.state.rate);
        }
    }
    read.startTicks = start;
    read.state.durationTicks = std::max<int64_t>(0, end - start);
    if (read.state.durationTicks > 0) {
        position = std::min(position, read.state.durationTicks);
    }
    read.state.positionTicks = std::max<int64_t>(0, position);
    read.state.anchorMs = GetTickCount64();
}

void ReadMediaProps(const MediaSession& session, MediaRead& read, bool fromEvent) {
    const auto props = MediaAwait(session.TryGetMediaPropertiesAsync(), 2000);
    if (!props) {
        return;
    }
    std::wstring title = WideFromHstring(props.Title());
    std::wstring artist = WideFromHstring(props.Artist());
    std::wstring album = WideFromHstring(props.AlbumTitle());
    if (artist.empty()) {
        artist = WideFromHstring(props.AlbumArtist());
    }
    // Spotify and Chromium publish an empty set mid track change; keep the
    // previous labels until the real metadata lands.
    if (title.empty() && artist.empty()) {
        return;
    }
    std::wstring key = title + L'\x1F' + artist + L'\x1F' + album;
    const bool newTrack = key != read.artKey;
    read.state.title = std::move(title);
    read.state.artist = std::move(artist);
    read.state.album = std::move(album);
    // Browsers attach artwork in a second metadata update, so an event on the
    // same track re-reads it; polls only retry tracks that had none.
    if (!newTrack && read.artLoaded && !fromEvent) {
        return;
    }
    std::shared_ptr<const std::vector<uint8_t>> art;
    try {
        const auto thumbnail = props.Thumbnail();
        art = thumbnail ? DecodeMediaArt(thumbnail) : nullptr;
    } catch (...) {
        art = nullptr;
    }
    const bool same = art && read.state.art && *art == *read.state.art;
    if (!same && (newTrack || art)) {
        read.state.art = std::move(art);
        read.state.artGen = NextMediaArtGen();
    }
    read.artKey = std::move(key);
    read.artLoaded = read.state.art != nullptr;
}

void SendMediaKeyFor(MediaCommand command) {
    switch (command) {
    case MediaCommand::Previous:
        SendMediaKey(VK_MEDIA_PREV_TRACK);
        break;
    case MediaCommand::Next:
        SendMediaKey(VK_MEDIA_NEXT_TRACK);
        break;
    case MediaCommand::TogglePlay:
        SendMediaKey(VK_MEDIA_PLAY_PAUSE);
        break;
    case MediaCommand::Seek:
        break;
    }
}

void RunMediaRequests(const MediaSession& session, const MediaRead& read, const std::vector<MediaRequest>& requests) {
    for (const MediaRequest& request : requests) {
        if (!session) {
            SendMediaKeyFor(request.command);
            continue;
        }
        try {
            switch (request.command) {
            case MediaCommand::Previous:
                static_cast<void>(MediaAwait(session.TrySkipPreviousAsync(), 2000));
                break;
            case MediaCommand::Next:
                static_cast<void>(MediaAwait(session.TrySkipNextAsync(), 2000));
                break;
            case MediaCommand::TogglePlay:
                if (read.canToggle || !read.state.canPlayPause) {
                    static_cast<void>(MediaAwait(session.TryTogglePlayPauseAsync(), 2000));
                } else if (MediaStatusOf(session) == MediaStatus::Playing) {
                    static_cast<void>(MediaAwait(session.TryPauseAsync(), 2000));
                } else {
                    static_cast<void>(MediaAwait(session.TryPlayAsync(), 2000));
                }
                break;
            case MediaCommand::Seek:
                static_cast<void>(
                    MediaAwait(session.TryChangePlaybackPositionAsync(read.startTicks + request.position), 2000));
                break;
            }
        } catch (...) {
            SendMediaKeyFor(request.command);
        }
    }
}

void PublishMediaRead(const MediaRead& read, bool haveSession) {
    MediaBridge& media = Media();
    MediaState next = haveSession ? read.state : MediaState{};
    if (!haveSession) {
        next.anchorMs = GetTickCount64();
    }
    std::lock_guard<std::mutex> lock(media.mutex);
    MediaState& current = media.state;
    const ULONGLONG now = GetTickCount64();
    if (media.playHoldUntil != 0) {
        if (now >= media.playHoldUntil || !next.have || next.playing == media.playHoldValue) {
            media.playHoldUntil = 0;
        } else {
            next.playing = current.playing;
            next.positionTicks = current.positionTicks;
            next.anchorMs = current.anchorMs;
        }
    }
    if (media.positionHoldUntil != 0) {
        const int64_t reported = MediaPositionAt(next, now);
        const bool landed = media.positionHoldSkip
            ? reported < 5 * kMediaTicksPerSecond
            : std::llabs(reported - MediaPositionAt(current, now)) < 2 * kMediaTicksPerSecond;
        if (now >= media.positionHoldUntil || !next.have || landed) {
            media.positionHoldUntil = 0;
        } else {
            next.positionTicks = current.positionTicks;
            next.anchorMs = current.anchorMs;
        }
    }
    // Re-anchoring every poll would bump the generation each second; only a
    // real jump (seek, drift, new track) moves the anchor.
    const bool moved = next.playing != current.playing ||
        std::llabs(MediaPositionAt(next, now) - MediaPositionAt(current, now)) > 250LL * kMediaTicksPerMs ||
        std::fabs(next.rate - current.rate) > 0.001;
    const bool changed = moved || next.have != current.have || next.canPrev != current.canPrev ||
        next.canNext != current.canNext || next.canPlayPause != current.canPlayPause ||
        next.durationTicks != current.durationTicks || next.title != current.title ||
        next.artist != current.artist || next.album != current.album || next.app != current.app ||
        next.artGen != current.artGen;
    if (!changed) {
        return;
    }
    if (!moved) {
        next.positionTicks = current.positionTicks;
        next.anchorMs = current.anchorMs;
    }
    current = std::move(next);
    ++media.gen;
}

void MediaWorker() {
    MediaBridge& media = Media();
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    MediaManager manager{nullptr};
    try {
        manager = MediaAwait(MediaManager::RequestAsync(), 10000);
    } catch (...) {
        manager = nullptr;
    }
    if (!manager) {
        std::lock_guard<std::mutex> lock(media.mutex);
        media.unavailable = true;
        ++media.gen;
        return;
    }
    try {
        static_cast<void>(
            manager.CurrentSessionChanged([](auto&&, auto&&) { WakeMedia(kMediaDirtyPlayback); }));
        static_cast<void>(manager.SessionsChanged(
            [](auto&&, auto&&) { WakeMedia(kMediaDirtySessions | kMediaDirtyPlayback); }));
    } catch (...) {
    }
    std::vector<MediaHook> hooks;
    MediaSession session{nullptr};
    std::wstring sessionId;
    MediaRead read;
    ULONGLONG lastPoll = 0;
    ULONGLONG lastProps = 0;
    unsigned bits = kMediaDirtyAll;
    std::vector<MediaRequest> requests;
    for (;;) {
        bool demand = false;
        if (bits == 0) {
            std::unique_lock<std::mutex> lock(media.mutex);
            for (;;) {
                const ULONGLONG now = GetTickCount64();
                demand = media.demandMs != 0 && now - media.demandMs < kMediaDemandMs;
                if (media.dirty != 0 || !media.requests.empty()) {
                    break;
                }
                // Safety poll only while Quick Settings is open: corrects drift
                // and catches apps that skip events. Closed = events only.
                if (!demand) {
                    media.sleeping = true;
                    media.wake.wait(lock);
                    media.sleeping = false;
                    continue;
                }
                if (now - lastPoll >= kMediaPollMs) {
                    break;
                }
                media.wake.wait_for(
                    lock, std::chrono::milliseconds(static_cast<long long>(kMediaPollMs - (now - lastPoll))));
            }
            bits = std::exchange(media.dirty, 0U);
            requests.swap(media.requests);
        }
        const ULONGLONG now = GetTickCount64();
        bool propsEvent = (bits & kMediaDirtyProps) != 0;
        if (demand && now - lastPoll >= kMediaPollMs) {
            bits |= kMediaDirtyPlayback | kMediaDirtyTimeline;
            if (now - lastProps >= kMediaPropsPollMs) {
                bits |= kMediaDirtyProps;
            }
        }
        try {
            if (!requests.empty()) {
                RunMediaRequests(session, read, requests);
                requests.clear();
                bits |= kMediaDirtyPlayback | kMediaDirtyTimeline;
            }
            if ((bits & kMediaDirtySessions) != 0) {
                HookMediaSessions(manager, hooks);
            }
            if ((bits & (kMediaDirtySessions | kMediaDirtyPlayback)) != 0 || !session) {
                MediaSession picked = PickMediaSession(manager, sessionId);
                if (!picked) {
                    session = nullptr;
                    sessionId.clear();
                    read = MediaRead{};
                } else {
                    std::wstring pickedId = MediaAumid(picked);
                    if (!session || pickedId != sessionId) {
                        read = MediaRead{};
                        read.state.artGen = NextMediaArtGen();
                        read.state.app = MediaAppName(pickedId);
                        sessionId = std::move(pickedId);
                        bits |= kMediaDirtyProps | kMediaDirtyTimeline | kMediaDirtyPlayback;
                        propsEvent = true;
                    } else if (winrt::get_abi(picked) != winrt::get_abi(session)) {
                        // Same app, different tab/window: metadata may differ.
                        bits |= kMediaDirtyProps | kMediaDirtyTimeline;
                    }
                    session = picked;
                }
            }
            if (session) {
                if ((bits & (kMediaDirtyPlayback | kMediaDirtyTimeline)) != 0) {
                    ReadMediaPlayback(session, read);
                }
                if ((bits & kMediaDirtyProps) != 0) {
                    ReadMediaProps(session, read, propsEvent);
                    lastProps = now;
                }
                if ((bits & (kMediaDirtyPlayback | kMediaDirtyTimeline)) != 0) {
                    ReadMediaTimeline(session, read);
                }
            }
        } catch (...) {
        }
        if ((bits & (kMediaDirtyPlayback | kMediaDirtyTimeline)) != 0) {
            lastPoll = now;
        }
        PublishMediaRead(read, static_cast<bool>(session));
        bits = 0;
    }
}

void StartMediaWatch() {
    MediaBridge& media = Media();
    {
        std::lock_guard<std::mutex> lock(media.mutex);
        if (media.started) {
            return;
        }
        media.started = true;
    }
    try {
        std::thread([] {
            try {
                MediaWorker();
            } catch (...) {
            }
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(media.mutex);
        media.unavailable = true;
    }
}

// Transport click from the UI thread. Updates the snapshot optimistically so
// the glyph flips on this frame; the worker issues the GSMTC call.
void ControlMediaSession(int command) {
    StartMediaWatch();
    MediaBridge& media = Media();
    const MediaCommand action = command == 0 ? MediaCommand::Previous
        : (command == 2 ? MediaCommand::Next : MediaCommand::TogglePlay);
    bool keys = false;
    {
        std::lock_guard<std::mutex> lock(media.mutex);
        if (media.unavailable) {
            keys = true;
        } else {
            MediaState& state = media.state;
            const ULONGLONG now = GetTickCount64();
            if (state.have && action == MediaCommand::TogglePlay && state.canPlayPause) {
                state.positionTicks = MediaPositionAt(state, now);
                state.anchorMs = now;
                state.playing = !state.playing;
                media.playHoldValue = state.playing;
                media.playHoldUntil = now + kMediaHoldMs;
                ++media.gen;
            } else if (state.have && ((action == MediaCommand::Next && state.canNext) ||
                                         (action == MediaCommand::Previous && state.canPrev))) {
                state.positionTicks = 0;
                state.anchorMs = now;
                media.positionHoldSkip = true;
                media.positionHoldUntil = now + kMediaHoldMs;
                ++media.gen;
            }
            media.requests.push_back({action, 0});
        }
    }
    if (keys) {
        SendMediaKeyFor(action);
    } else {
        media.wake.notify_one();
    }
}

// Copy the latest snapshot into the UI cache (no IPC) and re-derive the
// extrapolated position. Labels = metadata/art changed (full rebuild);
// MetersOnly = transport state, time or scrub moved (live present only).
QsLiveChange RefreshQuickSettingsMedia(QuickSettingsCache& cache) {
    StartMediaWatch();
    MediaBridge& media = Media();
    const ULONGLONG now = GetTickCount64();
    const bool beforePlaying = cache.mediaPlaying;
    const unsigned beforeFlags = (cache.mediaCanPrev ? 1U : 0U) | (cache.mediaCanNext ? 2U : 0U) |
        (cache.mediaCanPlayPause ? 4U : 0U);
    const int64_t beforeElapsed = cache.mediaElapsedMs / 1000;
    const int64_t beforeDuration = cache.mediaDurationMs / 1000;
    const int beforeProgress = static_cast<int>(std::lround(cache.mediaProgress * 4000.0F));
    bool labels = false;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(media.mutex);
        media.demandMs = now;
        wake = media.sleeping;
        if (media.gen != cache.mediaGen) {
            const MediaState& state = media.state;
            labels = cache.mediaHave != state.have || cache.mediaTitle != state.title ||
                cache.mediaArtist != state.artist || cache.mediaAlbum != state.album ||
                cache.mediaApp != state.app || cache.mediaArtGen != state.artGen;
            cache.mediaHave = state.have;
            cache.mediaPlaying = state.playing;
            cache.mediaCanPrev = state.canPrev;
            cache.mediaCanNext = state.canNext;
            cache.mediaCanPlayPause = state.canPlayPause;
            if (labels) {
                cache.mediaTitle = state.title;
                cache.mediaArtist = state.artist;
                cache.mediaAlbum = state.album;
                cache.mediaApp = state.app;
                cache.mediaArtGen = state.artGen;
                cache.mediaArt = state.art;
            }
            cache.mediaPositionTicks = state.positionTicks;
            cache.mediaAnchorMs = state.anchorMs;
            cache.mediaRate = state.rate;
            cache.mediaDurationTicks = state.durationTicks;
            cache.mediaGen = media.gen;
        }
    }
    if (wake) {
        media.wake.notify_one();
    }
    MediaState anchor;
    anchor.playing = cache.mediaPlaying;
    anchor.positionTicks = cache.mediaPositionTicks;
    anchor.anchorMs = cache.mediaAnchorMs;
    anchor.rate = cache.mediaRate;
    anchor.durationTicks = cache.mediaDurationTicks;
    const int64_t duration = cache.mediaHave ? cache.mediaDurationTicks : 0;
    int64_t position = cache.mediaHave ? MediaPositionAt(anchor, now) : 0;
    if (duration > 0 && cache.mediaScrubPreview >= 0.0F) {
        position = static_cast<int64_t>(static_cast<double>(duration) * cache.mediaScrubPreview);
    }
    cache.mediaDurationMs = duration / kMediaTicksPerMs;
    cache.mediaElapsedMs = position / kMediaTicksPerMs;
    cache.mediaProgress = duration > 0
        ? std::clamp(static_cast<float>(static_cast<double>(position) / static_cast<double>(duration)), 0.0F, 1.0F)
        : 0.0F;
    if (labels) {
        return QsLiveChange::Labels;
    }
    const unsigned flags = (cache.mediaCanPrev ? 1U : 0U) | (cache.mediaCanNext ? 2U : 0U) |
        (cache.mediaCanPlayPause ? 4U : 0U);
    if (cache.mediaPlaying != beforePlaying || flags != beforeFlags ||
        cache.mediaElapsedMs / 1000 != beforeElapsed || cache.mediaDurationMs / 1000 != beforeDuration ||
        static_cast<int>(std::lround(cache.mediaProgress * 4000.0F)) != beforeProgress) {
        return QsLiveChange::MetersOnly;
    }
    return QsLiveChange::None;
}

// "245 Mbps", "8.4 Mbps", "1.25 Gbps"; en dash when unmeasured.
std::wstring FormatMbps(double mbps) {
    if (mbps < 0.0) {
        return L"\u2013";
    }
    wchar_t buffer[32]{};
    if (mbps >= 1000.0) {
        swprintf_s(buffer, L"%.2f Gbps", mbps / 1000.0);
    } else if (mbps >= 10.0) {
        swprintf_s(buffer, L"%.0f Mbps", mbps);
    } else {
        swprintf_s(buffer, L"%.1f Mbps", mbps);
    }
    return buffer;
}

std::wstring FormatPingMs(double ms) {
    if (ms < 0.0) {
        return L"\u2013";
    }
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%.0f ms", ms);
    return buffer;
}

std::wstring FormatMediaTime(int64_t ms) {
    const int64_t total = std::max<int64_t>(0, ms) / 1000;
    wchar_t buffer[32]{};
    if (total >= 3600) {
        swprintf_s(buffer, L"%lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
    } else {
        swprintf_s(buffer, L"%lld:%02lld", total / 60, total % 60);
    }
    return buffer;
}

// Media module time line: "1:23 / 3:45" (elapsed only if that is too wide).
std::wstring MediaTimeText(const QuickSettingsCache& cache, HFONT font, LONG width) {
    if (!cache.mediaHave) {
        return {};
    }
    if (cache.mediaDurationMs <= 0) {
        return cache.mediaPlaying ? L"Playing" : L"Paused";
    }
    const std::wstring elapsed = FormatMediaTime(cache.mediaElapsedMs);
    std::wstring full = elapsed + L" / " + FormatMediaTime(cache.mediaDurationMs);
    if (font == nullptr || MeasureFlyoutLine(font, full) <= width) {
        return full;
    }
    return elapsed;
}

int MeterBucket(float peak) noexcept {
    const float shaped = std::pow(std::clamp(peak, 0.0F, 1.0F), 0.45F);
    return static_cast<int>(std::lround(shaped * 32.0F));
}

// Home Mic capsule meter: slim rounded track with an accent fill, confined to `meter`.
void PaintMicPillMeter(uint8_t* pixels, int width, int height, const RECT& meter, float level,
    uint8_t inkR, uint8_t inkG, uint8_t inkB) {
    const float barH = static_cast<float>(meter.bottom - meter.top);
    const float radius = std::max(1.0F, barH * 0.5F);
    const float cy = 0.5F * static_cast<float>(meter.top + meter.bottom);
    const float left = static_cast<float>(meter.left) + radius;
    const float right = std::max(left, static_cast<float>(meter.right) - radius);
    FillPillColorPremul(pixels, width, height, left, right, cy, radius, 0.28F, inkB, inkG, inkR);
    const float clamped = std::clamp(level, 0.0F, 1.0F);
    if (clamped > 0.02F) {
        FillPillColorPremul(pixels, width, height, left, left + (right - left) * clamped, cy, radius, 1.0F,
            g_qsAccentB, g_qsAccentG, g_qsAccentR);
    }
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

void FillQsNetworkCache(QuickSettingsCache& cache, QuickSettingsPage page, HWND anchor) {
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
    QueryDarkMode(cache);
    QueryRefreshRate(cache, anchor);
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

// Anti-aliased convex polygon (3-4 vertices, either winding) with rounded
// corners: every edge is pulled in by `corner`, then the SDF is re-inflated.
void FillRoundedPolygonPremul(uint8_t* dest, int destWidth, int destHeight, const float* xs, const float* ys,
    int count, float corner, float alpha, uint8_t blue, uint8_t green, uint8_t red) {
    if (dest == nullptr || count < 3 || count > 4 || alpha <= 0.0F) {
        return;
    }
    float centerX = 0.0F;
    float centerY = 0.0F;
    for (int index = 0; index < count; ++index) {
        centerX += xs[index];
        centerY += ys[index];
    }
    centerX /= static_cast<float>(count);
    centerY /= static_cast<float>(count);
    float normalX[4]{};
    float normalY[4]{};
    float offset[4]{};
    for (int index = 0; index < count; ++index) {
        const int next = (index + 1) % count;
        const float edgeX = xs[next] - xs[index];
        const float edgeY = ys[next] - ys[index];
        const float length = std::sqrt(edgeX * edgeX + edgeY * edgeY);
        if (length < 1e-4F) {
            return;
        }
        float outX = edgeY / length;
        float outY = -edgeX / length;
        if ((centerX - xs[index]) * outX + (centerY - ys[index]) * outY > 0.0F) {
            outX = -outX;
            outY = -outY;
        }
        normalX[index] = outX;
        normalY[index] = outY;
        offset[index] = outX * xs[index] + outY * ys[index] - corner;
    }
    float innerX[4]{};
    float innerY[4]{};
    for (int index = 0; index < count; ++index) {
        const int previous = (index + count - 1) % count;
        const float det = normalX[previous] * normalY[index] - normalY[previous] * normalX[index];
        if (std::fabs(det) < 1e-6F) {
            return;
        }
        innerX[index] = (offset[previous] * normalY[index] - normalY[previous] * offset[index]) / det;
        innerY[index] = (normalX[previous] * offset[index] - offset[previous] * normalX[index]) / det;
    }
    float minX = xs[0];
    float maxX = xs[0];
    float minY = ys[0];
    float maxY = ys[0];
    for (int index = 1; index < count; ++index) {
        minX = std::min(minX, xs[index]);
        maxX = std::max(maxX, xs[index]);
        minY = std::min(minY, ys[index]);
        maxY = std::max(maxY, ys[index]);
    }
    const int left = std::max(0, static_cast<int>(std::floor(minX - 1.5F)));
    const int right = std::min(destWidth, static_cast<int>(std::ceil(maxX + 1.5F)));
    const int top = std::max(0, static_cast<int>(std::floor(minY - 1.5F)));
    const int bottom = std::min(destHeight, static_cast<int>(std::ceil(maxY + 1.5F)));
    if (right <= left || bottom <= top) {
        return;
    }
    std::vector<uint8_t> row(static_cast<size_t>(right - left) * 4U);
    for (int y = top; y < bottom; ++y) {
        const float py = static_cast<float>(y) + 0.5F;
        for (int x = left; x < right; ++x) {
            const float px = static_cast<float>(x) + 0.5F;
            float inside = -1e9F;
            for (int index = 0; index < count; ++index) {
                inside = std::max(inside, normalX[index] * px + normalY[index] * py - offset[index]);
            }
            float distance = inside;
            if (inside > 0.0F) {
                distance = 1e9F;
                for (int index = 0; index < count; ++index) {
                    const int next = (index + 1) % count;
                    const float segX = innerX[next] - innerX[index];
                    const float segY = innerY[next] - innerY[index];
                    const float lengthSq = std::max(1e-6F, segX * segX + segY * segY);
                    const float t = std::clamp(
                        ((px - innerX[index]) * segX + (py - innerY[index]) * segY) / lengthSq, 0.0F, 1.0F);
                    const float dx = px - (innerX[index] + segX * t);
                    const float dy = py - (innerY[index] + segY * t);
                    distance = std::min(distance, std::sqrt(dx * dx + dy * dy));
                }
            }
            const float coverage = std::clamp(0.5F - (distance - corner), 0.0F, 1.0F) * alpha;
            uint8_t* pixel = row.data() + static_cast<size_t>(x - left) * 4U;
            pixel[0] = static_cast<uint8_t>(std::lround(static_cast<float>(blue) * coverage));
            pixel[1] = static_cast<uint8_t>(std::lround(static_cast<float>(green) * coverage));
            pixel[2] = static_cast<uint8_t>(std::lround(static_cast<float>(red) * coverage));
            pixel[3] = static_cast<uint8_t>(std::lround(255.0F * coverage));
        }
        CompositePremul(dest, destWidth, destHeight, left, y, row.data(), right - left, 1);
    }
}

enum class MediaGlyph : uint8_t {
    Previous,
    Play,
    Pause,
    Next,
};

// Filled macOS-style transport glyphs (Segoe icons are outline/thin at this
// size). `unit` is px per pt.
void PaintMediaGlyph(uint8_t* dest, int destWidth, int destHeight, MediaGlyph glyph, float cx, float cy,
    float unit, float alpha, uint8_t red, uint8_t green, uint8_t blue) {
    switch (glyph) {
    case MediaGlyph::Play: {
        const float xs[3] = {cx - 6.0F * unit, cx - 6.0F * unit, cx + 8.5F * unit};
        const float ys[3] = {cy - 8.5F * unit, cy + 8.5F * unit, cy};
        FillRoundedPolygonPremul(dest, destWidth, destHeight, xs, ys, 3, 1.6F * unit, alpha, blue, green, red);
        break;
    }
    case MediaGlyph::Pause:
        for (int bar = 0; bar < 2; ++bar) {
            const float barLeft = cx + (bar == 0 ? -7.0F : 2.0F) * unit;
            const float barRight = barLeft + 5.0F * unit;
            const float xs[4] = {barLeft, barRight, barRight, barLeft};
            const float ys[4] = {cy - 8.5F * unit, cy - 8.5F * unit, cy + 8.5F * unit, cy + 8.5F * unit};
            FillRoundedPolygonPremul(dest, destWidth, destHeight, xs, ys, 4, 1.4F * unit, alpha, blue, green,
                red);
        }
        break;
    case MediaGlyph::Previous:
    case MediaGlyph::Next: {
        const float direction = glyph == MediaGlyph::Next ? 1.0F : -1.0F;
        for (int part = 0; part < 2; ++part) {
            const float base = cx + direction * (part == 0 ? -11.0F : 0.0F) * unit;
            const float tip = base + direction * 11.0F * unit;
            const float xs[3] = {base, base, tip};
            const float ys[3] = {cy - 7.0F * unit, cy + 7.0F * unit, cy};
            FillRoundedPolygonPremul(dest, destWidth, destHeight, xs, ys, 3, 1.3F * unit, alpha, blue, green,
                red);
        }
        break;
    }
    }
}

// Album art box-filtered from kMediaArtPx to `extent` with rounded corners.
// Cached by generation: hover and live presents never re-filter it.
const std::vector<uint8_t>& MediaArtRaster(const std::shared_ptr<const std::vector<uint8_t>>& art, uint64_t gen,
    int extent, float corner) {
    static std::vector<uint8_t> raster;
    static uint64_t rasterGen = 0;
    static int rasterExtent = 0;
    static float rasterCorner = -1.0F;
    constexpr int sourceExtent = kMediaArtPx;
    if (!art || extent <= 0 ||
        art->size() != static_cast<size_t>(sourceExtent) * static_cast<size_t>(sourceExtent) * 4U) {
        raster.clear();
        rasterGen = 0;
        return raster;
    }
    if (rasterGen == gen && rasterExtent == extent && rasterCorner == corner && !raster.empty()) {
        return raster;
    }
    rasterGen = gen;
    rasterExtent = extent;
    rasterCorner = corner;
    raster.assign(static_cast<size_t>(extent) * static_cast<size_t>(extent) * 4U, 0);
    const std::vector<uint8_t>& source = *art;
    const float step = static_cast<float>(sourceExtent) / static_cast<float>(extent);
    const float half = static_cast<float>(extent) * 0.5F;
    auto span = [&](int index, int& from, int& to) {
        from = std::min(sourceExtent - 1, static_cast<int>(std::floor(static_cast<float>(index) * step)));
        to = std::max(from + 1, std::min(sourceExtent, static_cast<int>(std::ceil(static_cast<float>(index + 1) * step))));
    };
    for (int y = 0; y < extent; ++y) {
        int sy0 = 0;
        int sy1 = 0;
        span(y, sy0, sy1);
        const float qy = std::fabs(static_cast<float>(y) + 0.5F - half) - (half - corner);
        for (int x = 0; x < extent; ++x) {
            int sx0 = 0;
            int sx1 = 0;
            span(x, sx0, sx1);
            uint32_t sum[4]{};
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint8_t* src = source.data() +
                    (static_cast<size_t>(sy) * static_cast<size_t>(sourceExtent) + static_cast<size_t>(sx0)) * 4U;
                for (int sx = sx0; sx < sx1; ++sx, src += 4) {
                    sum[0] += src[0];
                    sum[1] += src[1];
                    sum[2] += src[2];
                    sum[3] += src[3];
                }
            }
            const float samples = static_cast<float>((sy1 - sy0) * (sx1 - sx0));
            const float qx = std::fabs(static_cast<float>(x) + 0.5F - half) - (half - corner);
            const float outsideX = std::max(qx, 0.0F);
            const float outsideY = std::max(qy, 0.0F);
            const float sd = std::sqrt(outsideX * outsideX + outsideY * outsideY) +
                std::min(std::max(qx, qy), 0.0F) - corner;
            const float weight = std::clamp(0.5F - sd, 0.0F, 1.0F) / samples;
            uint8_t* out = raster.data() + (static_cast<size_t>(y) * static_cast<size_t>(extent) +
                static_cast<size_t>(x)) * 4U;
            for (int channel = 0; channel < 4; ++channel) {
                out[channel] = static_cast<uint8_t>(
                    std::min(255L, std::lround(static_cast<float>(sum[channel]) * weight)));
            }
        }
    }
    return raster;
}

// Home CPU / GPU tile line: "CPU: 32°C | 35W". A missing value is dropped;
// neither -> "CPU: –".
std::wstring TempPowerLine(const std::wstring& label, int celsius, int watts) {
    std::wstring out = label + L": ";
    if (celsius < 0 && watts < 0) {
        return out + L"\u2013";
    }
    if (celsius >= 0) {
        out += std::to_wstring(celsius) + L"\u00B0C";
    }
    if (watts >= 0) {
        if (celsius >= 0) {
            out += L" | ";
        }
        out += std::to_wstring(watts) + L"W";
    }
    return out;
}

// One line per GPU ("GPU 1: ...") for up to three; with more, the last line
// covers the rest (hottest temperature, summed watts). A single GPU is "GPU".
std::vector<std::wstring> GpuTempLines(const std::vector<QsGpuTemp>& gpus) {
    if (gpus.size() <= 1) {
        const QsGpuTemp only = gpus.empty() ? QsGpuTemp{} : gpus.front();
        return {TempPowerLine(L"GPU", only.celsius, only.watts)};
    }
    std::vector<std::wstring> lines;
    const size_t shown = gpus.size() > 3U ? 2U : gpus.size();
    for (size_t index = 0; index < shown; ++index) {
        lines.push_back(TempPowerLine(L"GPU " + std::to_wstring(index + 1U), gpus[index].celsius,
            gpus[index].watts));
    }
    if (gpus.size() > 3U) {
        int hottest = -1;
        int total = -1;
        for (size_t index = shown; index < gpus.size(); ++index) {
            hottest = std::max(hottest, gpus[index].celsius);
            if (gpus[index].watts >= 0) {
                total = std::max(total, 0) + gpus[index].watts;
            }
        }
        lines.push_back(TempPowerLine(L"GPU " + std::to_wstring(shown + 1U) + L"\u2013" +
            std::to_wstring(gpus.size()), hottest, total));
    }
    return lines;
}

// Temperature lines centred in `area`: the label font when every line fits
// (at most two lines), else the status font, character-ellipsized if needed.
void DrawTempLines(uint8_t* pixels, int width, int height, const RECT& area, const std::vector<std::wstring>& lines,
    HFONT labelFace, HFONT statusFace) {
    if (lines.empty()) {
        return;
    }
    const LONG areaW = area.right - area.left;
    HFONT font = statusFace != nullptr ? statusFace : labelFace;
    if (labelFace != nullptr && lines.size() <= 2U) {
        bool fits = true;
        for (const std::wstring& line : lines) {
            fits = fits && MeasureFlyoutLine(labelFace, line) <= areaW - 2L;
        }
        if (fits) {
            font = labelFace;
        }
    }
    LOGFONTW logFont{};
    if (font == nullptr || GetObjectW(font, sizeof(logFont), &logFont) == 0) {
        return;
    }
    const LONG em = std::max(1L, std::abs(logFont.lfHeight));
    const LONG lineH = em + em * 2L / 5L;
    LONG top = (area.top + area.bottom - lineH * static_cast<LONG>(lines.size())) / 2L;
    for (const std::wstring& line : lines) {
        DrawFlyoutText(pixels, width, height, {area.left, top, area.right, top + lineH}, font,
            FitFlyoutLine(font, line, areaW), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
        top += lineH;
    }
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

// Shell icon for a Power page app row, cached per executable path (UI thread
// only). Bounded: the cache is dropped wholesale if it ever grows large.
HICON EnergyAppIcon(const std::wstring& path) {
    static std::unordered_map<std::wstring, HICON> icons;
    if (path.empty()) {
        return nullptr;
    }
    const auto found = icons.find(path);
    if (found != icons.end()) {
        return found->second;
    }
    if (icons.size() >= 64U) {
        for (const auto& cached : icons) {
            if (cached.second != nullptr) {
                DestroyIcon(cached.second);
            }
        }
        icons.clear();
    }
    SHFILEINFOW info{};
    HICON icon = nullptr;
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) != 0) {
        icon = info.hIcon;
    }
    icons.emplace(path, icon);
    return icon;
}

const wchar_t* PowerLevelLabel(QsPowerLevel level) noexcept {
    switch (level) {
    case QsPowerLevel::VeryHigh:
        return L"Very high";
    case QsPowerLevel::High:
        return L"High";
    case QsPowerLevel::Moderate:
        return L"Moderate";
    case QsPowerLevel::Low:
        return L"Low";
    case QsPowerLevel::VeryLow:
        break;
    }
    return L"Very low";
}

}  // namespace

void SeekQuickSettingsMedia(float level) noexcept {
    level = std::clamp(level, 0.0F, 1.0F);
    MediaBridge& media = Media();
    try {
        {
            std::lock_guard<std::mutex> lock(media.mutex);
            MediaState& state = media.state;
            if (media.unavailable || !state.have || state.durationTicks <= 0) {
                return;
            }
            const ULONGLONG now = GetTickCount64();
            const int64_t target = std::llround(static_cast<double>(state.durationTicks) * level);
            state.positionTicks = target;
            state.anchorMs = now;
            media.positionHoldSkip = false;
            media.positionHoldUntil = now + kMediaHoldMs;
            ++media.gen;
            media.requests.push_back({MediaCommand::Seek, target});
        }
        media.wake.notify_one();
    } catch (...) {
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
        HWND anchor = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_network.mutex);
            page = g_network.page;
            anchor = g_network.notifyHwnd;
        }
        QuickSettingsCache local{};
        FillQsNetworkCache(local, page, anchor);
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
        cache.outputName != r.outputName ||
        cache.inputName != r.inputName || cache.inputGain != r.inputGain ||
        cache.inputMuted != r.inputMuted || cache.inputNote != r.inputNote ||
        cache.powerName != r.powerName || cache.powerMode != r.powerMode ||
        cache.hdrOn != r.hdrOn || cache.hdrSupported != r.hdrSupported ||
        cache.nightLight != r.nightLight || cache.nightKnown != r.nightKnown ||
        cache.darkMode != r.darkMode || cache.darkKnown != r.darkKnown ||
        cache.refreshHz != r.refreshHz || cache.refreshRates != r.refreshRates ||
        cache.refreshDevice != r.refreshDevice || cache.refreshMonitor != r.refreshMonitor ||
        cache.refreshWidth != r.refreshWidth || cache.refreshHeight != r.refreshHeight ||
        cache.wifi.size() != r.wifi.size() || cache.vpn.size() != r.vpn.size() ||
        cache.renderDevices.size() != r.renderDevices.size() ||
        cache.captureDevices.size() != r.captureDevices.size();
    cache.wifiRadioOn = r.wifiRadioOn;
    cache.haveWifiInterface = r.haveWifiInterface;
    cache.wifiInterface = r.wifiInterface;
    cache.wifi = r.wifi;
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
    cache.hdrOn = r.hdrOn;
    cache.hdrSupported = r.hdrSupported;
    cache.nightLight = r.nightLight;
    cache.nightKnown = r.nightKnown;
    cache.darkMode = r.darkMode;
    cache.darkKnown = r.darkKnown;
    cache.refreshHz = r.refreshHz;
    cache.refreshRates = r.refreshRates;
    cache.refreshDevice = r.refreshDevice;
    cache.refreshMonitor = r.refreshMonitor;
    cache.refreshWidth = r.refreshWidth;
    cache.refreshHeight = r.refreshHeight;
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
        if (local.energyPending) {
            // Fresh baseline: take the first real sample shortly after rather
            // than waiting a whole 2 s timer tick on "Measuring".
            Sleep(700);
            QueryEnergyApps(local);
        }
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
                cache.energyApps[i].path != g_energy.result.energyApps[i].path ||
                cache.energyApps[i].level != g_energy.result.energyApps[i].level) {
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
    int cpuW = -1;
    std::vector<QsGpuTemp> gpus;
    if (LatestSystemTemps(reading) && GetTickCount64() - reading.stamp <= kStaleMs) {
        cpu = reading.cpuC;
        cpuW = reading.cpuW;
        gpus.reserve(reading.gpus.size());
        for (const GpuTempsReading& gpu : reading.gpus) {
            gpus.push_back({gpu.celsius, gpu.watts});
        }
    }
    const bool changed = cpu != cache.cpuTempC || cpuW != cache.cpuWatts || gpus != cache.gpus;
    cache.cpuTempC = cpu;
    cache.cpuWatts = cpuW;
    cache.gpus = std::move(gpus);
    return changed;
}

QsLiveChange RefreshQuickSettingsLive(QuickSettingsCache& cache) {
    const int beforeBucket = MeterBucket(cache.inputPeak);
    const float prevPeak = cache.inputPeak;
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
    // Fast attack, slower release so the Home Mic meter reads as a smooth level bar.
    {
        const float raw = cache.inputPeak;
        const float k = raw > prevPeak ? 0.65F : 0.18F;
        float smooth = prevPeak + (raw - prevPeak) * k;
        if (smooth < 0.002F) {
            smooth = 0.0F;
        }
        cache.inputPeak = smooth;
    }

    // GSMTC snapshot from the media worker: no WinRT IPC on the UI thread.
    const QsLiveChange media = RefreshQuickSettingsMedia(cache);

    const bool labels = media == QsLiveChange::Labels || cache.inputNote != beforeNote;
    const bool meters = MeterBucket(cache.inputPeak) != beforeBucket || media == QsLiveChange::MetersOnly;
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
            // First entry only: the 2 s timer + ArmOverflowLiveWorkers own the
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
    // Latest GSMTC snapshot from the media worker (starts it on first open).
    static_cast<void>(RefreshQuickSettingsMedia(m_qsCache));
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
    ResetQsChevronFade();
    if (page == QuickSettingsPage::RefreshRate) {
        m_qsRefreshNote.clear();
    }
    if (page == QuickSettingsPage::Wifi) {
        m_qsWifiOthersOpen = false;
    } else if (page == QuickSettingsPage::Bluetooth) {
        m_qsBtOthersOpen = m_bluetoothSnapshot.discovering;
    }
    // Expire TTLs so the worker refreshes page-specific data (wifi list, etc.)
    // after this paint - never run WLAN/COM/UIA on the UI thread here.
    m_qsCache.stampNetwork = 0;
    m_qsCache.stampNight = 0;
    if (page == QuickSettingsPage::SystemTray) {
        // UIA scrape on a worker; the page shows the last scan meanwhile.
        RequestTrayIconsAsync();
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
    ResetQsChevronFade();
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
        // Preview only while dragging (one live-rect present per move); the
        // seek is sent once on release so the player is not flooded.
        if (!m_qsCache.mediaHave || m_qsCache.mediaDurationMs <= 0) {
            return;
        }
        const RECT& bar = m_qsScrubValid ? m_qsScrubRect : track;
        const float radius = static_cast<float>(bar.bottom - bar.top) * 0.5F;
        const float left = static_cast<float>(bar.left) + radius;
        const float right = std::max(left + 1.0F, static_cast<float>(bar.right) - radius);
        m_qsCache.mediaScrubPreview = std::clamp((static_cast<float>(x) - left) / (right - left), 0.0F, 1.0F);
        static_cast<void>(RefreshQuickSettingsMedia(m_qsCache));
        PaintOverflowLiveFast();
    }
}

void DockApp::CommitQuickSettingsMediaSeek() {
    const float level = m_qsCache.mediaScrubPreview;
    m_qsCache.mediaScrubPreview = -1.0F;
    if (level < 0.0F) {
        return;
    }
    SeekQuickSettingsMedia(level);
    static_cast<void>(RefreshQuickSettingsMedia(m_qsCache));
    PaintOverflowLiveFast();
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
    // Reused across paints: assign keeps capacity, so no fresh multi-MB
    // allocation (and its page faults) per Quick Settings rebuild.
    std::vector<uint8_t>& content = m_qsContentScratch;
    content.assign(bytes, 0);
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
        OffsetRect(&m_qsMediaLiveRect, shadowMargin, shadowMargin);
        OffsetRect(&m_qsMediaTimeRect, shadowMargin, shadowMargin);
        for (RECT& button : m_qsMediaTransportRect) {
            OffsetRect(&button, shadowMargin, shadowMargin);
        }
    }
    if (m_qsTempsValid) {
        OffsetRect(&m_qsTempCpuRect, shadowMargin, shadowMargin);
        OffsetRect(&m_qsTempGpuRect, shadowMargin, shadowMargin);
    }
    if (m_qsBoostStatusValid) {
        OffsetRect(&m_qsBoostStatusRect, shadowMargin, shadowMargin);
    }
    if (m_qsBtStatusValid) {
        OffsetRect(&m_qsBtStatusRect, shadowMargin, shadowMargin);
    }
    if (m_qsTrayStatusValid) {
        OffsetRect(&m_qsTrayStatusRect, shadowMargin, shadowMargin);
    }
    for (RECT& chevron : m_qsTileChevron) {
        OffsetRect(&chevron, shadowMargin, shadowMargin);
    }
    for (RECT& area : m_qsTrayArrowRects) {
        OffsetRect(&area, shadowMargin, shadowMargin);
    }
    if (m_qsModuleRectsValid) {
        for (RECT& module : m_qsModuleRect) {
            OffsetRect(&module, shadowMargin, shadowMargin);
        }
        m_qsGridLeft += shadowMargin;
        m_qsGridTop += shadowMargin;
    }
    m_overflowGearX += static_cast<int>(shadowMargin);
    m_overflowGearY += static_cast<int>(shadowMargin);
}

void DockApp::QsLiveSignatures(int64_t& meter, int64_t& scrub) const noexcept {
    meter = -1;
    scrub = -1;
    const int64_t accent = (static_cast<int64_t>(g_qsAccentR) << 16) | (static_cast<int64_t>(g_qsAccentG) << 8) |
        static_cast<int64_t>(g_qsAccentB);
    if (m_qsMeterValid) {
        const int64_t ink = (static_cast<int64_t>(m_qsMeterInkR) << 16) |
            (static_cast<int64_t>(m_qsMeterInkG) << 8) | static_cast<int64_t>(m_qsMeterInkB);
        uint32_t level = 0;
        if (m_qsPage == QuickSettingsPage::Microphone) {
            level = m_qsCache.inputMuted ? 0U
                : static_cast<uint32_t>(std::lround(
                      std::pow(std::clamp(m_qsCache.inputPeak, 0.0F, 1.0F), 0.45F) * 16.0F));
        } else {
            // Exact float the pill meter draws with (see PaintQsLiveOverlays).
            const float shaped = m_qsCache.inputMuted
                ? 0.0F
                : std::pow(std::clamp(m_qsCache.inputPeak, 0.0F, 1.0F), 0.45F);
            std::memcpy(&level, &shaped, sizeof(level));
        }
        meter = (((ink << 24) | accent) * 1000003LL) ^ static_cast<int64_t>(level);
    }
    if (m_qsScrubValid && m_qsPage == QuickSettingsPage::Home) {
        const int64_t ink = (static_cast<int64_t>(m_qsScrubInkR) << 16) |
            (static_cast<int64_t>(m_qsScrubInkG) << 8) | static_cast<int64_t>(m_qsScrubInkB);
        const float level = m_qsCache.mediaHave ? std::clamp(m_qsCache.mediaProgress, 0.0F, 1.0F) : 0.0F;
        // Quarter-pixel knob position: finer motion is below the AA edge step.
        const float span = static_cast<float>(std::max(1L, m_qsScrubRect.right - m_qsScrubRect.left));
        const int64_t position = std::lround(level * span * 4.0F);
        // The scrub element also carries the transport glyphs (and the knob
        // grows while dragging), so their state is part of its signature.
        const uint64_t state = (m_qsCache.mediaHave ? 1U : 0U) | (m_qsCache.mediaPlaying ? 2U : 0U) |
            (m_qsCache.mediaCanPrev ? 4U : 0U) | (m_qsCache.mediaCanNext ? 8U : 0U) |
            (m_qsCache.mediaCanPlayPause ? 16U : 0U) | (m_qsCache.mediaDurationMs > 0 ? 32U : 0U) |
            (m_qsCache.mediaScrubPreview >= 0.0F ? 64U : 0U);
        const uint64_t hash = (static_cast<uint64_t>((((ink << 24) | accent) * 1000003LL) ^ position) ^ state) *
            1099511628211ULL;
        scrub = static_cast<int64_t>(hash >> 1U);
    }
}

int64_t DockApp::QsMediaTimeSignature() const noexcept {
    if (!m_qsScrubValid || m_qsPage != QuickSettingsPage::Home) {
        return -1;
    }
    uint64_t hash = (static_cast<uint64_t>(m_qsScrubInkR) << 16) | (static_cast<uint64_t>(m_qsScrubInkG) << 8) |
        static_cast<uint64_t>(m_qsScrubInkB);
    auto mix = [&hash](uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
    mix((m_qsCache.mediaHave ? 1U : 0U) | (m_qsCache.mediaPlaying ? 2U : 0U));
    mix(static_cast<uint64_t>(m_qsCache.mediaElapsedMs / 1000));
    mix(static_cast<uint64_t>(m_qsCache.mediaDurationMs / 1000));
    mix(static_cast<uint64_t>(m_qsMediaTimeRect.right - m_qsMediaTimeRect.left));
    return static_cast<int64_t>(hash >> 1U);
}

void DockApp::PaintQsLiveOverlays(uint8_t* pixels, int width, int height, float scale,
    bool includeTemps, bool includeBoost, bool includeBluetooth, bool includeMeter, bool includeScrub,
    bool includeTray, bool includeMediaTime) const {
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }
    const uint8_t inkR = m_qsMeterInkR;
    const uint8_t inkG = m_qsMeterInkG;
    const uint8_t inkB = m_qsMeterInkB;
    if (includeMeter && m_qsMeterValid) {
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
                    index < lit ? g_qsAccentB : inkB, index < lit ? g_qsAccentG : inkG, index < lit ? g_qsAccentR : inkR);
            }
        } else {
            const float level = m_qsCache.inputMuted
                ? 0.0F
                : std::pow(std::clamp(m_qsCache.inputPeak, 0.0F, 1.0F), 0.45F);
            PaintMicPillMeter(pixels, width, height, meter, level, inkR, inkG, inkB);
        }
    }
    if (includeScrub && m_qsScrubValid && m_qsPage == QuickSettingsPage::Home) {
        // Media live element: scrub, transport glyphs and time line. None of
        // it is in the underlay, so a play/pause click or a scrub drag
        // presents this module's rows only.
        const QuickSettingsCache& media = m_qsCache;
        const RECT& track = m_qsScrubRect;
        const bool seekable = media.mediaHave && media.mediaDurationMs > 0;
        const float radius = std::max(1.5F, static_cast<float>(track.bottom - track.top) * 0.5F);
        const float cy = 0.5F * static_cast<float>(track.top + track.bottom);
        const float left = static_cast<float>(track.left) + radius;
        const float right = std::max(left + 1.0F, static_cast<float>(track.right) - radius);
        FillPillColorPremul(pixels, width, height, left, right, cy, radius, seekable ? 0.24F : 0.12F,
            m_qsScrubInkB, m_qsScrubInkG, m_qsScrubInkR);
        if (seekable) {
            const float level = std::clamp(media.mediaProgress, 0.0F, 1.0F);
            const float fill = left + (right - left) * level;
            if (level > 0.0F) {
                FillPillColorPremul(pixels, width, height, left, std::max(left, fill), cy, radius, 0.98F,
                    g_qsAccentB, g_qsAccentG, g_qsAccentR);
            }
            const float knob = media.mediaScrubPreview >= 0.0F ? 2.6F : 1.8F;
            FillCirclePremul(pixels, width, height, fill, cy, radius + std::max(1.5F, knob * scale), 0.99F,
                false);
        }
        for (size_t button = 0; button < 3U; ++button) {
            const RECT& area = m_qsMediaTransportRect[button];
            const bool enabled = media.mediaHave
                ? (button == 0U ? media.mediaCanPrev : (button == 2U ? media.mediaCanNext : media.mediaCanPlayPause))
                : button == 1U;
            const MediaGlyph glyph = button == 0U ? MediaGlyph::Previous
                : (button == 2U ? MediaGlyph::Next : (media.mediaPlaying ? MediaGlyph::Pause : MediaGlyph::Play));
            PaintMediaGlyph(pixels, width, height, glyph, 0.5F * static_cast<float>(area.left + area.right),
                0.5F * static_cast<float>(area.top + area.bottom), scale, enabled ? 0.92F : 0.30F,
                m_qsScrubInkR, m_qsScrubInkG, m_qsScrubInkB);
        }
    }
    // Own element (QsMediaTimeSignature) so knob ticks do not re-render text.
    if (includeMediaTime && m_qsScrubValid && m_qsPage == QuickSettingsPage::Home &&
        m_overflowStatusFont != nullptr) {
        const std::wstring timeText =
            MediaTimeText(m_qsCache, m_overflowStatusFont, m_qsMediaTimeRect.right - m_qsMediaTimeRect.left);
        if (!timeText.empty()) {
            const uint8_t savedR = g_flyoutInkR;
            const uint8_t savedG = g_flyoutInkG;
            const uint8_t savedB = g_flyoutInkB;
            SetFlyoutChromeInk(m_qsScrubInkR, m_qsScrubInkG, m_qsScrubInkB);
            DrawFlyoutText(pixels, width, height, m_qsMediaTimeRect, m_overflowStatusFont, timeText,
                DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 160);
            SetFlyoutChromeInk(savedR, savedG, savedB);
        }
    }
    if (includeTemps && m_qsTempsValid && m_qsPage == QuickSettingsPage::Home &&
        m_overflowLabelFont != nullptr) {
        // Values live outside the underlay so a new reading presents only
        // these two rects (PaintOverflowLiveFast) instead of a full rebuild.
        // Each tile has its own adaptive ink (separate glass shapes).
        const uint8_t savedR = g_flyoutInkR;
        const uint8_t savedG = g_flyoutInkG;
        const uint8_t savedB = g_flyoutInkB;
        SetFlyoutChromeInk(m_qsTempsInkR, m_qsTempsInkG, m_qsTempsInkB);
        DrawTempLines(pixels, width, height, m_qsTempCpuRect,
            {TempPowerLine(L"CPU", m_qsCache.cpuTempC, m_qsCache.cpuWatts)}, m_overflowLabelFont,
            m_overflowStatusFont);
        SetFlyoutChromeInk(m_qsTempsGpuInkR, m_qsTempsGpuInkG, m_qsTempsGpuInkB);
        DrawTempLines(pixels, width, height, m_qsTempGpuRect, GpuTempLines(m_qsCache.gpus), m_overflowLabelFont,
            m_overflowStatusFont);
        SetFlyoutChromeInk(savedR, savedG, savedB);
    }
    // Gate like temps: LiveFast meter ticks must not redraw Boost without
    // restoring the underlay rect first, or glyphs overstrike and look bold.
    if (includeBoost && m_qsBoostStatusValid && m_qsPage == QuickSettingsPage::Home &&
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
    // Bluetooth tile status: same contract as Boost. Worker snapshots and the
    // radio toggle present only this rect (restored from the underlay first)
    // so there is no full PaintQuickSettings and no overstrike/bold text.
    if (includeBluetooth && m_qsBtStatusValid && m_qsPage == QuickSettingsPage::Home) {
        HFONT statusFont =
            m_overflowStatusFont != nullptr ? m_overflowStatusFont : m_overflowLabelFont;
        if (statusFont != nullptr) {
            const std::vector<std::wstring> candidates = BluetoothTileStatusCandidates(m_bluetoothSnapshot);
            // GDI advance vs DirectWrite render: keep a few px of slack.
            const LONG avail = (std::max)(0L, m_qsBtStatusRect.right - m_qsBtStatusRect.left - 4L);
            std::wstring chosen;
            for (const std::wstring& candidate : candidates) {
                if (MeasureFlyoutLine(statusFont, candidate) <= avail) {
                    chosen = candidate;
                    break;
                }
            }
            if (chosen.empty()) {
                // Nothing fits the status band:
                // clean character ellipsis on the short form.
                chosen = FitFlyoutLine(statusFont, candidates.back(), avail);
            }
            const uint8_t savedR = g_flyoutInkR;
            const uint8_t savedG = g_flyoutInkG;
            const uint8_t savedB = g_flyoutInkB;
            SetFlyoutChromeInk(m_qsBtStatusInkR, m_qsBtStatusInkG, m_qsBtStatusInkB);
            DrawFlyoutText(pixels, width, height, m_qsBtStatusRect, statusFont, chosen,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 196);
            SetFlyoutChromeInk(savedR, savedG, savedB);
        }
    }
    // System Tray tile item count: same contract as Bluetooth (the async tray
    // scan presents only this rect).
    if (includeTray && m_qsTrayStatusValid && m_qsPage == QuickSettingsPage::Home) {
        HFONT statusFont =
            m_overflowStatusFont != nullptr ? m_overflowStatusFont : m_overflowLabelFont;
        if (statusFont != nullptr) {
            const uint8_t savedR = g_flyoutInkR;
            const uint8_t savedG = g_flyoutInkG;
            const uint8_t savedB = g_flyoutInkB;
            SetFlyoutChromeInk(m_qsTrayStatusInkR, m_qsTrayStatusInkG, m_qsTrayStatusInkB);
            DrawFlyoutText(pixels, width, height, m_qsTrayStatusRect, statusFont, TrayItemCountText(),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 196);
            SetFlyoutChromeInk(savedR, savedG, savedB);
        }
    }
}

std::wstring DockApp::TrayItemCountText() const {
    if (!m_overflowIconsLoaded) {
        return L"\u2026";
    }
    const size_t count = m_overflowIcons.size();
    if (count == 0) {
        return L"No items";
    }
    return std::to_wstring(count) + (count == 1 ? L" item" : L" items");
}

namespace dock_detail {

uint8_t g_qsAccentR = 152;
uint8_t g_qsAccentG = 168;
uint8_t g_qsAccentB = 105;

void SetQsAccent(unsigned rgb) noexcept {
    g_qsAccentR = static_cast<uint8_t>((rgb >> 16) & 0xFFU);
    g_qsAccentG = static_cast<uint8_t>((rgb >> 8) & 0xFFU);
    g_qsAccentB = static_cast<uint8_t>(rgb & 0xFFU);
}

void CcOnAccentInk(uint8_t accentR, uint8_t accentG, uint8_t accentB, uint8_t& r, uint8_t& g,
    uint8_t& b) noexcept {
    const float luma = (0.2126F * static_cast<float>(accentR) + 0.7152F * static_cast<float>(accentG) +
                           0.0722F * static_cast<float>(accentB)) / 255.0F;
    if (luma > 0.55F) {
        r = 30;
        g = 33;
        b = 24;
    } else {
        r = 250;
        g = 250;
        b = 248;
    }
}

void CcDrawSwitch(uint8_t* pixels, int width, int height, float cxLeft, float cxRight, float cy,
    float radius, float scale, bool on, uint8_t accentR, uint8_t accentG, uint8_t accentB,
    uint8_t inkR, uint8_t inkG, uint8_t inkB) {
    if (on) {
        FillPillColorPremul(pixels, width, height, cxLeft, cxRight, cy, radius, 0.96F, accentB,
            accentG, accentR);
    } else {
        FillPillColorPremul(pixels, width, height, cxLeft, cxRight, cy, radius, 0.45F, inkB, inkG,
            inkR);
    }
    FillCirclePremul(pixels, width, height, on ? cxRight : cxLeft, cy,
        radius - std::max(2.0F, 2.0F * scale), 0.98F, false);
}

void CcDrawSlider(uint8_t* pixels, int width, int height, RECT track, float level, float scale,
    uint8_t accentR, uint8_t accentG, uint8_t accentB, uint8_t inkR, uint8_t inkG, uint8_t inkB) {
    level = std::clamp(level, 0.0F, 1.0F);
    const float radius = static_cast<float>(track.bottom - track.top) * 0.5F;
    const float cy = 0.5F * static_cast<float>(track.top + track.bottom);
    const float left = static_cast<float>(track.left) + radius;
    const float right = std::max(left + 1.0F, static_cast<float>(track.right) - radius);
    const float fill = left + (right - left) * level;
    FillPillColorPremul(pixels, width, height, left, right, cy, radius, 0.26F, inkB, inkG, inkR);
    if (level > 0.01F) {
        FillPillColorPremul(pixels, width, height, left, std::max(left, fill), cy, radius, 0.98F,
            accentB, accentG, accentR);
    }
    FillSoftDisc(pixels, width, height, fill, cy + std::max(1.0F, 1.5F * scale),
        radius + std::max(1.5F, 2.0F * scale), 0.20F, 60, 60, 66);
    FillCirclePremul(pixels, width, height, fill, cy, radius + std::max(1.0F, 1.5F * scale), 0.99F,
        false);
}

}  // namespace dock_detail

QsModuleOrder DefaultQsModuleOrder() noexcept {
    QsModuleOrder order{};
    for (size_t index = 0; index < kQsModuleCount; ++index) {
        order[index] = static_cast<uint8_t>(index);
    }
    return order;
}

int QsModuleCols(size_t module) noexcept {
    return module < kQsModuleCount ? kQsModuleW[module] : 1;
}

int QsModuleRows(size_t module) noexcept {
    return module < kQsModuleCount ? kQsModuleH[module] : 1;
}

bool PackQsModules(const QsModuleOrder& order, QsGridCell (&cells)[kQsModuleCount], int& rows) noexcept {
    constexpr int kMaxRows = 32;
    bool used[kMaxRows][kQsGridCols]{};
    bool seen[kQsModuleCount]{};
    rows = 0;
    for (const uint8_t module : order) {
        if (module >= kQsModuleCount || seen[module]) {
            return false;
        }
        seen[module] = true;
        const int w = kQsModuleW[module];
        const int h = kQsModuleH[module];
        bool placed = false;
        for (int row = 0; row + h <= kMaxRows && !placed; ++row) {
            for (int col = 0; col + w <= kQsGridCols && !placed; ++col) {
                bool free = true;
                for (int dy = 0; dy < h && free; ++dy) {
                    for (int dx = 0; dx < w && free; ++dx) {
                        free = !used[row + dy][col + dx];
                    }
                }
                if (!free) {
                    continue;
                }
                for (int dy = 0; dy < h; ++dy) {
                    for (int dx = 0; dx < w; ++dx) {
                        used[row + dy][col + dx] = true;
                    }
                }
                cells[module] = {col, row};
                rows = std::max(rows, row + h);
                placed = true;
            }
        }
        if (!placed) {
            return false;
        }
    }
    return true;
}

std::wstring FormatQsModuleOrder(const QsModuleOrder& order) {
    std::wstring text;
    for (const uint8_t module : order) {
        if (module >= kQsModuleCount) {
            continue;
        }
        if (!text.empty()) {
            text += L',';
        }
        text += kQsModuleNames[module];
    }
    return text;
}

bool ParseQsModuleOrder(const std::wstring& text, QsModuleOrder& order) {
    std::vector<uint8_t> parsed;
    bool seen[kQsModuleCount]{};
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L',', start);
        if (end == std::wstring::npos) {
            end = text.size();
        }
        std::wstring name = text.substr(start, end - start);
        name.erase(0, name.find_first_not_of(L" \t"));
        const size_t last = name.find_last_not_of(L" \t");
        name.erase(last == std::wstring::npos ? 0 : last + 1);
        for (wchar_t& ch : name) {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
        for (size_t module = 0; module < kQsModuleCount; ++module) {
            if (!seen[module] && name == kQsModuleNames[module]) {
                seen[module] = true;
                parsed.push_back(static_cast<uint8_t>(module));
                break;
            }
        }
        start = end + 1;
    }
    for (size_t module = 0; module < kQsModuleCount; ++module) {
        if (!seen[module]) {
            parsed.push_back(static_cast<uint8_t>(module));
        }
    }
    QsModuleOrder candidate{};
    std::copy(parsed.begin(), parsed.end(), candidate.begin());
    QsGridCell cells[kQsModuleCount]{};
    int rows = 0;
    if (!PackQsModules(candidate, cells, rows) || rows != kQsGridRows) {
        return false;
    }
    order = candidate;
    return true;
}

void DockApp::LayoutQuickSettings(bool draw, uint8_t* pixels, int width, int height, HDC memory,
    float scale, LONG padding, LONG gearSize, LONG headerHeight, HFONT titleFont, HFONT sectionFont,
    HFONT labelFont, HFONT statusFont, LONG& panelWidth, LONG& contentBottom) {
    if (draw) {
        m_qsMeterValid = false;
        m_qsScrubValid = false;
        m_qsTempsValid = false;
        m_qsBoostStatusValid = false;
        m_qsBtStatusValid = false;
        m_qsMeterRect = {};
        m_qsScrubRect = {};
        m_qsMediaLiveRect = {};
        m_qsMediaTimeRect = {};
        for (RECT& button : m_qsMediaTransportRect) {
            button = {};
        }
        m_qsTempCpuRect = {};
        m_qsTempGpuRect = {};
        m_qsBoostStatusRect = {};
        m_qsBtStatusRect = {};
        for (RECT& chevron : m_qsTileChevron) {
            chevron = {};
        }
        m_qsModuleRectsValid = false;
        m_qsTrayStatusValid = false;
        m_qsTrayStatusRect = {};
        m_qsTrayArrowRects.clear();
    }
    SetQsAccent(m_config.QsIconColor());
    const bool home = m_qsPage == QuickSettingsPage::Home;
    panelWidth = std::max(320L, std::lround((home ? 600.0F : 380.0F) * scale));
    m_qsCcShapes.clear();
    m_qsCcShapes.push_back({RECT{0, 0, 0, 0}, 34.0F * scale});
    if (home) {
        // Control Centre grid: 4 x 63pt cells, 13pt gutters = 291pt of content.
        padding = std::max(12L, std::lround(20.0F * scale));
        panelWidth = padding * 2L + std::lround(291.0F * scale);
    }
    const LONG gap = std::max(8L, std::lround(10.0F * scale));
    const LONG sliderH = std::max(22L, std::lround(28.0F * scale));
    const LONG listRow = std::max(52L, std::lround(58.0F * scale));
    const LONG sectionH = std::max(22L, std::lround(26.0F * scale));
    // The CC panel face is always light (spec W/B tables), so panel ink is dark.
    const bool light = true;
    const uint8_t inkR = kInkDarkR;
    const uint8_t inkG = kInkDarkG;
    const uint8_t inkB = kInkDarkB;
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
            FillSquircleColorPremul(pixels, width, height, bounds, radius, 0.96F, g_qsAccentB, g_qsAccentG,
                g_qsAccentR);
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
                g_qsAccentB, g_qsAccentG, g_qsAccentR);
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
            FillPillColorPremul(pixels, width, height, cx, cx, cy, radius * 0.46F, 1.0F, g_qsAccentB,
                g_qsAccentG, g_qsAccentR);
        }
    };
    auto toggleRect = [&](LONG centerY, LONG right, bool enabled) {
        const LONG switchW = std::max(40L, std::lround(44.0F * scale));
        const LONG switchH = std::max(22L, std::lround(24.0F * scale));
        const LONG left = right - switchW;
        const LONG top = centerY - switchH / 2L;
        if (draw) {
            const float radius = static_cast<float>(switchH) * 0.5F;
            CcDrawSwitch(pixels, width, height, static_cast<float>(left) + radius,
                static_cast<float>(right) - radius, static_cast<float>(top) + radius, radius, scale,
                enabled, g_qsAccentR, g_qsAccentG, g_qsAccentB, inkR, inkG, inkB);
        }
        return RECT{left - 8, top - 6, right + 6, top + switchH + 6};
    };
    auto beginHeader = [&](const wchar_t* title, bool gear, int toggleIndex, bool toggleOn) {
        if (m_qsPage != QuickSettingsPage::Home) {
            const RECT back{padding, y, padding + headerHeight, y + headerHeight};
            // Mirror of the Home tile expand chevron (\uE76C), header-sized.
            const LONG backExtent = std::max(12L, std::lround(16.0F * scale));
            icon(back.left + (headerHeight - backExtent) / 2L, back.top + (headerHeight - backExtent) / 2L,
                L'\uE76B', static_cast<UINT>(backExtent), inkR, inkG, inkB);
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
        dock_detail::g_flyoutTextHalo = false;  // title sits on the CC glass face
        text({titleLeft, y, rightLimit, y + headerHeight}, titleFont, title,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
        dock_detail::g_flyoutTextHalo = false;
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
            // Links follow the dock's Quick Settings colour (QuickSettingsIconColor).
            SetFlyoutChromeInk(g_qsAccentR, g_qsAccentG, g_qsAccentB);
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
        // ---- macOS 26 Control Centre grid (pt scaled by DPI) ---------------
        // Cell 63, gutter 13 (pitch 76), 4 columns = 291 wide. Every module is a
        // separate glass shape baked by the renderer (DOCK_FX_CC); this pass
        // only lays out shapes, glyphs, text and live controls on top.
        auto P = [&](float pt) { return static_cast<LONG>(std::lround(pt * scale)); };
        const LONG gridTop = y + P(10.0F);
        auto cellRect = [&](int col, int row, int w, int h) {
            return QsGridCellRect(padding, gridTop, scale, col, row, w, h);
        };
        // Modules flow into the grid in the user's order (drag to rearrange).
        // Drawing stays in module order so glass shape indices are stable.
        SyncQsModuleOrder();
        QsGridCell cells[kQsModuleCount]{};
        int gridRows = 0;
        if (!PackQsModules(m_qsOrder, cells, gridRows)) {
            m_qsOrder = DefaultQsModuleOrder();
            static_cast<void>(PackQsModules(m_qsOrder, cells, gridRows));
        }
        if (draw) {
            m_qsGridLeft = padding;
            m_qsGridTop = gridTop;
            m_qsGridScale = scale;
            m_qsModuleRectsValid = true;
        }
        auto moduleRect = [&](QsModule module) {
            const size_t index = static_cast<size_t>(module);
            const RECT bounds = cellRect(cells[index].col, cells[index].row, kQsModuleW[index], kQsModuleH[index]);
            if (draw) {
                m_qsModuleRect[index] = bounds;
            }
            return bounds;
        };
        uint64_t inkSigNew = 0;
        struct CcInk {
            uint8_t r, g, b;
            bool dark;
        };
        auto addShape = [&](RECT bounds, float radius) {
            m_qsCcShapes.push_back({bounds, radius});
            return m_qsCcShapes.size() - 1U;
        };
        // Glyph/text ink follows the adaptive face luminance with hysteresis.
        auto ccInk = [&](size_t idx) {
            const std::vector<float>& faces = m_renderer.CcFaceLuma();
            const float face = idx < faces.size() ? faces[idx] : 0.80F;
            const bool prev = idx < 64U && ((m_qsCcInkSig >> idx) & 1ULL) != 0ULL;
            const bool dark = face > (prev ? 0.52F : 0.62F);
            if (dark && idx < 64U) {
                inkSigNew |= 1ULL << idx;
            }
            return dark ? CcInk{kInkDarkR, kInkDarkG, kInkDarkB, true}
                        : CcInk{kInkLightR, kInkLightG, kInkLightB, false};
        };
        uint8_t kOnAccentR = 30;
        uint8_t kOnAccentG = 33;
        uint8_t kOnAccentB = 24;
        CcOnAccentInk(g_qsAccentR, g_qsAccentG, g_qsAccentB, kOnAccentR, kOnAccentG, kOnAccentB);
        auto glyphDisc = [&](float cx, float cy, float radius, bool active, const CcInk& ink,
                             wchar_t symbol, UINT extent) {
            if (!draw) {
                return;
            }
            if (active) {
                FillPillColorPremul(pixels, width, height, cx, cx, cy, radius, 0.98F, g_qsAccentB, g_qsAccentG,
                    g_qsAccentR);
            } else {
                FillPillColorPremul(pixels, width, height, cx, cx, cy, radius, 0.16F, ink.b, ink.g,
                    ink.r);
            }
            const LONG left = static_cast<LONG>(std::lround(cx - static_cast<float>(extent) * 0.5F));
            const LONG top = static_cast<LONG>(std::lround(cy - static_cast<float>(extent) * 0.5F));
            if (active) {
                icon(left, top, symbol, extent, kOnAccentR, kOnAccentG, kOnAccentB);
            } else {
                icon(left, top, symbol, extent, ink.r, ink.g, ink.b);
            }
        };
        // Hover arrow for a Home tile (kQsChevronTiles order), centred in `area`.
        // Only the glyph is cached here; PaintQsTileChevrons fades it in.
        auto tileChevron = [&](int tile, RECT area, const CcInk& ink) {
            if (!draw || tile < 0 || static_cast<size_t>(tile) >= kQsChevronTiles) {
                return;
            }
            const UINT extent = static_cast<UINT>(std::max(10L, P(12.0F)));
            const size_t index = static_cast<size_t>(tile);
            m_qsTileChevron[index] = area;
            uint8_t* cachedInk = m_qsTileChevronInk[index];
            // Re-copy the glyph only when its extent or adaptive ink changes.
            if (m_qsTileChevronGlyph[index].empty() || m_qsTileChevronExtent != extent ||
                cachedInk[0] != ink.r || cachedInk[1] != ink.g || cachedInk[2] != ink.b) {
                cachedInk[0] = ink.r;
                cachedInk[1] = ink.g;
                cachedInk[2] = ink.b;
                m_qsTileChevronGlyph[index] = CachedGlyph(m_tray, L'\uE76C', extent, ink.r, ink.g, ink.b);
            }
            if (m_qsTileChevronExtent != extent) {
                // A DPI change invalidates every tile's cached glyph, not just this one.
                for (size_t other = 0; other < kQsChevronTiles; ++other) {
                    if (other != index) {
                        m_qsTileChevronGlyph[other].clear();
                    }
                }
            }
            m_qsTileChevronExtent = extent;
        };
        // 2x1 capsule: O36 glyph disc at 14pt inset, title + status to its right.
        // chevronTile >= 0 fades in a macOS style arrow on hover. With a pageKind
        // the glyph disc runs `kind` and the rest of the tile opens the page;
        // without one the whole tile is `kind`.
        auto capsule = [&](RECT b, wchar_t symbol, const wchar_t* title, const std::wstring& sub,
                           TrayFlyoutHitKind kind, bool active, int live, int chevronTile,
                           TrayFlyoutHitKind pageKind) {
            const float h = static_cast<float>(b.bottom - b.top);
            const size_t idx = addShape(b, h * 0.5F);
            const CcInk ink = ccInk(idx);
            const LONG cy = (b.top + b.bottom) / 2L;
            const float disc = static_cast<float>(P(36.0F));
            const float cx = static_cast<float>(b.left + P(14.0F)) + disc * 0.5F;
            glyphDisc(cx, static_cast<float>(cy), disc * 0.5F, active, ink, symbol,
                static_cast<UINT>(std::max(14L, P(19.0F))));
            if (draw) {
                SetFlyoutChromeInk(ink.r, ink.g, ink.b);
            }
            const LONG textL = static_cast<LONG>(std::lround(cx + disc * 0.5F)) + P(8.0F);
            // The title keeps the full width ("Bluetooth" needs ~61pt). The hover
            // arrow is a 12pt glyph centred 12pt in and vertically centred, so the
            // status line below it may run to 14pt in ("Tap to boost" ~66pt) and
            // only grazes the arrow's lower stroke while hovered.
            const LONG textR = b.right - P(10.0F);
            const LONG statusR = chevronTile >= 0 ? b.right - P(14.0F) : textR;
            text({textL, cy - P(21.0F), textR, cy + P(1.0F)}, labelFont, title,
                DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
            const RECT statusRect{textL, cy + P(1.0F), statusR, cy + P(21.0F)};
            if (live == 1) {
                if (draw) {
                    m_qsBoostStatusRect = statusRect;
                    m_qsBoostStatusValid = true;
                    m_qsBoostStatusInkR = ink.r;
                    m_qsBoostStatusInkG = ink.g;
                    m_qsBoostStatusInkB = ink.b;
                }
            } else if (live == 2) {
                if (draw) {
                    m_qsBtStatusRect = statusRect;
                    m_qsBtStatusValid = true;
                    m_qsBtStatusInkR = ink.r;
                    m_qsBtStatusInkG = ink.g;
                    m_qsBtStatusInkB = ink.b;
                }
            } else if (live == 3) {
                if (draw) {
                    m_qsTrayStatusRect = statusRect;
                    m_qsTrayStatusValid = true;
                    m_qsTrayStatusInkR = ink.r;
                    m_qsTrayStatusInkG = ink.g;
                    m_qsTrayStatusInkB = ink.b;
                }
            } else {
                text(statusRect, statusFont, sub, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 190);
            }
            if (pageKind != TrayFlyoutHitKind::None) {
                // macOS Control Centre: the glyph disc acts and the rest of the
                // module opens its menu. The disc hit is pushed first so it wins
                // over the whole-tile page hit.
                const LONG discHalf = static_cast<LONG>(std::lround(disc * 0.5F)) + P(2.0F);
                const LONG discCx = static_cast<LONG>(std::lround(cx));
                push(kind, {discCx - discHalf, cy - discHalf, discCx + discHalf, cy + discHalf});
                push(pageKind, b);
            } else {
                push(kind, b);
            }
            if (chevronTile >= 0) {
                // Arrow centred 12pt in from the edge, like the macOS module arrow.
                tileChevron(chevronTile, {std::max(b.left, b.right - P(24.0F)), b.top, b.right, b.bottom}, ink);
            }
        };
        auto accentSlider = [&](RECT track, float level, const CcInk& ink) {
            if (!draw) {
                return;
            }
            CcDrawSlider(pixels, width, height, track, level, scale, g_qsAccentR, g_qsAccentG,
                g_qsAccentB, ink.r, ink.g, ink.b);
        };
        auto sliderCapsule = [&](RECT b, wchar_t symbol, const wchar_t* title, const std::wstring& value,
                                 float level, TrayFlyoutHitKind sliderKind, bool sliderEnabled,
                                 TrayFlyoutHitKind drillKind, int chevronTile) {
            const float h = static_cast<float>(b.bottom - b.top);
            const size_t idx = addShape(b, h * 0.5F);
            const CcInk ink = ccInk(idx);
            const LONG padX = P(22.0F);
            const UINT extent = static_cast<UINT>(std::max(14L, P(18.0F)));
            if (draw) {
                SetFlyoutChromeInk(ink.r, ink.g, ink.b);
                icon(b.left + padX, b.top + P(10.0F), symbol, extent, ink.r, ink.g, ink.b);
            }
            const LONG rowTop = b.top + P(8.0F);
            const LONG rowBottom = rowTop + P(22.0F);
            text({b.left + padX + static_cast<LONG>(extent) + P(8.0F), rowTop, b.right - padX - P(56.0F), rowBottom},
                labelFont, title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
            text({b.right - padX - P(56.0F), rowTop, b.right - padX, rowBottom}, statusFont, value,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE, 190);
            const RECT track{b.left + padX, b.top + P(36.0F), b.right - padX, b.top + P(54.0F)};
            accentSlider(track, level, ink);
            if (sliderEnabled) {
                push(sliderKind, track);
            }
            push(drillKind, b);
            // Title-row arrow, 12pt in from the edge (right of the value text).
            tileChevron(chevronTile, {std::max(b.left, b.right - P(24.0F)), rowTop, b.right, rowBottom}, ink);
        };

        const bool wifiOn = tray.network == TrayNetworkKind::Wifi;
        std::wstring wifiLabel = L"Off";
        if (wifiOn) {
            wifiLabel = tray.networkName.empty() ? L"Connected" : tray.networkName;
        } else if (m_qsCache.wifiRadioOn) {
            wifiLabel = L"On";
        }
        const bool btOn = m_bluetoothSnapshot.radioOn;
        // Row 0: Wi-Fi + Bluetooth capsules.
        capsule(moduleRect(QsModule::Wifi), L'\uE701', L"Wi-Fi", wifiLabel, TrayFlyoutHitKind::WifiRadio,
            wifiOn || m_qsCache.wifiRadioOn, 0, 0, TrayFlyoutHitKind::WifiPage);
        capsule(moduleRect(QsModule::Bluetooth), L'\uE702', L"Bluetooth", std::wstring{},
            TrayFlyoutHitKind::BluetoothRadio, btOn, 2, 1, TrayFlyoutHitKind::BluetoothPage);
        // Row 1: Boost + Power capsules. The Boost disc runs a boost; the tile
        // opens the last-run report. Power opens its page from anywhere.
        capsule(moduleRect(QsModule::Boost), L'\uE945', L"Boost", m_boostStatus, TrayFlyoutHitKind::Boost, false, 1,
            2, TrayFlyoutHitKind::BoostPage);
        capsule(moduleRect(QsModule::Power), L'\uE83F', L"Power", EnergyModeLabel(m_qsCache.powerMode),
            TrayFlyoutHitKind::Power, false, 0, 3, TrayFlyoutHitKind::None);
        // Row 2: System Tray capsule (item count, live overlay) + Microphone.
        capsule(moduleRect(QsModule::SystemTray), L'\uE70E', L"System Tray", std::wstring{},
            TrayFlyoutHitKind::SystemTrayPage, false, 3, 6, TrayFlyoutHitKind::None);
        {
            const RECT b = moduleRect(QsModule::Microphone);
            const float h = static_cast<float>(b.bottom - b.top);
            const size_t idx = addShape(b, h * 0.5F);
            const CcInk ink = ccInk(idx);
            const LONG cy = (b.top + b.bottom) / 2L;
            const float disc = static_cast<float>(P(36.0F));
            const float cx = static_cast<float>(b.left + P(14.0F)) + disc * 0.5F;
            glyphDisc(cx, static_cast<float>(cy), disc * 0.5F, !m_qsCache.inputMuted, ink, L'\uE720',
                static_cast<UINT>(std::max(14L, P(19.0F))));
            if (draw) {
                SetFlyoutChromeInk(ink.r, ink.g, ink.b);
            }
            const LONG textL = static_cast<LONG>(std::lround(cx + disc * 0.5F)) + P(9.0F);
            // Leave the right edge for the hover arrow (tile 7), like the capsules.
            const LONG textR = b.right - P(22.0F);
            text({textL, cy - P(22.0F), textR, cy - P(2.0F)}, labelFont, L"Mic",
                DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
            const LONG barH = std::max(4L, P(5.0F));
            const RECT meter{textL, cy + P(6.0F), textR, cy + P(6.0F) + barH};
            if (draw) {
                m_qsMeterRect = meter;
                m_qsMeterValid = true;
                m_qsMeterInkR = ink.r;
                m_qsMeterInkG = ink.g;
                m_qsMeterInkB = ink.b;
                PaintMicPillMeter(pixels, width, height, meter, 0.0F, ink.r, ink.g, ink.b);
            }
            push(TrayFlyoutHitKind::Microphone, b);
            tileChevron(7, {std::max(b.left, b.right - P(24.0F)), b.top, b.right, b.bottom}, ink);
        }
        // Row 3: Sound slider capsule (4x1).
        sliderCapsule(moduleRect(QsModule::Sound), tray.volumeMuted ? L'\uE74F' : L'\uE767', L"Sound",
            std::wstring{}, volume,
            TrayFlyoutHitKind::VolumeSlider, true, TrayFlyoutHitKind::Sound, 4);
        // Row 4: Display slider capsule (4x1, brightness).
        const bool brightnessUsable = tray.brightnessAvailable || projected >= 0;
        sliderCapsule(moduleRect(QsModule::Display), L'\uE706', L"Display",
            brightnessUsable
                ? std::wstring{}
                : ((m_qsCache.nightKnown && m_qsCache.nightLight) ? std::wstring(L"Night light")
                                                                    : std::wstring(L"On")),
            brightness, TrayFlyoutHitKind::BrightnessSlider, brightnessUsable, TrayFlyoutHitKind::Display, 5);
        // Rows 5-6: CPU/GPU (2x2) and Now Playing (2x2).
        // CPU and GPU tiles (2x1 each). The reading lines ("CPU: 32°C | 35W",
        // one line per GPU) are live overlays (PaintQsLiveOverlays), so a new
        // sensor poll presents only these rects.
        auto tempsTile = [&](QsModule module, RECT& valueRect, uint8_t& outR, uint8_t& outG, uint8_t& outB) {
            const RECT b = moduleRect(module);
            const size_t idx = addShape(b, static_cast<float>(b.bottom - b.top) * 0.5F);
            const CcInk ink = ccInk(idx);
            if (draw) {
                valueRect = {b.left + P(16.0F), b.top + P(4.0F), b.right - P(16.0F), b.bottom - P(4.0F)};
                outR = ink.r;
                outG = ink.g;
                outB = ink.b;
            }
        };
        tempsTile(QsModule::Temps, m_qsTempCpuRect, m_qsTempsInkR, m_qsTempsInkG, m_qsTempsInkB);
        tempsTile(QsModule::Gpu, m_qsTempGpuRect, m_qsTempsGpuInkR, m_qsTempsGpuInkG, m_qsTempsGpuInkB);
        if (draw) {
            m_qsTempsValid = true;
        }
        {
            // Now Playing (2x2), after the macOS Control Centre module: artwork
            // with the source app and time beside it, title and "Artist -
            // Album" below, then scrub and filled transport glyphs. Scrub,
            // glyphs and time are live overlays (PaintQsLiveOverlays), so
            // play/pause, seeking and the clock never rebuild the panel.
            const RECT b = moduleRect(QsModule::Media);
            const size_t idx = addShape(b, static_cast<float>(P(30.0F)));
            const CcInk ink = ccInk(idx);
            if (draw) {
                SetFlyoutChromeInk(ink.r, ink.g, ink.b);
            }
            const LONG inset = P(14.0F);
            const LONG art = P(42.0F);
            const RECT artRect{b.left + inset, b.top + inset, b.left + inset + art, b.top + inset + art};
            if (draw) {
                const float corner = static_cast<float>(art) * 0.17F;
                const std::vector<uint8_t>& raster =
                    MediaArtRaster(m_qsCache.mediaArt, m_qsCache.mediaArtGen, static_cast<int>(art), corner);
                if (!raster.empty()) {
                    CompositePremul(pixels, width, height, static_cast<int>(artRect.left),
                        static_cast<int>(artRect.top), raster.data(), static_cast<int>(art), static_cast<int>(art));
                } else {
                    FillSquircleColorPremul(pixels, width, height, artRect, corner, 0.14F, ink.b, ink.g, ink.r);
                    const LONG note = std::max(12L, art / 2L);
                    icon(artRect.left + (art - note) / 2L, artRect.top + (art - note) / 2L, L'\uEC4F',
                        static_cast<UINT>(note), ink.r, ink.g, ink.b);
                }
            }
            const LONG textLeft = b.left + inset;
            const LONG textRight = b.right - inset;
            const LONG sideLeft = artRect.right + P(10.0F);
            if (m_qsCache.mediaHave && !m_qsCache.mediaApp.empty()) {
                text({sideLeft, artRect.top - P(1.0F), textRight, artRect.top + P(17.0F)}, statusFont,
                    m_qsCache.mediaApp, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 150);
            }
            const RECT timeRect{sideLeft, artRect.bottom - P(18.0F), textRight, artRect.bottom + P(1.0F)};
            std::wstring nowTitle = L"Not Playing";
            std::wstring nowSubtitle;
            if (m_qsCache.mediaHave) {
                nowTitle = !m_qsCache.mediaTitle.empty() ? m_qsCache.mediaTitle
                    : (!m_qsCache.mediaApp.empty() ? m_qsCache.mediaApp : std::wstring(L"Now Playing"));
                nowSubtitle = m_qsCache.mediaArtist;
                if (!m_qsCache.mediaAlbum.empty() && m_qsCache.mediaAlbum != m_qsCache.mediaTitle &&
                    m_qsCache.mediaAlbum != m_qsCache.mediaArtist) {
                    nowSubtitle += nowSubtitle.empty() ? m_qsCache.mediaAlbum : L" \u2013 " + m_qsCache.mediaAlbum;
                }
            }
            const LONG lineW = textRight - textLeft;
            const LONG titleTop = artRect.bottom + P(5.0F);
            const LONG titleH = P(18.0F);
            const LONG subtitleH = P(16.0F);
            text({textLeft, titleTop, textRight, titleTop + titleH}, labelFont,
                draw ? FitFlyoutLine(labelFont, nowTitle, lineW) : nowTitle,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
            text({textLeft, titleTop + titleH, textRight, titleTop + titleH + subtitleH}, statusFont,
                draw ? FitFlyoutLine(statusFont, nowSubtitle, lineW) : nowSubtitle,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 185);
            const LONG scrubH = std::max(4L, P(5.0F));
            const LONG scrubTop = b.top + P(100.0F);
            const RECT scrub{textLeft, scrubTop, textRight, scrubTop + scrubH};
            // Taller than the bar so it is easy to grab; same left/right so the
            // drag maps 1:1 onto the drawn track.
            const RECT scrubHit{scrub.left, scrubTop - P(6.0F), scrub.right, scrub.bottom + P(5.0F)};
            const LONG rowTop = scrubHit.bottom;
            const LONG rowBottom = b.bottom - P(3.0F);
            const LONG third = (b.right - b.left) / 3L;
            const RECT prev{b.left, rowTop, b.left + third, rowBottom};
            const RECT play{prev.right, rowTop, b.right - third, rowBottom};
            const RECT next{play.right, rowTop, b.right, rowBottom};
            if (draw) {
                m_qsScrubRect = scrub;
                m_qsScrubValid = true;
                m_qsScrubInkR = ink.r;
                m_qsScrubInkG = ink.g;
                m_qsScrubInkB = ink.b;
                m_qsMediaTimeRect = timeRect;
                m_qsMediaTransportRect[0] = prev;
                m_qsMediaTransportRect[1] = play;
                m_qsMediaTransportRect[2] = next;
                m_qsMediaLiveRect = {textLeft - P(6.0F), scrubHit.top, textRight + P(6.0F), rowBottom};
            }
            if (m_qsCache.mediaHave) {
                push(TrayFlyoutHitKind::MediaSeek, scrubHit);
            }
            push(TrayFlyoutHitKind::MediaTransport, prev, 0);
            push(TrayFlyoutHitKind::MediaTransport, play, 1);
            push(TrayFlyoutHitKind::MediaTransport, next, 2);
        }
        // macOS 26 "Edit Controls" capsule under the grid; it reads "Done" in
        // edit mode, where modules drag instead of acting.
        {
            const LONG gridBottom = cellRect(0, std::max(1, gridRows) - 1, 1, 1).bottom;
            const LONG buttonH = std::max(24L, P(28.0F));
            const LONG buttonW = P(112.0F);
            const LONG centreX = panelWidth / 2L;
            const LONG top = gridBottom + P(14.0F);
            const RECT button{centreX - buttonW / 2L, top, centreX + buttonW / 2L, top + buttonH};
            const size_t idx = addShape(button, static_cast<float>(buttonH) * 0.5F);
            const CcInk ink = ccInk(idx);
            // Same plain glass capsule in both states; only the label changes.
            if (draw) {
                SetFlyoutChromeInk(ink.r, ink.g, ink.b);
            }
            text(button, labelFont, m_qsEditMode ? L"Done" : L"Edit Controls",
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
            push(TrayFlyoutHitKind::QsEditControls, button);
            y = button.bottom + padding;
        }
        if (draw) {
            m_qsCcInkSig = inkSigNew;
        }
        contentBottom = y;
    m_qsCcShapes[0].bounds = RECT{0, 0, panelWidth, y};
        m_qsCcShapes[0].bounds = RECT{0, 0, panelWidth, y};
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

    // macOS Control Centre module menus (Wi-Fi / Bluetooth): flat rows on the
    // glass split by hairlines, round glyph discs, and a "Settings..." link.
    const LONG menuRow = std::max(40L, std::lround(44.0F * scale));
    const LONG menuInset = std::max(4L, std::lround(6.0F * scale));
    constexpr uint8_t dimR = 128;
    constexpr uint8_t dimG = 130;
    constexpr uint8_t dimB = 134;
    uint8_t onAccentR = 30;
    uint8_t onAccentG = 33;
    uint8_t onAccentB = 24;
    CcOnAccentInk(g_qsAccentR, g_qsAccentG, g_qsAccentB, onAccentR, onAccentG, onAccentB);
    auto hairline = [&]() {
        const LONG lineY = y + gap / 2L;
        if (draw) {
            FillRectColorPremul(pixels, width, height,
                {padding + menuInset, lineY, panelWidth - padding - menuInset,
                    lineY + std::max(1L, std::lround(scale))},
                0.14F, inkB, inkG, inkR);
        }
        y += gap;
    };
    auto menuSection = [&](const wchar_t* label) {
        text({padding + menuInset, y, panelWidth - padding - menuInset, y + sectionH}, sectionFont, label,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE, 150);
        y += sectionH;
    };
    auto menuNote = [&](const std::wstring& label) {
        text({padding + menuInset, y, panelWidth - padding - menuInset, y + menuRow}, labelFont, label,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 150);
        y += menuRow;
    };
    // Glyph disc + title, optional trailing status text and trailing glyph.
    // Active rows fill the disc with the accent; dim rows grey out the title.
    auto menuItem = [&](wchar_t symbol, bool active, bool dim, const std::wstring& title,
                        const std::wstring& trailing, wchar_t trailingGlyph, TrayFlyoutHitKind kind, int index) {
        const RECT row{padding, y, panelWidth - padding, y + menuRow};
        const float radius = static_cast<float>(std::max(12L, std::lround(15.0F * scale)));
        const float cx = static_cast<float>(row.left + menuInset) + radius;
        const float cy = 0.5F * static_cast<float>(row.top + row.bottom);
        const UINT extent = static_cast<UINT>(std::max(12L, std::lround(16.0F * scale)));
        if (draw) {
            if (active) {
                FillPillColorPremul(pixels, width, height, cx, cx, cy, radius, 0.98F, g_qsAccentB, g_qsAccentG,
                    g_qsAccentR);
            } else {
                FillPillColorPremul(pixels, width, height, cx, cx, cy, radius, dim ? 0.08F : 0.14F, inkB, inkG,
                    inkR);
            }
        }
        const LONG glyphLeft = static_cast<LONG>(std::lround(cx - static_cast<float>(extent) * 0.5F));
        const LONG glyphTop = static_cast<LONG>(std::lround(cy - static_cast<float>(extent) * 0.5F));
        if (active) {
            icon(glyphLeft, glyphTop, symbol, extent, onAccentR, onAccentG, onAccentB);
        } else if (dim) {
            icon(glyphLeft, glyphTop, symbol, extent, dimR, dimG, dimB);
        } else {
            icon(glyphLeft, glyphTop, symbol, extent, inkR, inkG, inkB);
        }
        LONG textRight = row.right - menuInset;
        if (trailingGlyph != 0) {
            const UINT trailExtent = static_cast<UINT>(std::max(10L, std::lround(13.0F * scale)));
            icon(textRight - static_cast<LONG>(trailExtent),
                static_cast<LONG>(std::lround(cy - static_cast<float>(trailExtent) * 0.5F)), trailingGlyph,
                trailExtent, dimR, dimG, dimB);
            textRight -= static_cast<LONG>(trailExtent) + std::lround(8.0F * scale);
        }
        if (!trailing.empty()) {
            const LONG trailW = std::lround(110.0F * scale);
            text({textRight - trailW, row.top, textRight, row.bottom}, statusFont, trailing,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 160);
            textRight -= trailW + std::lround(6.0F * scale);
        }
        const LONG textLeft = static_cast<LONG>(std::lround(cx + radius)) + std::lround(10.0F * scale);
        const LONG textW = (std::max)(0L, textRight - textLeft);
        text({textLeft, row.top, textRight, row.bottom}, labelFont,
            draw ? FitFlyoutLine(labelFont, title, textW) : title,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, dim ? 150 : 255);
        if (kind != TrayFlyoutHitKind::None) {
            push(kind, row, index);
        }
        y += menuRow;
    };
    // Grey section title with a right chevron that turns down when expanded.
    auto menuDisclosure = [&](const wchar_t* label, bool open, TrayFlyoutHitKind kind) {
        const RECT row{padding, y, panelWidth - padding, y + menuRow};
        const UINT extent = static_cast<UINT>(std::max(10L, std::lround(13.0F * scale)));
        text({row.left + menuInset, row.top, row.right - menuInset - static_cast<LONG>(extent) - 8, row.bottom},
            sectionFont, label, DT_LEFT | DT_VCENTER | DT_SINGLELINE, 150);
        icon(row.right - menuInset - static_cast<LONG>(extent), (row.top + row.bottom - static_cast<LONG>(extent)) / 2L,
            open ? L'\uE70D' : L'\uE76C', extent, dimR, dimG, dimB);
        push(kind, row);
        y += menuRow;
    };
    auto menuLink = [&](const wchar_t* label, int link) {
        const RECT row{padding, y, panelWidth - padding, y + menuRow};
        if (draw) {
            // Links follow the dock's Quick Settings colour (QuickSettingsIconColor).
            SetFlyoutChromeInk(g_qsAccentR, g_qsAccentG, g_qsAccentB);
            text({row.left + menuInset, row.top, row.right - menuInset, row.bottom}, labelFont, label,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE, 255);
            restoreInk();
        }
        push(TrayFlyoutHitKind::MoreSettings, row, link);
        y += menuRow;
    };

    if (m_qsPage == QuickSettingsPage::Wifi) {
        beginHeader(L"Wi-Fi", false, kToggleWifi, m_qsCache.wifiRadioOn);
        hairline();
        if (m_qsCache.wifiRadioOn) {
            // Filtered passes over the cache instead of per-layout index vectors:
            // connected first, then other saved profiles, then unsaved networks.
            static const std::wstring kHiddenNetwork = L"Hidden network";
            static const std::wstring kNoTrailing;
            const std::vector<QsWifiNetwork>& networks = m_qsCache.wifi;
            size_t knownCount = 0;
            for (const QsWifiNetwork& network : networks) {
                knownCount += (network.connected || network.hasProfile) ? 1U : 0U;
            }
            const size_t otherCount = networks.size() - knownCount;
            auto networkRows = [&](auto&& include) {
                for (size_t index = 0; index < networks.size(); ++index) {
                    const QsWifiNetwork& network = networks[index];
                    if (!include(network)) {
                        continue;
                    }
                    constexpr wchar_t kBars[4] = {L'\uE872', L'\uE873', L'\uE874', L'\uE701'};
                    menuItem(kBars[std::clamp(network.bars, 0, 3)], network.connected, false,
                        network.name.empty() ? kHiddenNetwork : network.name, kNoTrailing,
                        network.secure ? L'\uE72E' : L'\0', TrayFlyoutHitKind::WifiNetwork, static_cast<int>(index));
                }
            };
            if (networks.empty()) {
                menuNote(L"No networks found");
                hairline();
            }
            if (knownCount > 0) {
                menuSection(knownCount == 1 ? L"Known Network" : L"Known Networks");
                networkRows([](const QsWifiNetwork& network) { return network.connected; });
                networkRows([](const QsWifiNetwork& network) { return !network.connected && network.hasProfile; });
                hairline();
            }
            if (otherCount > 0) {
                menuDisclosure(L"Other Networks", m_qsWifiOthersOpen, TrayFlyoutHitKind::WifiOthers);
                if (m_qsWifiOthersOpen) {
                    networkRows([](const QsWifiNetwork& network) { return !network.connected && !network.hasProfile; });
                }
                hairline();
            }
        }
        // Internet speed test (any connection, so outside the radio block).
        {
            const SpeedTestService::Snapshot speed = m_speedTest.GetSnapshot();
            const bool testing = m_speedTest.Running();
            menuSection(L"Speed Test");
            if (testing) {
                const wchar_t* phase = speed.phase == SpeedTestService::Phase::Upload ? L"Testing upload\u2026"
                    : speed.phase == SpeedTestService::Phase::Download                ? L"Testing download\u2026"
                                                                                      : L"Testing latency\u2026";
                const std::wstring live = speed.liveValue < 0.0 ? std::wstring{}
                    : speed.phase == SpeedTestService::Phase::Latency ? FormatPingMs(speed.liveValue)
                                                                       : FormatMbps(speed.liveValue);
                menuItem(L'\uEC4A', true, false, phase, live, L'\0', TrayFlyoutHitKind::None, -1);
            } else {
                const bool failed = speed.phase == SpeedTestService::Phase::Failed;
                const bool haveResult = speed.phase == SpeedTestService::Phase::Done;
                menuItem(L'\uEC4A', false, false, haveResult || failed ? L"Run Again" : L"Run Speed Test",
                    failed ? speed.error : std::wstring{}, L'\0', TrayFlyoutHitKind::SpeedTest, 0);
            }
            if (!testing && speed.phase == SpeedTestService::Phase::Done) {
                const std::wstring results = L"\u2193 " + FormatMbps(speed.downloadMbps) + L"     \u2191 " +
                    FormatMbps(speed.uploadMbps) + L"     Ping " + FormatPingMs(speed.pingMs);
                text({padding + menuInset, y, panelWidth - padding - menuInset, y + menuRow}, labelFont, results,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
                y += menuRow;
            }
            hairline();
        }
        menuLink(L"Wi-Fi Settings\u2026", kLinkWifi);
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
                    index < lit ? g_qsAccentB : inkB, index < lit ? g_qsAccentG : inkG, index < lit ? g_qsAccentR : inkR);
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
        // macOS Battery menu: one flat glass face (no lighter/darker cards),
        // grey section titles, hairlines and glyph-disc rows.
        SYSTEM_POWER_STATUS power{};
        const bool powerKnown = GetSystemPowerStatus(&power) != FALSE;
        const bool onBattery = powerKnown && power.BatteryFlag != 128 && power.BatteryFlag != 255 &&
            power.BatteryLifePercent <= 100;
        const LONG headerTop = y;
        beginHeader(onBattery ? L"Battery" : L"Power", false, -1, false);
        if (onBattery) {
            text({padding + menuInset, headerTop, panelWidth - padding - menuInset, headerTop + headerHeight},
                titleFont, std::to_wstring(power.BatteryLifePercent) + L"%",
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE, 200);
            const wchar_t* source = power.ACLineStatus == 1 ? L"Power Source: Power Adapter"
                                                            : L"Power Source: Battery";
            text({padding + menuInset, y, panelWidth - padding - menuInset, y + sectionH}, statusFont, source,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 200);
            y += sectionH;
        }
        hairline();
        menuSection(L"Energy Mode");
        // Display order follows macOS; the hit index stays the power mode
        // (0 High, 1 Automatic, 2 Low).
        struct ModeRow {
            int mode;
            wchar_t glyph;
            const wchar_t* label;
        };
        static constexpr ModeRow kModeRows[3] = {
            {1, L'\uE83F', L"Automatic"},
            {2, L'\uE851', L"Low Power"},
            {0, L'\uE945', L"High Power"},
        };
        static const std::wstring kNoTrailing;
        for (const ModeRow& mode : kModeRows) {
            menuItem(mode.glyph, m_qsCache.powerMode == mode.mode, false, mode.label, kNoTrailing, 0,
                TrayFlyoutHitKind::PowerMode, mode.mode);
        }
        hairline();
        menuSection(L"Using Significant Energy");
        if (!m_qsCache.energyLive || m_qsCache.energyApps.empty()) {
            menuNote(m_qsCache.energyNote.empty()
                    ? std::wstring(m_qsCache.energyPending ? L"Measuring energy use\u2026"
                                                           : L"Energy usage unavailable")
                    : m_qsCache.energyNote);
        } else {
            const LONG appIcon = std::max(16L, std::lround(22.0F * scale));
            const LONG levelW = std::lround(80.0F * scale);
            for (const QsEnergyApp& energyApp : m_qsCache.energyApps) {
                const RECT row{padding, y, panelWidth - padding, y + menuRow};
                const LONG iconLeft = row.left + menuInset;
                const LONG iconTop = row.top + (menuRow - appIcon) / 2L;
                if (draw) {
                    HICON appHandle = EnergyAppIcon(energyApp.path);
                    if (appHandle != nullptr) {
                        BlitIcon(pixels, width, height, memory, appHandle, static_cast<int>(iconLeft),
                            static_cast<int>(iconTop), static_cast<int>(appIcon));
                    } else {
                        icon(iconLeft, iconTop, L'\uECAA', static_cast<UINT>(appIcon), inkR, inkG, inkB);
                    }
                }
                const LONG levelRight = row.right - menuInset;
                text({levelRight - levelW, row.top, levelRight, row.bottom}, statusFont,
                    PowerLevelLabel(energyApp.level), DT_RIGHT | DT_VCENTER | DT_SINGLELINE, 160);
                const LONG nameLeft = iconLeft + appIcon + std::lround(10.0F * scale);
                const LONG nameRight = levelRight - levelW - std::lround(6.0F * scale);
                text({nameLeft, row.top, nameRight, row.bottom}, labelFont,
                    draw ? FitFlyoutLine(labelFont, energyApp.name, (std::max)(0L, nameRight - nameLeft))
                         : energyApp.name,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
                y += menuRow;
            }
        }
        hairline();
        menuLink(L"Power Settings\u2026", kLinkPower);
        y += gap / 2L;

    } else if (m_qsPage == QuickSettingsPage::SystemTray) {
        // Windows' notification area as a Control Centre menu: every tray
        // item with its icon and status. Clicking a row is the item's normal
        // (left) click; the arrow that appears on hover opens its own menu,
        // which on the taskbar needs a right-click.
        beginHeader(L"System Tray", false, -1, false);
        {
            const std::wstring summary = TrayItemCountText();
            text({padding + menuInset, y, panelWidth - padding - menuInset, y + sectionH}, statusFont, summary,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 170);
            y += sectionH + 2;
        }
        hairline();
        const UINT arrowExtent = static_cast<UINT>(std::max(10L, std::lround(12.0F * scale)));
        if (draw) {
            m_qsTrayArrowExtent = arrowExtent;
            m_qsTrayArrowGlyph = CachedGlyph(m_tray, L'\uE76C', arrowExtent, inkR, inkG, inkB);
        }
        const LONG rowH = std::max(40L, std::lround(46.0F * scale));
        const LONG iconExtent = std::max(16L, std::lround(20.0F * scale));
        const LONG arrowW = std::max(28L, std::lround(34.0F * scale));
        if (!m_overflowIconsLoaded) {
            menuNote(L"Loading tray items\u2026");
        } else if (m_overflowIcons.empty()) {
            menuNote(L"No tray items");
        }
        // Keep the panel on screen with a very full tray.
        constexpr size_t kMaxTrayRows = 14;
        const size_t shown = std::min(m_overflowIcons.size(), kMaxTrayRows);
        for (size_t index = 0; index < shown; ++index) {
            const TrayNotifyIcon& item = m_overflowIcons[index];
            const RECT row{padding, y, panelWidth - padding, y + rowH};
            const RECT arrow{row.right - menuInset - arrowW, row.top + std::lround(4.0F * scale), row.right - menuInset,
                row.bottom - std::lround(4.0F * scale)};
            const LONG iconLeft = row.left + menuInset + std::lround(4.0F * scale);
            const LONG iconTop = row.top + (rowH - iconExtent) / 2L;
            if (draw) {
                if (item.icon != nullptr) {
                    BlitIcon(pixels, width, height, memory, item.icon, static_cast<int>(iconLeft),
                        static_cast<int>(iconTop), static_cast<int>(iconExtent));
                } else {
                    icon(iconLeft, iconTop, static_cast<wchar_t>(0xE8A5), static_cast<UINT>(iconExtent), inkR, inkG,
                        inkB);
                }
            }
            std::wstring title = NotifyIconTitle(item);
            const size_t paren = title.find(L" (");
            if (paren != std::wstring::npos && paren > 0) {
                title.resize(paren);
            }
            const std::wstring status = NotifyIconStatus(item);
            const LONG textLeft = iconLeft + iconExtent + std::lround(12.0F * scale);
            const LONG textRight = arrow.left - std::lround(4.0F * scale);
            if (status.empty()) {
                text({textLeft, row.top, textRight, row.bottom}, labelFont, title,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
            } else {
                const LONG mid = (row.top + row.bottom) / 2L;
                text({textLeft, row.top + 2, textRight, mid + 1}, labelFont, title,
                    DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
                text({textLeft, mid + 1, textRight, row.bottom - 2}, statusFont, status,
                    DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 160);
            }
            if (draw) {
                m_qsTrayArrowRects.push_back(arrow);
            }
            // Arrow first so it wins over the row hit. Its click target is
            // the full row height and well wider than the glyph area, so it
            // is easy to hit; the glyph itself stays put.
            const RECT arrowHit{std::max(textLeft, row.right - std::lround(64.0F * scale)), row.top, row.right,
                row.bottom};
            push(TrayFlyoutHitKind::TrayIconOptions, arrowHit, static_cast<int>(index));
            push(TrayFlyoutHitKind::NotifyIcon, row, static_cast<int>(index));
            y += rowH;
        }
        if (draw) {
            // Keep alphas across rebuilds of the same rows (no re-fade).
            m_qsTrayArrowAlpha.resize(m_qsTrayArrowRects.size(), 0.0F);
        }
        if (m_overflowIcons.size() > shown) {
            text({padding + menuInset, y, panelWidth - padding - menuInset, y + sectionH}, statusFont,
                L"+" + std::to_wstring(m_overflowIcons.size() - shown) + L" more in Taskbar settings",
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 150);
            y += sectionH;
        }
        hairline();
        menuLink(L"Taskbar Settings\u2026", kLinkTaskbar);
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
            m_bluetoothSnapshot.radioOn ? L"On" : L"Off", TrayFlyoutHitKind::MoreSettings, kLinkBluetoothPage);
        y += listRow * 2L + gap;
        footer(L"More network settings", kLinkNetwork);
    } else if (m_qsPage == QuickSettingsPage::Bluetooth) {
        // Bluetooth drill-in (Home tile or Airplane row). Every action
        // here only enqueues a BluetoothService job; WinRT radio / enumeration /
        // pair / connect / disconnect run on the BT worker, which posts
        // kBluetoothMessage back and ApplyBluetoothSnapshot queues the repaint.
        const BluetoothSnapshot& bt = m_bluetoothSnapshot;
        beginHeader(L"Bluetooth", false, -1, false);
        if (bt.ready && bt.radioPresent) {
            const RECT toggle = toggleRect(padding + headerHeight / 2L, panelWidth - padding, bt.radioOn);
            push(TrayFlyoutHitKind::BluetoothRadio, toggle);
        }
        hairline();
        if (!bt.hint.empty() || !bt.ready || !bt.radioPresent) {
            const std::wstring summary = BluetoothPageSummary(bt);
            const LONG summaryW = panelWidth - padding * 2L - menuInset * 2L;
            text({padding + menuInset, y, panelWidth - padding - menuInset, y + sectionH}, statusFont,
                draw ? FitFlyoutLine(statusFont, summary, summaryW) : summary,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 170);
            y += sectionH + 4;
        }
        // Worker status words that only restate the row's look stay hidden;
        // progress / errors ("Connecting...", "Couldn't connect") show trailing.
        static const std::wstring kNoTrailing;
        static const std::wstring kUnknownDevice = L"Unknown device";
        auto trailingStatus = [](const BluetoothDeviceInfo& device) -> const std::wstring& {
            if (device.status == L"Connected" || device.status == L"Not connected" ||
                device.status == L"Available") {
                return kNoTrailing;
            }
            return device.status;
        };
        // Glyph lookup lowercases the name; the measure pass draws nothing.
        auto deviceGlyph = [&](const std::wstring& name) { return draw ? BluetoothDeviceGlyph(name) : L'\uE702'; };
        if (bt.ready && bt.radioPresent && bt.radioOn) {
            const size_t paired = std::min(bt.paired.size(), static_cast<size_t>(6));
            if (paired == 0) {
                menuNote(L"No paired devices");
            }
            for (size_t index = 0; index < paired; ++index) {
                const BluetoothDeviceInfo& device = bt.paired[index];
                menuItem(deviceGlyph(device.name), device.connected, !device.connected && !device.busy,
                    device.name, trailingStatus(device), L'\0', TrayFlyoutHitKind::BluetoothConnect,
                    static_cast<int>(index));
            }
            if (bt.hiddenPaired > 0) {
                text({padding + menuInset, y, panelWidth - padding - menuInset, y + sectionH}, statusFont,
                    L"+" + std::to_wstring(bt.hiddenPaired) + L" more in Bluetooth settings",
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 160);
                y += sectionH;
            }
            hairline();
            menuDisclosure(L"Other Devices", m_qsBtOthersOpen, TrayFlyoutHitKind::BluetoothOthers);
            if (m_qsBtOthersOpen) {
                const size_t found = std::min(bt.discovered.size(), static_cast<size_t>(4));
                for (size_t index = 0; index < found; ++index) {
                    const BluetoothDeviceInfo& device = bt.discovered[index];
                    menuItem(deviceGlyph(device.name), false, false,
                        device.name.empty() ? kUnknownDevice : device.name, trailingStatus(device),
                        L'\0', TrayFlyoutHitKind::BluetoothPair, static_cast<int>(index));
                }
                if (bt.discovering) {
                    if (found == 0) {
                        menuNote(L"Searching\u2026");
                    }
                } else {
                    menuItem(L'\uE721', false, false, L"Search for devices", kNoTrailing, L'\0',
                        TrayFlyoutHitKind::BluetoothDiscover, -1);
                }
            }
            hairline();
        }
        menuLink(L"Bluetooth Settings\u2026", kLinkBluetooth);
    } else if (m_qsPage == QuickSettingsPage::RefreshRate) {
        // macOS Control Centre style menu: one row per rate for the display
        // Quick Settings is on, the current one filled with the accent.
        static const std::wstring kNoTrailing;
        beginHeader(L"Refresh Rate", false, -1, false);
        {
            std::wstring subtitle = m_qsCache.refreshMonitor.empty() ? std::wstring(L"Display") : m_qsCache.refreshMonitor;
            if (m_qsCache.refreshWidth > 0 && m_qsCache.refreshHeight > 0) {
                subtitle += L"  \u00B7  " + std::to_wstring(m_qsCache.refreshWidth) + L" \u00D7 " +
                    std::to_wstring(m_qsCache.refreshHeight);
            }
            const LONG summaryW = panelWidth - padding * 2L - menuInset * 2L;
            text({padding + menuInset, y, panelWidth - padding - menuInset, y + sectionH}, statusFont,
                draw ? FitFlyoutLine(statusFont, subtitle, summaryW) : subtitle,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 170);
            y += sectionH + 2;
        }
        hairline();
        const std::vector<int>& rates = m_qsCache.refreshRates;
        if (rates.empty()) {
            menuNote(m_qsCache.refreshDevice.empty() ? L"Checking display\u2026" : L"Refresh rate unavailable");
        }
        const int fastest = rates.empty() ? 0 : rates.front();
        for (const int hz : rates) {
            const bool pending = hz == m_qsRefreshPendingHz;
            const bool current = m_qsRefreshPendingHz == 0 ? hz == m_qsCache.refreshHz : pending;
            static const std::wstring kApplying = L"Applying\u2026";
            static const std::wstring kSmoothest = L"Smoothest";
            const std::wstring& trailing = pending ? kApplying : (hz == fastest && rates.size() > 1U ? kSmoothest : kNoTrailing);
            menuItem(L'\uEC4A', current, m_qsRefreshPendingHz != 0 && !pending, std::to_wstring(hz) + L" Hz",
                trailing, L'\0', TrayFlyoutHitKind::RefreshRate, hz);
        }
        if (!m_qsRefreshNote.empty()) {
            const LONG noteW = panelWidth - padding * 2L - menuInset * 2L;
            text({padding + menuInset, y, panelWidth - padding - menuInset, y + sectionH}, statusFont,
                draw ? FitFlyoutLine(statusFont, m_qsRefreshNote, noteW) : m_qsRefreshNote,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 170);
            y += sectionH;
        }
        hairline();
        menuLink(L"Advanced Display Settings\u2026", kLinkDisplayAdvanced);
    } else if (m_qsPage == QuickSettingsPage::Boost) {
        // Boost report: what the most recent run closed, freed and turned off,
        // in the same flat macOS menu style as the Wi-Fi / Bluetooth pages.
        static const std::wstring kNoTrailing;
        const bool running = m_boostInFlight.load();
        const BoostRunReport* run = m_boostReport.has_value() ? &*m_boostReport : nullptr;
        auto P = [&](float pt) { return static_cast<LONG>(std::lround(pt * scale)); };
        beginHeader(L"Boost", false, -1, false);
        hairline();
        {
            // Hero: accent bolt disc, headline result and when it ran.
            const RECT row{padding, y, panelWidth - padding, y + std::max(52L, P(60.0F))};
            const float radius = static_cast<float>(std::max(16L, P(20.0F)));
            const float cx = static_cast<float>(row.left + menuInset) + radius;
            const float cy = 0.5F * static_cast<float>(row.top + row.bottom);
            const UINT extent = static_cast<UINT>(std::max(14L, P(19.0F)));
            if (draw) {
                FillPillColorPremul(pixels, width, height, cx, cx, cy, radius, 0.98F, g_qsAccentB, g_qsAccentG,
                    g_qsAccentR);
            }
            icon(static_cast<LONG>(std::lround(cx - static_cast<float>(extent) * 0.5F)),
                static_cast<LONG>(std::lround(cy - static_cast<float>(extent) * 0.5F)), L'\uE945', extent,
                onAccentR, onAccentG, onAccentB);
            std::wstring headline = L"Ready to boost";
            std::wstring subline = L"Closes idle apps and trims startup to free memory.";
            if (running) {
                headline = L"Boosting\u2026";
                subline = m_boostStatus;
            } else if (run != nullptr) {
                headline = run->freedBytes > 0 ? L"Freed " + PerfBoost::FormatMegabytes(run->freedBytes)
                                               : run->outcome;
                subline = BoostAgeText(GetTickCount64() - m_boostReportTick);
            }
            const LONG textLeft = static_cast<LONG>(std::lround(cx + radius)) + P(12.0F);
            const LONG textRight = row.right - menuInset;
            text({textLeft, row.top, textRight, (row.top + row.bottom) / 2L + P(3.0F)}, titleFont, headline,
                DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
            text({textLeft, (row.top + row.bottom) / 2L + P(3.0F), textRight, row.bottom}, statusFont,
                draw ? FitFlyoutLine(statusFont, subline, std::max(0L, textRight - textLeft)) : subline,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 170);
            y = row.bottom + gap / 2L;
        }
        if (run != nullptr) {
            // Three stat tiles: memory freed, apps closed, startup apps turned off.
            int closedCount = 0;
            for (const BoostReportItem& item : run->closed) {
                closedCount += item.count;
            }
            const std::wstring stats[3][2] = {
                {run->freedBytes > 0 ? PerfBoost::FormatMegabytes(run->freedBytes) : L"0 MB", L"Freed"},
                {std::to_wstring(closedCount), closedCount == 1 ? L"App closed" : L"Apps closed"},
                {std::to_wstring(run->startupDisabled.size()), L"Startup off"},
            };
            const LONG statH = std::max(48L, P(54.0F));
            const LONG statGap = std::max(6L, P(8.0F));
            const LONG statLeft = padding + menuInset;
            const LONG statW = (panelWidth - padding - menuInset - statLeft - statGap * 2L) / 3L;
            for (int index = 0; index < 3; ++index) {
                const LONG left = statLeft + index * (statW + statGap);
                const RECT tile{left, y, left + statW, y + statH};
                if (draw) {
                    FillSquircleColorPremul(pixels, width, height, tile, ContentSquircleRadius(tile, scale), 0.07F,
                        inkB, inkG, inkR);
                }
                text({tile.left + 4, tile.top + P(6.0F), tile.right - 4, (tile.top + tile.bottom) / 2L + P(4.0F)},
                    labelFont, stats[index][0], DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS, 255);
                text({tile.left + 4, (tile.top + tile.bottom) / 2L + P(4.0F), tile.right - 4, tile.bottom - P(4.0F)},
                    statusFont, stats[index][1], DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS, 160);
            }
            y += statH + gap / 2L;
            hairline();

            constexpr size_t kMaxClosedRows = 4;
            constexpr size_t kMaxStartupRows = 3;
            menuSection(L"Apps closed");
            if (run->closed.empty()) {
                menuNote(L"No idle apps needed closing");
            }
            for (size_t index = 0; index < std::min(run->closed.size(), kMaxClosedRows); ++index) {
                const BoostReportItem& item = run->closed[index];
                menuItem(L'\uE71D', false, false, BoostAppLabel(item),
                    item.bytes > 0 ? PerfBoost::FormatMegabytes(item.bytes) : kNoTrailing, L'\0',
                    TrayFlyoutHitKind::None, -1);
            }
            if (run->closed.size() > kMaxClosedRows) {
                text({padding + menuInset, y, panelWidth - padding - menuInset, y + sectionH}, statusFont,
                    L"+" + std::to_wstring(run->closed.size() - kMaxClosedRows) + L" more",
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE, 150);
                y += sectionH;
            }
            menuSection(L"Startup apps turned off");
            if (run->startupDisabled.empty()) {
                menuNote(L"Startup apps left unchanged");
            }
            for (size_t index = 0; index < std::min(run->startupDisabled.size(), kMaxStartupRows); ++index) {
                menuItem(L'\uE7E8', false, false, run->startupDisabled[index], L"Off", L'\0',
                    TrayFlyoutHitKind::None, -1);
            }
            if (run->startupDisabled.size() > kMaxStartupRows) {
                text({padding + menuInset, y, panelWidth - padding - menuInset, y + sectionH}, statusFont,
                    L"+" + std::to_wstring(run->startupDisabled.size() - kMaxStartupRows) + L" more",
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE, 150);
                y += sectionH;
            }
            hairline();

            menuSection(L"Details");
            const LONG detailH = std::max(24L, P(28.0F));
            auto detail = [&](const wchar_t* label, const std::wstring& value) {
                const LONG left = padding + menuInset;
                const LONG right = panelWidth - padding - menuInset;
                const LONG valueW = (right - left) * 3L / 5L;
                text({left, y, right - valueW, y + detailH}, labelFont, label,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 220);
                text({right - valueW, y, right, y + detailH}, statusFont, value,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 160);
                y += detailH;
            };
            detail(L"Result", run->outcome);
            detail(L"Chosen by", run->method);
            detail(L"Checked", std::to_wstring(run->processesChecked) +
                    (run->processesChecked == 1 ? L" app, " : L" apps, ") + std::to_wstring(run->startupChecked) +
                    L" startup");
            detail(L"Took", BoostDurationText(run->durationMs));
            if (!run->note.empty()) {
                // Fallback / partial-result explanation, up to two wrapped lines.
                const LONG noteH = sectionH * 2L;
                text({padding + menuInset, y + 2, panelWidth - padding - menuInset, y + noteH}, statusFont,
                    run->note, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS, 150);
                y += noteH;
            }
            hairline();
        }
        menuItem(L'\uE945', false, running,
            running ? std::wstring(L"Boosting\u2026") : std::wstring(run != nullptr ? L"Boost Again" : L"Boost Now"),
            running ? m_boostStatus : kNoTrailing, L'\0', TrayFlyoutHitKind::Boost, -1);
        menuLink(L"Startup Apps Settings\u2026", kLinkStartupApps);
    } else {
        // macOS Control Centre Display module: brightness slider between two
        // suns, a row of round buttons, then a "Display Settings..." link.
        auto P = [&](float pt) { return static_cast<LONG>(std::lround(pt * scale)); };
        beginHeader(L"Display", false, -1, false);
        hairline();
        {
            const LONG rowH = std::max(36L, P(42.0F));
            const RECT row{padding + menuInset, y, panelWidth - padding - menuInset, y + rowH};
            const LONG cy = (row.top + row.bottom) / 2L;
            const UINT sunSmall = static_cast<UINT>(std::max(12L, P(14.0F)));
            const UINT sunLarge = static_cast<UINT>(std::max(16L, P(20.0F)));
            icon(row.left, cy - static_cast<LONG>(sunSmall) / 2L, L'\uE706', sunSmall, inkR, inkG, inkB);
            icon(row.right - static_cast<LONG>(sunLarge), cy - static_cast<LONG>(sunLarge) / 2L, L'\uE706', sunLarge, inkR,
                inkG, inkB);
            const LONG trackH = std::max(14L, P(18.0F));
            const RECT track{row.left + static_cast<LONG>(sunSmall) + P(12.0F), cy - trackH / 2L,
                row.right - static_cast<LONG>(sunLarge) - P(12.0F), cy - trackH / 2L + trackH};
            if (draw) {
                CcDrawSlider(pixels, width, height, track, brightness, scale, g_qsAccentR, g_qsAccentG, g_qsAccentB,
                    inkR, inkG, inkB);
            }
            // Pushed even when DDC/WMI brightness is unavailable: a click then
            // opens Windows display settings (ApplyQuickSettingsSlider).
            push(TrayFlyoutHitKind::BrightnessSlider, track);
            y = row.bottom + P(6.0F);
        }
        {
            struct DisplayButton {
                wchar_t glyph;
                const wchar_t* title;
                std::wstring status;
                bool active;
                bool enabled;
                TrayFlyoutHitKind kind;
                int index;
            };
            const DisplayButton buttons[] = {
                {L'\uE7A1', L"Dark Mode",
                    m_qsCache.darkKnown ? std::wstring(m_qsCache.darkMode ? L"On" : L"Off") : std::wstring(L"\u2014"),
                    m_qsCache.darkMode, m_qsCache.darkKnown, TrayFlyoutHitKind::Toggle, kToggleDarkMode},
                {L'\uE708', L"Night Light",
                    m_qsCache.nightKnown ? std::wstring(m_qsCache.nightLight ? L"On" : L"Off")
                                         : std::wstring(L"Settings"),
                    m_qsCache.nightKnown && m_qsCache.nightLight, true, TrayFlyoutHitKind::MoreSettings, kLinkNight},
                {L'\uEC4A', L"Refresh Rate",
                    m_qsCache.refreshHz > 0 ? std::to_wstring(m_qsCache.refreshHz) + L" Hz" : std::wstring(L"\u2014"),
                    false, true, TrayFlyoutHitKind::Toggle, kToggleRefreshRate},
                {L'\uE790', L"HDR",
                    m_qsCache.hdrSupported ? std::wstring(m_qsCache.hdrOn ? L"On" : L"Off")
                                           : std::wstring(L"Unavailable"),
                    m_qsCache.hdrSupported && m_qsCache.hdrOn, m_qsCache.hdrSupported, TrayFlyoutHitKind::Toggle,
                    kToggleHdr},
            };
            constexpr LONG kColumns = static_cast<LONG>(sizeof(buttons) / sizeof(buttons[0]));
            const LONG left = padding;
            const LONG colW = (panelWidth - padding * 2L) / kColumns;
            const float radius = static_cast<float>(std::max(18L, P(23.0F)));
            const UINT extent = static_cast<UINT>(std::max(14L, P(19.0F)));
            const LONG discTop = y + P(4.0F);
            const LONG titleTop = discTop + static_cast<LONG>(radius * 2.0F) + P(6.0F);
            const LONG titleH = P(20.0F);
            const LONG statusH = P(18.0F);
            for (LONG column = 0; column < kColumns; ++column) {
                const DisplayButton& button = buttons[column];
                const LONG colLeft = left + column * colW;
                const LONG colRight = colLeft + colW;
                const float cx = static_cast<float>(colLeft) + static_cast<float>(colW) * 0.5F;
                const float cy = static_cast<float>(discTop) + radius;
                if (draw) {
                    if (button.active) {
                        FillPillColorPremul(pixels, width, height, cx, cx, cy, radius, 0.98F, g_qsAccentB,
                            g_qsAccentG, g_qsAccentR);
                    } else {
                        FillPillColorPremul(pixels, width, height, cx, cx, cy, radius,
                            button.enabled ? 0.14F : 0.07F, inkB, inkG, inkR);
                    }
                }
                const LONG glyphLeft = static_cast<LONG>(std::lround(cx - static_cast<float>(extent) * 0.5F));
                const LONG glyphTop = static_cast<LONG>(std::lround(cy - static_cast<float>(extent) * 0.5F));
                if (button.active) {
                    icon(glyphLeft, glyphTop, button.glyph, extent, onAccentR, onAccentG, onAccentB);
                } else if (button.enabled) {
                    icon(glyphLeft, glyphTop, button.glyph, extent, inkR, inkG, inkB);
                } else {
                    icon(glyphLeft, glyphTop, button.glyph, extent, dimR, dimG, dimB);
                }
                text({colLeft + 2, titleTop, colRight - 2, titleTop + titleH}, labelFont, button.title,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, button.enabled ? 255 : 150);
                text({colLeft + 2, titleTop + titleH, colRight - 2, titleTop + titleH + statusH}, statusFont,
                    button.status, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, 160);
                if (button.enabled) {
                    push(button.kind, {colLeft, discTop - P(4.0F), colRight, titleTop + titleH + statusH + P(4.0F)},
                        button.index);
                }
            }
            y = titleTop + titleH + statusH + P(10.0F);
        }
        hairline();
        menuLink(L"Display Settings\u2026", kLinkDisplay);
    }
    y += padding;
    contentBottom = y;
    m_qsCcShapes[0].bounds = RECT{0, 0, panelWidth, y};
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
    case TrayFlyoutHitKind::QsEditControls:
    case TrayFlyoutHitKind::TrayIconOptions:
        break;
    case TrayFlyoutHitKind::Back:
        CloseQuickSettingsPage();
        break;
    case TrayFlyoutHitKind::BluetoothPage:
        // Home Bluetooth tile (outside the glyph disc). Right-click opens Windows Settings.
        navigate(QuickSettingsPage::Bluetooth, L"ms-settings:bluetooth");
        break;
    case TrayFlyoutHitKind::WifiPage:
        // Home Wi-Fi tile (outside the glyph disc). Right-click opens Windows Settings.
        navigate(QuickSettingsPage::Wifi, L"ms-settings:network-wifi");
        break;
    case TrayFlyoutHitKind::BoostPage:
        // Home Boost tile (outside the glyph disc): last-run report.
        navigate(QuickSettingsPage::Boost, L"ms-settings:startupapps");
        break;
    case TrayFlyoutHitKind::SpeedTest:
        if (!m_speedTest.Running() && m_window != nullptr) {
            m_speedTest.Run(m_window, kSpeedTestMessage);
        }
        QueueOverflowPaint();
        break;
    case TrayFlyoutHitKind::RefreshRate: {
        const int hz = hit.index;
        const std::wstring device = m_qsCache.refreshDevice;
        if (alternate || device.empty()) {
            openUri(L"ms-settings:display-advanced");
            break;
        }
        if (hz <= 1 || hz == m_qsCache.refreshHz || m_qsRefreshPendingHz != 0) {
            break;
        }
        // Mode switches block for ~1 s while the display retrains: worker
        // only. The row shows "Applying" until kQsRefreshResultMessage.
        m_qsRefreshPendingHz = hz;
        m_qsRefreshNote.clear();
        const HWND notify = m_overflowWindow;
        std::thread([device, hz, notify] {
            const bool ok = SetRefreshRate(device, hz);
            if (notify != nullptr) {
                PostMessageW(notify, kQsRefreshResultMessage, static_cast<WPARAM>(hz), ok ? 1 : 0);
            }
        }).detach();
        m_overflowHover = -1;
        m_overflowHoverDirtyValid = false;
        QueueOverflowPaint();
        break;
    }
    case TrayFlyoutHitKind::WifiRadio:
        // Home Wi-Fi tile glyph disc: flip the radio like the Bluetooth disc.
        // Right-click keeps the old shortcut to the Windows network flyout.
        if (alternate) {
            CloseOverflowPopup();
            if (!SystemTray::OpenNetworkPanel()) {
                Log(L"Network panel did not open.");
            }
            break;
        }
        m_qsCache.wifiRadioOn = !m_qsCache.wifiRadioOn;
        SetWifiRadioAsync(m_qsCache.wifiRadioOn, m_overflowWindow, kQsCacheResultMessage, m_qsPage);
        QueueOverflowPaint();
        break;
    case TrayFlyoutHitKind::WifiOthers:
        m_qsWifiOthersOpen = !m_qsWifiOthersOpen;
        m_overflowHover = -1;
        m_overflowHoverDirtyValid = false;
        QueueOverflowPaint();
        break;
    case TrayFlyoutHitKind::BluetoothOthers:
        // Expanding "Other Devices" starts a scan; collapsing stops it.
        m_lastBluetoothUiAt = QpcSeconds();
        m_qsBtOthersOpen = !m_qsBtOthersOpen;
        if (m_qsBtOthersOpen && !m_bluetoothSnapshot.discovering && m_bluetoothSnapshot.radioOn) {
            if (m_window != nullptr) {
                SetTimer(m_window, kBluetoothTimerId, 4000, nullptr);
            }
            m_bluetoothSnapshot.discovering = true;
            m_bluetoothSnapshot.hint = L"Searching...";
            m_bluetooth.StartDiscovery();
        } else if (!m_qsBtOthersOpen && m_bluetoothSnapshot.discovering && !m_bluetooth.IsPairing()) {
            m_bluetoothSnapshot.discovering = false;
            m_bluetoothSnapshot.discovered.clear();
            m_bluetoothSnapshot.hint.clear();
            m_bluetooth.StopDiscovery();
        }
        m_overflowHover = -1;
        m_overflowHoverDirtyValid = false;
        QueueOverflowPaint();
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
            // Optimistic flip; the worker applies it and the 1 Hz tray tick
            // picks up the new network state for the dock.
            m_qsCache.wifiRadioOn = !m_qsCache.wifiRadioOn;
            SetWifiRadioAsync(m_qsCache.wifiRadioOn, m_overflowWindow, kQsCacheResultMessage, m_qsPage);
            QueueOverflowPaint();
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
        case kToggleAirplane: {
            const bool radiosOn = !m_qsCache.wifiRadioOn &&
                (!m_bluetoothSnapshot.radioPresent || !m_bluetoothSnapshot.radioOn);
            m_qsCache.wifiRadioOn = radiosOn;
            SetWifiRadioAsync(radiosOn, m_overflowWindow, kQsCacheResultMessage, m_qsPage);
            if (m_bluetoothSnapshot.radioPresent) {
                m_bluetoothSnapshot.radioOn = radiosOn;
                m_bluetoothSnapshot.discovering = radiosOn ? m_bluetoothSnapshot.discovering : false;
                if (!radiosOn) {
                    m_bluetoothSnapshot.discovered.clear();
                }
                m_bluetooth.SetRadioEnabled(radiosOn);
            }
            QueueOverflowPaint();
            break;
        }
        case kToggleDarkMode: {
            // Optimistic flip; the registry write + broadcast run on a worker,
            // which then refreshes the cache to confirm.
            const bool dark = !m_qsCache.darkMode;
            m_qsCache.darkMode = dark;
            m_qsCache.darkKnown = true;
            const HWND notify = m_overflowWindow;
            const QuickSettingsPage page = m_qsPage;
            std::thread([dark, notify, page] {
                static_cast<void>(SetDarkMode(dark));
                RequestQuickSettingsCacheAsync(notify, kQsCacheResultMessage, page);
            }).detach();
            QueueOverflowPaint();
            break;
        }
        case kToggleRefreshRate:
            // Control Centre style: the button opens a menu of rates for this
            // display. Right-click keeps the shortcut to Windows settings.
            navigate(QuickSettingsPage::RefreshRate, L"ms-settings:display-advanced");
            break;
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
        case kLinkBluetoothPage:
            OpenQuickSettingsPage(QuickSettingsPage::Bluetooth);
            break;
        case kLinkStartupApps:
            openUri(L"ms-settings:startupapps");
            break;
        case kLinkDisplayAdvanced:
            openUri(L"ms-settings:display-advanced");
            break;
        default:
            break;
        }
        break;
    case TrayFlyoutHitKind::MediaTransport:
        // Optimistic snapshot: the glyph/time flip on this frame via a live
        // present; GSMTC runs on the media worker.
        ControlMediaSession(hit.index == 0 ? 0 : (hit.index == 2 ? 2 : 1));
        if (RefreshQuickSettingsMedia(m_qsCache) == QsLiveChange::Labels) {
            QueueOverflowPaint();
        } else {
            PaintOverflowLiveFast();
        }
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

// ---- Home module drag-to-rearrange ----------------------------------------

void DockApp::SyncQsModuleOrder() {
    const std::wstring& layout = m_config.QsLayout();
    if (m_qsOrderSynced && layout == m_qsOrderSource) {
        return;
    }
    m_qsOrderSynced = true;
    m_qsOrderSource = layout;
    QsModuleOrder parsed = DefaultQsModuleOrder();
    if (!layout.empty() && !ParseQsModuleOrder(layout, parsed)) {
        parsed = DefaultQsModuleOrder();
    }
    m_qsOrder = parsed;
}

int DockApp::QsModuleAt(POINT point) const noexcept {
    if (!m_qsModuleRectsValid || m_qsPage != QuickSettingsPage::Home) {
        return -1;
    }
    for (size_t module = 0; module < kQsModuleCount; ++module) {
        const RECT& bounds = m_qsModuleRect[module];
        if (point.x >= bounds.left && point.x < bounds.right && point.y >= bounds.top && point.y < bounds.bottom) {
            return static_cast<int>(module);
        }
    }
    return -1;
}

RECT DockApp::QsModuleSlot(size_t module, const QsGridCell& cell) const noexcept {
    return QsGridCellRect(m_qsGridLeft, m_qsGridTop, m_qsGridScale, cell.col, cell.row, QsModuleCols(module),
        QsModuleRows(module));
}

bool DockApp::BeginQsReorder(int module, POINT cursor) {
    if (m_qsReorderActive || module < 0 || static_cast<size_t>(module) >= kQsModuleCount ||
        m_qsPage != QuickSettingsPage::Home || !m_qsModuleRectsValid || m_overflowWindow == nullptr ||
        m_qsCcDescs.empty()) {
        return false;
    }
    const LONG width = m_overflowPresentSize.cx;
    const LONG height = m_overflowPresentSize.cy;
    const size_t bytes = static_cast<size_t>(std::max(0L, width)) * static_cast<size_t>(std::max(0L, height)) * 4U;
    if (bytes == 0 || m_overflowBaseBits.size() != bytes || m_overflowPresentBits.size() != bytes ||
        width != m_overflowSize.cx || height != m_overflowSize.cy) {
        return false;
    }
    QsGridCell cells[kQsModuleCount]{};
    int rows = 0;
    if (!PackQsModules(m_qsOrder, cells, rows)) {
        return false;
    }
    POINT origin{};
    LONG caret = m_overflowCaretX;
    if (!OverflowScreenOrigin(origin, caret)) {
        return false;
    }
    // Tile-free backdrop: the same panel glass baked with only shape 0. Tile
    // faces are a fixed colour, so moved snapshots match a real rebake.
    std::vector<uint8_t> panel(bytes);
    const std::vector<Renderer::CcShapeDesc> shapes = m_qsCcDescs;
    m_qsCcDescs.assign(shapes.begin(), shapes.begin() + 1);
    const bool baked = TryBakePopupGlass(origin, width, height, panel.data(), panel.size(), true, true);
    m_qsCcDescs = shapes;
    m_renderer.SetCcShapes(shapes);
    if (!baked) {
        Log(L"Quick Settings reorder: panel glass bake unavailable.");
        return false;
    }

    const uint8_t* base = m_overflowBaseBits.data();
    m_qsReorderBackdrop = m_overflowBaseBits;
    RECT bounds = m_qsModuleRect[0];
    for (size_t index = 0; index < kQsModuleCount; ++index) {
        const RECT& rect = m_qsModuleRect[index];
        bounds.left = std::min(bounds.left, rect.left);
        bounds.top = std::min(bounds.top, rect.top);
        bounds.right = std::max(bounds.right, rect.right);
        bounds.bottom = std::max(bounds.bottom, rect.bottom);
        QsReorderSprite& sprite = m_qsReorderSprites[index];
        sprite.width = std::max(0L, rect.right - rect.left);
        sprite.height = std::max(0L, rect.bottom - rect.top);
        sprite.radius = QsModuleRadius(index, rect, m_qsGridScale);
        // Planes carry a 2px border for CcGlass's dark outline ring, which is
        // drawn just outside the shape and would otherwise vanish on a move.
        constexpr LONG kEdge = QsReorderSprite::kEdge;
        const LONG fullW = sprite.width + kEdge * 2L;
        const LONG fullH = sprite.height + kEdge * 2L;
        const size_t count = static_cast<size_t>(fullW) * static_cast<size_t>(fullH);
        sprite.bits.assign(count * 4U, 0);
        sprite.cover.assign(count, 0);
        sprite.ring.assign(count, 0);
        // Same rounded-rect SDF, 0.5px AA band and ring as CcGlass.
        const float hw = 0.5F * static_cast<float>(sprite.width);
        const float hh = 0.5F * static_cast<float>(sprite.height);
        const float r = std::min({sprite.radius, hw, hh});
        for (LONG y = 0; y < fullH; ++y) {
            const LONG sy = rect.top - kEdge + y;
            if (sy < 0 || sy >= height) {
                continue;
            }
            const float qy = std::fabs(static_cast<float>(y - kEdge) + 0.5F - hh) - (hh - r);
            for (LONG x = 0; x < fullW; ++x) {
                const LONG sx = rect.left - kEdge + x;
                if (sx < 0 || sx >= width) {
                    continue;
                }
                const float qx = std::fabs(static_cast<float>(x - kEdge) + 0.5F - hw) - (hw - r);
                const float ox = std::max(qx, 0.0F);
                const float oy = std::max(qy, 0.0F);
                const float sd = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0F) - r;
                const float cov = std::clamp(0.5F - sd, 0.0F, 1.0F);
                const float ringA = sd > 0.0F ? std::clamp(1.0F - std::fabs(sd - 0.9F), 0.0F, 1.0F) * 0.16F : 0.0F;
                const size_t at = static_cast<size_t>(y) * static_cast<size_t>(fullW) + static_cast<size_t>(x);
                sprite.cover[at] = static_cast<uint8_t>(std::lround(cov * 255.0F));
                sprite.ring[at] = static_cast<uint8_t>(std::lround(ringA * 255.0F));
                if (cov <= 0.0F) {
                    continue;
                }
                const uint8_t* src = base + (static_cast<size_t>(sy) * static_cast<size_t>(width) +
                                                static_cast<size_t>(sx)) * 4U;
                // AA edge pixels already carry the ring in base: undo it here
                // so the blit's ring pass does not darken them twice.
                const float unring = 1.0F / (1.0F - ringA);
                for (size_t channel = 0; channel < 4U; ++channel) {
                    const float value = channel < 3U ? static_cast<float>(src[channel]) * unring
                                                     : static_cast<float>(src[channel]);
                    sprite.bits[at * 4U + channel] =
                        static_cast<uint8_t>(std::clamp(value * cov + 0.5F, 0.0F, 255.0F));
                }
            }
        }
        // Backdrop: tile (plus its 1-2px AA ring) replaced by bare panel glass.
        const LONG ring = 3;
        for (LONG y = std::max(0L, rect.top - ring); y < std::min(height, rect.bottom + ring); ++y) {
            const LONG left = std::max(0L, rect.left - ring);
            const LONG right = std::min(width, rect.right + ring);
            if (right <= left) {
                continue;
            }
            const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(left)) * 4U;
            std::memcpy(m_qsReorderBackdrop.data() + offset, panel.data() + offset,
                static_cast<size_t>(right - left) * 4U);
        }
        m_qsReorderPos[index][0] = static_cast<float>(rect.left);
        m_qsReorderPos[index][1] = static_cast<float>(rect.top);
        m_qsReorderVel[index][0] = 0.0F;
        m_qsReorderVel[index][1] = 0.0F;
    }
    m_qsReorderBounds = bounds;
    // The lifted tile grows ~6% and casts a ~14pt blurred shadow 7pt down;
    // tiles themselves never leave the grid box.
    const LONG margin = std::max(24L, std::lround(30.0F * m_qsGridScale));
    m_qsReorderDirty = RECT{std::max(0L, bounds.left - margin), std::max(0L, bounds.top - margin),
        std::min(width, bounds.right + margin), std::min(height, bounds.bottom + margin)};

    const RECT& grabbed = m_qsModuleRect[static_cast<size_t>(module)];
    m_qsReorderGrab = POINT{cursor.x - grabbed.left, cursor.y - grabbed.top};
    m_qsReorderCursor = cursor;
    m_qsReorderModule = module;
    m_qsReorderRows = rows;
    m_qsReorderOrder = m_qsOrder;
    m_qsReorderStartOrder = m_qsOrder;
    m_qsReorderLift = 0.0F;
    m_qsReorderScale = 1.0F;
    m_qsReorderScaleVel = 0.0F;
    {
        // Soft lift shadow: the lifted tile's coverage, padded and blurred once.
        const QsReorderSprite& lifted = m_qsReorderSprites[static_cast<size_t>(module)];
        const LONG pad = std::max(8L, std::lround(14.0F * m_qsGridScale));
        const LONG liftedW = lifted.width + QsReorderSprite::kEdge * 2L;
        const LONG liftedH = lifted.height + QsReorderSprite::kEdge * 2L;
        const LONG shadowW = liftedW + pad * 2L;
        const LONG shadowH = liftedH + pad * 2L;
        m_qsReorderShadowPad = pad;
        m_qsReorderShadow.assign(static_cast<size_t>(shadowW) * static_cast<size_t>(shadowH), 0);
        for (LONG y = 0; y < liftedH; ++y) {
            std::memcpy(m_qsReorderShadow.data() + static_cast<size_t>(y + pad) * static_cast<size_t>(shadowW) +
                    static_cast<size_t>(pad),
                lifted.cover.data() + static_cast<size_t>(y) * static_cast<size_t>(liftedW),
                static_cast<size_t>(liftedW));
        }
        BlurAlphaPlane(m_qsReorderShadow, shadowW, shadowH, static_cast<int>(std::max(2L, pad / 3L)));
    }
    m_qsReorderActive = true;
    m_qsReorderDropped = false;
    m_qsReorderPaintPending = false;
    m_qsDragging = false;
    // Present is rebuilt from base so no hover plate / tile arrow survives.
    m_overflowHover = -1;
    m_overflowHoverDirtyValid = false;
    ResetQsChevronFade();
    std::memcpy(m_overflowPresentBits.data(), m_overflowBaseBits.data(), bytes);
    m_overflowLayerDib.contentValid = false;
    m_qsReorderLastMs = GetTickCount64();
    SetTimer(m_overflowWindow, kQsReorderTimerId, 16, nullptr);
    PresentQsReorderFrame();
    return true;
}

void DockApp::UpdateQsReorder(POINT cursor) {
    if (!m_qsReorderActive || m_qsReorderDropped) {
        return;
    }
    m_qsReorderCursor = cursor;
    const size_t dragged = static_cast<size_t>(m_qsReorderModule);
    const QsReorderSprite& sprite = m_qsReorderSprites[dragged];
    // The dragged tile tracks the cursor directly, clamped to the grid box.
    m_qsReorderPos[dragged][0] = static_cast<float>(std::clamp(cursor.x - m_qsReorderGrab.x,
        m_qsReorderBounds.left, std::max(m_qsReorderBounds.left, m_qsReorderBounds.right - sprite.width)));
    m_qsReorderPos[dragged][1] = static_cast<float>(std::clamp(cursor.y - m_qsReorderGrab.y,
        m_qsReorderBounds.top, std::max(m_qsReorderBounds.top, m_qsReorderBounds.bottom - sprite.height)));
    const float centreX = m_qsReorderPos[dragged][0] + 0.5F * static_cast<float>(sprite.width);
    const float centreY = m_qsReorderPos[dragged][1] + 0.5F * static_cast<float>(sprite.height);
    // Try the dragged module at every position in the order; keep layouts that
    // still tile the grid without gaps and pick the slot nearest the tile.
    uint8_t rest[kQsModuleCount - 1]{};
    size_t restCount = 0;
    for (const uint8_t module : m_qsReorderOrder) {
        if (module != dragged) {
            rest[restCount++] = module;
        }
    }
    auto slotDistance = [&](const QsModuleOrder& order, float& distance) {
        QsGridCell cells[kQsModuleCount]{};
        int rows = 0;
        if (!PackQsModules(order, cells, rows) || rows != m_qsReorderRows) {
            return false;
        }
        const RECT slot = QsModuleSlot(dragged, cells[dragged]);
        const float dx = 0.5F * static_cast<float>(slot.left + slot.right) - centreX;
        const float dy = 0.5F * static_cast<float>(slot.top + slot.bottom) - centreY;
        distance = dx * dx + dy * dy;
        return true;
    };
    float currentDistance = 0.0F;
    const bool currentValid = slotDistance(m_qsReorderOrder, currentDistance);
    QsModuleOrder best = m_qsReorderOrder;
    float bestDistance = currentValid ? currentDistance : std::numeric_limits<float>::max();
    for (size_t insert = 0; insert <= restCount; ++insert) {
        QsModuleOrder candidate{};
        size_t write = 0;
        for (size_t index = 0; index < restCount; ++index) {
            if (index == insert) {
                candidate[write++] = static_cast<uint8_t>(dragged);
            }
            candidate[write++] = rest[index];
        }
        if (insert == restCount) {
            candidate[write++] = static_cast<uint8_t>(dragged);
        }
        float distance = 0.0F;
        if (candidate != m_qsReorderOrder && slotDistance(candidate, distance) && distance < bestDistance) {
            best = candidate;
            bestDistance = distance;
        }
    }
    // Hysteresis: only move once the new slot is clearly closer (no flicker
    // between two layouts while the tile sits halfway).
    const float hysteresis = 12.0F * m_qsGridScale;
    if (best != m_qsReorderOrder &&
        (!currentValid || std::sqrt(bestDistance) + hysteresis < std::sqrt(currentDistance))) {
        m_qsReorderOrder = best;
    }
    // Resuming from an idle hold: start the spring clock now, not at the hold.
    const ULONGLONG now = GetTickCount64();
    if (now - m_qsReorderLastMs > 50ULL) {
        m_qsReorderLastMs = now;
    }
    SetTimer(m_overflowWindow, kQsReorderTimerId, 16, nullptr);
    PresentQsReorderFrame();
}

void DockApp::EndQsReorder(bool commit) {
    if (!m_qsReorderActive || m_qsReorderDropped) {
        return;
    }
    if (!commit) {
        m_qsReorderOrder = m_qsReorderStartOrder;
    }
    m_qsReorderDropped = true;
    m_qsReorderLastMs = GetTickCount64();
    SetTimer(m_overflowWindow, kQsReorderTimerId, 16, nullptr);
}

void DockApp::StepQsReorder() {
    if (!m_qsReorderActive || !IsOverflowOpen()) {
        if (m_overflowWindow != nullptr) {
            KillTimer(m_overflowWindow, kQsReorderTimerId);
        }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const float dt = std::clamp(static_cast<float>(now - m_qsReorderLastMs) / 1000.0F, 0.0F, 0.05F);
    m_qsReorderLastMs = now;
    QsGridCell cells[kQsModuleCount]{};
    int rows = 0;
    if (!PackQsModules(m_qsReorderOrder, cells, rows)) {
        m_qsReorderOrder = m_qsReorderStartOrder;
        static_cast<void>(PackQsModules(m_qsReorderOrder, cells, rows));
    }
    const size_t dragged = static_cast<size_t>(m_qsReorderModule);
    // UIKit-like springs: displaced tiles glide (0.32 s, 0.86); the dropped
    // tile is a touch looser so it visibly settles into its slot.
    bool settled = true;
    for (size_t module = 0; module < kQsModuleCount; ++module) {
        float* pos = m_qsReorderPos[module];
        float* vel = m_qsReorderVel[module];
        const QsReorderSprite& sprite = m_qsReorderSprites[module];
        if (module == dragged && !m_qsReorderDropped) {
            pos[0] = static_cast<float>(std::clamp(m_qsReorderCursor.x - m_qsReorderGrab.x, m_qsReorderBounds.left,
                std::max(m_qsReorderBounds.left, m_qsReorderBounds.right - sprite.width)));
            pos[1] = static_cast<float>(std::clamp(m_qsReorderCursor.y - m_qsReorderGrab.y, m_qsReorderBounds.top,
                std::max(m_qsReorderBounds.top, m_qsReorderBounds.bottom - sprite.height)));
            vel[0] = 0.0F;
            vel[1] = 0.0F;
            continue;
        }
        const RECT slot = QsModuleSlot(module, cells[module]);
        const float target[2] = {static_cast<float>(slot.left), static_cast<float>(slot.top)};
        const float response = module == dragged ? 0.38F : 0.32F;
        const float damping = module == dragged ? 0.74F : 0.86F;
        for (int axis = 0; axis < 2; ++axis) {
            StepSpring(pos[axis], vel[axis], target[axis], response, damping, dt);
            if (std::fabs(target[axis] - pos[axis]) < 0.35F && std::fabs(vel[axis]) < 8.0F) {
                pos[axis] = target[axis];
                vel[axis] = 0.0F;
            } else {
                settled = false;
            }
        }
    }
    // Lift: the held tile pops up ~6% (slightly bouncy) and drops back to 1.
    constexpr float kLiftScale = 1.06F;
    const float scaleGoal = m_qsReorderDropped ? 1.0F : kLiftScale;
    StepSpring(m_qsReorderScale, m_qsReorderScaleVel, scaleGoal, 0.3F, m_qsReorderDropped ? 0.8F : 0.62F, dt);
    if (std::fabs(scaleGoal - m_qsReorderScale) < 0.0015F && std::fabs(m_qsReorderScaleVel) < 0.02F) {
        m_qsReorderScale = scaleGoal;
        m_qsReorderScaleVel = 0.0F;
    } else {
        settled = false;
    }
    m_qsReorderLift = std::clamp((m_qsReorderScale - 1.0F) / (kLiftScale - 1.0F), 0.0F, 1.0F);
    PresentQsReorderFrame();
    if (!settled) {
        return;
    }
    if (m_qsReorderDropped) {
        FinishQsReorder();
    } else if (m_overflowWindow != nullptr) {
        // Holding still: idle until the cursor moves again.
        KillTimer(m_overflowWindow, kQsReorderTimerId);
    }
}

void DockApp::PresentQsReorderFrame() {
    POINT origin{};
    LONG caret = m_overflowCaretX;
    const LONG width = m_overflowPresentSize.cx;
    const LONG height = m_overflowPresentSize.cy;
    const size_t bytes = static_cast<size_t>(std::max(0L, width)) * static_cast<size_t>(std::max(0L, height)) * 4U;
    if (!m_qsReorderActive || m_overflowWindow == nullptr || bytes == 0 || m_overflowPresentBits.size() != bytes ||
        m_qsReorderBackdrop.size() != bytes || !OverflowScreenOrigin(origin, caret)) {
        return;
    }
    m_overflowCaretX = caret;
    uint8_t* frame = m_overflowPresentBits.data();
    const RECT dirty = m_qsReorderDirty;
    for (LONG y = dirty.top; y < dirty.bottom; ++y) {
        const size_t offset =
            (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(dirty.left)) * 4U;
        std::memcpy(frame + offset, m_qsReorderBackdrop.data() + offset,
            static_cast<size_t>(dirty.right - dirty.left) * 4U);
    }
    // Coverage lerp (not src-over): the snapshot already holds the panel
    // under the tile, so translucent glass is not doubled. Then the outline
    // ring darkens colour and builds alpha exactly as CcGlass does.
    auto blendPixel = [](uint8_t* dst, const float* src, float cover, float ring) {
        const float keep = 1.0F - cover;
        for (size_t channel = 0; channel < 4U; ++channel) {
            float value = src[channel] + static_cast<float>(dst[channel]) * keep;
            if (channel < 3U) {
                value *= 1.0F - ring;
            } else {
                value += (255.0F - value) * ring;
            }
            dst[channel] = static_cast<uint8_t>(std::clamp(value + 0.5F, 0.0F, 255.0F));
        }
    };
    auto blit = [&](size_t module) {
        const QsReorderSprite& sprite = m_qsReorderSprites[module];
        constexpr LONG kEdge = QsReorderSprite::kEdge;
        const LONG fullW = sprite.width + kEdge * 2L;
        const LONG fullH = sprite.height + kEdge * 2L;
        const LONG left = static_cast<LONG>(std::lround(m_qsReorderPos[module][0])) - kEdge;
        const LONG top = static_cast<LONG>(std::lround(m_qsReorderPos[module][1])) - kEdge;
        for (LONG y = 0; y < fullH; ++y) {
            const LONG dy = top + y;
            if (dy < dirty.top || dy >= dirty.bottom) {
                continue;
            }
            for (LONG x = 0; x < fullW; ++x) {
                const LONG dx = left + x;
                const size_t at = static_cast<size_t>(y) * static_cast<size_t>(fullW) + static_cast<size_t>(x);
                const uint8_t c = sprite.cover[at];
                const uint8_t g = sprite.ring[at];
                if ((c == 0U && g == 0U) || dx < dirty.left || dx >= dirty.right) {
                    continue;
                }
                const uint8_t* bits = sprite.bits.data() + at * 4U;
                const float src[4] = {static_cast<float>(bits[0]), static_cast<float>(bits[1]),
                    static_cast<float>(bits[2]), static_cast<float>(bits[3])};
                blendPixel(frame + (static_cast<size_t>(dy) * static_cast<size_t>(width) + static_cast<size_t>(dx)) * 4U,
                    src, static_cast<float>(c) / 255.0F, static_cast<float>(g) / 255.0F);
            }
        }
    };
    const size_t dragged = static_cast<size_t>(m_qsReorderModule);
    for (size_t module = 0; module < kQsModuleCount; ++module) {
        if (module != dragged) {
            blit(module);
        }
    }
    if (dragged < kQsModuleCount) {
        // Lifted tile: scaled about its centre (bilinear), over a blurred
        // shadow that deepens and drops as it rises.
        const QsReorderSprite& sprite = m_qsReorderSprites[dragged];
        const float scaleNow = std::max(0.5F, m_qsReorderScale);
        const float lift = m_qsReorderLift;
        const float centreX = m_qsReorderPos[dragged][0] + 0.5F * static_cast<float>(sprite.width);
        const float centreY = m_qsReorderPos[dragged][1] + 0.5F * static_cast<float>(sprite.height);
        const LONG pad = m_qsReorderShadowPad;
        const LONG shadowW = sprite.width + QsReorderSprite::kEdge * 2L + pad * 2L;
        const LONG shadowH = sprite.height + QsReorderSprite::kEdge * 2L + pad * 2L;
        if (lift > 0.01F && m_qsReorderShadow.size() == static_cast<size_t>(shadowW) * static_cast<size_t>(shadowH)) {
            const float drop = 7.0F * m_qsGridScale * lift;
            const float strength = 0.30F * lift;
            const float halfW = 0.5F * static_cast<float>(shadowW) * scaleNow;
            const float halfH = 0.5F * static_cast<float>(shadowH) * scaleNow;
            const LONG x0 = std::max(dirty.left, static_cast<LONG>(std::floor(centreX - halfW)));
            const LONG x1 = std::min(dirty.right, static_cast<LONG>(std::ceil(centreX + halfW)));
            const LONG y0 = std::max(dirty.top, static_cast<LONG>(std::floor(centreY + drop - halfH)));
            const LONG y1 = std::min(dirty.bottom, static_cast<LONG>(std::ceil(centreY + drop + halfH)));
            for (LONG y = y0; y < y1; ++y) {
                const float v = (static_cast<float>(y) + 0.5F - (centreY + drop - halfH)) / scaleNow - 0.5F;
                for (LONG x = x0; x < x1; ++x) {
                    const float u = (static_cast<float>(x) + 0.5F - (centreX - halfW)) / scaleNow - 0.5F;
                    const float a = SamplePlane(m_qsReorderShadow.data(), shadowW, shadowH, 1U, 0U, u, v) / 255.0F *
                        strength;
                    if (a <= 0.002F) {
                        continue;
                    }
                    uint8_t* dst = frame + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4U;
                    // Premultiplied black over: colour darkens, alpha builds.
                    const float keep = 1.0F - a;
                    dst[0] = static_cast<uint8_t>(static_cast<float>(dst[0]) * keep + 0.5F);
                    dst[1] = static_cast<uint8_t>(static_cast<float>(dst[1]) * keep + 0.5F);
                    dst[2] = static_cast<uint8_t>(static_cast<float>(dst[2]) * keep + 0.5F);
                    dst[3] = static_cast<uint8_t>(static_cast<float>(dst[3]) * keep + 255.0F * a + 0.5F);
                }
            }
        }
        if (std::fabs(scaleNow - 1.0F) < 0.001F) {
            blit(dragged);
        } else {
            constexpr LONG kEdge = QsReorderSprite::kEdge;
            const LONG fullW = sprite.width + kEdge * 2L;
            const LONG fullH = sprite.height + kEdge * 2L;
            const float halfW = 0.5F * static_cast<float>(fullW) * scaleNow;
            const float halfH = 0.5F * static_cast<float>(fullH) * scaleNow;
            const LONG x0 = std::max(dirty.left, static_cast<LONG>(std::floor(centreX - halfW)));
            const LONG x1 = std::min(dirty.right, static_cast<LONG>(std::ceil(centreX + halfW)));
            const LONG y0 = std::max(dirty.top, static_cast<LONG>(std::floor(centreY - halfH)));
            const LONG y1 = std::min(dirty.bottom, static_cast<LONG>(std::ceil(centreY + halfH)));
            for (LONG y = y0; y < y1; ++y) {
                const float v = (static_cast<float>(y) + 0.5F - (centreY - halfH)) / scaleNow - 0.5F;
                for (LONG x = x0; x < x1; ++x) {
                    const float u = (static_cast<float>(x) + 0.5F - (centreX - halfW)) / scaleNow - 0.5F;
                    const float c = SamplePlane(sprite.cover.data(), fullW, fullH, 1U, 0U, u, v);
                    const float g = SamplePlane(sprite.ring.data(), fullW, fullH, 1U, 0U, u, v);
                    if (c <= 0.5F && g <= 0.5F) {
                        continue;
                    }
                    float src[4]{};
                    for (size_t channel = 0; channel < 4U; ++channel) {
                        src[channel] = SamplePlane(sprite.bits.data(), fullW, fullH, 4U, channel, u, v);
                    }
                    blendPixel(frame + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4U,
                        src, c / 255.0F, g / 255.0F);
                }
            }
        }
    }
    static_cast<void>(PresentLayeredBits(m_overflowWindow, origin, width, height, frame, bytes, m_overflowLayerDib,
        &dirty));
}

void DockApp::FinishQsReorder() {
    if (m_overflowWindow != nullptr) {
        KillTimer(m_overflowWindow, kQsReorderTimerId);
    }
    const bool changed = m_qsReorderOrder != m_qsOrder;
    const bool pending = m_qsReorderPaintPending;
    m_qsReorderActive = false;
    m_qsReorderDropped = false;
    m_qsReorderPaintPending = false;
    m_qsReorderModule = -1;
    for (QsReorderSprite& sprite : m_qsReorderSprites) {
        std::vector<uint8_t>().swap(sprite.bits);
        std::vector<uint8_t>().swap(sprite.cover);
        std::vector<uint8_t>().swap(sprite.ring);
    }
    std::vector<uint8_t>().swap(m_qsReorderBackdrop);
    std::vector<uint8_t>().swap(m_qsReorderShadow);
    m_overflowHover = -1;
    m_overflowHoverDirtyValid = false;
    if (changed) {
        m_qsOrder = m_qsReorderOrder;
        m_qsOrderSource = m_qsOrder == DefaultQsModuleOrder() ? std::wstring{} : FormatQsModuleOrder(m_qsOrder);
        m_config.SetQsLayout(m_qsOrderSource);
        ScheduleConfigSave();
    }
    if (changed || pending) {
        // The settled frame already matches the new layout; the rebuild
        // re-bakes the glass shapes and hit rects behind it.
        QueueOverflowPaint(false);
        return;
    }
    if (m_overflowBaseBits.size() == m_overflowPresentBits.size()) {
        m_overflowPresentBits = m_overflowBaseBits;
        m_overflowLayerDib.contentValid = false;
        PresentOverflowLayer();
    }
}

void DockApp::AbortQsReorder() noexcept {
    m_qsPressArmed = false;
    if (!m_qsReorderActive) {
        return;
    }
    if (m_overflowWindow != nullptr) {
        KillTimer(m_overflowWindow, kQsReorderTimerId);
    }
    try {
        // A released drag is a decision; save it even if the panel closes mid-settle.
        if (m_qsReorderDropped && m_qsReorderOrder != m_qsOrder) {
            m_qsOrder = m_qsReorderOrder;
            m_qsOrderSource = m_qsOrder == DefaultQsModuleOrder() ? std::wstring{} : FormatQsModuleOrder(m_qsOrder);
            m_config.SetQsLayout(m_qsOrderSource);
            ScheduleConfigSave();
        }
    } catch (...) {
    }
    m_qsReorderActive = false;
    m_qsReorderDropped = false;
    m_qsReorderPaintPending = false;
    m_qsReorderModule = -1;
    for (QsReorderSprite& sprite : m_qsReorderSprites) {
        std::vector<uint8_t>().swap(sprite.bits);
        std::vector<uint8_t>().swap(sprite.cover);
        std::vector<uint8_t>().swap(sprite.ring);
    }
    std::vector<uint8_t>().swap(m_qsReorderBackdrop);
    std::vector<uint8_t>().swap(m_qsReorderShadow);
    // The present frame is a reorder composite: never reopen with it.
    std::vector<uint8_t>().swap(m_overflowPresentBits);
    std::vector<uint8_t>().swap(m_overflowBaseBits);
    std::vector<uint8_t>().swap(m_overflowUnderlayBits);
    m_overflowPresentSize = {};
}
