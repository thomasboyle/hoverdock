#include <winsock2.h>
#include <ws2bth.h>

#include "Bluetooth.h"
#include "Profile.h"

#include <winioctl.h>
#include <bluetoothapis.h>
#include <roapi.h>

#pragma warning(push)
#pragma warning(disable : 4061 4100 4265 4365 4371 4458 4464 4619 4668 4986 5026 5027 5204 5220 5246)
#include <windows.devices.bluetooth.h>
#include <windows.devices.enumeration.h>
#include <windows.devices.radios.h>
#pragma warning(pop)

#pragma warning(push)
#pragma warning(disable : 4061 4100 4265 4365 4371 4458 4464 4619 4668 4986 5026 5027 5204 5220 5246)
#include <mmdeviceapi.h>
#include <devicetopology.h>
#pragma warning(pop)

#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <map>
#include <string>
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
// Round 1: event-driven link waits (fallback poll quantum + bounds).
constexpr DWORD kLinkPollMs = 50;
constexpr DWORD kDisconnectWaitMs = 3000;   // measured ACL down 150 ms median (IOCTL only)
constexpr DWORD kPageLinkWaitMs = 5000;     // measured link-up 1.6 s median, 4.1 s max
constexpr DWORD kPageErrorGraceMs = 1500;   // link may still come up after an L2CAP error
constexpr DWORD kToggleLinkWaitMs = 5000;   // after the toggle has returned
constexpr DWORD kToggleTotalWaitMs = 12000; // round 2: toggle median 6.13 s, link up inside it
constexpr DWORD kTaskJoinMs = 7000;         // bounded wait for a previous service task
constexpr DWORD kTaskChainWaitMs = 15000;   // helper thread waits for its predecessor
// KSPROPSETID_BtAudio (shared/ksmedia.h, 7FA06C40-B8F6-4C7E-8556-E8C33A12E54D).
constexpr GUID kKsPropSetBtAudio = {0x7fa06c40, 0xb8f6, 0x4c7e, {0x85, 0x56, 0xe8, 0xc3, 0x3a, 0x12, 0xe5, 0x4d}};
constexpr ULONG kKsPropOneShotReconnect = 0; // KSPROPERTY_ONESHOT_RECONNECT
constexpr ULONG kKsPropertyTypeGet = 0x00000001; // KSPROPERTY_TYPE_GET
constexpr int kMaxRefreshRetries = 3;
constexpr DWORD kRefreshRetryMs = 1500;
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
    PostConnect, // Round 1: AVRCP / HFP enable after link-up, off the critical path
    Quit,
};

struct Job {
    JobKind kind = JobKind::Refresh;
    bool enableRadio = false;
    std::wstring id;
    bool forcePaired = false;
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
    if (name.size() != 17 || name[2] != L':' || name[5] != L':' || name[8] != L':' ||
        name[11] != L':' || name[14] != L':') {
        return false;
    }
    return ParseBluetoothAddress(name) != 0;
}

