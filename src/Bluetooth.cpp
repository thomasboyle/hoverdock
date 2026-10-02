#include <winsock2.h>
#include <ws2bth.h>

#include "Bluetooth.h"

#include <winioctl.h>
#include <bluetoothapis.h>
#include <roapi.h>

#pragma warning(push)
#pragma warning(disable : 4061 4100 4265 4365 4371 4458 4464 4619 4668 4986 5026 5027 5204 5220 5246)
#include <windows.devices.bluetooth.h>
#include <windows.devices.enumeration.h>
#include <windows.devices.radios.h>
#pragma warning(pop)

#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <utility>

using Microsoft::WRL::ComPtr;
using ABI::Windows::Foundation::AsyncStatus;
using ABI::Windows::Foundation::IAsyncInfo;

namespace {

constexpr DWORD kOpTimeoutMs = 20000;
constexpr DWORD kStatusTimeoutMs = 2500;
constexpr DWORD kPairTimeoutMs = 180000;
constexpr size_t kMaxPaired = 6;
constexpr size_t kMaxDiscovered = 4;
constexpr size_t kMaxTracked = 32;

constexpr wchar_t kBluetoothAqs[] =
    L"(System.Devices.Aep.ProtocolId:=\"{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}\" OR "
    L"System.Devices.Aep.ProtocolId:=\"{bb7bb05e-5972-42b5-94fc-76eaa7084d49}\")";

#ifndef FILE_DEVICE_BLUETOOTH
#define FILE_DEVICE_BLUETOOTH 0x00000041
#endif
constexpr DWORD kDisconnectIoctl = CTL_CODE(FILE_DEVICE_BLUETOOTH, 0x03, METHOD_BUFFERED, FILE_ANY_ACCESS);

using RadioListAsync = __FIAsyncOperation_1___FIVectorView_1_Windows__CDevices__CRadios__CRadio;
using RadioList = __FIVectorView_1_Windows__CDevices__CRadios__CRadio;
using AccessAsync = __FIAsyncOperation_1_Windows__CDevices__CRadios__CRadioAccessStatus;
using DeviceListAsync = __FIAsyncOperation_1_Windows__CDevices__CEnumeration__CDeviceInformationCollection;
using DeviceList = __FIVectorView_1_Windows__CDevices__CEnumeration__CDeviceInformation;
using DeviceAsync = __FIAsyncOperation_1_Windows__CDevices__CEnumeration__CDeviceInformation;
using PairAsync = __FIAsyncOperation_1_Windows__CDevices__CEnumeration__CDevicePairingResult;
using PropertyMap = __FIMapView_2_HSTRING_IInspectable;
using AddedHandler =
    __FITypedEventHandler_2_Windows__CDevices__CEnumeration__CDeviceWatcher_Windows__CDevices__CEnumeration__CDeviceInformation;
using UpdatedHandler =
    __FITypedEventHandler_2_Windows__CDevices__CEnumeration__CDeviceWatcher_Windows__CDevices__CEnumeration__CDeviceInformationUpdate;
using StoppedHandler = __FITypedEventHandler_2_Windows__CDevices__CEnumeration__CDeviceWatcher_IInspectable;
using ClassicDeviceAsync = __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CBluetoothDevice;
using LeDeviceAsync = __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CBluetoothLEDevice;
using GattAsync =
    __FIAsyncOperation_1_Windows__CDevices__CBluetooth__CGenericAttributeProfile__CGattDeviceServicesResult;

enum class JobKind : uint8_t {
    Refresh,
    SetRadio,
    Connect,
    Disconnect,
    StartDiscovery,
    StopDiscovery,
    Pair,
    Quit,
};

struct Job {
    JobKind kind = JobKind::Refresh;
    bool enableRadio = false;
    std::wstring id;
};

struct RadioDevice {
    std::wstring id;
    std::wstring name;
    uint64_t address = 0;
    int signal = 0;
    bool connected = false;
    bool canPair = false;
    bool lowEnergy = false;
};

GUID ServiceUuid(unsigned short shortId) noexcept {
    return GUID{shortId, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
}

std::wstring HStringToWide(HSTRING value) {
    if (value == nullptr) {
        return {};
    }
    UINT32 length = 0;
    const wchar_t* raw = WindowsGetStringRawBuffer(value, &length);
    if (raw == nullptr || length == 0) {
        return {};
    }
    return std::wstring(raw, length);
}

int HexValue(wchar_t character) noexcept {
    if (character >= L'0' && character <= L'9') {
        return character - L'0';
    }
    if (character >= L'a' && character <= L'f') {
        return character - L'a' + 10;
    }
    if (character >= L'A' && character <= L'F') {
        return character - L'A' + 10;
    }
    return -1;
}

uint64_t ParseBluetoothAddress(const std::wstring& text) {
    if (text.size() < 17) {
        return 0;
    }
    uint64_t found = 0;
    for (size_t start = 0; start + 17 <= text.size(); ++start) {
        unsigned bytes[6]{};
        bool matched = true;
        size_t cursor = start;
        for (int index = 0; index < 6; ++index) {
            if (cursor + 1 >= text.size()) {
                matched = false;
                break;
            }
            const int high = HexValue(text[cursor]);
            const int low = HexValue(text[cursor + 1]);
            if (high < 0 || low < 0) {
                matched = false;
                break;
            }
            bytes[index] = static_cast<unsigned>((high << 4) | low);
            cursor += 2;
            if (index < 5) {
                if (cursor >= text.size() || text[cursor] != L':') {
                    matched = false;
                    break;
                }
                ++cursor;
            }
        }
        if (!matched) {
            continue;
        }
        BLUETOOTH_ADDRESS address{};
        for (int index = 0; index < 6; ++index) {
            address.rgBytes[index] = static_cast<BYTE>(bytes[5 - index]);
        }
        found = address.ullLong;
    }
    return found;
}

uint64_t AddressFromTwelveHex(const wchar_t* hex) {
    if (hex == nullptr) {
        return 0;
    }
    unsigned bytes[6]{};
    for (int index = 0; index < 6; ++index) {
        const int high = HexValue(hex[index * 2]);
        const int low = HexValue(hex[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return 0;
        }
        bytes[index] = static_cast<unsigned>((high << 4) | low);
    }
    BLUETOOTH_ADDRESS address{};
    for (int index = 0; index < 6; ++index) {
        address.rgBytes[index] = static_cast<BYTE>(bytes[5 - index]);
    }
    return address.ullLong;
}

uint64_t AddressFromTaggedId(const std::wstring& text) {
    const wchar_t* tags[] = {L"BLUETOOTHDEVICE_", L"DEV_"};
    for (const wchar_t* tag : tags) {
        const size_t tagLength = wcslen(tag);
        if (text.size() < tagLength + 12) {
            continue;
        }
        for (size_t start = 0; start + tagLength + 12 <= text.size(); ++start) {
            bool matched = true;
            for (size_t index = 0; index < tagLength; ++index) {
                if (towlower(text[start + index]) != towlower(tag[index])) {
                    matched = false;
                    break;
                }
            }
            if (!matched) {
                continue;
            }
            const uint64_t address = AddressFromTwelveHex(text.c_str() + start + tagLength);
            if (address != 0) {
                return address;
            }
        }
    }
    return 0;
}

bool LooksLikeAddress(const std::wstring& name) {
    if (name.size() != 17) {
        return false;
    }
    return ParseBluetoothAddress(name) != 0 && name.find(L':') != std::wstring::npos;
}

bool ContainsInsensitive(const std::wstring& text, const wchar_t* needle) {
    if (needle == nullptr || needle[0] == L'\0') {
        return false;
    }
    return std::search(text.begin(), text.end(), needle, needle + wcslen(needle),
               [](wchar_t left, wchar_t right) {
                   return towlower(left) == towlower(right);
               }) != text.end();
}

bool IsProfileName(const std::wstring& name) {
    return ContainsInsensitive(name, L"Hands-Free") || ContainsInsensitive(name, L"Avrcp") ||
           ContainsInsensitive(name, L"Transport");
}

std::wstring StripProfileSuffix(std::wstring name) {
    const wchar_t* suffixes[] = {L" Avrcp Transport", L" Hands-Free", L" Hands Free"};
    bool stripped = true;
    while (stripped) {
        stripped = false;
        for (const wchar_t* suffix : suffixes) {
            const size_t length = wcslen(suffix);
            if (name.size() <= length) {
                continue;
            }
            if (ContainsInsensitive(name.substr(name.size() - length), suffix)) {
                name.resize(name.size() - length);
                stripped = true;
            }
        }
    }
    return name;
}

class HStringIterator : public Microsoft::WRL::RuntimeClass<
                            Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRt>,
                            ABI::Windows::Foundation::Collections::IIterator<HSTRING>> {
    InspectableClass(L"Hoverdock.HStringIterator", BaseTrust)

public:
    explicit HStringIterator(std::vector<std::wstring> items) : m_items(std::move(items)) {}

    HRESULT STDMETHODCALLTYPE get_Current(HSTRING* current) override {
        if (current == nullptr) {
            return E_POINTER;
        }
        *current = nullptr;
        if (m_index >= m_items.size()) {
            return E_BOUNDS;
        }
        const std::wstring& item = m_items[m_index];
        return WindowsCreateString(item.data(), static_cast<UINT32>(item.size()), current);
    }

    HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean* hasCurrent) override {
        if (hasCurrent == nullptr) {
            return E_POINTER;
        }
        *hasCurrent = m_index < m_items.size() ? TRUE : FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE MoveNext(boolean* hasCurrent) override {
        if (hasCurrent == nullptr) {
            return E_POINTER;
        }
        if (m_index < m_items.size()) {
            ++m_index;
        }
        *hasCurrent = m_index < m_items.size() ? TRUE : FALSE;
        return S_OK;
    }

private:
    std::vector<std::wstring> m_items;
    size_t m_index = 0;
};

class HStringIterable : public Microsoft::WRL::RuntimeClass<
                            Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRt>,
                            ABI::Windows::Foundation::Collections::IIterable<HSTRING>> {
    InspectableClass(L"Hoverdock.HStringIterable", BaseTrust)

public:
    explicit HStringIterable(std::vector<std::wstring> items) : m_items(std::move(items)) {}

    HRESULT STDMETHODCALLTYPE First(
        ABI::Windows::Foundation::Collections::IIterator<HSTRING>** first) override {
        if (first == nullptr) {
            return E_POINTER;
        }
        *first = nullptr;
        auto iterator = Microsoft::WRL::Make<HStringIterator>(m_items);
        if (iterator == nullptr) {
            return E_OUTOFMEMORY;
        }
        *first = iterator.Detach();
        return S_OK;
    }

private:
    std::vector<std::wstring> m_items;
};

ComPtr<ABI::Windows::Foundation::Collections::IIterable<HSTRING>> PropertyList(bool inquiry) {
    std::vector<std::wstring> names{
        L"System.Devices.Aep.DeviceAddress",
        L"System.Devices.Aep.IsConnected",
        L"System.Devices.Aep.IsPaired",
        L"System.Devices.Aep.CanPair",
        L"System.Devices.Aep.SignalStrength",
    };
    if (inquiry) {
        names.emplace_back(L"System.Devices.Aep.Bluetooth.IssueInquiry");
    }
    return Microsoft::WRL::Make<HStringIterable>(std::move(names));
}

bool ReadProperty(PropertyMap* properties, const wchar_t* name, bool& value) {
    if (properties == nullptr || name == nullptr) {
        return false;
    }
    Microsoft::WRL::Wrappers::HStringReference key(name);
    ComPtr<IInspectable> inspected;
    if (FAILED(properties->Lookup(key.Get(), &inspected)) || inspected == nullptr) {
        return false;
    }
    ComPtr<ABI::Windows::Foundation::IPropertyValue> property;
    if (FAILED(inspected.As(&property))) {
        return false;
    }
    boolean stored = FALSE;
    if (FAILED(property->GetBoolean(&stored))) {
        return false;
    }
    value = stored != FALSE;
    return true;
}

bool ReadProperty(PropertyMap* properties, const wchar_t* name, std::wstring& value) {
    if (properties == nullptr || name == nullptr) {
        return false;
    }
    Microsoft::WRL::Wrappers::HStringReference key(name);
    ComPtr<IInspectable> inspected;
    if (FAILED(properties->Lookup(key.Get(), &inspected)) || inspected == nullptr) {
        return false;
    }
    ComPtr<ABI::Windows::Foundation::IPropertyValue> property;
    if (FAILED(inspected.As(&property))) {
        return false;
    }
    HSTRING stored = nullptr;
    if (FAILED(property->GetString(&stored))) {
        return false;
    }
    value = HStringToWide(stored);
    WindowsDeleteString(stored);
    return !value.empty();
}

bool ReadProperty(PropertyMap* properties, const wchar_t* name, int& value) {
    if (properties == nullptr || name == nullptr) {
        return false;
    }
    Microsoft::WRL::Wrappers::HStringReference key(name);
    ComPtr<IInspectable> inspected;
    if (FAILED(properties->Lookup(key.Get(), &inspected)) || inspected == nullptr) {
        return false;
    }
    ComPtr<ABI::Windows::Foundation::IPropertyValue> property;
    if (FAILED(inspected.As(&property))) {
        return false;
    }
    INT32 stored = 0;
    if (FAILED(property->GetInt32(&stored))) {
        return false;
    }
    value = stored;
    return true;
}

bool ForEachRadio(const auto& visit) {
    BLUETOOTH_FIND_RADIO_PARAMS params{};
    params.dwSize = sizeof(params);
    HANDLE radio = nullptr;
    const HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
    if (find == nullptr) {
        return false;
    }
    bool matched = false;
    for (;;) {
        if (radio != nullptr) {
            matched = visit(radio) || matched;
            CloseHandle(radio);
            radio = nullptr;
        }
        if (matched) {
            break;
        }
        if (BluetoothFindNextRadio(find, &radio) == FALSE) {
            break;
        }
    }
    BluetoothFindRadioClose(find);
    if (radio != nullptr) {
        CloseHandle(radio);
    }
    return matched;
}

bool LoadDeviceInfo(HANDLE radio, uint64_t address, BLUETOOTH_DEVICE_INFO& info) {
    info = {};
    info.dwSize = sizeof(info);
    info.Address.ullLong = address;
    return BluetoothGetDeviceInfo(radio, &info) == ERROR_SUCCESS;
}

bool RadioConnected(HANDLE radio, uint64_t address) {
    BLUETOOTH_DEVICE_INFO info{};
    return LoadDeviceInfo(radio, address, info) && info.fConnected != FALSE;
}

bool AnyRadioConnected(uint64_t address) {
    return ForEachRadio([&](HANDLE radio) { return RadioConnected(radio, address); });
}

bool WaitForLink(uint64_t address, DWORD timeoutMs, const std::atomic<bool>& running, bool wantConnected) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (AnyRadioConnected(address) == wantConnected) {
            return true;
        }
        if (!running.load() || GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(200);
    }
}

void SetService(HANDLE radio, BLUETOOTH_DEVICE_INFO& info, unsigned short id, DWORD state) {
    const GUID service = ServiceUuid(id);
    BluetoothSetServiceState(radio, &info, &service, state);
}

std::vector<GUID> InstalledServices(HANDLE radio, BLUETOOTH_DEVICE_INFO& info) {
    std::vector<GUID> services;
    DWORD count = 0;
    if (BluetoothEnumerateInstalledServices(radio, &info, &count, nullptr) != ERROR_SUCCESS || count == 0) {
        return services;
    }
    services.resize(count);
    if (BluetoothEnumerateInstalledServices(radio, &info, &count, services.data()) != ERROR_SUCCESS) {
        services.clear();
        return services;
    }
    services.resize(count);
    return services;
}

bool IsVoiceService(const GUID& service) {
    return IsEqualGUID(service, ServiceUuid(0x1108)) != FALSE ||
           IsEqualGUID(service, ServiceUuid(0x111E)) != FALSE;
}

bool UsesAudio(HANDLE radio, const BLUETOOTH_DEVICE_INFO& info) {
    const ULONG majorClass = (info.ulClassofDevice >> 8) & 0x1Fu;
    if (majorClass == 0x04u || ContainsInsensitive(info.szName, L"AirPod")) {
        return true;
    }
    BLUETOOTH_DEVICE_INFO mutableInfo = info;
    for (const GUID& service : InstalledServices(radio, mutableInfo)) {
        if (IsEqualGUID(service, ServiceUuid(0x110B)) != FALSE) {
            return true;
        }
    }
    return false;
}

void EnableAudio(HANDLE radio, BLUETOOTH_DEVICE_INFO& info) {
    // Enable A2DP/AVRCP only. Do not DisableVoice/HFP here: flipping SCO
    // profiles forces Windows to rebuild the audio graph and can glitch
    // browser WASAPI playback even when the user is mid-video.
    SetService(radio, info, 0x110B, BLUETOOTH_SERVICE_ENABLE);
    SetService(radio, info, 0x110E, BLUETOOTH_SERVICE_ENABLE);
}

void EnableGeneral(HANDLE radio, BLUETOOTH_DEVICE_INFO& info) {
    bool enabled = false;
    for (const GUID& service : InstalledServices(radio, info)) {
        if (IsVoiceService(service) || IsEqualGUID(service, ServiceUuid(0x110B)) != FALSE ||
            IsEqualGUID(service, ServiceUuid(0x110A)) != FALSE) {
            continue;
        }
        BluetoothSetServiceState(radio, &info, &service, BLUETOOTH_SERVICE_ENABLE);
        enabled = true;
    }
    if (!enabled) {
        SetService(radio, info, 0x1124, BLUETOOTH_SERVICE_ENABLE);
    }
}

void DisableProfiles(HANDLE radio, BLUETOOTH_DEVICE_INFO& info) {
    const unsigned short ids[] = {0x110B, 0x110E, 0x1108, 0x111E, 0x1124, 0x110A};
    for (unsigned short id : ids) {
        SetService(radio, info, id, BLUETOOTH_SERVICE_DISABLE);
    }
    for (const GUID& service : InstalledServices(radio, info)) {
        BluetoothSetServiceState(radio, &info, &service, BLUETOOTH_SERVICE_DISABLE);
    }
}

bool RememberedClassic(uint64_t address) {
    return ForEachRadio([&](HANDLE radio) {
        BLUETOOTH_DEVICE_INFO info{};
        return LoadDeviceInfo(radio, address, info) && info.fRemembered != FALSE;
    });
}

bool PageAudio(uint64_t address) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }
    bool paged = false;
    const SOCKET sock = socket(AF_BTH, SOCK_STREAM, BTHPROTO_L2CAP);
    if (sock != INVALID_SOCKET) {
        u_long nonblocking = 1;
        ioctlsocket(sock, FIONBIO, &nonblocking);
        SOCKADDR_BTH remote{};
        remote.addressFamily = AF_BTH;
        remote.btAddr = address;
        remote.serviceClassId = ServiceUuid(0x110B);
        remote.port = 0x0019;
        const int started = connect(sock, reinterpret_cast<SOCKADDR*>(&remote), sizeof(remote));
        if (started == 0) {
            paged = true;
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            const WSAEVENT ready = WSACreateEvent();
            if (ready != WSA_INVALID_EVENT && WSAEventSelect(sock, ready, FD_CONNECT) == 0) {
                const DWORD waited = WSAWaitForMultipleEvents(1, &ready, FALSE, 3000, FALSE);
                WSANETWORKEVENTS network{};
                if (waited == WSA_WAIT_EVENT_0 &&
                    WSAEnumNetworkEvents(sock, ready, &network) == 0 &&
                    (network.lNetworkEvents & FD_CONNECT) != 0 &&
                    network.iErrorCode[FD_CONNECT_BIT] == 0) {
                    paged = true;
                }
            }
            if (ready != WSA_INVALID_EVENT) {
                WSACloseEvent(ready);
            }
        }
        if (paged) {
            ForEachRadio([&](HANDLE radio) {
                BLUETOOTH_DEVICE_INFO info{};
                if (!LoadDeviceInfo(radio, address, info)) {
                    return false;
                }
                EnableAudio(radio, info);
                return RadioConnected(radio, address);
            });
        }
        closesocket(sock);
    }
    WSACleanup();
    return paged;
}

