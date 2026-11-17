#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32) && !defined(DOCK_AUDIO_POLICY_NO_WIN32)
#pragma warning(push)
#pragma warning(disable : 4061 4100 4265 4365 4371 4458 4464 4619 4668 4986 5026 5027 5204 5220 5246)
#include <Windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <endpointvolume.h>
#pragma warning(pop)
#endif

namespace audio_policy {

inline constexpr size_t kMaxAudioChannels = 8;
inline constexpr float kBluetoothBalanceTolerance = 0.002F;
inline constexpr float kWiredBalancedTolerance = 0.015F;

enum class BtAudioProfile : uint8_t {
    None = 0,
    A2dpStereo = 1,
    HandsFree = 2,
};

[[nodiscard]] constexpr wchar_t AsciiLower(wchar_t ch) noexcept {
    return (ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
}

[[nodiscard]] inline bool ContainsInsensitive(std::wstring_view haystack,
    std::wstring_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    const size_t last = haystack.size() - needle.size();
    for (size_t start = 0; start <= last; ++start) {
        bool match = true;
        for (size_t index = 0; index < needle.size(); ++index) {
            if (AsciiLower(haystack[start + index]) != AsciiLower(needle[index])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool IsBluetoothConnectorId(std::wstring_view id) noexcept {
    return !id.empty() && (ContainsInsensitive(id, L"btha2dp") ||
        ContainsInsensitive(id, L"bthhfenum") ||
        ContainsInsensitive(id, L"bthenum") ||
        ContainsInsensitive(id, L"bthle") ||
        ContainsInsensitive(id, L"bluetoothdevice_"));
}

[[nodiscard]] inline BtAudioProfile ClassifyBtAudioFilter(std::wstring_view filterId,
    bool isRenderEndpoint) noexcept {
    if (!IsBluetoothConnectorId(filterId)) {
        return BtAudioProfile::None;
    }
    if (!isRenderEndpoint ||
        ContainsInsensitive(filterId, L"bthhfenum") ||
        ContainsInsensitive(filterId, L"bthhfpaudio") ||
        ContainsInsensitive(filterId, L"0000111e") ||
        ContainsInsensitive(filterId, L"00001108") ||
        ContainsInsensitive(filterId, L"handsfree") ||
        ContainsInsensitive(filterId, L"hands-free")) {
        return BtAudioProfile::HandsFree;
    }
    return BtAudioProfile::A2dpStereo;
}

[[nodiscard]] inline bool FilterMatchesBtAddress(std::wstring_view filterId,
    uint64_t address) noexcept {
    if (address == 0 || filterId.empty()) {
        return false;
    }
    constexpr wchar_t kHex[] = L"0123456789ABCDEF";
    wchar_t compact[12]{};
    wchar_t colon[17]{};
    for (int i = 0; i < 6; ++i) {
        const unsigned byte = static_cast<unsigned>((address >> ((5 - i) * 8)) & 0xFFu);
        compact[i * 2] = kHex[(byte >> 4) & 0xFu];
        compact[i * 2 + 1] = kHex[byte & 0xFu];
        colon[i * 3] = compact[i * 2];
        colon[i * 3 + 1] = compact[i * 2 + 1];
        if (i < 5) {
            colon[i * 3 + 2] = L':';
        }
    }
    return ContainsInsensitive(filterId, std::wstring_view(compact, 12)) ||
        ContainsInsensitive(filterId, std::wstring_view(colon, 17));
}

// Connect pages stereo A2DP alone so HFP never precedes or contends with AVDTP;
// falls back to HFP only for voice-only headsets when Hands-Free is enabled.
[[nodiscard]] inline std::vector<std::wstring> SelectConnectFilters(
    const std::vector<std::wstring>& a2dpFilters,
    const std::vector<std::wstring>& hfpFilters,
    bool handsFreeEnabled) {
    if (!a2dpFilters.empty()) {
        return a2dpFilters;
    }
    return handsFreeEnabled ? hfpFilters : std::vector<std::wstring>{};
}

[[nodiscard]] inline std::vector<std::wstring> SelectDisconnectFilters(
    const std::vector<std::wstring>& a2dpFilters,
    const std::vector<std::wstring>& hfpFilters) {
    std::vector<std::wstring> combined = a2dpFilters;
    for (const std::wstring& filter : hfpFilters) {
        if (std::find(combined.begin(), combined.end(), filter) == combined.end()) {
            combined.push_back(filter);
        }
    }
    return combined;
}

[[nodiscard]] constexpr bool ShouldReconnectHfpPostConnect(bool handsFreeEnabled,
    bool a2dpActive, bool anyHfpActive, bool hasHfpFilters) noexcept {
    return handsFreeEnabled && a2dpActive && !anyHfpActive && hasHfpFilters;
}

// Opening IAudioClient::Start() on a Bluetooth capture endpoint forces the
// headset from A2DP stereo into HFP eSCO call mode, muting stereo playback.
[[nodiscard]] constexpr bool ShouldOpenCaptureStream(bool popupVisible,
    bool onHomePage, bool onMicrophonePage, bool isBluetoothEndpoint,
    bool handsFreeEnabled) noexcept {
    if (!popupVisible || (!onHomePage && !onMicrophonePage)) {
        return false;
    }
    return isBluetoothEndpoint ? (onMicrophonePage && handsFreeEnabled) : true;
}

[[nodiscard]] inline bool NeedsBluetoothBalanceRepair(const float* channelLevels,
    size_t channelCount, float* outTargetLevel = nullptr) noexcept {
    if (channelLevels == nullptr || channelCount < 2) {
        if (outTargetLevel != nullptr) {
            *outTargetLevel = (channelLevels != nullptr && channelCount == 1)
                ? std::clamp(channelLevels[0], 0.0F, 1.0F)
                : 0.0F;
        }
        return false;
    }
    const size_t count = (std::min)(channelCount, kMaxAudioChannels);
    float lo = std::clamp(channelLevels[0], 0.0F, 1.0F);
    float hi = lo;
    for (size_t i = 1; i < count; ++i) {
        const float v = std::clamp(channelLevels[i], 0.0F, 1.0F);
        lo = (std::min)(lo, v);
        hi = (std::max)(hi, v);
    }
    if (outTargetLevel != nullptr) {
        *outTargetLevel = hi;
    }
    return (hi - lo) > kBluetoothBalanceTolerance;
}

struct SavedChannelBalance {
    std::array<float, kMaxAudioChannels> weights{};
    size_t channelCount = 0;
    bool valid = false;
};

inline void ComputeChannelVolumeTargets(float targetMaster, bool isBluetooth,
    const float* currentChannels, size_t channelCount, SavedChannelBalance& saved,
    float* outTargets) noexcept {
    if (outTargets == nullptr || channelCount == 0) {
        return;
    }
    const float master = std::clamp(targetMaster, 0.0F, 1.0F);
    const size_t count = (std::min)(channelCount, kMaxAudioChannels);
    const auto fillEqual = [&]() noexcept {
        for (size_t i = 0; i < count; ++i) {
            outTargets[i] = master;
        }
    };
    if (isBluetooth || count < 2 || currentChannels == nullptr) {
        saved = {};
        return fillEqual();
    }

    float lo = std::clamp(currentChannels[0], 0.0F, 1.0F);
    float hi = lo;
    for (size_t i = 1; i < count; ++i) {
        const float v = std::clamp(currentChannels[i], 0.0F, 1.0F);
        lo = (std::min)(lo, v);
        hi = (std::max)(hi, v);
    }
    if (hi > 1.0e-4F) {
        if ((hi - lo) <= kWiredBalancedTolerance) {
            saved = {};
            return fillEqual();
        }
        if (!saved.valid || saved.channelCount != count || hi >= 0.10F) {
            saved.channelCount = count;
            saved.valid = true;
            for (size_t i = 0; i < count; ++i) {
                saved.weights[i] = std::clamp(currentChannels[i], 0.0F, 1.0F) / hi;
            }
        }
    } else if (!saved.valid || saved.channelCount != count) {
        return fillEqual();
    }
    for (size_t i = 0; i < count; ++i) {
        outTargets[i] = std::clamp(master * saved.weights[i], 0.0F, 1.0F);
    }
}

#if defined(_WIN32) && !defined(DOCK_AUDIO_POLICY_NO_WIN32)

inline constexpr GUID kVolumeEventContext = {
    0x9d41e8c2, 0x6f73, 0x4b91, {0xa4, 0x5d, 0x2e, 0x8b, 0x17, 0xc3, 0x90, 0x4a}};
inline constexpr CLSID kPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

struct PolicyDeviceShareMode;

MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, PolicyDeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, PolicyDeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

[[nodiscard]] inline std::wstring EndpointDeviceId(IMMDevice* device) {
    if (device == nullptr) {
        return {};
    }
    LPWSTR rawId = nullptr;
    if (FAILED(device->GetId(&rawId)) || rawId == nullptr) {
        return {};
    }
    std::wstring id(rawId);
    CoTaskMemFree(rawId);
    return id;
}

[[nodiscard]] inline bool IsBluetoothEndpoint(IMMDevice* device, uint64_t matchAddress = 0) {
    if (device == nullptr) {
        return false;
    }
    IDeviceTopology* topology = nullptr;
    if (SUCCEEDED(device->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&topology))) &&
        topology != nullptr) {
        UINT connectors = 0;
        topology->GetConnectorCount(&connectors);
        bool matched = false;
        for (UINT index = 0; index < connectors && !matched; ++index) {
            IConnector* connector = nullptr;
            if (FAILED(topology->GetConnector(index, &connector)) || connector == nullptr) {
                continue;
            }
            LPWSTR connectedId = nullptr;
            if (SUCCEEDED(connector->GetDeviceIdConnectedTo(&connectedId)) &&
                connectedId != nullptr) {
                matched = IsBluetoothConnectorId(connectedId) &&
                    (matchAddress == 0 || FilterMatchesBtAddress(connectedId, matchAddress));
                CoTaskMemFree(connectedId);
            }
            connector->Release();
        }
        topology->Release();
        if (matched) {
            return true;
        }
    }
    return matchAddress == 0 && IsBluetoothConnectorId(EndpointDeviceId(device));
}

inline bool RepairBluetoothBalance(IAudioEndpointVolume* volume, float* outRepairedLevel = nullptr) {
    if (volume == nullptr) {
        return false;
    }
    UINT channelCount = 0;
    if (FAILED(volume->GetChannelCount(&channelCount)) || channelCount < 2) {
        return false;
    }
    const size_t tracked = (std::min)(static_cast<size_t>(channelCount), kMaxAudioChannels);
    std::array<float, kMaxAudioChannels> channels{};
    for (size_t i = 0; i < tracked; ++i) {
        if (FAILED(volume->GetChannelVolumeLevelScalar(static_cast<UINT>(i), &channels[i]))) {
            return false;
        }
    }
    float target = 0.0F;
    if (!NeedsBluetoothBalanceRepair(channels.data(), tracked, &target)) {
        return false;
    }
    for (size_t i = 0; i < tracked; ++i) {
        if (std::fabs(channels[i] - target) > kBluetoothBalanceTolerance) {
            static_cast<void>(volume->SetChannelVolumeLevelScalar(
                static_cast<UINT>(i), target, &kVolumeEventContext));
        }
    }
    if (outRepairedLevel != nullptr) {
        *outRepairedLevel = target;
    }
    return true;
}

#endif  // defined(_WIN32) && !defined(DOCK_AUDIO_POLICY_NO_WIN32)

}  // namespace audio_policy