bool ContainsInsensitive(const std::wstring& text, const wchar_t* needle) {
    if (needle == nullptr || needle[0] == L'\0' || text.empty()) {
        return false;
    }
    const size_t needleLength = wcslen(needle);
    if (needleLength > text.size()) {
        return false;
    }
    const size_t last = text.size() - needleLength;
    for (size_t start = 0; start <= last; ++start) {
        if (_wcsnicmp(text.c_str() + start, needle, needleLength) == 0) {
            return true;
        }
    }
    return false;
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
            if (_wcsnicmp(name.c_str() + (name.size() - length), suffix, length) == 0) {
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

// Radio handles opened once per wait instead of per poll.
class RadioHandles {
public:
    RadioHandles() {
        BLUETOOTH_FIND_RADIO_PARAMS params{};
        params.dwSize = sizeof(params);
        HANDLE radio = nullptr;
        const HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
        if (find == nullptr) {
            return;
        }
        do {
            if (radio != nullptr) {
                m_handles.push_back(radio);
                radio = nullptr;
            }
        } while (BluetoothFindNextRadio(find, &radio) != FALSE);
        BluetoothFindRadioClose(find);
        if (radio != nullptr) {
            CloseHandle(radio);
        }
    }
    ~RadioHandles() {
        for (HANDLE handle : m_handles) {
            CloseHandle(handle);
        }
    }
    RadioHandles(const RadioHandles&) = delete;
    RadioHandles& operator=(const RadioHandles&) = delete;

    [[nodiscard]] bool Any() const noexcept { return !m_handles.empty(); }
    [[nodiscard]] bool Connected(uint64_t address) const {
        for (HANDLE handle : m_handles) {
            if (RadioConnected(handle, address)) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<HANDLE> m_handles;
};

enum class LinkWait : uint8_t {
    Reached,
    Timeout,
    Cancelled,
    Extra, // caller-supplied handle (e.g. L2CAP page socket event) signalled
};

// Signal shared with a WinRT ConnectionStatusChanged callback. Held by
// shared_ptr so a late callback on a thread-pool thread never touches a
// closed handle.
struct LinkSignal {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    LinkSignal() = default;
    LinkSignal(const LinkSignal&) = delete;
    LinkSignal& operator=(const LinkSignal&) = delete;
    ~LinkSignal() {
        if (event != nullptr) {
            CloseHandle(event);
        }
    }
};

// Timing-only instrumentation (phase 2): same scope names as 9d0c953.
const char* ServiceScopeName(unsigned short id, DWORD state) noexcept {
    const bool enable = state == BLUETOOTH_SERVICE_ENABLE;
    switch (id) {
    case 0x110B:
        return enable ? "Bluetooth::SetServiceState 110B(A2DP sink) enable"
                      : "Bluetooth::SetServiceState 110B(A2DP sink) disable";
    case 0x110E:
        return enable ? "Bluetooth::SetServiceState 110E(AVRCP) enable"
                      : "Bluetooth::SetServiceState 110E(AVRCP) disable";
    case 0x111E:
        return enable ? "Bluetooth::SetServiceState 111E(HFP) enable"
                      : "Bluetooth::SetServiceState 111E(HFP) disable";
    case 0x1108:
        return enable ? "Bluetooth::SetServiceState 1108(HSP) enable"
                      : "Bluetooth::SetServiceState 1108(HSP) disable";
    case 0x1124:
        return enable ? "Bluetooth::SetServiceState 1124(HID) enable"
                      : "Bluetooth::SetServiceState 1124(HID) disable";
    default:
        return enable ? "Bluetooth::SetServiceState other enable" : "Bluetooth::SetServiceState other disable";
    }
}

unsigned short ShortServiceId(const GUID& service) noexcept {
    const GUID base = ServiceUuid(0);
    if (service.Data2 == base.Data2 && service.Data3 == base.Data3 &&
        memcmp(service.Data4, base.Data4, sizeof(base.Data4)) == 0 && (service.Data1 >> 16) == 0) {
        return static_cast<unsigned short>(service.Data1 & 0xFFFFu);
    }
    return 0;
}

DWORD SetServiceScoped(HANDLE radio, BLUETOOTH_DEVICE_INFO& info, const GUID& service, DWORD state) {
    ProfileScope profileScope(ServiceScopeName(ShortServiceId(service), state));
    return BluetoothSetServiceState(radio, &info, &service, state);
}

void SetService(HANDLE radio, BLUETOOTH_DEVICE_INFO& info, unsigned short id, DWORD state) {
    const GUID service = ServiceUuid(id);
    SetServiceScoped(radio, info, service, state);
}

std::vector<GUID> InstalledServices(HANDLE radio, BLUETOOTH_DEVICE_INFO& info) {
    std::vector<GUID> services;
    DWORD count = 0;
    // The sizing call reports ERROR_MORE_DATA (documented as success with the
    // needed count). Treating that as failure made this always return empty.
    const DWORD sized = BluetoothEnumerateInstalledServices(radio, &info, &count, nullptr);
    if ((sized != ERROR_SUCCESS && sized != ERROR_MORE_DATA) || count == 0) {
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

void EnableGeneral(HANDLE radio, BLUETOOTH_DEVICE_INFO& info) {
    bool enabled = false;
    for (const GUID& service : InstalledServices(radio, info)) {
        if (IsVoiceService(service) || IsEqualGUID(service, ServiceUuid(0x110B)) != FALSE ||
            IsEqualGUID(service, ServiceUuid(0x110A)) != FALSE) {
            continue;
        }
        SetServiceScoped(radio, info, service, BLUETOOTH_SERVICE_ENABLE);
        enabled = true;
    }
    if (!enabled) {
        SetService(radio, info, 0x1124, BLUETOOTH_SERVICE_ENABLE);
    }
}

bool RememberedClassic(uint64_t address) {
    return ForEachRadio([&](HANDLE radio) {
        BLUETOOTH_DEVICE_INFO info{};
        return LoadDeviceInfo(radio, address, info) && info.fRemembered != FALSE;
    });
}

// Round 3: blocking BluetoothSetServiceState calls run on a helper thread
// owned by a ServiceTask so the worker can publish link-up / run a
// disconnect while a toggle is still inside the driver. The thread only
// touches the task (shared_ptr), never BluetoothService::Impl.
struct ServiceStep {
    unsigned short id = 0;
    DWORD state = BLUETOOTH_SERVICE_ENABLE;
};

struct ServiceTask {
    uint64_t address = 0;
    const char* scopeName = nullptr;
    std::vector<ServiceStep> steps;
    std::shared_ptr<ServiceTask> previous;
    std::atomic<bool> cancel{false};
    std::atomic<bool> leftDisabled{false}; // cancelled between a disable and its enable
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ServiceTask() = default;
    ServiceTask(const ServiceTask&) = delete;
    ServiceTask& operator=(const ServiceTask&) = delete;
    ~ServiceTask() {
        if (done != nullptr) {
            CloseHandle(done);
        }
    }
    [[nodiscard]] bool Done() const noexcept {
        return done == nullptr || WaitForSingleObject(done, 0) == WAIT_OBJECT_0;
    }
};

void MarkServiceResult(unsigned short id, DWORD state, DWORD rc) {
    char text[112]{};
    std::snprintf(text, sizeof(text), "Bluetooth::SetServiceState %04X %s rc=%lu", static_cast<unsigned>(id),
        state == BLUETOOTH_SERVICE_ENABLE ? "enable" : "disable", static_cast<unsigned long>(rc));
    ProfileScope::Mark(text);
}

void RunServiceTask(ServiceTask& task) {
    if (task.previous != nullptr) {
        if (task.previous->done != nullptr &&
            WaitForSingleObject(task.previous->done, kTaskChainWaitMs) != WAIT_OBJECT_0) {
            ProfileScope::Mark("Bluetooth::ServiceTask predecessor still running");
        }
        task.previous.reset();
    }
    ProfileScope taskScope(task.scopeName != nullptr ? task.scopeName : "Bluetooth::ServiceTask");
    bool pendingEnable = false;
    for (const ServiceStep& step : task.steps) {
        if (task.cancel.load()) {
            ProfileScope::Mark("Bluetooth::ServiceTask cancelled before step");
            if (pendingEnable) {
                task.leftDisabled.store(true);
                ProfileScope::Mark("Bluetooth::A2dpToggle cancelled between disable and enable");
            }
            break;
        }
        ForEachRadio([&](HANDLE radio) {
            BLUETOOTH_DEVICE_INFO info{};
            if (LoadDeviceInfo(radio, task.address, info)) {
                const GUID service = ServiceUuid(step.id);
                MarkServiceResult(step.id, step.state, SetServiceScoped(radio, info, service, step.state));
            }
            return false;
        });
        pendingEnable = step.state == BLUETOOTH_SERVICE_DISABLE;
    }
    if (task.done != nullptr) {
        SetEvent(task.done);
    }
}

DWORD WINAPI ServiceTaskThread(LPVOID param) {
    std::unique_ptr<std::shared_ptr<ServiceTask>> holder(static_cast<std::shared_ptr<ServiceTask>*>(param));
    if (holder != nullptr && *holder != nullptr) {
        RunServiceTask(**holder);
    }
    return 0;
}

// Round 3 (1): ask the Bluetooth audio KS filter to reconnect. This is the
// documented KSPROPSETID_BtAudio / KSPROPERTY_ONESHOT_RECONNECT path the
// Windows audio stack uses for A2DP/HFP endpoints; it pages the device and
// brings up the profile without changing any service state. The filter is
// found via the render endpoint whose connector points at a BTHENUM filter
// interface containing this device address. Worker thread (COM MTA via
// RoInitialize) only.
HRESULT KsReconnect(uint64_t address) {
    ProfileScope profileScope("Bluetooth::KsReconnect");
    wchar_t addressText[16]{};
    std::swprintf(addressText, std::size(addressText), L"%012llX", static_cast<unsigned long long>(address));
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        ProfileScope::Mark("Bluetooth::KsReconnect no MMDeviceEnumerator");
        return hr;
    }
    ComPtr<IMMDeviceCollection> endpoints;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &endpoints);
    if (FAILED(hr) || endpoints == nullptr) {
        return FAILED(hr) ? hr : E_FAIL;
    }
    UINT endpointCount = 0;
    endpoints->GetCount(&endpointCount);
    HRESULT last = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    int filtersTried = 0;
    for (UINT index = 0; index < endpointCount; ++index) {
        ComPtr<IMMDevice> endpoint;
        ComPtr<IDeviceTopology> topology;
        if (FAILED(endpoints->Item(index, &endpoint)) || endpoint == nullptr ||
            FAILED(endpoint->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(topology.GetAddressOf()))) ||
            topology == nullptr) {
            continue;
        }
        UINT connectors = 0;
        topology->GetConnectorCount(&connectors);
        for (UINT c = 0; c < connectors; ++c) {
            ComPtr<IConnector> connector;
            if (FAILED(topology->GetConnector(c, &connector)) || connector == nullptr) {
                continue;
            }
            LPWSTR filterId = nullptr;
            if (FAILED(connector->GetDeviceIdConnectedTo(&filterId)) || filterId == nullptr) {
                continue;
            }
            const std::wstring filter(filterId);
            CoTaskMemFree(filterId);
            if (!ContainsInsensitive(filter, addressText) || !ContainsInsensitive(filter, L"BTH")) {
                continue;
            }
            ComPtr<IMMDevice> filterDevice;
            ComPtr<IKsControl> control;
            if (FAILED(enumerator->GetDevice(filter.c_str(), &filterDevice)) || filterDevice == nullptr ||
                FAILED(filterDevice->Activate(__uuidof(IKsControl), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(control.GetAddressOf()))) ||
                control == nullptr) {
                ProfileScope::Mark("Bluetooth::KsReconnect filter not activatable");
                continue;
            }
            ++filtersTried;
            KSPROPERTY property{};
            property.Set = kKsPropSetBtAudio;
            property.Id = kKsPropOneShotReconnect;
            property.Flags = kKsPropertyTypeGet;
            ULONG returned = 0;
            last = control->KsProperty(&property, sizeof(property), nullptr, 0, &returned);
            char text[96]{};
            std::snprintf(text, sizeof(text), "Bluetooth::KsReconnect hr=0x%08lX", static_cast<unsigned long>(last));
            ProfileScope::Mark(text);
            if (SUCCEEDED(last)) {
                return last;
            }
        }
    }
    if (filtersTried == 0) {
        ProfileScope::Mark("Bluetooth::KsReconnect no BT audio filter for device");
    }
    return last;
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
    // Manual-reset. Set when a newer opposite Connect/Disconnect (or Stop)
    // supersedes the in-flight job; reset under queueMutex on every pop.
    HANDLE cancelJob = nullptr;
    // Guarded by queueMutex.
    bool inflightActive = false;
    JobKind inflightKind = JobKind::Refresh;
    std::wstring inflightId;
    // Worker-only (round 3): background SetServiceState tasks.
    std::shared_ptr<ServiceTask> lastTask;
    std::shared_ptr<ServiceTask> toggleTask; // last A2DP toggle / re-enable
    std::vector<HANDLE> taskThreads;
    // Worker-only (P4 bounded refresh retry).
    bool refreshSuspect = false;
    int refreshRetries = 0;
    ULONGLONG retryDueMs = 0;
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
    ULONGLONG lastRadioRefreshMs = 0;
    ULONGLONG lastPairedRefreshMs = 0;
    std::wstring hint;
    std::wstring busyId;
    std::wstring busyText;
    std::wstring errorId;
    std::wstring errorText;
    std::vector<ComPtr<ABI::Windows::Devices::Radios::IRadio>> radios;
    std::vector<RadioDevice> paired;
    std::vector<RadioDevice> discovered;
    // HOVERDOCK_BT_DRYRUN state: simulated results layered over real
    // enumeration so later refreshes do not undo a simulated action.
    const bool dryRun = BluetoothService::DryRun();
    bool dryRadioSet = false;
    bool dryRadioOn = false;
    std::map<std::wstring, bool> dryConnected;
    std::vector<RadioDevice> dryPaired;
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
            if (JobCancelled()) {
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
            if (JobCancelled()) {
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
            if (job.kind == JobKind::Connect || job.kind == JobKind::Disconnect) {
                // P3: newest intent per device wins. Drop queued Connect /
                // Disconnect / PostConnect for the same id, and cancel an
                // in-flight opposite job (its waits observe cancelJob).
                const size_t before = queue.size();
                std::erase_if(queue, [&](const Job& pending) {
                    return pending.id == job.id &&
                        (pending.kind == JobKind::Connect || pending.kind == JobKind::Disconnect ||
                            pending.kind == JobKind::PostConnect);
                });
                if (queue.size() != before) {
                    ProfileScope::Mark("Bluetooth::Supersede queued");
                }
                // Round 3 (4): user jobs never wait behind a refresh. Abort an
                // in-flight refresh (its WinRT waits observe cancelJob; it keeps
                // last-good state and retries) and queue ahead of refresh /
                // PostConnect / discovery jobs.
                if (inflightActive && inflightKind == JobKind::Refresh && cancelJob != nullptr) {
                    SetEvent(cancelJob);
                    ProfileScope::Mark("Bluetooth::Supersede preempt refresh");
                }
                if (inflightActive && inflightId == job.id && cancelJob != nullptr) {
                    const bool opposite =
                        (inflightKind == JobKind::Connect && job.kind == JobKind::Disconnect) ||
                        (inflightKind == JobKind::Disconnect && job.kind == JobKind::Connect) ||
                        (inflightKind == JobKind::PostConnect && job.kind == JobKind::Disconnect);
                    if (opposite) {
                        SetEvent(cancelJob);
                        ProfileScope::Mark("Bluetooth::Supersede cancel in-flight");
                    }
                }
            }
            if (job.kind == JobKind::Connect || job.kind == JobKind::Disconnect) {
                const auto at = std::find_if(queue.begin(), queue.end(), [](const Job& pending) {
                    return pending.kind != JobKind::Connect && pending.kind != JobKind::Disconnect &&
                        pending.kind != JobKind::Pair && pending.kind != JobKind::SetRadio &&
                        pending.kind != JobKind::Quit;
                });
                if (at != queue.end()) {
                    ProfileScope::Mark("Bluetooth::Enqueue user job ahead of refresh");
                }
                queue.insert(at, std::move(job));
                SetEvent(wake);
                return true;
            }
            if (job.kind == JobKind::Refresh) {
                for (Job& pending : queue) {
                    if (pending.kind == JobKind::Refresh) {
                        if (job.forcePaired) {
                            pending.forcePaired = true;
                        }
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

    static bool DeviceInfoEqual(const BluetoothDeviceInfo& a, const BluetoothDeviceInfo& b) {
        return a.id == b.id && a.name == b.name && a.status == b.status && a.connected == b.connected &&
            a.busy == b.busy;
    }

    static bool SnapshotEqual(const BluetoothSnapshot& a, const BluetoothSnapshot& b) {
        if (a.ready != b.ready || a.radioPresent != b.radioPresent || a.radioOn != b.radioOn ||
            a.discovering != b.discovering || a.hiddenPaired != b.hiddenPaired || a.hint != b.hint ||
            a.paired.size() != b.paired.size() || a.discovered.size() != b.discovered.size()) {
            return false;
        }
        for (size_t i = 0; i < a.paired.size(); ++i) {
            if (!DeviceInfoEqual(a.paired[i], b.paired[i])) {
                return false;
            }
        }
        for (size_t i = 0; i < a.discovered.size(); ++i) {
            if (!DeviceInfoEqual(a.discovered[i], b.discovered[i])) {
                return false;
            }
        }
        return true;
    }

    void Publish() {
    ProfileScope profileScope("Bluetooth::Publish");
        BluetoothSnapshot next;
        next.ready = ready;
        next.radioPresent = radioPresent;
        next.radioOn = radioOn;
        next.discovering = discovering;
        next.hint = hint;

        std::vector<RadioDevice> pairedRanked = paired;
        if (dryRun) {
            if (dryRadioSet && radioPresent) {
                next.radioOn = dryRadioOn;
            }
            for (const RadioDevice& simulated : dryPaired) {
                bool present = false;
                for (const RadioDevice& device : pairedRanked) {
                    present = present || device.id == simulated.id;
                }
                if (!present) {
                    pairedRanked.push_back(simulated);
                }
            }
            for (RadioDevice& device : pairedRanked) {
                if (const auto it = dryConnected.find(device.id); it != dryConnected.end()) {
                    device.connected = it->second;
                }
            }
        }
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

        bool changed = true;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            changed = !SnapshotEqual(snapshot, next);
            snapshot = std::move(next);
        }
        if (changed) {
            Notify();
        }
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
        if (FAILED(WaitResult(operation.Get(), &status, kStatusTimeoutMs))) {
            // Transient (timeout / stack busy): retry on a later refresh.
            ProfileScope::Mark("Bluetooth::EnsureAccess failed");
            accessKnown = false;
            return false;
        }
        accessAllowed = status == ABI::Windows::Devices::Radios::RadioAccessStatus_Allowed;
        return accessAllowed;
    }

    // P5: IRadio objects are cached indefinitely; each refresh reads
    // get_State (~0.1 ms). Re-enumerate (GetRadiosAsync) only when the cache
    // is empty, a get_State fails, or reenumerate is requested.
    // P4: a failed / timed-out enumeration keeps the last good radio state
    // and flags refreshSuspect so the worker retries (bounded).
    void RefreshRadio(bool reenumerate = false) {
    ProfileScope profileScope("Bluetooth::RefreshRadio");
        if (!EnsureAccess()) {
            if (!accessKnown) {
                refreshSuspect = true; // transient; keep last good radio state
                return;
            }
            radioPresent = false;
            radioOn = false;
            radios.clear();
            hint = L"Bluetooth access was denied";
            return;
        }
        if (!reenumerate && !radios.empty()) {
            ProfileScope cachedScope("Bluetooth::RefreshRadio cached");
            bool stateOk = true;
            bool anyOn = false;
            for (const ComPtr<ABI::Windows::Devices::Radios::IRadio>& radio : radios) {
                ABI::Windows::Devices::Radios::RadioState state =
                    ABI::Windows::Devices::Radios::RadioState_Unknown;
                if (radio == nullptr || FAILED(radio->get_State(&state))) {
                    stateOk = false;
                    break;
                }
                if (state == ABI::Windows::Devices::Radios::RadioState_On) {
                    anyOn = true;
                }
            }
            if (stateOk) {
                radioPresent = true;
                radioOn = anyOn;
                return;
            }
            ProfileScope::Mark("Bluetooth::RefreshRadio cached state failed, re-enumerating");
        }
        ProfileScope enumScope("Bluetooth::RefreshRadio enum");
        const auto keepLast = [&](const char* why) {
            ProfileScope::Mark(why);
            refreshSuspect = true;
        };
        ComPtr<ABI::Windows::Devices::Radios::IRadioStatics> statics;
        Microsoft::WRL::Wrappers::HStringReference className(
            RuntimeClass_Windows_Devices_Radios_Radio);
        if (FAILED(RoGetActivationFactory(className.Get(), IID_PPV_ARGS(&statics)))) {
            keepLast("Bluetooth::RefreshRadio keep-last (activation failed)");
            return;
        }
        ComPtr<RadioListAsync> operation;
        if (FAILED(statics->GetRadiosAsync(&operation))) {
            keepLast("Bluetooth::RefreshRadio keep-last (GetRadiosAsync failed)");
            return;
        }
        ComPtr<RadioList> list;
        if (FAILED(WaitResult(operation.Get(), list.GetAddressOf(), kStatusTimeoutMs)) || list == nullptr) {
            keepLast("Bluetooth::RefreshRadio keep-last (GetRadiosAsync timeout/error)");
            return;
        }
        unsigned count = 0;
        if (FAILED(list->get_Size(&count))) {
            keepLast("Bluetooth::RefreshRadio keep-last (list size failed)");
            return;
        }
        std::vector<ComPtr<ABI::Windows::Devices::Radios::IRadio>> found;
        bool foundOn = false;
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
            ABI::Windows::Devices::Radios::RadioState state =
                ABI::Windows::Devices::Radios::RadioState_Unknown;
            if (SUCCEEDED(radio->get_State(&state)) &&
                state == ABI::Windows::Devices::Radios::RadioState_On) {
                foundOn = true;
            }
            found.push_back(std::move(radio));
        }
        if (found.empty() && radioPresent && RadioHandles().Any()) {
            // WinRT lists no Bluetooth radio but the Win32 stack still has one
            // (seen during the profile-disable tail): don't publish radio=0.
            keepLast("Bluetooth::RefreshRadio keep-last (WinRT no radio, Win32 radio present)");
            return;
        }
        radios = std::move(found);
        radioPresent = !radios.empty();
        radioOn = foundOn;
        lastRadioRefreshMs = GetTickCount64();
        if (radioOn && hint == L"Bluetooth access was denied") {
            hint.clear();
        }
    }

    bool SetRadioState(bool enabled) {
        if (radios.empty()) {
            RefreshRadio(true);
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
        // P5: read the cached IRadio state; RefreshRadio re-enumerates only if
        // get_State fails (avoids the measured 4.9 s cold GetRadiosAsync).
        RefreshRadio(false);
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

    ComPtr<DeviceListAsync> StartCollect(bool lowEnergy) {
        HSTRING selector = nullptr;
        if (lowEnergy) {
            ComPtr<ABI::Windows::Devices::Bluetooth::IBluetoothLEDeviceStatics2> statics;
            Microsoft::WRL::Wrappers::HStringReference className(
                RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEDevice);
            if (FAILED(RoGetActivationFactory(className.Get(), IID_PPV_ARGS(&statics))) || statics == nullptr ||
                FAILED(statics->GetDeviceSelectorFromPairingState(TRUE, &selector))) {
                return nullptr;
            }
        } else {
            ComPtr<ABI::Windows::Devices::Bluetooth::IBluetoothDeviceStatics2> statics;
            Microsoft::WRL::Wrappers::HStringReference className(
                RuntimeClass_Windows_Devices_Bluetooth_BluetoothDevice);
            if (FAILED(RoGetActivationFactory(className.Get(), IID_PPV_ARGS(&statics))) || statics == nullptr ||
                FAILED(statics->GetDeviceSelectorFromPairingState(TRUE, &selector))) {
                return nullptr;
            }
        }
        if (selector == nullptr) {
            return nullptr;
        }
        ComPtr<ABI::Windows::Devices::Enumeration::IDeviceInformationStatics> enumeration;
        Microsoft::WRL::Wrappers::HStringReference deviceClass(
            RuntimeClass_Windows_Devices_Enumeration_DeviceInformation);
        auto properties = PropertyList(false);
        if (FAILED(RoGetActivationFactory(deviceClass.Get(), IID_PPV_ARGS(&enumeration))) ||
            enumeration == nullptr || properties == nullptr) {
            WindowsDeleteString(selector);
            return nullptr;
        }
        // PairingState(true) includes IssueInquiry:=False. IsPaired alone returns nothing.
        ComPtr<DeviceListAsync> operation;
        const HRESULT queried = enumeration->FindAllAsyncAqsFilterAndAdditionalProperties(
            selector, properties.Get(), operation.GetAddressOf());
        WindowsDeleteString(selector);
        if (FAILED(queried) || operation == nullptr) {
            return nullptr;
        }
        return operation;
    }

    // Waits for a query started by StartCollect, until the shared deadline.
    bool CollectPaired(bool lowEnergy, DeviceListAsync* operation, ULONGLONG deadline, std::vector<RadioDevice>& dest) {
    ProfileScope profileScope("Bluetooth::CollectPaired");
        if (operation == nullptr) {
            ProfileScope::Mark(lowEnergy ? "Bluetooth::CollectPaired LE start failed"
                                         : "Bluetooth::CollectPaired classic start failed");
            return false;
        }
        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = now >= deadline ? 1 : static_cast<DWORD>(deadline - now);
        ComPtr<DeviceList> list;
        const HRESULT waited = WaitResult(operation, list.GetAddressOf(), remaining);
        if (FAILED(waited) || list == nullptr) {
            char text[128]{};
            std::snprintf(text, sizeof(text), "Bluetooth::CollectPaired %s hr=0x%08lX",
                lowEnergy ? "LE" : "classic", static_cast<unsigned long>(waited));
            ProfileScope::Mark(text);
            ProfileScope::Mark(lowEnergy ? "Bluetooth::CollectPaired LE timeout/error"
                                         : "Bluetooth::CollectPaired classic timeout/error");
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
            // Trust AEP IsConnected from DeviceFromInformation. Per-device
            // FromBluetoothAddressAsync (ApplyClassic/LeConnection) dominated
            // RefreshPaired wall time and is not needed for list correctness.
            Upsert(dest, std::move(device));
        }
        return true;
    }

    // One radio enumeration updates classic fConnected for all remembered addresses.
    // Preserves connect/disconnect UI accuracy without N WinRT Async round-trips.
    void ReconcileClassicLinks(std::vector<RadioDevice>& devices) {
        ProfileScope profileScope("Bluetooth::ReconcileClassicLinks");
        bool anyClassic = false;
        for (const RadioDevice& device : devices) {
            if (device.address != 0 && !device.lowEnergy) {
                anyClassic = true;
                break;
            }
        }
        if (!anyClassic) {
            return;
        }
        for (RadioDevice& device : devices) {
            if (device.address != 0 && !device.lowEnergy) {
                device.connected = false;
            }
        }
        ForEachRadio([&](HANDLE radio) {
            for (RadioDevice& device : devices) {
                if (device.address == 0 || device.lowEnergy) {
                    continue;
                }
                if (RadioConnected(radio, device.address)) {
                    device.connected = true;
                }
            }
            return false; // visit every radio
        });
    }

    void RefreshPaired() {
    ProfileScope profileScope("Bluetooth::RefreshPaired");
        std::vector<RadioDevice> next;
        // Round 3 (4): classic and LE queries run in parallel under ONE
        // kStatusTimeoutMs bound (was two serial 2.5 s timeouts = 5.0 s).
        ComPtr<DeviceListAsync> classicOp;
        ComPtr<DeviceListAsync> leOp;
        {
            ProfileScope startScope("Bluetooth::RefreshPaired start queries");
            classicOp = StartCollect(false);
            leOp = StartCollect(true);
        }
        const ULONGLONG deadline = GetTickCount64() + kStatusTimeoutMs;
        const bool classicOk = CollectPaired(false, classicOp.Get(), deadline, next);
        const bool leOk = CollectPaired(true, leOp.Get(), deadline, next);
        if (JobCancelled()) {
            ProfileScope::Mark("Bluetooth::Refresh preempted by user job");
        }
        if (!classicOk && !leOk) {
            // P4: both queries failed / timed out: keep the last good list.
            ProfileScope::Mark("Bluetooth::RefreshPaired keep-last (both queries failed)");
            refreshSuspect = true;
            return;
        }
        // P4: a classic device that the Win32 stack still remembers (paired)
        // but that this enumeration dropped is a transient AEP gap (seen
        // after the old profile-disable tail). Keep the last good entry and
        // retry instead of publishing an empty list.
        if (radioOn) {
            for (const RadioDevice& previous : paired) {
                if (previous.address == 0 || previous.lowEnergy) {
                    continue;
                }
                const bool present = std::any_of(next.begin(), next.end(), [&](const RadioDevice& device) {
                    return device.id == previous.id || (device.address != 0 && device.address == previous.address);
                });
                if (!present && RememberedClassic(previous.address)) {
                    ProfileScope::Mark("Bluetooth::RefreshPaired keep-last (paired device missing from AEP)");
                    refreshSuspect = true;
                    next.push_back(previous);
                }
            }
        }
        ReconcileClassicLinks(next);
        paired = std::move(next);
        lastPairedRefreshMs = GetTickCount64();
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
        // Keep LE connect bounded â€” a 20s GATT wait made classic-fallback
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

    [[nodiscard]] bool JobCancelled() const noexcept {
        return !running.load() ||
            (cancelJob != nullptr && WaitForSingleObject(cancelJob, 0) == WAIT_OBJECT_0);
    }

    // Event-driven link wait. Wakes on: the job cancel event, the classic
    // BluetoothDevice.ConnectionStatusChanged notification, an optional
    // caller handle, or a kLinkPollMs fallback that reads ACL fConnected on
    // radio handles opened once. Bounded by timeoutMs. Never runs on the UI
    // thread (worker only).
    LinkWait WaitLink(uint64_t address, bool wantConnected, DWORD timeoutMs, HANDLE extra = nullptr) {
        ProfileScope profileScope("Bluetooth::WaitForLink");
        const RadioHandles radiosForWait;
        const auto signal = std::make_shared<LinkSignal>();
        ComPtr<ABI::Windows::Devices::Bluetooth::IBluetoothDevice> device;
        ComPtr<ClassicDeviceAsync> opening;
        EventRegistrationToken token{};
        bool registered = false;
        {
            ComPtr<ABI::Windows::Devices::Bluetooth::IBluetoothDeviceStatics> statics;
            Microsoft::WRL::Wrappers::HStringReference className(
                RuntimeClass_Windows_Devices_Bluetooth_BluetoothDevice);
            if (SUCCEEDED(RoGetActivationFactory(className.Get(), IID_PPV_ARGS(&statics))) && statics != nullptr) {
                // Started, not awaited: polled below so it never delays the wait.
                statics->FromBluetoothAddressAsync(address, opening.GetAddressOf());
            }
        }
        const auto tryRegister = [&]() {
            if (registered || opening == nullptr || signal->event == nullptr) {
                return;
            }
            ComPtr<IAsyncInfo> info;
            AsyncStatus status = AsyncStatus::Started;
            if (FAILED(opening.As(&info)) || FAILED(info->get_Status(&status))) {
                opening.Reset();
                return;
            }
            if (status == AsyncStatus::Started) {
                return;
            }
            if (status == AsyncStatus::Completed && SUCCEEDED(opening->GetResults(device.GetAddressOf())) &&
                device != nullptr) {
                auto handler = Microsoft::WRL::Callback<
                    __FITypedEventHandler_2_Windows__CDevices__CBluetooth__CBluetoothDevice_IInspectable>(
                    [signal](ABI::Windows::Devices::Bluetooth::IBluetoothDevice*, IInspectable*) -> HRESULT {
                        SetEvent(signal->event);
                        return S_OK;
                    });
                if (handler != nullptr && SUCCEEDED(device->add_ConnectionStatusChanged(handler.Get(), &token))) {
                    registered = true;
                }
            }
            opening.Reset();
        };
        const auto deviceSays = [&]() {
            if (!registered || device == nullptr) {
                return false;
            }
            ABI::Windows::Devices::Bluetooth::BluetoothConnectionStatus status =
                ABI::Windows::Devices::Bluetooth::BluetoothConnectionStatus_Disconnected;
            return SUCCEEDED(device->get_ConnectionStatus(&status)) &&
                (status == ABI::Windows::Devices::Bluetooth::BluetoothConnectionStatus_Connected) == wantConnected;
        };
        constexpr DWORD kNone = MAXDWORD;
        HANDLE handles[3]{};
        DWORD count = 0;
        if (cancelJob != nullptr) {
            handles[count++] = cancelJob;
        }
        DWORD linkIndex = kNone;
        if (signal->event != nullptr) {
            linkIndex = count;
            handles[count++] = signal->event;
        }
        DWORD extraIndex = kNone;
        if (extra != nullptr) {
            extraIndex = count;
            handles[count++] = extra;
        }
        LinkWait result = LinkWait::Timeout;
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;) {
            if (radiosForWait.Connected(address) == wantConnected || deviceSays()) {
                ProfileScope::Mark(wantConnected ? "Bluetooth::WaitForLink up" : "Bluetooth::WaitForLink down");
                result = LinkWait::Reached;
                break;
            }
            if (JobCancelled()) {
                ProfileScope::Mark("Bluetooth::WaitForLink cancelled");
                result = LinkWait::Cancelled;
                break;
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                ProfileScope::Mark("Bluetooth::WaitForLink timeout");
                break;
            }
            const DWORD slice = static_cast<DWORD>(std::min<ULONGLONG>(kLinkPollMs, deadline - now));
            DWORD waited = WAIT_TIMEOUT;
            if (count == 0) {
                Sleep(slice);
            } else {
                waited = WaitForMultipleObjects(count, handles, FALSE, slice);
            }
            if (linkIndex != kNone && waited == WAIT_OBJECT_0 + linkIndex) {
                ProfileScope::Mark("Bluetooth::WaitForLink event");
            } else if (extraIndex != kNone && waited == WAIT_OBJECT_0 + extraIndex) {
                result = LinkWait::Extra;
                break;
            } else if (waited == WAIT_FAILED) {
                Sleep(slice);
            }
            tryRegister();
        }
        if (registered && device != nullptr) {
            device->remove_ConnectionStatusChanged(token);
        }
        if (opening != nullptr) {
            ComPtr<IAsyncInfo> info;
            if (SUCCEEDED(opening.As(&info))) {
                info->Cancel();
            }
        }
        if (device != nullptr) {
            ComPtr<ABI::Windows::Foundation::IClosable> closable;
            if (SUCCEEDED(device.As(&closable))) {
                closable->Close();
            }
        }
        return result;
    }

    // Starts steps on a helper thread, chained after the previous task.
    std::shared_ptr<ServiceTask> StartServiceTask(uint64_t address, const char* scopeName,
        std::vector<ServiceStep> steps) {
        for (auto it = taskThreads.begin(); it != taskThreads.end();) {
            if (WaitForSingleObject(*it, 0) == WAIT_OBJECT_0) {
                CloseHandle(*it);
                it = taskThreads.erase(it);
            } else {
                ++it;
            }
        }
        auto task = std::make_shared<ServiceTask>();
        task->address = address;
        task->scopeName = scopeName;
        task->steps = std::move(steps);
        if (lastTask != nullptr && !lastTask->Done()) {
            task->previous = lastTask;
        }
        if (task->done == nullptr) {
            return nullptr;
        }
        auto* holder = new std::shared_ptr<ServiceTask>(task);
        HANDLE helper = CreateThread(nullptr, 0, ServiceTaskThread, holder, 0, nullptr);
        if (helper == nullptr) {
            delete holder;
            ProfileScope::Mark("Bluetooth::ServiceTask thread failed");
            return nullptr;
        }
        taskThreads.push_back(helper);
        lastTask = task;
        return task;
    }

    // Bounded, cancellable wait for the previous service task (worker).
    bool WaitTaskIdle(DWORD timeoutMs) {
        if (lastTask == nullptr || lastTask->Done()) {
            return true;
        }
        ProfileScope profileScope("Bluetooth::WaitServiceTask");
        HANDLE handles[2]{lastTask->done, cancelJob};
        const DWORD count = cancelJob != nullptr ? 2u : 1u;
        return WaitForMultipleObjects(count, handles, FALSE, timeoutMs) == WAIT_OBJECT_0;
    }

    // Watches the link while a service task runs; returns at link-up even if
    // the task is still inside SetServiceState (round 3, item 2).
    LinkWait WaitLinkDuringTask(uint64_t address, const std::shared_ptr<ServiceTask>& task) {
        const ULONGLONG deadline = GetTickCount64() + kToggleTotalWaitMs;
        HANDLE extra = task != nullptr ? task->done : nullptr;
        ULONGLONG tailDeadline = deadline;
        for (;;) {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG until = std::min(deadline, tailDeadline);
            const DWORD remaining = now >= until ? 0 : static_cast<DWORD>(until - now);
            const LinkWait waited = WaitLink(address, true, remaining, extra);
            if (waited != LinkWait::Extra) {
                if (waited == LinkWait::Reached && task != nullptr && !task->Done()) {
                    ProfileScope::Mark("Bluetooth::Connect link-up during service task");
                }
                if (waited == LinkWait::Cancelled && task != nullptr) {
                    task->cancel.store(true);
                }
                return waited;
            }
            ProfileScope::Mark("Bluetooth::ServiceTask done before link");
            extra = nullptr;
            tailDeadline = GetTickCount64() + kToggleLinkWaitMs;
        }
    }

    // Diagnostic / secondary page: L2CAP PSM 0x19 via Winsock. Round 2 saw
    // an immediate connect error in 17/19 jobs; the code is now logged and
    // an immediate error returns at once (no 1.5 s grace).
    LinkWait L2capPage(uint64_t address) {
        ProfileScope profileScope("Bluetooth::L2capPage");
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return LinkWait::Timeout;
        }
        char text[112]{};
        LinkWait result = LinkWait::Timeout;
        const SOCKET sock = socket(AF_BTH, SOCK_STREAM, BTHPROTO_L2CAP);
        if (sock == INVALID_SOCKET) {
            std::snprintf(text, sizeof(text), "Bluetooth::PageAudio socket error wsa=%d", WSAGetLastError());
            ProfileScope::Mark(text);
            WSACleanup();
            return result;
        }
        const WSAEVENT pageReady = WSACreateEvent();
        if (pageReady != WSA_INVALID_EVENT && WSAEventSelect(sock, pageReady, FD_CONNECT) == 0) {
            SOCKADDR_BTH remote{};
            remote.addressFamily = AF_BTH;
            remote.btAddr = address;
            remote.serviceClassId = ServiceUuid(0x110B);
            remote.port = 0x0019;
            if (connect(sock, reinterpret_cast<SOCKADDR*>(&remote), sizeof(remote)) != 0) {
                const int error = WSAGetLastError();
                if (error != WSAEWOULDBLOCK) {
                    std::snprintf(text, sizeof(text), "Bluetooth::PageAudio connect error wsa=%d", error);
                    ProfileScope::Mark("Bluetooth::PageAudio connect error");
                    ProfileScope::Mark(text);
                } else {
                    ProfileScope::Mark("Bluetooth::PageAudio l2cap pending");
                    result = WaitLink(address, true, kPageLinkWaitMs, pageReady);
                    if (result == LinkWait::Extra) {
                        WSANETWORKEVENTS network{};
                        int code = -1;
                        if (WSAEnumNetworkEvents(sock, pageReady, &network) == 0 &&
                            (network.lNetworkEvents & FD_CONNECT) != 0) {
                            code = network.iErrorCode[FD_CONNECT_BIT];
                        }
                        std::snprintf(text, sizeof(text), "Bluetooth::PageAudio l2cap result wsa=%d", code);
                        ProfileScope::Mark(text);
                        ProfileScope::Mark(code == 0 ? "Bluetooth::PageAudio l2cap connected"
                                                     : "Bluetooth::PageAudio l2cap error");
                        result = WaitLink(address, true, code == 0 ? kPageLinkWaitMs : kPageErrorGraceMs);
                    }
                }
            } else {
                ProfileScope::Mark("Bluetooth::PageAudio l2cap connected");
                result = WaitLink(address, true, kPageLinkWaitMs);
            }
        } else {
            ProfileScope::Mark("Bluetooth::PageAudio socket setup failed");
        }
        closesocket(sock);
        if (pageReady != WSA_INVALID_EVENT) {
            WSACloseEvent(pageReady);
        }
        WSACleanup();
        return result;
    }

    // P2 page. 1) KS one-shot reconnect on the device's BT audio filter;
    // 2) L2CAP PSM 0x19 (diagnostic, fails fast). No SetServiceState here.
    LinkWait PageAudio(uint64_t address) {
        ProfileScope profileScope("Bluetooth::PageAudio");
        const HRESULT ks = KsReconnect(address);
        if (SUCCEEDED(ks)) {
            ProfileScope::Mark("Bluetooth::PageAudio branch=ks");
            const LinkWait waited = WaitLink(address, true, kPageLinkWaitMs);
            if (waited != LinkWait::Timeout) {
                return waited;
            }
            ProfileScope::Mark("Bluetooth::PageAudio ks no link");
        }
        if (JobCancelled()) {
            return LinkWait::Cancelled;
        }
        ProfileScope::Mark("Bluetooth::PageAudio branch=l2cap");
        return L2capPage(address);
    }

    // P2 connect. Audio: KS reconnect page, then (only if no link) ONE A2DP
    // toggle on a helper thread, publishing at link-up even mid-toggle.
    LinkWait ConnectClassic(uint64_t address, bool& audio) {
        ProfileScope profileScope("Bluetooth::ConnectClassic");
        audio = false;
        if (address == 0 || !running.load()) {
            return LinkWait::Timeout;
        }
        ForEachRadio([&](HANDLE radio) {
            BLUETOOTH_DEVICE_INFO info{};
            if (!LoadDeviceInfo(radio, address, info)) {
                return false;
            }
            audio = UsesAudio(radio, info);
            return true;
        });
        if (AnyRadioConnected(address)) {
            ProfileScope::Mark("Bluetooth::Connect already up");
            return LinkWait::Reached;
        }
        if (!audio) {
            // Non-audio devices: unchanged profile nudge, event-driven waits.
            const auto nudge = [&](HANDLE radio) {
                BLUETOOTH_DEVICE_INFO info{};
                if (LoadDeviceInfo(radio, address, info)) {
                    EnableGeneral(radio, info);
                }
                return false;
            };
            ForEachRadio(nudge);
            LinkWait waited = WaitLink(address, true, 3500);
            if (waited != LinkWait::Timeout) {
                return waited;
            }
            ForEachRadio(nudge);
            return WaitLink(address, true, 2000);
        }
        // A superseded toggle may still be inside the driver: never stack a
        // page on top of it (bounded, cancellable).
        if (!WaitTaskIdle(kTaskJoinMs)) {
            if (JobCancelled()) {
                return LinkWait::Cancelled;
            }
            ProfileScope::Mark("Bluetooth::Connect previous service task still running");
        }
        if (toggleTask != nullptr && toggleTask->address == address && toggleTask->Done() &&
            toggleTask->leftDisabled.load()) {
            // A cancelled toggle left 110B disabled: enable-only is the page
            // (round 1: enabling a disabled 110B paged, link-up 1.6 s median).
            ProfileScope::Mark("Bluetooth::Connect branch=reenable-a2dp");
            toggleTask = StartServiceTask(address, "Bluetooth::A2dpReenable",
                {ServiceStep{0x110B, BLUETOOTH_SERVICE_ENABLE}});
            if (toggleTask != nullptr) {
                const LinkWait waited = WaitLinkDuringTask(address, toggleTask);
                if (waited != LinkWait::Timeout) {
                    return waited;
                }
            }
        }
        const LinkWait paged = PageAudio(address);
        if (paged != LinkWait::Timeout) {
            return paged; // Reached or Cancelled
        }
        if (JobCancelled()) {
            return LinkWait::Cancelled;
        }
        ProfileScope::Mark("Bluetooth::Connect page failed, A2DP toggle");
        ProfileScope::Mark("Bluetooth::Connect branch=toggle");
        toggleTask = StartServiceTask(address, "Bluetooth::A2dpToggle",
            {ServiceStep{0x110B, BLUETOOTH_SERVICE_DISABLE}, ServiceStep{0x110B, BLUETOOTH_SERVICE_ENABLE}});
        if (toggleTask == nullptr) {
            return LinkWait::Timeout;
        }
        return WaitLinkDuringTask(address, toggleTask);
    }

    // P1: IOCTL_BTH_DISCONNECT_DEVICE only (no BluetoothSetServiceState
    // disables), then an event wait for the link to go down. Runs at once
    // even if a superseded toggle is still in SetServiceState; that task is
    // told to stop before its next step.
    LinkWait DisconnectClassic(uint64_t address) {
        ProfileScope profileScope("Bluetooth::DisconnectClassic");
        if (address == 0) {
            return LinkWait::Timeout;
        }
        if (lastTask != nullptr && !lastTask->Done() && lastTask->address == address) {
            lastTask->cancel.store(true);
            ProfileScope::Mark("Bluetooth::Disconnect cancel running service task");
        }
        bool issued = false;
        ForEachRadio([&](HANDLE radio) {
            DWORD returned = 0;
            ProfileScope ioctlScope("Bluetooth::DisconnectIoctl");
            if (DeviceIoControl(radio, kDisconnectIoctl, &address, sizeof(address), nullptr, 0, &returned,
                    nullptr) != FALSE) {
                issued = true;
            }
            return false; // every radio
        });
        if (!issued) {
            ProfileScope::Mark("Bluetooth::DisconnectIoctl failed");
        }
        const LinkWait waited = WaitLink(address, false, kDisconnectWaitMs);
        if (waited == LinkWait::Timeout && !AnyRadioConnected(address)) {
            return LinkWait::Reached;
        }
        return waited;
    }

    // After a disconnect was published: if a superseded service task was
    // still running, wait for it (bounded, cancellable by a newer Connect)
    // and drop the link again if its enable re-paged the device.
    void ReapTaskAfterDisconnect(uint64_t address) {
        if (lastTask == nullptr || lastTask->Done() || lastTask->address != address) {
            return;
        }
        if (!WaitTaskIdle(kTaskJoinMs) || JobCancelled()) {
            return;
        }
        if (AnyRadioConnected(address)) {
            ProfileScope::Mark("Bluetooth::Disconnect re-drop after service task");
            DisconnectClassic(address);
        }
    }

    LinkWait ConnectDevice(const RadioDevice& device, bool& audio) {
        audio = false;
        uint64_t address = device.address;
        if (address == 0) {
            address = AddressFromTaggedId(device.id);
        }
        if (address == 0) {
            address = ParseBluetoothAddress(device.id);
        }
        const LinkWait classic = ConnectClassic(address, audio);
        if (classic == LinkWait::Reached || classic == LinkWait::Cancelled) {
            return classic;
        }
        if (device.lowEnergy && !RememberedClassic(address)) {
            return ConnectLe(address) ? LinkWait::Reached : LinkWait::Timeout;
        }
        return classic;
    }

    // Runs as its own worker job after the connect result is published.
    // Enable-only and only for services that are missing; cancelled by a
    // newer Disconnect between steps.
    void PostConnect(const Job& job) {
        const RadioDevice* device = FindMutable(job.id);
        if (device == nullptr) {
            return;
        }
        uint64_t address = device->address;
        if (address == 0) {
            address = AddressFromTaggedId(device->id);
        }
        if (address == 0 || JobCancelled() || !AnyRadioConnected(address)) {
            return;
        }
        std::vector<ServiceStep> steps;
        ForEachRadio([&](HANDLE radio) {
            BLUETOOTH_DEVICE_INFO info{};
            if (!LoadDeviceInfo(radio, address, info) || !UsesAudio(radio, info)) {
                return false;
            }
            const std::vector<GUID> installed = InstalledServices(radio, info);
            // Diagnostic: which services Windows has enabled for the device.
            std::string list = "Bluetooth::PostConnect services:";
            for (const GUID& service : installed) {
                char item[16]{};
                const unsigned short id = ShortServiceId(service);
                if (id != 0) {
                    std::snprintf(item, sizeof(item), " %04X", static_cast<unsigned>(id));
                } else {
                    std::snprintf(item, sizeof(item), " other");
                }
                list += item;
            }
            ProfileScope::Mark(list.c_str());
            if (installed.empty()) {
                ProfileScope::Mark("Bluetooth::PostConnect services unknown");
                return true;
            }
            const auto has = [&](unsigned short id) {
                const GUID wanted = ServiceUuid(id);
                return std::any_of(installed.begin(), installed.end(),
                    [&](const GUID& service) { return IsEqualGUID(service, wanted) != FALSE; });
            };
            if (!has(0x110E)) {
                steps.push_back(ServiceStep{0x110E, BLUETOOTH_SERVICE_ENABLE});
            }
            if (!has(0x111E)) {
                ProfileScope::Mark("Bluetooth::RestoreVoice HFP missing");
                steps.push_back(ServiceStep{0x111E, BLUETOOTH_SERVICE_ENABLE});
            }
            return true;
        });
        if (!steps.empty() && !JobCancelled()) {
            // Enable-only, on the helper thread (chained after any toggle) so
            // a Disconnect never waits behind it.
            StartServiceTask(address, "Bluetooth::PostConnectEnable", std::move(steps));
        }
    }

    void RememberError(const std::wstring& id, const wchar_t* text, bool sectionHint) {
        errorId = id;
        errorText = text == nullptr ? L"" : text;
        if (sectionHint) {
            hint = errorText;
        }
    }

    static const char* JobName(JobKind kind) noexcept {
        switch (kind) {
        case JobKind::Refresh:
            return "Bluetooth::Job Refresh";
        case JobKind::SetRadio:
            return "Bluetooth::Job SetRadio";
        case JobKind::Connect:
            return "Bluetooth::Job Connect";
        case JobKind::Disconnect:
            return "Bluetooth::Job Disconnect";
        case JobKind::StartDiscovery:
            return "Bluetooth::Job StartDiscovery";
        case JobKind::StopDiscovery:
            return "Bluetooth::Job StopDiscovery";
        case JobKind::Pair:
            return "Bluetooth::Job Pair";
        case JobKind::PostConnect:
            return "Bluetooth::Job PostConnect";
        case JobKind::Quit:
            return "Bluetooth::Job Quit";
        }
        return "Bluetooth::Job ?";
    }

    // Simulated mutating job (HOVERDOCK_BT_DRYRUN=1). Same busy/publish
    // sequence as the real path so the UI is exercised end-to-end; no WinRT
    // Radio / pairing / IOCTL calls are made.
    void HandleDryRun(const Job& job) {
        ProfileScope::Mark((std::string("Bluetooth::DryRun ") + JobName(job.kind)).c_str());
        switch (job.kind) {
        case JobKind::SetRadio:
            dryRadioSet = true;
            dryRadioOn = job.enableRadio;
            radioOn = radioPresent && job.enableRadio;
            if (!job.enableRadio) {
                StopWatcher();
                discovering = false;
                discovered.clear();
                enumerationCompleted = false;
                hint.clear();
            }
            Sleep(400);
            break;
        case JobKind::Connect:
        case JobKind::Disconnect: {
            const bool connect = job.kind == JobKind::Connect;
            busyId = job.id;
            busyText = connect ? L"Connecting..." : L"Disconnecting...";
            errorId.clear();
            Publish();
            Sleep(900);
            dryConnected[job.id] = connect;
            busyId.clear();
            busyText.clear();
            break;
        }
        case JobKind::Pair: {
            // Copy up front: watcher callbacks may reshuffle `discovered`.
            const RadioDevice* found = FindMutable(job.id);
            const bool known = found != nullptr;
            RadioDevice simulated = known ? *found : RadioDevice{};
            busyId = job.id;
            busyText = L"Pairing...";
            hint = L"Confirm the pairing prompt";
            Publish();
            Sleep(1200);
            if (known) {
                simulated.canPair = false;
                busyText = L"Connecting...";
                Publish();
                Sleep(700);
                simulated.connected = true;
                dryConnected[simulated.id] = true;
                dryPaired.push_back(simulated);
                RemoveId(job.id);
            }
            busyId.clear();
            busyText.clear();
            hint.clear();
            pairing.store(false);
            break;
        }
        default:
            break;
        }
        ready = true;
        Publish();
    }

    void Handle(const Job& job) {
        // Call-path audit (HOVERDOCK_PROFILE): every job logs the worker tid
        // so it can be compared with the UI thread tid marked on click.
        ProfileScope::Mark(BluetoothService::ThreadTag(JobName(job.kind)).c_str());
        ProfileScope jobScope(JobName(job.kind));
        if (dryRun && (job.kind == JobKind::SetRadio || job.kind == JobKind::Connect ||
                          job.kind == JobKind::Disconnect || job.kind == JobKind::Pair)) {
            HandleDryRun(job);
            return;
        }
        switch (job.kind) {
        case JobKind::Refresh: {
            ProfileScope profileScope("Bluetooth::HandleRefresh");
            RefreshRadio();
            if (!discovering && !pairing.load()) {
                constexpr ULONGLONG kPairedCacheMs = 12000;
                const ULONGLONG now = GetTickCount64();
                const bool cacheFresh = lastPairedRefreshMs != 0 &&
                    (now - lastPairedRefreshMs) < kPairedCacheMs && !paired.empty();
                if (job.forcePaired || !cacheFresh) {
                    RefreshPaired();
                }
            }
            // P4: bounded retry when this refresh kept last-good state.
            if (refreshSuspect && refreshRetries < kMaxRefreshRetries) {
                ++refreshRetries;
                retryDueMs = GetTickCount64() + kRefreshRetryMs;
                ProfileScope::Mark("Bluetooth::Refresh retry scheduled");
            } else if (!refreshSuspect) {
                refreshRetries = 0;
                retryDueMs = 0;
            } else {
                ProfileScope::Mark("Bluetooth::Refresh retries exhausted");
                retryDueMs = 0;
            }
            refreshSuspect = false;
            ready = true;
            Publish();
            break;
        }
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
            bool audio = false;
            const LinkWait outcome = connect ? ConnectDevice(copy, audio) : DisconnectClassic(address);
            const bool ok = outcome == LinkWait::Reached;
            const bool cancelled = outcome == LinkWait::Cancelled;
            if (connect && ok) {
                ProfileScope::Mark("Bluetooth::Connect marked connected at link-up");
            }
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
                } else if (cancelled) {
                    // Superseded by a newer opposite request: no error row;
                    // reflect the real link, the queued job decides the rest.
                    ProfileScope::Mark("Bluetooth::Job superseded");
                    if (address != 0) {
                        current->connected = AnyRadioConnected(address);
                    }
                } else {
                    RememberError(job.id, connect ? L"Couldn't connect" : L"Couldn't disconnect", false);
                }
            } else if (!ok && !cancelled) {
                RememberError(job.id, connect ? L"Couldn't connect" : L"Couldn't disconnect", false);
            }
            Publish();
            if (!connect && ok && running.load()) {
                ReapTaskAfterDisconnect(address);
                if (address != 0 && AnyRadioConnected(address)) {
                    if (RadioDevice* current = FindMutable(job.id); current != nullptr) {
                        current->connected = true;
                    }
                    Publish();
                }
            }
            // Reconcile with the radio shortly after; do not block this job.
            if (ok && running.load()) {
                Job refresh{JobKind::Refresh, false, {}};
                refresh.forcePaired = true;
                Enqueue(std::move(refresh));
                if (connect && audio) {
                    // AVRCP / HFP restore off the critical path (after link-up,
                    // after the result is published).
                    Job post{JobKind::PostConnect, false, job.id};
                    Enqueue(std::move(post));
                }
            }
            break;
        }
        case JobKind::PostConnect:
            PostConnect(job);
            break;
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
            if (dryRun && discovering) {
                // Dry-run harness: one simulated nearby device so the pair +
                // connect UI path can be exercised without real hardware.
                RadioDevice fake;
                fake.id = L"dryrun#BluetoothTestSpeaker";
                fake.name = L"HoverDock Dry-Run Speaker With A Very Long Device Name";
                fake.canPair = true;
                fake.signal = -40;
                Upsert(discovered, std::move(fake));
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
            if (pairedOk) {
                errorId.clear();
                errorText.clear();
                hint.clear();
                uint64_t address = 0;
                if (const RadioDevice* found = FindMutable(job.id); found != nullptr) {
                    address = found->address;
                }
                if (address == 0) {
                    address = AddressFromTaggedId(job.id);
                }
                RemoveId(job.id);
                RefreshPaired();
                // Pair + connect: many headsets link on their own after
                // pairing; if not, connect once (still on this worker).
                RadioDevice* added = nullptr;
                for (RadioDevice& device : paired) {
                    if (device.id == job.id || (address != 0 && device.address == address)) {
                        added = &device;
                        break;
                    }
                }
                if (added != nullptr && !added->connected && running.load()) {
                    const RadioDevice copy = *added;
                    busyId = copy.id;
                    busyText = L"Connecting...";
                    Publish();
                    bool pairAudio = false;
                    const bool linked = ConnectDevice(copy, pairAudio) == LinkWait::Reached;
                    if (linked && pairAudio && running.load()) {
                        Enqueue(Job{JobKind::PostConnect, false, copy.id});
                    }
                    if (RadioDevice* current = FindMutable(copy.id); current != nullptr) {
                        if (linked) {
                            current->connected = true;
                        } else {
                            RememberError(copy.id, L"Paired \u00B7 couldn't connect", false);
                        }
                    }
                }
            }
            busyId.clear();
            busyText.clear();
            pairing.store(false);
            if (!pairedOk && running.load()) {
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
            // One job at a time so a newer Connect/Disconnect can still
            // supersede queued jobs (P3) and cancel the in-flight one.
            Job job;
            bool have = false;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (!queue.empty()) {
                    job = std::move(queue.front());
                    queue.erase(queue.begin());
                    have = true;
                    inflightActive = true;
                    inflightKind = job.kind;
                    inflightId = job.id;
                    if (cancelJob != nullptr && running.load()) {
                        ResetEvent(cancelJob);
                    }
                } else {
                    inflightActive = false;
                    inflightId.clear();
                }
            }
            if (!have) {
                DWORD timeout = INFINITE;
                if (retryDueMs != 0) {
                    const ULONGLONG now = GetTickCount64();
                    timeout = now >= retryDueMs ? 0 : static_cast<DWORD>(retryDueMs - now);
                }
                if (WaitForSingleObject(wake, timeout) == WAIT_TIMEOUT && retryDueMs != 0) {
                    retryDueMs = 0;
                    Job retry{JobKind::Refresh, false, {}};
                    retry.forcePaired = true;
                    Enqueue(std::move(retry));
                }
                continue;
            }
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
        StopWatcher();
        if (SUCCEEDED(ro)) {
            RoUninitialize();
        }
    }

    bool Stop() noexcept {
        running.store(false);
        pairing.store(false);
        if (cancelJob != nullptr) {
            SetEvent(cancelJob);
        }
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
            // Worker has exited: helper threads only touch their own task.
            // Ask any in-flight toggle to stop between steps, then join
            // finished ones. A thread still inside SetServiceState is
            // detached (its task lives via the thread's own shared_ptr);
            // its HANDLE is left open so CloseHandle is not raced.
            if (lastTask != nullptr) {
                lastTask->cancel.store(true);
            }
            for (HANDLE helper : taskThreads) {
                if (WaitForSingleObject(helper, 50) == WAIT_OBJECT_0) {
                    CloseHandle(helper);
                }
            }
            taskThreads.clear();
            lastTask.reset();
            toggleTask.reset();
            if (wake != nullptr) {
                CloseHandle(wake);
                wake = nullptr;
            }
            if (cancelJob != nullptr) {
                CloseHandle(cancelJob);
                cancelJob = nullptr;
            }
        } else {
            if (wake != nullptr) {
                CloseHandle(wake);
                wake = nullptr;
            }
            if (cancelJob != nullptr) {
                CloseHandle(cancelJob);
                cancelJob = nullptr;
            }
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
    m_impl->cancelJob = CreateEventW(nullptr, TRUE, FALSE, nullptr);
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
        if (m_impl->cancelJob != nullptr) {
            CloseHandle(m_impl->cancelJob);
            m_impl->cancelJob = nullptr;
        }
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

void BluetoothService::RequestRefresh(bool forcePaired) {
    if (m_impl != nullptr) {
        Job job{JobKind::Refresh, false, {}};
        job.forcePaired = forcePaired;
        m_impl->Enqueue(std::move(job));
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

bool BluetoothService::DryRun() noexcept {
    static const bool enabled = []() {
        wchar_t value[8]{};
        return GetEnvironmentVariableW(L"HOVERDOCK_BT_DRYRUN", value, static_cast<DWORD>(std::size(value))) != 0 &&
            value[0] != L'0';
    }();
    return enabled;
}

std::string BluetoothService::ThreadTag(const char* label) {
    char buffer[160]{};
    std::snprintf(buffer, sizeof(buffer), "%s tid=%lu", label == nullptr ? "" : label,
        static_cast<unsigned long>(GetCurrentThreadId()));
    return buffer;
}

bool BluetoothService::IsPairing() const noexcept {
    return m_impl != nullptr && m_impl->pairing.load();
}

void BluetoothService::AllowNextNotify() noexcept {
    if (m_impl != nullptr) {
        m_impl->AllowNextNotify();
    }
}