bool ConnectClassic(uint64_t address, const std::atomic<bool>& running) {
    if (address == 0 || !running.load()) {
        return false;
    }
    // Fast path: enable the right profiles once. Never CM_Disable Bluetooth
    // device nodes - that tears down audio endpoints and breaks browser
    // WASAPI render while a video is playing.
    bool audio = false;
    const auto applyProfile = [&](HANDLE radio) {
        BLUETOOTH_DEVICE_INFO info{};
        if (!LoadDeviceInfo(radio, address, info)) {
            return false;
        }
        if (UsesAudio(radio, info)) {
            audio = true;
            EnableAudio(radio, info);
        } else {
            EnableGeneral(radio, info);
        }
        return RadioConnected(radio, address);
    };
    if (ForEachRadio(applyProfile) || AnyRadioConnected(address)) {
        return true;
    }
    if (!running.load()) {
        return false;
    }
    // Audio devices often need an L2CAP page to bring the ACL up; keep it brief.
    if (audio) {
        PageAudio(address);
        if (!running.load()) {
            return false;
        }
        ForEachRadio(applyProfile);
        if (AnyRadioConnected(address)) {
            return true;
        }
    }
    // One short confirmation wait, then a single profile re-nudge. Total budget
    // stays well under the old multi-stage ~15s path so Connect feels responsive.
    if (WaitForLink(address, 3500, running, true)) {
        return true;
    }
    if (!running.load()) {
        return false;
    }
    ForEachRadio(applyProfile);
    return WaitForLink(address, 2000, running, true) || AnyRadioConnected(address);
}

bool DisconnectClassic(uint64_t address, const std::atomic<bool>& running) {
    if (address == 0) {
        return false;
    }
    // Issue the radio disconnect IOCTL first so the link drops immediately,
    // then disable profiles. Avoid long WaitForLink polls that made Disconnect
    // feel stuck for multiple seconds.
    ForEachRadio([&](HANDLE radio) {
        DWORD returned = 0;
        DeviceIoControl(radio, kDisconnectIoctl, &address, sizeof(address), nullptr, 0, &returned,
            nullptr);
        return false;
    });
    ForEachRadio([&](HANDLE radio) {
        BLUETOOTH_DEVICE_INFO info{};
        if (!LoadDeviceInfo(radio, address, info)) {
            return false;
        }
        DisableProfiles(radio, info);
        return !RadioConnected(radio, address);
    });
    if (!running.load()) {
        return !AnyRadioConnected(address);
    }
    // Brief confirmation only. If the stack is still settling, treat the force
    // attempt as success — Windows finishes the drop asynchronously.
    if (WaitForLink(address, 700, running, false)) {
        return true;
    }
    return !AnyRadioConnected(address);
}

const wchar_t* PairFailureText(
    ABI::Windows::Devices::Enumeration::DevicePairingResultStatus status) noexcept {
    using ABI::Windows::Devices::Enumeration::DevicePairingResultStatus_AlreadyPaired;
    using ABI::Windows::Devices::Enumeration::DevicePairingResultStatus_AuthenticationFailure;
    using ABI::Windows::Devices::Enumeration::DevicePairingResultStatus_AuthenticationNotAllowed;
    using ABI::Windows::Devices::Enumeration::DevicePairingResultStatus_AuthenticationTimeout;
    using ABI::Windows::Devices::Enumeration::DevicePairingResultStatus_ConnectionRejected;
    using ABI::Windows::Devices::Enumeration::DevicePairingResultStatus_NotReadyToPair;
    using ABI::Windows::Devices::Enumeration::DevicePairingResultStatus_Paired;
    using ABI::Windows::Devices::Enumeration::DevicePairingResultStatus_PairingCanceled;
    switch (status) {
    case DevicePairingResultStatus_PairingCanceled:
        return L"Pairing canceled";
    case DevicePairingResultStatus_NotReadyToPair:
        return L"Put the device in pairing mode";
    case DevicePairingResultStatus_ConnectionRejected:
    case DevicePairingResultStatus_AuthenticationFailure:
    case DevicePairingResultStatus_AuthenticationTimeout:
    case DevicePairingResultStatus_AuthenticationNotAllowed:
        return L"Pairing was declined";
    case DevicePairingResultStatus_AlreadyPaired:
    case DevicePairingResultStatus_Paired:
        return L"";
    default:
        return L"Couldn't pair";
    }
}

}  // namespace

struct BluetoothService::Impl {
    HWND notifyWindow = nullptr;
    UINT notifyMessage = 0;
    HANDLE wake = nullptr;
    HANDLE thread = nullptr;
    std::atomic<bool> running{false};
    std::atomic<bool> pairing{false};
    std::atomic<bool> notifyQueued{false};
    std::atomic<uint64_t> watchGeneration{0};

    std::mutex queueMutex;
    std::vector<Job> queue;

    mutable std::mutex stateMutex;
    BluetoothSnapshot snapshot;

    std::mutex asyncMutex;
    std::mutex callbackMutex;
    ComPtr<IAsyncInfo> outstanding;

    bool winrtReady = false;
    bool accessKnown = false;
    bool accessAllowed = false;
    bool ready = false;
    bool radioPresent = false;
    bool radioOn = false;
    bool discovering = false;
    bool enumerationCompleted = false;
    std::wstring hint;
    std::wstring busyId;
    std::wstring busyText;
    std::wstring errorId;
    std::wstring errorText;
    std::vector<ComPtr<ABI::Windows::Devices::Radios::IRadio>> radios;
    std::vector<RadioDevice> paired;
    std::vector<RadioDevice> discovered;
    ComPtr<ABI::Windows::Devices::Enumeration::IDeviceWatcher> watcher;
    EventRegistrationToken addedToken{};
    EventRegistrationToken updatedToken{};
    EventRegistrationToken removedToken{};
    EventRegistrationToken completedToken{};

    void Track(IAsyncInfo* info) {
        std::lock_guard<std::mutex> lock(asyncMutex);
        outstanding = info;
    }

    void CancelOutstanding() noexcept {
        ComPtr<IAsyncInfo> info;
        {
            std::lock_guard<std::mutex> lock(asyncMutex);
            info = outstanding;
        }
        if (info != nullptr) {
            info->Cancel();
        }
    }

    template <typename TAsync, typename TResult>
    HRESULT WaitResult(TAsync* operation, TResult* result, DWORD timeoutMs) {
        if (operation == nullptr || result == nullptr) {
            return E_POINTER;
        }
        ComPtr<IAsyncInfo> info;
        HRESULT hr = operation->QueryInterface(IID_PPV_ARGS(&info));
        if (FAILED(hr)) {
            return hr;
        }
        Track(info.Get());
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;) {
            if (!running.load()) {
                info->Cancel();
                hr = E_ABORT;
                break;
            }
            AsyncStatus status = AsyncStatus::Started;
            hr = info->get_Status(&status);
            if (FAILED(hr)) {
                break;
            }
            if (status == AsyncStatus::Completed) {
                hr = operation->GetResults(result);
                break;
            }
            if (status == AsyncStatus::Error) {
                HRESULT error = E_FAIL;
                info->get_ErrorCode(&error);
                hr = FAILED(error) ? error : E_FAIL;
                break;
            }
            if (status == AsyncStatus::Canceled) {
                hr = E_ABORT;
                break;
            }
            if (GetTickCount64() >= deadline) {
                info->Cancel();
                hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                break;
            }
            Sleep(20);
        }
        Track(nullptr);
        return hr;
    }

    template <typename TAsync>
    HRESULT WaitDone(TAsync* operation, DWORD timeoutMs) {
        if (operation == nullptr) {
            return E_POINTER;
        }
        ComPtr<IAsyncInfo> info;
        HRESULT hr = operation->QueryInterface(IID_PPV_ARGS(&info));
        if (FAILED(hr)) {
            return hr;
        }
        Track(info.Get());
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;) {
            if (!running.load()) {
                info->Cancel();
                hr = E_ABORT;
                break;
            }
            AsyncStatus status = AsyncStatus::Started;
            hr = info->get_Status(&status);
            if (FAILED(hr)) {
                break;
            }
            if (status == AsyncStatus::Completed) {
                hr = S_OK;
                break;
            }
            if (status == AsyncStatus::Error) {
                HRESULT error = E_FAIL;
                info->get_ErrorCode(&error);
                hr = FAILED(error) ? error : E_FAIL;
                break;
            }
            if (status == AsyncStatus::Canceled) {
                hr = E_ABORT;
                break;
            }
            if (GetTickCount64() >= deadline) {
                info->Cancel();
                hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                break;
            }
            Sleep(20);
        }
        Track(nullptr);
        return hr;
    }

    bool Enqueue(Job job) {
        if (wake == nullptr) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (!running.load() && job.kind != JobKind::Quit) {
                return false;
            }
            if (job.kind == JobKind::Refresh) {
                for (const Job& pending : queue) {
                    if (pending.kind == JobKind::Refresh) {
                        return true;
                    }
                }
            }
            queue.push_back(std::move(job));
        }
        SetEvent(wake);
        return true;
    }

    void AllowNextNotify() noexcept {
        notifyQueued.store(false);
    }

    void Notify() {
        bool expected = false;
        if (!notifyQueued.compare_exchange_strong(expected, true)) {
            return;
        }
        HWND window = nullptr;
        UINT message = 0;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            window = notifyWindow;
            message = notifyMessage;
        }
        if (window == nullptr || message == 0 || PostMessageW(window, message, 0, 0) == FALSE) {
            notifyQueued.store(false);
        }
    }

    BluetoothSnapshot CopySnapshot() const {
        std::lock_guard<std::mutex> lock(stateMutex);
        return snapshot;
    }

    void Publish() {
        BluetoothSnapshot next;
        next.ready = ready;
        next.radioPresent = radioPresent;
        next.radioOn = radioOn;
        next.discovering = discovering;
        next.hint = hint;

        std::vector<RadioDevice> pairedRanked = paired;
        std::sort(pairedRanked.begin(), pairedRanked.end(), [](const RadioDevice& left, const RadioDevice& right) {
            if (left.connected != right.connected) {
                return left.connected;
            }
            return CompareStringOrdinal(left.name.c_str(), -1, right.name.c_str(), -1, TRUE) ==
                CSTR_LESS_THAN;
        });
        if (pairedRanked.size() > kMaxPaired) {
            next.hiddenPaired = static_cast<int>(pairedRanked.size() - kMaxPaired);
            pairedRanked.resize(kMaxPaired);
        }
        next.paired.reserve(pairedRanked.size());
        for (const RadioDevice& device : pairedRanked) {
            BluetoothDeviceInfo info;
            info.id = device.id;
            info.name = device.name.empty() || LooksLikeAddress(device.name) ? L"Bluetooth device"
                                                                              : device.name;
            info.connected = device.connected;
            info.busy = !busyId.empty() && device.id == busyId;
            if (info.busy) {
                info.status = busyText;
            } else if (!errorId.empty() && device.id == errorId) {
                info.status = errorText;
            } else {
                info.status = device.connected ? L"Connected" : L"Not connected";
            }
            next.paired.push_back(std::move(info));
        }

        std::vector<RadioDevice> found = discovered;
        std::sort(found.begin(), found.end(), [](const RadioDevice& left, const RadioDevice& right) {
            if (left.signal != right.signal) {
                return left.signal > right.signal;
            }
            return CompareStringOrdinal(left.name.c_str(), -1, right.name.c_str(), -1, TRUE) ==
                CSTR_LESS_THAN;
        });
        if (found.size() > kMaxDiscovered) {
            found.resize(kMaxDiscovered);
            if (next.hint.empty()) {
                next.hint = L"More devices are in range";
            }
        }
        next.discovered.reserve(found.size());
        for (const RadioDevice& device : found) {
            BluetoothDeviceInfo info;
            info.id = device.id;
            info.name = device.name;
            info.busy = !busyId.empty() && device.id == busyId;
            if (info.busy) {
                info.status = busyText;
            } else if (!errorId.empty() && device.id == errorId) {
                info.status = errorText;
            } else {
                info.status = L"Available";
            }
            next.discovered.push_back(std::move(info));
        }
        if (discovering && enumerationCompleted && next.discovered.empty() && next.hint.empty()) {
            next.hint = L"Put the device in pairing mode";
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex);
            snapshot = std::move(next);
        }
        Notify();
    }

    RadioDevice* FindMutable(const std::wstring& id) {
        for (RadioDevice& device : paired) {
            if (device.id == id) {
                return &device;
            }
        }
        for (RadioDevice& device : discovered) {
            if (device.id == id) {
                return &device;
            }
        }
        return nullptr;
    }

    bool EnsureAccess() {
        if (accessKnown) {
            return accessAllowed;
        }
        accessKnown = true;
        ComPtr<ABI::Windows::Devices::Radios::IRadioStatics> statics;
        Microsoft::WRL::Wrappers::HStringReference className(
            RuntimeClass_Windows_Devices_Radios_Radio);
        if (FAILED(RoGetActivationFactory(className.Get(), IID_PPV_ARGS(&statics)))) {
            return false;
        }
        ComPtr<AccessAsync> operation;
        if (FAILED(statics->RequestAccessAsync(&operation))) {
            return false;
        }
        ABI::Windows::Devices::Radios::RadioAccessStatus status =
            ABI::Windows::Devices::Radios::RadioAccessStatus_Unspecified;
        if (FAILED(WaitResult(operation.Get(), &status, kOpTimeoutMs))) {
            return false;
        }
        accessAllowed = status == ABI::Windows::Devices::Radios::RadioAccessStatus_Allowed;
        return accessAllowed;
    }

    void RefreshRadio() {
        radioPresent = false;
        radioOn = false;
        radios.clear();
        if (!EnsureAccess()) {
            hint = L"Bluetooth access was denied";
            return;
        }
        ComPtr<ABI::Windows::Devices::Radios::IRadioStatics> statics;
        Microsoft::WRL::Wrappers::HStringReference className(
            RuntimeClass_Windows_Devices_Radios_Radio);
        if (FAILED(RoGetActivationFactory(className.Get(), IID_PPV_ARGS(&statics)))) {
            return;
        }
        ComPtr<RadioListAsync> operation;
        if (FAILED(statics->GetRadiosAsync(&operation))) {
            return;
        }
        ComPtr<RadioList> list;
        if (FAILED(WaitResult(operation.Get(), list.GetAddressOf(), kOpTimeoutMs)) || list == nullptr) {
            return;
        }
        unsigned count = 0;
        if (FAILED(list->get_Size(&count))) {
            return;
        }
        for (unsigned index = 0; index < count; ++index) {
            ComPtr<ABI::Windows::Devices::Radios::IRadio> radio;
            if (FAILED(list->GetAt(index, radio.GetAddressOf())) || radio == nullptr) {
                continue;
            }
            ABI::Windows::Devices::Radios::RadioKind kind =
                ABI::Windows::Devices::Radios::RadioKind_Other;
            if (FAILED(radio->get_Kind(&kind)) ||
                kind != ABI::Windows::Devices::Radios::RadioKind_Bluetooth) {
                continue;
            }
            radioPresent = true;
            ABI::Windows::Devices::Radios::RadioState state =
                ABI::Windows::Devices::Radios::RadioState_Unknown;
            if (SUCCEEDED(radio->get_State(&state)) &&
                state == ABI::Windows::Devices::Radios::RadioState_On) {
                radioOn = true;
            }
            radios.push_back(std::move(radio));
        }
        if (radioOn && hint == L"Bluetooth access was denied") {
            hint.clear();
        }
    }

    bool SetRadioState(bool enabled) {
        if (radios.empty()) {
            RefreshRadio();
        }
        bool changed = false;
        const ABI::Windows::Devices::Radios::RadioState target = enabled
            ? ABI::Windows::Devices::Radios::RadioState_On
            : ABI::Windows::Devices::Radios::RadioState_Off;
        for (const ComPtr<ABI::Windows::Devices::Radios::IRadio>& radio : radios) {
            if (radio == nullptr) {
                continue;
            }
            ComPtr<AccessAsync> operation;
            if (FAILED(radio->SetStateAsync(target, &operation))) {
                continue;
            }
            ABI::Windows::Devices::Radios::RadioAccessStatus status =
                ABI::Windows::Devices::Radios::RadioAccessStatus_Unspecified;
            if (SUCCEEDED(WaitResult(operation.Get(), &status, kOpTimeoutMs)) &&
                status == ABI::Windows::Devices::Radios::RadioAccessStatus_Allowed) {
                changed = true;
            }
        }
        RefreshRadio();
        return changed || radioOn == enabled;
    }

    ComPtr<ABI::Windows::Devices::Enumeration::IDeviceInformationStatics2> DeviceStatics() const {
        ComPtr<ABI::Windows::Devices::Enumeration::IDeviceInformationStatics2> statics;
        Microsoft::WRL::Wrappers::HStringReference className(
            RuntimeClass_Windows_Devices_Enumeration_DeviceInformation);
        if (FAILED(RoGetActivationFactory(className.Get(), IID_PPV_ARGS(&statics)))) {
            return nullptr;
        }
        return statics;
    }

    RadioDevice DeviceFromInformation(
        ABI::Windows::Devices::Enumeration::IDeviceInformation* info) const {
        RadioDevice device;
        if (info == nullptr) {
            return device;
        }
        HSTRING id = nullptr;
        if (SUCCEEDED(info->get_Id(&id))) {
            device.id = HStringToWide(id);
            WindowsDeleteString(id);
        }
        HSTRING name = nullptr;
        if (SUCCEEDED(info->get_Name(&name))) {
            device.name = HStringToWide(name);
            WindowsDeleteString(name);
        }
        device.lowEnergy = ContainsInsensitive(device.id, L"BluetoothLE");
        ComPtr<PropertyMap> properties;
        if (SUCCEEDED(info->get_Properties(&properties)) && properties != nullptr) {
            std::wstring addressText;
            if (ReadProperty(properties.Get(), L"System.Devices.Aep.DeviceAddress", addressText)) {
                device.address = ParseBluetoothAddress(addressText);
                if (device.address == 0 && addressText.size() == 12) {
                    device.address = AddressFromTwelveHex(addressText.c_str());
                }
            }
            bool connected = false;
            if (ReadProperty(properties.Get(), L"System.Devices.Aep.IsConnected", connected)) {
                device.connected = connected;
            }
            bool canPair = false;
            if (ReadProperty(properties.Get(), L"System.Devices.Aep.CanPair", canPair)) {
                device.canPair = canPair;
            }
            int signal = 0;
            if (ReadProperty(properties.Get(), L"System.Devices.Aep.SignalStrength", signal)) {
                device.signal = signal;
            }
        }
        if (device.address == 0) {
            device.address = ParseBluetoothAddress(device.id);
        }
        if (device.address == 0) {
            device.address = AddressFromTaggedId(device.id);
        }
        ComPtr<ABI::Windows::Devices::Enumeration::IDeviceInformation2> info2;
        if (SUCCEEDED(info->QueryInterface(IID_PPV_ARGS(&info2))) && info2 != nullptr) {
            ComPtr<ABI::Windows::Devices::Enumeration::IDeviceInformationPairing> pairingInfo;
            if (SUCCEEDED(info2->get_Pairing(&pairingInfo)) && pairingInfo != nullptr) {
                boolean isPaired = FALSE;
                boolean canPairNow = FALSE;
                if (SUCCEEDED(pairingInfo->get_IsPaired(&isPaired)) && isPaired != FALSE) {
                    device.canPair = false;
                }
                if (SUCCEEDED(pairingInfo->get_CanPair(&canPairNow))) {
                    device.canPair = canPairNow != FALSE;
                }
            }
        }
        return device;
    }

    bool IsPairedInformation(ABI::Windows::Devices::Enumeration::IDeviceInformation* info) const {
        ComPtr<ABI::Windows::Devices::Enumeration::IDeviceInformation2> info2;
        if (SUCCEEDED(info->QueryInterface(IID_PPV_ARGS(&info2))) && info2 != nullptr) {
            ComPtr<ABI::Windows::Devices::Enumeration::IDeviceInformationPairing> pairingInfo;
            if (SUCCEEDED(info2->get_Pairing(&pairingInfo)) && pairingInfo != nullptr) {
                boolean isPaired = FALSE;
                if (SUCCEEDED(pairingInfo->get_IsPaired(&isPaired))) {
                    return isPaired != FALSE;
                }
            }
        }
        ComPtr<PropertyMap> properties;
        if (SUCCEEDED(info->get_Properties(&properties)) && properties != nullptr) {
            bool isPaired = false;
            if (ReadProperty(properties.Get(), L"System.Devices.Aep.IsPaired", isPaired)) {
                return isPaired;
            }
        }
        return false;
    }

    static bool SameDevice(const RadioDevice& left, const RadioDevice& right) {
        if (!left.id.empty() && left.id == right.id) {
            return true;
        }
        return left.address != 0 && left.address == right.address;
    }

    static bool Prefer(const RadioDevice& candidate, const RadioDevice& current) {
        if (candidate.connected != current.connected) {
            return candidate.connected;
        }
        const bool candidateNamed = !candidate.name.empty() && !LooksLikeAddress(candidate.name);
        const bool currentNamed = !current.name.empty() && !LooksLikeAddress(current.name);
        if (candidateNamed != currentNamed) {
            return candidateNamed;
        }
        const bool candidateProfile = IsProfileName(candidate.name);
        const bool currentProfile = IsProfileName(current.name);
        if (candidateProfile != currentProfile) {
            return !candidateProfile;
        }
        if (candidate.lowEnergy != current.lowEnergy) {
            return !candidate.lowEnergy;
        }
        return candidate.name.size() > current.name.size();
    }

    void Upsert(std::vector<RadioDevice>& list, RadioDevice device) {
        if (device.id.empty()) {
            return;
        }
        for (RadioDevice& existing : list) {
            if (!SameDevice(existing, device)) {
                continue;
            }
            if (Prefer(device, existing)) {
                if (device.name.empty()) {
                    device.name = existing.name;
                }
                if (device.address == 0) {
                    device.address = existing.address;
                }
                device.connected = device.connected || existing.connected;
                existing = std::move(device);
            } else {
                if (existing.name.empty()) {
                    existing.name = device.name;
                }
                if (existing.address == 0) {
                    existing.address = device.address;
                }
                existing.connected = existing.connected || device.connected;
                existing.canPair = existing.canPair || device.canPair;
                if (device.signal > existing.signal) {
                    existing.signal = device.signal;
                }
            }
            return;
        }
        if (list.size() < kMaxTracked) {
            list.push_back(std::move(device));
        }
    }

    void RemoveId(const std::wstring& id) {
        auto drop = [&](std::vector<RadioDevice>& list) {
            list.erase(std::remove_if(list.begin(), list.end(),
                           [&](const RadioDevice& device) { return device.id == id; }),
                list.end());
        };
        drop(paired);
        drop(discovered);
    }

    void NoteDevice(ABI::Windows::Devices::Enumeration::IDeviceInformation* info, bool fromPairQuery) {
        RadioDevice device = DeviceFromInformation(info);
        if (device.id.empty()) {
            return;
        }
        const bool pairedDevice = fromPairQuery || IsPairedInformation(info);
        if (pairedDevice) {
            device.canPair = false;
            if (device.id == errorId && device.connected) {
                errorId.clear();
                errorText.clear();
            }
            const std::wstring id = device.id;
            const uint64_t address = device.address;
            Upsert(paired, std::move(device));
            discovered.erase(std::remove_if(discovered.begin(), discovered.end(),
                                 [&](const RadioDevice& item) {
                                     return item.id == id || (address != 0 && item.address == address);
                                 }),
                discovered.end());
            return;
        }
        if (!discovering || device.name.empty() || LooksLikeAddress(device.name) || !device.canPair) {
            return;
        }
        bool alreadyPaired = false;
        for (const RadioDevice& known : paired) {
            if (SameDevice(known, device)) {
                alreadyPaired = true;
                break;
            }
        }
        if (!alreadyPaired) {
            Upsert(discovered, std::move(device));
        }
    }

    void ApplyUpdate(ABI::Windows::Devices::Enumeration::IDeviceInformationUpdate* update) {
        if (update == nullptr) {
            return;
        }
        HSTRING idValue = nullptr;
        if (FAILED(update->get_Id(&idValue))) {
            return;
        }
        const std::wstring id = HStringToWide(idValue);
        WindowsDeleteString(idValue);
        RadioDevice* device = FindMutable(id);
        if (device == nullptr) {
            return;
        }
        ComPtr<PropertyMap> properties;
        if (FAILED(update->get_Properties(&properties)) || properties == nullptr) {
            return;
        }
        bool connected = device->connected;
        if (ReadProperty(properties.Get(), L"System.Devices.Aep.IsConnected", connected)) {
            device->connected = connected;
            if (connected && id == errorId) {
                errorId.clear();
                errorText.clear();
            }
        }
        int signal = device->signal;
        if (ReadProperty(properties.Get(), L"System.Devices.Aep.SignalStrength", signal)) {
            device->signal = signal;
        }
    }

    void ApplyClassicConnection(RadioDevice& device) {
        ComPtr<ABI::Windows::Devices::Bluetooth::IBluetoothDeviceStatics> statics;
        Microsoft::WRL::Wrappers::HStringReference className(
            RuntimeClass_Windows_Devices_Bluetooth_BluetoothDevice);
        if (FAILED(RoGetActivationFactory(className.Get(), IID_PPV_ARGS(&statics))) || statics == nullptr) {
            return;
        }
        ComPtr<ABI::Windows::Devices::Bluetooth::IBluetoothDevice> bt;
        if (device.address != 0) {
            ComPtr<ClassicDeviceAsync> operation;
            if (SUCCEEDED(statics->FromBluetoothAddressAsync(device.address, operation.GetAddressOf())) &&
                operation != nullptr) {
                WaitResult(operation.Get(), bt.GetAddressOf(), kStatusTimeoutMs);
            }
        }
        if (bt == nullptr && !device.id.empty()) {
            Microsoft::WRL::Wrappers::HStringReference id(device.id.c_str());
            ComPtr<ClassicDeviceAsync> operation;
            if (FAILED(statics->FromIdAsync(id.Get(), operation.GetAddressOf())) || operation == nullptr) {
                return;
            }
            if (FAILED(WaitResult(operation.Get(), bt.GetAddressOf(), kStatusTimeoutMs)) || bt == nullptr) {
                return;
            }
        }
        if (bt == nullptr) {
            return;
        }
        ABI::Windows::Devices::Bluetooth::BluetoothConnectionStatus status =
            ABI::Windows::Devices::Bluetooth::BluetoothConnectionStatus_Disconnected;
        if (SUCCEEDED(bt->get_ConnectionStatus(&status))) {
            device.connected =
                status == ABI::Windows::Devices::Bluetooth::BluetoothConnectionStatus_Connected;
        }
        if (device.address == 0) {
            UINT64 address = 0;
            if (SUCCEEDED(bt->get_BluetoothAddress(&address))) {
                device.address = address;
            }
        }
    }

    void ApplyLeConnection(RadioDevice& device) {
        ComPtr<ABI::Windows::Devices::Bluetooth::IBluetoothLEDeviceStatics> statics;
        Microsoft::WRL::Wrappers::HStringReference className(
            RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEDevice);
        if (FAILED(RoGetActivationFactory(className.Get(), IID_PPV_ARGS(&statics))) || statics == nullptr) {
            return;
        }
        ComPtr<ABI::Windows::Devices::Bluetooth::IBluetoothLEDevice> bt;
        if (device.address != 0) {
            ComPtr<LeDeviceAsync> operation;
            if (SUCCEEDED(statics->FromBluetoothAddressAsync(device.address, operation.GetAddressOf())) &&
                operation != nullptr) {
                WaitResult(operation.Get(), bt.GetAddressOf(), kStatusTimeoutMs);
            }
        }
        if (bt == nullptr && !device.id.empty()) {
            Microsoft::WRL::Wrappers::HStringReference id(device.id.c_str());
            ComPtr<LeDeviceAsync> operation;
            if (FAILED(statics->FromIdAsync(id.Get(), operation.GetAddressOf())) || operation == nullptr) {
                return;
            }
            if (FAILED(WaitResult(operation.Get(), bt.GetAddressOf(), kStatusTimeoutMs)) || bt == nullptr) {
                return;
            }
        }
        if (bt == nullptr) {
            return;
        }
        ABI::Windows::Devices::Bluetooth::BluetoothConnectionStatus status =
            ABI::Windows::Devices::Bluetooth::BluetoothConnectionStatus_Disconnected;
        if (SUCCEEDED(bt->get_ConnectionStatus(&status))) {
            device.connected =
                status == ABI::Windows::Devices::Bluetooth::BluetoothConnectionStatus_Connected;
        }
        if (device.address == 0) {
            UINT64 address = 0;
            if (SUCCEEDED(bt->get_BluetoothAddress(&address))) {
                device.address = address;
            }
        }
    }

    bool CollectPaired(bool lowEnergy, std::vector<RadioDevice>& dest) {
        HSTRING selector = nullptr;
        if (lowEnergy) {
            ComPtr<ABI::Windows::Devices::Bluetooth::IBluetoothLEDeviceStatics2> statics;
            Microsoft::WRL::Wrappers::HStringReference className(
                RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEDevice);
            if (FAILED(RoGetActivationFactory(className.Get(), IID_PPV_ARGS(&statics))) || statics == nullptr ||
                FAILED(statics->GetDeviceSelectorFromPairingState(TRUE, &selector))) {
                return false;
            }
        } else {
            ComPtr<ABI::Windows::Devices::Bluetooth::IBluetoothDeviceStatics2> statics;
            Microsoft::WRL::Wrappers::HStringReference className(
                RuntimeClass_Windows_Devices_Bluetooth_BluetoothDevice);
            if (FAILED(RoGetActivationFactory(className.Get(), IID_PPV_ARGS(&statics))) || statics == nullptr ||
                FAILED(statics->GetDeviceSelectorFromPairingState(TRUE, &selector))) {
                return false;
            }
        }
        if (selector == nullptr) {
            return false;
        }
        ComPtr<ABI::Windows::Devices::Enumeration::IDeviceInformationStatics> enumeration;
        Microsoft::WRL::Wrappers::HStringReference deviceClass(
            RuntimeClass_Windows_Devices_Enumeration_DeviceInformation);
        auto properties = PropertyList(false);
        if (FAILED(RoGetActivationFactory(deviceClass.Get(), IID_PPV_ARGS(&enumeration))) ||
            enumeration == nullptr || properties == nullptr) {
            WindowsDeleteString(selector);
            return false;
        }
        // PairingState(true) includes IssueInquiry:=False. IsPaired alone returns nothing.
        ComPtr<DeviceListAsync> operation;
        const HRESULT queried = enumeration->FindAllAsyncAqsFilterAndAdditionalProperties(
            selector, properties.Get(), operation.GetAddressOf());
        WindowsDeleteString(selector);
        if (FAILED(queried) || operation == nullptr) {
            return false;
        }
        ComPtr<DeviceList> list;
        if (FAILED(WaitResult(operation.Get(), list.GetAddressOf(), kOpTimeoutMs)) || list == nullptr) {
            return false;
        }
        unsigned count = 0;
        if (FAILED(list->get_Size(&count))) {
            return false;
        }
        for (unsigned index = 0; index < count; ++index) {
            ComPtr<ABI::Windows::Devices::Enumeration::IDeviceInformation> info;
            if (FAILED(list->GetAt(index, info.GetAddressOf())) || info == nullptr) {
                continue;
            }
            RadioDevice device = DeviceFromInformation(info.Get());
            if (device.id.empty()) {
                continue;
            }
            device.lowEnergy = lowEnergy || device.lowEnergy;
            device.canPair = false;
            device.name = StripProfileSuffix(std::move(device.name));
            if (lowEnergy) {
                ApplyLeConnection(device);
            } else {
                ApplyClassicConnection(device);
            }
            Upsert(dest, std::move(device));
        }
        return true;
    }

    void RefreshPaired() {
        std::vector<RadioDevice> next;
        const bool classicOk = CollectPaired(false, next);
        const bool leOk = CollectPaired(true, next);
        if (classicOk || leOk) {
            paired = std::move(next);
        }
    }

    void StopWatcher() {
        watchGeneration.fetch_add(1);
        {
            // Wait out a callback that already passed the generation check.
            std::lock_guard<std::mutex> callbackLock(callbackMutex);
        }
        ComPtr<ABI::Windows::Devices::Enumeration::IDeviceWatcher> current = watcher;
        watcher.Reset();
        if (current == nullptr) {
            return;
        }
        current->Stop();
        const ULONGLONG deadline = GetTickCount64() + 2000;
        for (;;) {
            ABI::Windows::Devices::Enumeration::DeviceWatcherStatus status =
                ABI::Windows::Devices::Enumeration::DeviceWatcherStatus_Created;
            if (FAILED(current->get_Status(&status)) ||
                status == ABI::Windows::Devices::Enumeration::DeviceWatcherStatus_Stopped ||
                status == ABI::Windows::Devices::Enumeration::DeviceWatcherStatus_Created ||
                status == ABI::Windows::Devices::Enumeration::DeviceWatcherStatus_Aborted ||
                GetTickCount64() >= deadline) {
                break;
            }
            Sleep(20);
        }
        current.Reset();
    }

    bool StartWatcher() {
        StopWatcher();
        auto statics = DeviceStatics();
        auto properties = PropertyList(true);
        if (statics == nullptr || properties == nullptr) {
            return false;
        }
        Microsoft::WRL::Wrappers::HStringReference filter(kBluetoothAqs);
        ComPtr<ABI::Windows::Devices::Enumeration::IDeviceWatcher> created;
        if (FAILED(statics->CreateWatcherWithKindAqsFilterAndAdditionalProperties(filter.Get(),
                properties.Get(),
                ABI::Windows::Devices::Enumeration::DeviceInformationKind_AssociationEndpoint,
                &created)) ||
            created == nullptr) {
            return false;
        }
        const uint64_t generation = watchGeneration.load();
        auto added = Microsoft::WRL::Callback<AddedHandler>(
            [this, generation](ABI::Windows::Devices::Enumeration::IDeviceWatcher*,
                ABI::Windows::Devices::Enumeration::IDeviceInformation* info) -> HRESULT {
                std::lock_guard<std::mutex> callbackLock(callbackMutex);
                if (generation != watchGeneration.load() || info == nullptr) {
                    return S_OK;
                }
                NoteDevice(info, false);
                Publish();
                return S_OK;
            });
        auto updated = Microsoft::WRL::Callback<UpdatedHandler>(
            [this, generation](ABI::Windows::Devices::Enumeration::IDeviceWatcher*,
                ABI::Windows::Devices::Enumeration::IDeviceInformationUpdate* update) -> HRESULT {
                std::lock_guard<std::mutex> callbackLock(callbackMutex);
                if (generation != watchGeneration.load() || update == nullptr) {
                    return S_OK;
                }
                ApplyUpdate(update);
                Publish();
                return S_OK;
            });
        auto removed = Microsoft::WRL::Callback<UpdatedHandler>(
            [this, generation](ABI::Windows::Devices::Enumeration::IDeviceWatcher*,
                ABI::Windows::Devices::Enumeration::IDeviceInformationUpdate* update) -> HRESULT {
                std::lock_guard<std::mutex> callbackLock(callbackMutex);
                if (generation != watchGeneration.load() || update == nullptr) {
                    return S_OK;
                }
                HSTRING idValue = nullptr;
                if (SUCCEEDED(update->get_Id(&idValue))) {
                    RemoveId(HStringToWide(idValue));
                    WindowsDeleteString(idValue);
                    Publish();
                }
                return S_OK;
            });
        auto completed = Microsoft::WRL::Callback<StoppedHandler>(
            [this, generation](ABI::Windows::Devices::Enumeration::IDeviceWatcher*, IInspectable*) -> HRESULT {
                std::lock_guard<std::mutex> callbackLock(callbackMutex);
                if (generation != watchGeneration.load()) {
                    return S_OK;
                }
                enumerationCompleted = true;
                if (discovering && discovered.empty()) {
                    hint = L"Put the device in pairing mode";
                }
                Publish();
                return S_OK;
            });
        if (added == nullptr || updated == nullptr || removed == nullptr || completed == nullptr) {
            return false;
        }
        if (FAILED(created->add_Added(added.Get(), &addedToken)) ||
            FAILED(created->add_Updated(updated.Get(), &updatedToken)) ||
            FAILED(created->add_Removed(removed.Get(), &removedToken)) ||
            FAILED(created->add_EnumerationCompleted(completed.Get(), &completedToken))) {
            return false;
        }
        if (FAILED(created->Start())) {
            return false;
        }
        watcher = std::move(created);
        return true;
    }

    bool ConnectLe(uint64_t address) {
        if (address == 0) {
            return false;
        }
        ComPtr<ABI::Windows::Devices::Bluetooth::IBluetoothLEDeviceStatics> statics;
        Microsoft::WRL::Wrappers::HStringReference className(
            RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEDevice);
        if (FAILED(RoGetActivationFactory(className.Get(), IID_PPV_ARGS(&statics)))) {
            return false;
        }
        ComPtr<LeDeviceAsync> operation;
        if (FAILED(statics->FromBluetoothAddressAsync(address, &operation))) {
            return false;
        }
        ComPtr<ABI::Windows::Devices::Bluetooth::IBluetoothLEDevice> device;
        // Keep LE connect bounded — a 20s GATT wait made classic-fallback
        // attempts feel like Connect did nothing.
        constexpr DWORD kLeConnectTimeoutMs = 5000;
        if (FAILED(WaitResult(operation.Get(), device.GetAddressOf(), kLeConnectTimeoutMs)) ||
            device == nullptr) {
            return false;
        }
        ComPtr<ABI::Windows::Devices::Bluetooth::IBluetoothLEDevice3> gatt;
        if (FAILED(device.As(&gatt)) || gatt == nullptr) {
            return false;
        }
        ComPtr<GattAsync> services;
        if (FAILED(gatt->GetGattServicesWithCacheModeAsync(
                ABI::Windows::Devices::Bluetooth::BluetoothCacheMode_Cached, &services))) {
            return false;
        }
        return SUCCEEDED(WaitDone(services.Get(), kLeConnectTimeoutMs));
    }

    bool ConnectDevice(const RadioDevice& device) {
        uint64_t address = device.address;
        if (address == 0) {
            address = AddressFromTaggedId(device.id);
        }
        if (address == 0) {
            address = ParseBluetoothAddress(device.id);
        }
        if (ConnectClassic(address, running)) {
            return true;
        }
        if (device.lowEnergy && !RememberedClassic(address)) {
            return ConnectLe(address);
        }
        return false;
    }

    void RememberError(const std::wstring& id, const wchar_t* text, bool sectionHint) {
        errorId = id;
        errorText = text == nullptr ? L"" : text;
        if (sectionHint) {
            hint = errorText;
        }
    }

    void Handle(const Job& job) {
        switch (job.kind) {
        case JobKind::Refresh:
            RefreshRadio();
            if (!discovering && !pairing.load()) {
                RefreshPaired();
            }
            ready = true;
            Publish();
            break;
        case JobKind::SetRadio:
            if (!job.enableRadio) {
                StopWatcher();
                discovering = false;
                discovered.clear();
                enumerationCompleted = false;
            }
            SetRadioState(job.enableRadio);
            if (radioOn && !discovering) {
                RefreshPaired();
            }
            if (!radioOn) {
                hint.clear();
            }
            ready = true;
            Publish();
            break;
        case JobKind::Connect:
        case JobKind::Disconnect: {
            RadioDevice* device = FindMutable(job.id);
            if (device == nullptr) {
                RememberError(job.id, L"Device is no longer available", false);
                Publish();
                break;
            }
            const RadioDevice copy = *device;
            const bool connect = job.kind == JobKind::Connect;
            busyId = job.id;
            busyText = connect ? L"Connecting..." : L"Disconnecting...";
            errorId.clear();
            Publish();
            uint64_t address = copy.address;
            if (address == 0) {
                address = AddressFromTaggedId(copy.id);
            }
            if (address == 0) {
                address = ParseBluetoothAddress(copy.id);
            }
            const bool ok =
                connect ? ConnectDevice(copy) : DisconnectClassic(address, running);
            busyId.clear();
            busyText.clear();
            if (RadioDevice* current = FindMutable(job.id); current != nullptr) {
                if (ok) {
                    current->connected = connect;
                    errorId.clear();
                    errorText.clear();
                    if (hint == L"Couldn't connect" || hint == L"Couldn't disconnect") {
                        hint.clear();
                    }
                } else {
                    RememberError(job.id, connect ? L"Couldn't connect" : L"Couldn't disconnect", false);
                }
            } else if (!ok) {
                RememberError(job.id, connect ? L"Couldn't connect" : L"Couldn't disconnect", false);
            }
            Publish();
            // Reconcile with the radio shortly after — do not block this job.
            if (ok && running.load()) {
                Enqueue(Job{JobKind::Refresh, false, {}});
            }
            break;
        }
        case JobKind::StartDiscovery:
            if (!radioOn) {
                RefreshRadio();
            }
            if (!radioOn) {
                hint = L"Turn Bluetooth on first";
                Publish();
                break;
            }
            discovered.clear();
            enumerationCompleted = false;
            discovering = true;
            hint = L"Searching...";
            errorId.clear();
            errorText.clear();
            if (!StartWatcher()) {
                discovering = false;
                hint = L"Couldn't search for devices";
            }
            Publish();
            break;
        case JobKind::StopDiscovery:
            StopWatcher();
            discovering = false;
            discovered.clear();
            enumerationCompleted = false;
            if (hint == L"Searching..." || hint == L"Put the device in pairing mode" ||
                hint == L"More devices are in range") {
                hint.clear();
            }
            RefreshPaired();
            Publish();
            break;
        case JobKind::Pair: {
            pairing.store(true);
            busyId = job.id;
            busyText = L"Pairing...";
            hint = L"Confirm the pairing prompt";
            Publish();
            const wchar_t* failure = L"Couldn't pair";
            bool pairedOk = false;
            auto statics = DeviceStatics();
            auto properties = PropertyList(false);
            if (statics != nullptr && properties != nullptr) {
                Microsoft::WRL::Wrappers::HStringReference id(job.id.c_str());
                ComPtr<DeviceAsync> created;
                if (SUCCEEDED(statics->CreateFromIdAsyncWithKindAndAdditionalProperties(id.Get(),
                        properties.Get(),
                        ABI::Windows::Devices::Enumeration::DeviceInformationKind_AssociationEndpoint,
                        &created))) {
                    ComPtr<ABI::Windows::Devices::Enumeration::IDeviceInformation> info;
                    if (SUCCEEDED(WaitResult(created.Get(), info.GetAddressOf(), kOpTimeoutMs)) &&
                        info != nullptr) {
                        ComPtr<ABI::Windows::Devices::Enumeration::IDeviceInformation2> info2;
                        ComPtr<ABI::Windows::Devices::Enumeration::IDeviceInformationPairing> pairingInfo;
                        if (SUCCEEDED(info.As(&info2)) && info2 != nullptr &&
                            SUCCEEDED(info2->get_Pairing(&pairingInfo)) && pairingInfo != nullptr) {
                            ComPtr<PairAsync> operation;
                            if (SUCCEEDED(pairingInfo->PairAsync(&operation))) {
                                ComPtr<ABI::Windows::Devices::Enumeration::IDevicePairingResult> result;
                                if (SUCCEEDED(WaitResult(operation.Get(), result.GetAddressOf(), kPairTimeoutMs)) &&
                                    result != nullptr) {
                                    ABI::Windows::Devices::Enumeration::DevicePairingResultStatus status =
                                        ABI::Windows::Devices::Enumeration::DevicePairingResultStatus_Failed;
                                    result->get_Status(&status);
                                    if (status ==
                                            ABI::Windows::Devices::Enumeration::DevicePairingResultStatus_Paired ||
                                        status == ABI::Windows::Devices::Enumeration::
                                            DevicePairingResultStatus_AlreadyPaired) {
                                        pairedOk = true;
                                    } else {
                                        failure = PairFailureText(status);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            busyId.clear();
            busyText.clear();
            pairing.store(false);
            if (pairedOk) {
                errorId.clear();
                errorText.clear();
                hint.clear();
                RemoveId(job.id);
                RefreshPaired();
            } else if (running.load()) {
                RememberError(job.id, failure, true);
            }
            Publish();
            break;
        }
        case JobKind::Quit:
            break;
        }
    }

    void ThreadMain() {
        const HRESULT ro = RoInitialize(RO_INIT_MULTITHREADED);
        winrtReady = SUCCEEDED(ro);
        if (!winrtReady) {
            ready = true;
            hint = L"Bluetooth is unavailable";
            Publish();
        }
        while (running.load()) {
            WaitForSingleObject(wake, INFINITE);
            std::vector<Job> jobs;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                jobs.swap(queue);
            }
            for (const Job& job : jobs) {
                if (job.kind == JobKind::Quit || !running.load()) {
                    StopWatcher();
                    discovering = false;
                    if (ro != RPC_E_CHANGED_MODE && SUCCEEDED(ro)) {
                        RoUninitialize();
                    }
                    return;
                }
                if (winrtReady) {
                    Handle(job);
                }
            }
        }
        StopWatcher();
        if (SUCCEEDED(ro)) {
            RoUninitialize();
        }
    }

    bool Stop() noexcept {
        running.store(false);
        pairing.store(false);
        CancelOutstanding();
        if (wake != nullptr) {
            Job quit;
            quit.kind = JobKind::Quit;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                queue.push_back(std::move(quit));
            }
            SetEvent(wake);
        }
        if (thread != nullptr) {
            if (WaitForSingleObject(thread, 8000) != WAIT_OBJECT_0) {
                return false;
            }
            CloseHandle(thread);
            thread = nullptr;
            if (wake != nullptr) {
                CloseHandle(wake);
                wake = nullptr;
            }
        } else if (wake != nullptr) {
            CloseHandle(wake);
            wake = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            notifyWindow = nullptr;
            notifyMessage = 0;
        }
        return true;
    }
};

BluetoothService::BluetoothService() : m_impl(std::make_unique<Impl>()) {}

BluetoothService::~BluetoothService() {
    Stop();
}

void BluetoothService::Start(HWND notifyWindow, UINT notifyMessage) {
    Stop();
    m_impl = std::make_unique<Impl>();
    m_impl->notifyWindow = notifyWindow;
    m_impl->notifyMessage = notifyMessage;
    m_impl->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_impl->wake == nullptr) {
        return;
    }
    m_impl->running.store(true);
    m_impl->thread = CreateThread(nullptr, 0,
        [](LPVOID param) -> DWORD {
            static_cast<Impl*>(param)->ThreadMain();
            return 0;
        },
        m_impl.get(), 0, nullptr);
    if (m_impl->thread == nullptr) {
        m_impl->running.store(false);
        CloseHandle(m_impl->wake);
        m_impl->wake = nullptr;
        return;
    }
    m_impl->Enqueue(Job{JobKind::Refresh, false, {}});
}

void BluetoothService::Stop() noexcept {
    if (m_impl != nullptr && !m_impl->Stop()) {
        // The worker is still inside a pairing prompt. Keep the state alive
        // until process exit instead of freeing it under that thread.
        m_impl.release();
    }
}

void BluetoothService::RequestRefresh() {
    if (m_impl != nullptr) {
        m_impl->Enqueue(Job{JobKind::Refresh, false, {}});
    }
}

void BluetoothService::SetRadioEnabled(bool enabled) {
    if (m_impl != nullptr) {
        m_impl->Enqueue(Job{JobKind::SetRadio, enabled, {}});
    }
}

void BluetoothService::Connect(const std::wstring& id) {
    if (m_impl != nullptr && !id.empty()) {
        m_impl->Enqueue(Job{JobKind::Connect, false, id});
    }
}

void BluetoothService::Disconnect(const std::wstring& id) {
    if (m_impl != nullptr && !id.empty()) {
        m_impl->Enqueue(Job{JobKind::Disconnect, false, id});
    }
}

void BluetoothService::StartDiscovery() {
    if (m_impl != nullptr) {
        m_impl->Enqueue(Job{JobKind::StartDiscovery, false, {}});
    }
}

void BluetoothService::StopDiscovery() {
    if (m_impl != nullptr) {
        m_impl->Enqueue(Job{JobKind::StopDiscovery, false, {}});
    }
}

void BluetoothService::Pair(const std::wstring& id) {
    if (m_impl == nullptr || id.empty()) {
        return;
    }
    m_impl->pairing.store(true);
    if (!m_impl->Enqueue(Job{JobKind::Pair, false, id})) {
        m_impl->pairing.store(false);
    }
}

BluetoothSnapshot BluetoothService::GetSnapshot() const {
    if (m_impl == nullptr) {
        return {};
    }
    return m_impl->CopySnapshot();
}

bool BluetoothService::IsPairing() const noexcept {
    return m_impl != nullptr && m_impl->pairing.load();
}

void BluetoothService::AllowNextNotify() noexcept {
    if (m_impl != nullptr) {
        m_impl->AllowNextNotify();
    }
}
