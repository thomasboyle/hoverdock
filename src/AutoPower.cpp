#include "AutoPower.h"

#include <winternl.h>

#include <Xinput.h>
#include <audiopolicy.h>
#include <d3dkmthk.h>
#include <mmdeviceapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <powersetting.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

namespace {

// {a5cc14c9-7d82-4ffc-a698-225beed1e73f} "Hoverdock Automatic". Fixed so the
// installer can hand the machine back to Balanced and delete it on uninstall.
const GUID kAutoPlan = {0xa5cc14c9, 0x7d82, 0x4ffc, {0xa6, 0x98, 0x22, 0x5b, 0xee, 0xd1, 0xe7, 0x3f}};
constexpr wchar_t kAutoPlanText[] = L"a5cc14c9-7d82-4ffc-a698-225beed1e73f";
constexpr wchar_t kAutoPlanName[] = L"Hoverdock Automatic";
constexpr wchar_t kAutoPlanDescription[] =
    L"Managed by Hoverdock: low power by default, high power for games and creative work.";
// SCHEME_BALANCED (GUID_TYPICAL_POWER_SAVINGS).
const GUID kBalancedPlan = {0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
constexpr wchar_t kBalancedPlanText[] = L"381b4222-f694-41f0-9685-ff5bb260df2e";
// SCHEME_MAX (GUID_MAX_POWER_SAVINGS): Hoverdock's Low Power mode.
const GUID kPowerSaverPlan = {0xa1841308, 0x3541, 0x4fab, {0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a}};

// GUID_ACTIVE_POWERSCHEME: notified with the new plan's GUID on every switch.
const GUID kActiveSchemeSetting = {0x31f9f286, 0x5084, 0x42fe, {0xb7, 0x20, 0x2b, 0x02, 0x64, 0x99, 0x37, 0x63}};

// DEVICE_NOTIFY_CALLBACK and DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS (powrprof.h),
// spelled out so the subscription does not depend on SDK header revisions.
constexpr DWORD kDeviceNotifyCallback = 2;
struct PlanSubscription {
    ULONG(CALLBACK* callback)(PVOID context, ULONG type, PVOID setting);
    PVOID context;
};

const GUID kProcessorSubgroup = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
const GUID kPcieSubgroup = {0x501a4d13, 0x42af, 0x4429, {0x9f, 0xd1, 0xa8, 0x21, 0x8c, 0x26, 0x8e, 0x20}};

// Processor policy the plan switches between. High power matches Hoverdock's
// High Power mode; low power is read from the Power saver plan (Hoverdock's
// Low Power mode), with lowFallback used when a machine has no Power saver
// plan (many Modern Standby laptops only ship Balanced). lowFixed settings
// always use lowFallback at low power. Display, sleep and other settings stay
// as in Balanced.
struct PlanSetting {
    const GUID* subgroup;
    GUID setting;
    DWORD highValue;
    DWORD lowFallback;
    bool lowFixed = false;
};

const PlanSetting kPlanSettings[] = {
    // Minimum processor state (P-cores / class 0, E-cores / class 1).
    {&kProcessorSubgroup, {0x893dee8e, 0x2bef, 0x41e0, {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c}}, 100, 5},
    {&kProcessorSubgroup, {0x893dee8e, 0x2bef, 0x41e0, {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4d}}, 100, 5},
    // Maximum processor state.
    {&kProcessorSubgroup, {0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}}, 100, 100},
    {&kProcessorSubgroup, {0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xed}}, 100, 100},
    // Processor performance boost mode: Aggressive / Disabled.
    {&kProcessorSubgroup, {0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}}, 2, 0},
    // Processor performance boost policy (0-100).
    {&kProcessorSubgroup, {0x45bcc044, 0xd885, 0x43e2, {0x86, 0x05, 0xee, 0x0e, 0xc6, 0xe9, 0x6b, 0x59}}, 100, 0},
    // Energy performance preference: 0 = favour performance, 100 = efficiency.
    {&kProcessorSubgroup, {0x36687f9e, 0xe3a5, 0x4dbf, {0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63}}, 0, 80},
    {&kProcessorSubgroup, {0x36687f9e, 0xe3a5, 0x4dbf, {0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x64}}, 0, 80},
    // Core parking minimum cores: 100% (no parking) / park freely.
    {&kProcessorSubgroup, {0x0cc5b647, 0xc1df, 0x4637, {0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83}}, 100, 5},
    {&kProcessorSubgroup, {0x0cc5b647, 0xc1df, 0x4637, {0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x84}}, 100, 5},
    // PCI Express link state power management: Off / Maximum power savings.
    {&kPcieSubgroup, {0xee12f906, 0xd277, 0x404b, {0xb6, 0xda, 0xe5, 0xfa, 0x1a, 0x57, 0x6d, 0xf5}}, 0, 2},
    // Latency sensitivity hints (P-cores / E-cores): Windows raises a hint on
    // clicks, key presses and launches, and these let the processor burst to
    // full performance and unpark every core for it on its own, within a
    // millisecond and without waiting for Hoverdock. Power saver leaves the
    // hint weak; at rest Automatic keeps it at full strength, so a click is
    // instant while the clock and core count still fall back right after.
    {&kProcessorSubgroup, {0x619b7505, 0x003b, 0x4e82, {0xb7, 0xa6, 0x4d, 0xd2, 0x9c, 0x30, 0x09, 0x71}}, 100, 100,
        true},
    {&kProcessorSubgroup, {0x619b7505, 0x003b, 0x4e82, {0xb7, 0xa6, 0x4d, 0xd2, 0x9c, 0x30, 0x09, 0x72}}, 100, 100,
        true},
    {&kProcessorSubgroup, {0x616cdaa5, 0x695e, 0x4545, {0x97, 0xad, 0x97, 0xdc, 0x2d, 0x1b, 0xdd, 0x88}}, 100, 100,
        true},
    {&kProcessorSubgroup, {0x616cdaa5, 0x695e, 0x4545, {0x97, 0xad, 0x97, 0xdc, 0x2d, 0x1b, 0xdd, 0x89}}, 100, 100,
        true},
};

constexpr size_t kPlanSettingCount = sizeof(kPlanSettings) / sizeof(kPlanSettings[0]);

// Engaged cadence: one load sample per tick. Faster while a focused creative
// tool waits to be driven (keyboard-only scrubbing such as J/K/L has no hook),
// slower when the user is away and nothing demanding runs. Mouse input wakes
// either at once through the dock's mouse hook.
constexpr DWORD kTickMs = 1500;
// Keyboard-only fallback poll when raw keyboard input is unavailable.
constexpr DWORD kCreativeTickMs = 500;
constexpr DWORD kParkedTickMs = 10000;
constexpr DWORD kParkedEventTickMs = 30000;
// Input in a focused app that is not at high power starts a short probe:
// its CPU is read every 100 ms (a few handle reads, no process scan), so
// work the user just started is confirmed in ~300 ms.
constexpr DWORD kProbeTickMs = 100;
constexpr ULONGLONG kProbeMs = 2000;
// An unknown fullscreen app is checked for 3D load every 250 ms for the
// first few seconds after it takes focus.
constexpr DWORD kFullscreenProbeTickMs = 250;
constexpr ULONGLONG kFullscreenProbeMs = 3000;
// Plan polling when GUID_ACTIVE_POWERSCHEME notifications are unavailable.
constexpr DWORD kDormantTickMs = 2000;
// Timer wakes can land a little early; still count them as a full interval.
constexpr ULONGLONG kLoadSampleMs = kTickMs - 100;
// PDH fallback only (no D3DKMT statistics): GPU engine counters are the
// costliest read, so outside the fullscreen probe they run every 4.5 s and
// only while a creative tool or AI runtime is open.
constexpr ULONGLONG kGpuSampleMs = 3 * kTickMs - 100;
// D3DKMT: per-process GPU time is only read while some engine is busier than
// this (globally), so an idle GPU costs one statistics call per node.
constexpr double kGpuBusyGatePercent = 3.0;
// Audio sessions are an RPC to the audio service: every other load sample.
constexpr ULONGLONG kAudioSampleMs = 2 * kTickMs - 100;
constexpr ULONGLONG kAudioDevicesRefreshMs = 30000;
// Gamepads: disconnected XInput slots are slow to query, so re-probe rarely.
constexpr ULONGLONG kGamepadReprobeMs = 5000;
// A game released for an away player is watched for its controller this often.
constexpr DWORD kGameAwayTickMs = 500;
// Learned history is written at most this often (and when going dormant).
constexpr ULONGLONG kHistorySaveMs = 60000;
constexpr ULONGLONG kRegisteredGamesRefreshMs = 5ULL * 60ULL * 1000ULL;
// Children of these are launched apps, not part of the launcher's work.
constexpr std::wstring_view kNeverFoldInto[] = {
    L"explorer.exe",
    L"svchost.exe",
    L"services.exe",
    L"steam.exe",
    L"epicgameslauncher.exe",
    L"galaxyclient.exe",
    L"battle.net.exe",
    L"eadesktop.exe",
    L"upc.exe",
    L"riotclientservices.exe",
    L"cmd.exe",
    L"powershell.exe",
    L"pwsh.exe",
    L"windowsterminal.exe",
    L"openconsole.exe",
    L"conhost.exe",
    L"runtimebroker.exe",
    L"sihost.exe",
    L"startmenuexperiencehost.exe",
    L"searchhost.exe",
};

std::wstring Lower(std::wstring text) {
    for (wchar_t& ch : text) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return text;
}

std::wstring DisplayName(const wchar_t* exe) {
    std::wstring name = exe != nullptr ? exe : L"";
    if (name.size() > 4 && Lower(name.substr(name.size() - 4)) == L".exe") {
        name.resize(name.size() - 4);
    }
    if (!name.empty()) {
        name[0] = static_cast<wchar_t>(std::towupper(name[0]));
    }
    return name;
}

ULONGLONG FileTimeValue(const FILETIME& time) noexcept {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

void Trace(const std::wstring& message) {
    OutputDebugStringW((L"[Hoverdock AutoPower] " + message + L"\n").c_str());
}

bool PlanExists() {
    for (DWORD index = 0; index <= 255; ++index) {
        GUID scheme{};
        DWORD bytes = sizeof(scheme);
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, static_cast<UCHAR>(index),
                reinterpret_cast<UCHAR*>(&scheme), &bytes) != ERROR_SUCCESS) {
            return false;
        }
        if (IsEqualGUID(scheme, kAutoPlan)) {
            return true;
        }
    }
    return false;
}

bool EnsurePlan() {
    if (!PlanExists() &&
        (!RunPowercfg(std::wstring(L"/duplicatescheme ") + kBalancedPlanText + L" " + kAutoPlanText) ||
            !PlanExists())) {
        Trace(L"Could not create the Hoverdock Automatic plan.");
        return false;
    }
    // Rewritten every time so plans created by earlier versions stay accurate.
    static_cast<void>(PowerWriteFriendlyName(nullptr, &kAutoPlan, nullptr, nullptr,
        reinterpret_cast<UCHAR*>(const_cast<wchar_t*>(kAutoPlanName)), sizeof(kAutoPlanName)));
    static_cast<void>(PowerWriteDescription(nullptr, &kAutoPlan, nullptr, nullptr,
        reinterpret_cast<UCHAR*>(const_cast<wchar_t*>(kAutoPlanDescription)), sizeof(kAutoPlanDescription)));
    return true;
}

bool ActivePlanIs(const GUID& plan) noexcept {
    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) != ERROR_SUCCESS || active == nullptr) {
        return false;
    }
    const bool match = IsEqualGUID(*active, plan) != FALSE;
    LocalFree(active);
    return match;
}

// Busiest GPU engine per pid (any engine type, and 3D only) from
// "\GPU Engine(*)\Utilization Percentage", the counters Task Manager reads.
class GpuSampler {
public:
    GpuSampler() = default;
    GpuSampler(const GpuSampler&) = delete;
    GpuSampler& operator=(const GpuSampler&) = delete;
    ~GpuSampler() {
        Close();
    }

    struct Load {
        double any = 0.0;
        double threeD = 0.0;
        double compute = 0.0;
    };

    void Sample(std::unordered_map<DWORD, Load>& out) {
        out.clear();
        if (m_failed) {
            return;
        }
        if (m_query == nullptr) {
            if (PdhOpenQueryW(nullptr, 0, &m_query) != ERROR_SUCCESS) {
                m_query = nullptr;
                m_failed = true;
                return;
            }
            if (PdhAddEnglishCounterW(m_query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &m_counter) !=
                ERROR_SUCCESS) {
                Close();
                m_failed = true;
                return;
            }
        }
        if (PdhCollectQueryData(m_query) != ERROR_SUCCESS) {
            return;
        }
        // Rate counter: the first collect only primes it.
        if (!m_primed) {
            m_primed = true;
            return;
        }
        DWORD bytes = 0;
        DWORD count = 0;
        if (PdhGetFormattedCounterArrayW(m_counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr) !=
                static_cast<PDH_STATUS>(PDH_MORE_DATA) ||
            bytes == 0) {
            return;
        }
        std::vector<BYTE> buffer(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(m_counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items) !=
            ERROR_SUCCESS) {
            return;
        }
        for (DWORD index = 0; index < count; ++index) {
            const PDH_FMT_COUNTERVALUE& value = items[index].FmtValue;
            if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) {
                continue;
            }
            // "pid_1234_luid_0x..._phys_0_eng_0_engtype_3D"
            const wchar_t* name = items[index].szName;
            const wchar_t* tag = name != nullptr ? std::wcsstr(name, L"pid_") : nullptr;
            if (tag == nullptr) {
                continue;
            }
            const DWORD pid = static_cast<DWORD>(std::wcstoul(tag + 4, nullptr, 10));
            Load& load = out[pid];
            load.any = (std::max)(load.any, value.doubleValue);
            if (std::wcsstr(name, L"engtype_3D") != nullptr) {
                load.threeD = (std::max)(load.threeD, value.doubleValue);
            } else if (std::wcsstr(name, L"engtype_Compute") != nullptr || std::wcsstr(name, L"engtype_Cuda") != nullptr) {
                load.compute = (std::max)(load.compute, value.doubleValue);
            }
        }
    }

    void Close() noexcept {
        if (m_query != nullptr) {
            PdhCloseQuery(m_query);
        }
        m_query = nullptr;
        m_counter = nullptr;
        m_primed = false;
    }

private:
    PDH_HQUERY m_query = nullptr;
    PDH_HCOUNTER m_counter = nullptr;
    bool m_failed = false;
    bool m_primed = false;
};

// Monotonic time in 100 ns units (the unit of CPU and GPU running times).
ULONGLONG Now100ns() noexcept {
    static const LONGLONG frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? value.QuadPart : 1;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<ULONGLONG>(counter.QuadPart / frequency * 10000000LL +
        counter.QuadPart % frequency * 10000000LL / frequency);
}

double MsSince(ULONGLONG start100ns) noexcept {
    return static_cast<double>(Now100ns() - start100ns) / 10000.0;
}

// Per-process GPU engine time from the kernel graphics statistics
// (D3DKMTQueryStatistics, what Task Manager's counters are built on) without
// PDH's instance enumeration. Each node's global running time is read every
// sample (one call per node); per-process times are only read for the nodes
// that were busy, and only while the GPU is busy at all.
class KmtGpuSampler {
public:
    using Load = GpuSampler::Load;

    KmtGpuSampler() = default;
    KmtGpuSampler(const KmtGpuSampler&) = delete;
    KmtGpuSampler& operator=(const KmtGpuSampler&) = delete;

    [[nodiscard]] bool Usable() {
        if (!m_initialized) {
            m_initialized = true;
            Init();
        }
        return !m_failed && !m_nodes.empty();
    }

    // Busiest node since the previous call (0-100), and remembers which nodes
    // did any work for SampleProcesses.
    double SampleGlobal() {
        const ULONGLONG now = Now100ns();
        const ULONGLONG elapsed = m_lastGlobal != 0 ? now - m_lastGlobal : 0;
        double busiest = 0.0;
        for (Node& node : m_nodes) {
            D3DKMT_QUERYSTATISTICS query{};
            query.Type = D3DKMT_QUERYSTATISTICS_NODE;
            query.AdapterLuid = node.luid;
            query.QueryNode.NodeId = node.id;
            node.busy = false;
            if (D3DKMTQueryStatistics(&query) != 0) {
                continue;
            }
            const auto running =
                static_cast<ULONGLONG>(query.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart);
            if (elapsed != 0 && running >= node.lastGlobal) {
                const double percent =
                    100.0 * static_cast<double>(running - node.lastGlobal) / static_cast<double>(elapsed);
                node.busy = running > node.lastGlobal;
                busiest = (std::max)(busiest, (std::min)(percent, 100.0));
            }
            node.lastGlobal = running;
        }
        m_lastGlobal = now;
        m_busiest = busiest;
        return busiest;
    }

    // Per-process load for the busy nodes. A pid's first reading of a node
    // (or one older than a few seconds) only primes it.
    void SampleProcesses(const std::vector<std::pair<DWORD, HANDLE>>& procs,
        std::unordered_map<DWORD, Load>& out) {
        const ULONGLONG now = Now100ns();
        for (const auto& [pid, handle] : procs) {
            if (handle == nullptr) {
                continue;
            }
            PidState& state = m_pids[pid];
            state.times.resize(m_nodes.size());
            Load load;
            for (size_t index = 0; index < m_nodes.size(); ++index) {
                const Node& node = m_nodes[index];
                if (!node.busy) {
                    continue;
                }
                D3DKMT_QUERYSTATISTICS query{};
                query.Type = D3DKMT_QUERYSTATISTICS_PROCESS_NODE;
                query.AdapterLuid = node.luid;
                query.hProcess = handle;
                query.QueryProcessNode.NodeId = node.id;
                if (D3DKMTQueryStatistics(&query) != 0) {
                    ++m_refused;
                    continue;
                }
                m_answered = true;
                const auto running =
                    static_cast<ULONGLONG>(query.QueryResult.ProcessNodeInformation.RunningTime.QuadPart);
                Sample& last = state.times[index];
                if (last.stamp != 0 && now - last.stamp <= 50000000ULL && now > last.stamp &&
                    running >= last.running) {
                    const double percent = (std::min)(
                        100.0 * static_cast<double>(running - last.running) / static_cast<double>(now - last.stamp),
                        100.0);
                    load.any = (std::max)(load.any, percent);
                    if (node.threeD) {
                        load.threeD = (std::max)(load.threeD, percent);
                    }
                    if (node.compute) {
                        load.compute = (std::max)(load.compute, percent);
                    }
                }
                last = {running, now};
            }
            out[pid] = load;
        }
        // Statistics refused for every process so far (access rules on this
        // build): hand GPU sampling back to PDH.
        if (!m_answered && m_refused >= 8) {
            Trace(L"D3DKMT process statistics unavailable; using PDH for GPU load.");
            m_failed = true;
        }
    }

    void Forget(const std::unordered_set<DWORD>& alive) {
        for (auto it = m_pids.begin(); it != m_pids.end();) {
            it = alive.contains(it->first) ? std::next(it) : m_pids.erase(it);
        }
    }

    [[nodiscard]] double Busiest() const noexcept {
        return m_busiest;
    }

private:
    struct Node {
        LUID luid{};
        UINT id = 0;
        bool threeD = false;
        bool compute = false;
        bool busy = false;
        ULONGLONG lastGlobal = 0;
    };
    struct Sample {
        ULONGLONG running = 0;
        ULONGLONG stamp = 0;
    };
    struct PidState {
        std::vector<Sample> times;
    };

    void Init() {
        D3DKMT_ENUMADAPTERS2 enumerate{};
        if (D3DKMTEnumAdapters2(&enumerate) != 0 || enumerate.NumAdapters == 0) {
            m_failed = true;
            return;
        }
        std::vector<D3DKMT_ADAPTERINFO> adapters(enumerate.NumAdapters);
        enumerate.pAdapters = adapters.data();
        if (D3DKMTEnumAdapters2(&enumerate) != 0) {
            m_failed = true;
            return;
        }
        adapters.resize(enumerate.NumAdapters);
        for (const D3DKMT_ADAPTERINFO& adapter : adapters) {
            D3DKMT_QUERYSTATISTICS query{};
            query.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
            query.AdapterLuid = adapter.AdapterLuid;
            if (D3DKMTQueryStatistics(&query) == 0) {
                const ULONG count = query.QueryResult.AdapterInformation.NodeCount;
                for (ULONG id = 0; id < count && id < 64; ++id) {
                    Node node;
                    node.luid = adapter.AdapterLuid;
                    node.id = id;
                    D3DKMT_NODEMETADATA meta{};
                    meta.NodeOrdinalAndAdapterIndex = id;
                    D3DKMT_QUERYADAPTERINFO info{};
                    info.hAdapter = adapter.hAdapter;
                    info.Type = KMTQAITYPE_NODEMETADATA;
                    info.pPrivateDriverData = &meta;
                    info.PrivateDriverDataSize = sizeof(meta);
                    if (D3DKMTQueryAdapterInfo(&info) == 0) {
                        meta.NodeData.FriendlyName[std::size(meta.NodeData.FriendlyName) - 1] = L'\0';
                        const std::wstring name = Lower(meta.NodeData.FriendlyName);
                        node.threeD = meta.NodeData.EngineType == DXGK_ENGINE_TYPE_3D || name.starts_with(L"3d");
                        node.compute = name.find(L"compute") != std::wstring::npos ||
                            name.find(L"cuda") != std::wstring::npos;
                    }
                    m_nodes.push_back(node);
                }
            }
            D3DKMT_CLOSEADAPTER close{};
            close.hAdapter = adapter.hAdapter;
            static_cast<void>(D3DKMTCloseAdapter(&close));
        }
        if (m_nodes.empty()) {
            m_failed = true;
        }
    }

    bool m_initialized = false;
    bool m_failed = false;
    bool m_answered = false;
    int m_refused = 0;
    double m_busiest = 0.0;
    ULONGLONG m_lastGlobal = 0;
    std::vector<Node> m_nodes;
    std::unordered_map<DWORD, PidState> m_pids;
};

// One NtQuerySystemInformation(SystemProcessInformation) call returns every
// process with its parent, image name, creation time and CPU times: no
// snapshot handle, no per-process OpenProcess, and it covers processes that
// cannot be opened (protected and anti-cheat games, other users' services).
struct ProcessEntry {
    // Layout of SYSTEM_PROCESS_INFORMATION up to SessionId (winternl.h keeps
    // most of these fields "Reserved").
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
};
static_assert(offsetof(ProcessEntry, ImageName) == offsetof(SYSTEM_PROCESS_INFORMATION, ImageName));
static_assert(offsetof(ProcessEntry, UniqueProcessId) == offsetof(SYSTEM_PROCESS_INFORMATION, UniqueProcessId));

// SystemProcessIdInformation: image path of any pid without opening it.
struct ProcessIdImage {
    HANDLE ProcessId;
    UNICODE_STRING ImageName;
};

constexpr ULONG kSystemProcessInformation = 5;
constexpr ULONG kSystemProcessIdInformation = 88;
constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004L);
using QuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

QuerySystemInformationFn QuerySystemInformation() noexcept {
    static const auto function = reinterpret_cast<QuerySystemInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    return function;
}

struct ScannedProcess {
    DWORD pid = 0;
    DWORD parent = 0;
    ULONGLONG created = 0;
    ULONGLONG cpu = 0;
    std::wstring_view exe;
};

class ProcessScanner {
public:
    // Fills out with every process; false when the call is unavailable.
    bool Scan(std::vector<ScannedProcess>& out) {
        out.clear();
        const QuerySystemInformationFn query = QuerySystemInformation();
        if (query == nullptr) {
            return false;
        }
        for (int attempt = 0; attempt < 4; ++attempt) {
            ULONG needed = 0;
            const LONG status = query(kSystemProcessInformation, m_buffer.data(),
                static_cast<ULONG>(m_buffer.size()), &needed);
            if (status == kStatusInfoLengthMismatch) {
                // Processes can start between the calls: leave headroom.
                m_buffer.resize((std::max)(static_cast<size_t>(needed), m_buffer.size()) + 64 * 1024);
                continue;
            }
            if (status < 0) {
                return false;
            }
            size_t offset = 0;
            for (;;) {
                if (offset + sizeof(ProcessEntry) > m_buffer.size()) {
                    break;
                }
                const auto* entry = reinterpret_cast<const ProcessEntry*>(m_buffer.data() + offset);
                ScannedProcess process;
                process.pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(entry->UniqueProcessId));
                process.parent = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(entry->InheritedFromUniqueProcessId));
                process.created = static_cast<ULONGLONG>(entry->CreateTime.QuadPart);
                process.cpu = static_cast<ULONGLONG>(entry->UserTime.QuadPart) +
                    static_cast<ULONGLONG>(entry->KernelTime.QuadPart);
                if (entry->ImageName.Buffer != nullptr) {
                    process.exe = std::wstring_view(entry->ImageName.Buffer, entry->ImageName.Length / sizeof(wchar_t));
                }
                out.push_back(process);
                if (entry->NextEntryOffset == 0) {
                    break;
                }
                offset += entry->NextEntryOffset;
            }
            return true;
        }
        return false;
    }

private:
    std::vector<BYTE> m_buffer = std::vector<BYTE>(512 * 1024);
};

// Full image path of a pid without a process handle, as a lowercase Win32
// path ("c:\\program files\\..."); empty when unknown.
class ImagePaths {
public:
    std::wstring Lookup(DWORD pid) {
        const QuerySystemInformationFn query = QuerySystemInformation();
        if (query == nullptr) {
            return {};
        }
        std::array<wchar_t, 1024> buffer{};
        ProcessIdImage info{};
        info.ProcessId = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(pid));
        info.ImageName.Buffer = buffer.data();
        info.ImageName.Length = 0;
        info.ImageName.MaximumLength = static_cast<USHORT>(buffer.size() * sizeof(wchar_t));
        if (query(kSystemProcessIdInformation, &info, sizeof(info), nullptr) < 0) {
            return {};
        }
        const std::wstring native = Lower(std::wstring(buffer.data(), info.ImageName.Length / sizeof(wchar_t)));
        return ToWin32(native);
    }

private:
    // "\\device\\harddiskvolume3\\..." -> "c:\\...".
    std::wstring ToWin32(const std::wstring& native) {
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& [device, drive] : m_devices) {
                if (native.size() > device.size() && native.compare(0, device.size(), device) == 0 &&
                    native[device.size()] == L'\\') {
                    return drive + native.substr(device.size());
                }
            }
            const ULONGLONG now = GetTickCount64();
            if (pass == 1 || (m_devicesAt != 0 && now - m_devicesAt < 30000)) {
                break;
            }
            m_devicesAt = now;
            m_devices.clear();
            for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
                const wchar_t drive[3] = {letter, L':', L'\0'};
                wchar_t target[MAX_PATH]{};
                if (QueryDosDeviceW(drive, target, MAX_PATH) != 0) {
                    m_devices.emplace_back(Lower(target), Lower(drive));
                }
            }
        }
        return {};
    }

    std::vector<std::pair<std::wstring, std::wstring>> m_devices;
    ULONGLONG m_devicesAt = 0;
};

// Processes with a live audio stream on any active endpoint (playback or
// capture), from the audio service's session list.
class AudioSessions {
public:
    void Sample(std::unordered_set<DWORD>& pids) {
        pids.clear();
        const ULONGLONG now = GetTickCount64();
        if (m_failed) {
            return;
        }
        if (m_managers.empty() || now - m_devicesAt >= kAudioDevicesRefreshMs) {
            RefreshDevices(now);
        }
        for (const auto& manager : m_managers) {
            Microsoft::WRL::ComPtr<IAudioSessionEnumerator> sessions;
            if (FAILED(manager->GetSessionEnumerator(&sessions))) {
                continue;
            }
            int count = 0;
            if (FAILED(sessions->GetCount(&count))) {
                continue;
            }
            for (int index = 0; index < count; ++index) {
                Microsoft::WRL::ComPtr<IAudioSessionControl> control;
                Microsoft::WRL::ComPtr<IAudioSessionControl2> control2;
                AudioSessionState state = AudioSessionStateInactive;
                if (FAILED(sessions->GetSession(index, &control)) || FAILED(control.As(&control2)) ||
                    FAILED(control2->GetState(&state)) || state != AudioSessionStateActive ||
                    control2->IsSystemSoundsSession() == S_OK) {
                    continue;
                }
                DWORD pid = 0;
                if (control2->GetProcessId(&pid) == S_OK && pid != 0) {
                    pids.insert(pid);
                }
            }
        }
    }

    void Close() noexcept {
        m_managers.clear();
        m_enumerator.Reset();
        m_devicesAt = 0;
    }

private:
    void RefreshDevices(ULONGLONG now) {
        m_devicesAt = now;
        m_managers.clear();
        if (!m_enumerator && FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 IID_PPV_ARGS(&m_enumerator)))) {
            m_failed = true;
            Trace(L"Audio sessions unavailable.");
            return;
        }
        Microsoft::WRL::ComPtr<IMMDeviceCollection> devices;
        UINT count = 0;
        if (FAILED(m_enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &devices)) ||
            FAILED(devices->GetCount(&count))) {
            return;
        }
        for (UINT index = 0; index < count; ++index) {
            Microsoft::WRL::ComPtr<IMMDevice> device;
            Microsoft::WRL::ComPtr<IAudioSessionManager2> manager;
            if (SUCCEEDED(devices->Item(index, &device)) &&
                SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(manager.GetAddressOf())))) {
                m_managers.push_back(std::move(manager));
            }
        }
    }

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_enumerator;
    std::vector<Microsoft::WRL::ComPtr<IAudioSessionManager2>> m_managers;
    ULONGLONG m_devicesAt = 0;
    bool m_failed = false;
};

// Apps using the camera or microphone right now, from the privacy consent
// store (what the taskbar's "in use" indicators read): an app entry whose
// LastUsedTimeStop is 0 is using the device. Re-read only when the store
// changes (registry change notification), so idle cost is one wait check.
class CapabilityWatcher {
public:
    struct InUse {
        // Lowercase Win32 paths of desktop apps.
        std::unordered_set<std::wstring> paths;
        // Lowercase package names (before the first '_') of packaged apps.
        std::vector<std::wstring> packages;
        [[nodiscard]] bool Empty() const noexcept {
            return paths.empty() && packages.empty();
        }
    };

    CapabilityWatcher() = default;
    CapabilityWatcher(const CapabilityWatcher&) = delete;
    CapabilityWatcher& operator=(const CapabilityWatcher&) = delete;
    ~CapabilityWatcher() {
        Close();
    }

    // True when camera / microphone use may have changed since the last call.
    bool Refresh() {
        if (m_failed) {
            return false;
        }
        if (m_event == nullptr) {
            m_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (m_event == nullptr) {
                m_failed = true;
                return false;
            }
            Arm();
            Read();
            return true;
        }
        if (WaitForSingleObject(m_event, 0) != WAIT_OBJECT_0) {
            return false;
        }
        ResetEvent(m_event);
        Arm();
        Read();
        return true;
    }

    [[nodiscard]] const InUse& Camera() const noexcept {
        return m_camera;
    }
    [[nodiscard]] const InUse& Microphone() const noexcept {
        return m_microphone;
    }

    void Close() noexcept {
        for (HKEY& key : m_keys) {
            if (key != nullptr) {
                RegCloseKey(key);
                key = nullptr;
            }
        }
        if (m_event != nullptr) {
            CloseHandle(m_event);
            m_event = nullptr;
        }
        m_camera = {};
        m_microphone = {};
    }

private:
    static constexpr const wchar_t* kStores[2] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone",
    };

    void Arm() {
        for (size_t index = 0; index < 2; ++index) {
            if (m_keys[index] == nullptr &&
                RegOpenKeyExW(HKEY_CURRENT_USER, kStores[index], 0, KEY_READ | KEY_NOTIFY, &m_keys[index]) !=
                    ERROR_SUCCESS) {
                m_keys[index] = nullptr;
                continue;
            }
            static_cast<void>(RegNotifyChangeKeyValue(m_keys[index], TRUE,
                REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, m_event, TRUE));
        }
    }

    static bool EntryInUse(HKEY parent, const wchar_t* sub) {
        ULONGLONG start = 0;
        ULONGLONG stop = 1;
        DWORD bytes = sizeof(start);
        if (RegGetValueW(parent, sub, L"LastUsedTimeStart", RRF_RT_REG_QWORD, nullptr, &start, &bytes) !=
            ERROR_SUCCESS) {
            return false;
        }
        bytes = sizeof(stop);
        if (RegGetValueW(parent, sub, L"LastUsedTimeStop", RRF_RT_REG_QWORD, nullptr, &stop, &bytes) !=
            ERROR_SUCCESS) {
            return false;
        }
        return start != 0 && stop == 0;
    }

    static void ReadStore(HKEY store, InUse& out) {
        out = {};
        if (store == nullptr) {
            return;
        }
        for (DWORD index = 0;; ++index) {
            wchar_t sub[512]{};
            DWORD length = 512;
            if (RegEnumKeyExW(store, index, sub, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                break;
            }
            if (_wcsicmp(sub, L"NonPackaged") == 0) {
                continue;
            }
            if (EntryInUse(store, sub)) {
                std::wstring name = Lower(sub);
                name.resize((std::min)(name.find(L'_'), name.size()));
                if (!name.empty()) {
                    out.packages.push_back(std::move(name));
                }
            }
        }
        HKEY desktop = nullptr;
        if (RegOpenKeyExW(store, L"NonPackaged", 0, KEY_READ, &desktop) != ERROR_SUCCESS) {
            return;
        }
        for (DWORD index = 0;; ++index) {
            wchar_t sub[1024]{};
            DWORD length = 1024;
            if (RegEnumKeyExW(desktop, index, sub, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                break;
            }
            if (EntryInUse(desktop, sub)) {
                // "C:#Program Files#App#app.exe"
                std::wstring path = Lower(sub);
                std::replace(path.begin(), path.end(), L'#', L'\\');
                out.paths.insert(std::move(path));
            }
        }
        RegCloseKey(desktop);
    }

    void Read() {
        ReadStore(m_keys[0], m_camera);
        ReadStore(m_keys[1], m_microphone);
    }

    HANDLE m_event = nullptr;
    HKEY m_keys[2] = {nullptr, nullptr};
    bool m_failed = false;
    InUse m_camera;
    InUse m_microphone;
};

// Controller activity (XInput), which GetLastInputInfo does not see: without
// it a player on a gamepad would look away from the keyboard.
class Gamepads {
public:
    // True when any connected controller's state changed since the last poll.
    bool Poll(ULONGLONG now) {
        if (!Load()) {
            return false;
        }
        bool active = false;
        const bool reprobe = now - m_reprobeAt >= kGamepadReprobeMs;
        if (reprobe) {
            m_reprobeAt = now;
        }
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            Slot& state = m_slots[slot];
            if (!state.connected && !reprobe) {
                continue;
            }
            XINPUT_STATE input{};
            if (m_getState(slot, &input) != ERROR_SUCCESS) {
                state.connected = false;
                continue;
            }
            active = active || (state.connected && input.dwPacketNumber != state.packet);
            state.connected = true;
            state.packet = input.dwPacketNumber;
        }
        return active;
    }

private:
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    struct Slot {
        bool connected = false;
        DWORD packet = 0;
    };

    bool Load() {
        if (!m_loaded) {
            m_loaded = true;
            HMODULE module = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (module == nullptr) {
                module = LoadLibraryExW(L"xinput9_1_0.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            }
            if (module != nullptr) {
                m_getState = reinterpret_cast<GetStateFn>(
                    reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState")));
            }
        }
        return m_getState != nullptr;
    }

    bool m_loaded = false;
    GetStateFn m_getState = nullptr;
    ULONGLONG m_reprobeAt = 0;
    std::array<Slot, XUSER_MAX_COUNT> m_slots{};
};

// %LOCALAPPDATA%\LiquidGlassDock\autopower-history.txt (UTF-8).
std::wstring HistoryPath() {
    wchar_t appData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", appData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    const std::wstring directory = std::wstring(appData) + L"\\LiquidGlassDock";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\autopower-history.txt";
}

std::wstring ReadTextFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::string bytes;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0 && size.QuadPart < 1024 * 1024) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) == FALSE) {
            read = 0;
        }
        bytes.resize(read);
    }
    CloseHandle(file);
    if (bytes.empty()) {
        return {};
    }
    const int chars = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(static_cast<size_t>((std::max)(chars, 0)), L'\0');
    if (chars > 0) {
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), chars);
    }
    return text;
}

bool WriteTextFile(const std::wstring& path, const std::wstring& text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    std::string bytes(static_cast<size_t>((std::max)(length, 0)), '\0');
    if (length > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), length, nullptr,
            nullptr);
    }
    // Write beside and swap in, so a crash never leaves a torn file.
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = bytes.empty() ||
        (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
            written == bytes.size());
    CloseHandle(file);
    return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

// Whole days since 1601 (only differences matter).
uint32_t Today() noexcept {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return static_cast<uint32_t>(FileTimeValue(now) / (10000000ULL * 60ULL * 60ULL * 24ULL));
}

struct TickResult {
    AutoPowerStatus status;
    DWORD waitMs = kTickMs;
    // Wake on the next input of any kind (pointer motion included).
    bool wantInput = false;
    // Wake on the next click or key (starts a probe).
    bool wantCommit = false;
};

// Everything the worker owns. Lives only on the worker thread.
class Engine {
public:
    Engine() {
        m_cpuCount = (std::max)(1UL, static_cast<unsigned long>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
        ReadBaseline();
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    ~Engine() {
        GoDormant();
    }

    // One evaluation: status to publish and how long to sleep. hookInputTick
    // is the GetTickCount of the last hook-reported input (0 = none).
    // commitWake: woken by input that may start work (probe the focused app);
    // keyboardInput: keyboard input reaches NoteInput too.
    TickResult Tick(bool forceLoadSample, bool planActive, DWORD hookInputTick, bool commitWake,
        bool keyboardInput) {
        TickResult result;
        AutoPowerStatus& status = result.status;
        if (!planActive) {
            GoDormant();
            return result;
        }
        status.engaged = true;
        const ULONGLONG now = GetTickCount64();
        const uint32_t today = Today();
        if (!m_engaged) {
            m_engaged = true;
            m_planFull.reset();
            for (Written& written : m_written) {
                written.valid = false;
            }
            forceLoadSample = true;
            LoadHistory(today);
            Trace(L"Engaged.");
        }

        m_fullscreenCache = -1;
        const DWORD foregroundPid = ForegroundPid();
        const bool focusChanged = foregroundPid != m_foregroundPid;
        if (focusChanged) {
            m_fullscreenProbeUntil = now + kFullscreenProbeMs;
        }
        if (commitWake && !m_lastFull) {
            m_probeUntil = now + kProbeMs;
        }
        const bool probing = now < m_probeUntil;
        if (forceLoadSample || focusChanged || now - m_lastLoadSample >= kLoadSampleMs || m_groups.empty()) {
            m_foregroundPid = foregroundPid;
            SampleLoads(now);
        } else if (probing || now < m_fullscreenProbeUntil) {
            SampleForeground(now);
        }

        autopower::Inputs inputs;
        inputs.nowMs = now;
        inputs.idleMs = IdleMs();
        // The hook sees input before GetLastInputInfo is updated.
        if (hookInputTick != 0) {
            inputs.idleMs = (std::min)(inputs.idleMs, static_cast<uint64_t>(GetTickCount() - hookInputTick));
        }
        // A player on a controller is not away: gamepads do not reset the
        // session idle timer, so watch them while a game is focused.
        const bool gameFocused = ForegroundGameish();
        if (gameFocused && m_gamepads.Poll(now)) {
            m_gamepadTick = GetTickCount();
            m_gamepadSeen = true;
        }
        if (m_gamepadSeen) {
            inputs.idleMs = (std::min)(inputs.idleMs, static_cast<uint64_t>(GetTickCount() - m_gamepadTick));
        }
        inputs.apps.reserve(m_groups.size());
        const auto foregroundGroup = m_groupOfPid.find(foregroundPid);
        const std::wstring foregroundKey = foregroundGroup != m_groupOfPid.end() ? foregroundGroup->second : L"";
        // Fullscreen state only matters for an app that could be an unknown
        // game: known games, creative tools and browsers/players skip it.
        bool fullscreen = false;
        bool exclusive = false;
        if (ForegroundCouldBeUnknownGame()) {
            fullscreen = ForegroundFullscreen();
            QUERY_USER_NOTIFICATION_STATE state{};
            exclusive = SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN;
        }
        for (const auto& [key, group] : m_groups) {
            autopower::AppSample app = group.sample;
            app.foreground = key == foregroundKey;
            app.fullscreen = app.foreground && fullscreen;
            app.exclusiveFullscreen = app.foreground && exclusive;
            app.knownDemanding = m_history.IsKnown(key, today);
            inputs.apps.push_back(std::move(app));
        }

        const autopower::Decision decision = m_policy.Update(inputs);
        for (auto& [key, group] : m_groups) {
            group.sample.freshLoad = false;
        }
        for (const std::wstring& key : decision.confirmed) {
            m_historyDirty = m_history.Record(key, today) || m_historyDirty;
        }
        SaveHistory(now, false);
        if (decision.fullPower) {
            // Already at high power: nothing left for the probe to find.
            m_probeUntil = 0;
        }
        const double switchMs = ApplyPlan(decision.fullPower);
        if (switchMs >= 0.0) {
            TraceSwitch(decision.fullPower, switchMs, inputs.idleMs);
        }
        // Exemptions only change with the exempt set or group membership
        // (rebuilt by each process scan); probe ticks usually change neither.
        if (decision.unthrottle != m_lastUnthrottle || m_throttleGeneration != m_groupGeneration) {
            ApplyThrottling(decision.unthrottle);
            m_lastUnthrottle = decision.unthrottle;
            m_throttleGeneration = m_groupGeneration;
        }

        status.fullPower = decision.fullPower;
        status.reason = decision.reason;
        status.appName = decision.appName;
        if (status.fullPower != m_lastFull || status.appName != m_lastApp) {
            Trace(status.fullPower ? L"Full power for " + status.appName + L" (" +
                    autopower::ReasonLabel(status.reason) + L")"
                                   : std::wstring(L"Low power."));
            m_lastFull = status.fullPower;
            m_lastApp = status.appName;
        }

        const auto foregroundApp = m_groups.find(foregroundKey);
        const bool creativeFocused = foregroundApp != m_groups.end() &&
            foregroundApp->second.sample.kind == autopower::AppKind::Creative;
        bool demandingRunning = false;
        for (const auto& [key, group] : m_groups) {
            demandingRunning = demandingRunning || group.sample.kind == autopower::AppKind::Creative ||
                group.sample.kind == autopower::AppKind::Game || group.sample.kind == autopower::AppKind::Ai ||
                group.sample.registeredGame;
        }
        if (!decision.fullPower) {
            // A click or key starts a probe, so arm that wake unless one runs.
            result.wantCommit = now >= m_probeUntil;
            if (creativeFocused) {
                // Driving a creative tool is any input, pointer motion too;
                // each wake probes whether real work follows it.
                result.wantInput = true;
                if (!keyboardInput) {
                    result.waitMs = kCreativeTickMs;
                }
            } else if (gameFocused) {
                // A game released for an away player comes back on any input,
                // or on its controller (polled).
                result.wantInput = true;
                result.waitMs = kGameAwayTickMs;
            } else if (!demandingRunning && inputs.idleMs > m_policy.Settings().heavyIdleMs) {
                // Away from the desk with nothing demanding open: only input
                // or a focus change (both events) can create demand.
                result.wantInput = true;
                result.waitMs = keyboardInput ? kParkedEventTickMs : kParkedTickMs;
            }
        }
        if (now < m_probeUntil) {
            result.waitMs = (std::min)(result.waitMs, static_cast<DWORD>(
                (std::min)(static_cast<ULONGLONG>(kProbeTickMs), m_probeUntil - now)));
        }
        if (now < m_fullscreenProbeUntil && ForegroundCouldBeUnknownGame() && ForegroundFullscreen()) {
            result.waitMs = (std::min)(result.waitMs, kFullscreenProbeTickMs);
        }
        return result;
    }

    // Restore everything this engine changed: calm plan, OS-managed throttling.
    void GoDormant() {
        RestoreAllThrottling();
        if (m_engaged || !m_planFull.has_value() || *m_planFull) {
            static_cast<void>(ApplyPlan(false));
        }
        if (m_engaged) {
            SaveHistory(GetTickCount64(), true);
            Trace(L"Dormant.");
        }
        m_engaged = false;
        m_policy.Reset();
        m_groups.clear();
        m_groupOfPid.clear();
        for (auto& [pid, proc] : m_procs) {
            CloseProc(proc);
        }
        m_procs.clear();
        m_gpu.Close();
        m_gpuLoads.clear();
        m_audio.Close();
        m_audioPids.clear();
        m_lastAudioSample = 0;
        m_capabilities.Close();
        m_flagRoots.clear();
        m_lastGpuSample = 0;
        m_probeUntil = 0;
        m_fullscreenProbeUntil = 0;
        m_lastUnthrottle.clear();
        ++m_groupGeneration;
        m_foregroundPid = 0;
        m_lastFull = false;
        m_lastApp.clear();
        m_gamepadSeen = false;
    }

private:
    struct Baseline {
        DWORD ac = 0;
        DWORD dc = 0;
    };

    struct Written {
        bool valid = false;
        DWORD ac = 0;
        DWORD dc = 0;
    };

    struct Proc {
        std::wstring exe;
        std::wstring path;
        std::wstring display;
        autopower::AppKind kind = autopower::AppKind::Other;
        bool registeredGame = false;
        DWORD parent = 0;
        ULONGLONG created = 0;
        HANDLE handle = nullptr;
        bool canSet = false;
        bool openFailed = false;
        bool setFailed = false;
        // CPU time (100 ns) at the last reading, and when (Now100ns).
        ULONGLONG lastCpu = 0;
        ULONGLONG lastCpuStamp = 0;
        double lastPercent = 0.0;
        double lastCores = 0.0;
        unsigned seenGeneration = 0;
        bool exempt = false;
        bool haveOriginal = false;
        PROCESS_POWER_THROTTLING_STATE original{};
        // Live audio stream, camera and microphone use.
        bool audio = false;
        bool camera = false;
        bool mic = false;
    };

    struct Group {
        autopower::AppSample sample;
        std::vector<DWORD> pids;
    };

    // Low power values: the Power saver plan's, so Automatic at rest is
    // exactly Hoverdock's Low Power for the processor.
    void ReadBaseline() {
        for (size_t index = 0; index < kPlanSettingCount; ++index) {
            const PlanSetting& setting = kPlanSettings[index];
            Baseline& base = m_baseline[index];
            if (setting.lowFixed) {
                base = {setting.lowFallback, setting.lowFallback};
                continue;
            }
            if (PowerReadACValueIndex(nullptr, &kPowerSaverPlan, setting.subgroup, &setting.setting, &base.ac) !=
                ERROR_SUCCESS) {
                base.ac = setting.lowFallback;
            }
            if (PowerReadDCValueIndex(nullptr, &kPowerSaverPlan, setting.subgroup, &setting.setting, &base.dc) !=
                ERROR_SUCCESS) {
                base.dc = setting.lowFallback;
            }
        }
    }

    // Writes the plan for full or low power; returns how long the switch took
    // in ms, or -1 when nothing had to change.
    double ApplyPlan(bool full) {
        if (m_planFull.has_value() && *m_planFull == full) {
            return -1.0;
        }
        const ULONGLONG start = Now100ns();
        // Each write is a power-service round trip: skip values already in the
        // plan (the 100% maximum state is the same in both modes).
        bool wrote = false;
        bool failed = false;
        int writes = 0;
        for (size_t index = 0; index < kPlanSettingCount; ++index) {
            const Baseline& base = m_baseline[index];
            const PlanSetting& setting = kPlanSettings[index];
            const DWORD ac = full ? setting.highValue : base.ac;
            const DWORD dc = full ? setting.highValue : base.dc;
            Written& written = m_written[index];
            if (written.valid && written.ac == ac && written.dc == dc) {
                continue;
            }
            const bool acOk =
                PowerWriteACValueIndex(nullptr, &kAutoPlan, setting.subgroup, &setting.setting, ac) == ERROR_SUCCESS;
            const bool dcOk =
                PowerWriteDCValueIndex(nullptr, &kAutoPlan, setting.subgroup, &setting.setting, dc) == ERROR_SUCCESS;
            writes += 2;
            // Settings this Windows build lacks (E-core variants) are not
            // retried: values that never change between modes are settled.
            written = {(acOk && dcOk) || setting.lowFixed, ac, dc};
            wrote = wrote || acOk || dcOk;
            failed = failed || !acOk || !dcOk;
        }
        const double writeMs = MsSince(start);
        // Re-activating the active plan is what applies freshly written values.
        if (wrote && ActivePlanIs(kAutoPlan)) {
            static_cast<void>(PowerSetActiveScheme(nullptr, &kAutoPlan));
        }
        if (failed && !wrote) {
            Trace(L"Plan values could not be written; only throttling is managed.");
        }
        m_planFull = full;
        const double totalMs = MsSince(start);
        m_lastWrites = writes;
        m_lastWriteMs = writeMs;
        return totalMs;
    }

    // Switch timing (DebugView / debugger output): how long the power service
    // took, and how soon after the user's last input the switch landed.
    void TraceSwitch(bool full, double ms, uint64_t idleMs) {
        ++m_switches;
        m_switchTotalMs += ms;
        m_switchMaxMs = (std::max)(m_switchMaxMs, ms);
        wchar_t line[256]{};
        swprintf_s(line,
            L"Switched to %s power in %.1f ms (%d writes %.1f ms, activate %.1f ms; %llu ms after input; "
            L"average %.1f ms, slowest %.1f ms over %u switches).",
            full ? L"high" : L"low", ms, m_lastWrites, m_lastWriteMs, ms - m_lastWriteMs,
            static_cast<unsigned long long>(idleMs), m_switchTotalMs / m_switches, m_switchMaxMs, m_switches);
        Trace(line);
    }

    void LoadHistory(uint32_t today) {
        if (m_historyLoaded) {
            return;
        }
        m_historyLoaded = true;
        m_historyPath = HistoryPath();
        if (!m_historyPath.empty()) {
            m_history.Parse(ReadTextFile(m_historyPath));
            m_history.Prune(today);
        }
    }

    void SaveHistory(ULONGLONG now, bool force) {
        if (!m_historyDirty || m_historyPath.empty() || (!force && now - m_historySavedAt < kHistorySaveMs)) {
            return;
        }
        m_historySavedAt = now;
        if (WriteTextFile(m_historyPath, m_history.Serialize())) {
            m_historyDirty = false;
        }
    }

    static DWORD IdleMs() noexcept {
        LASTINPUTINFO input{};
        input.cbSize = sizeof(input);
        if (GetLastInputInfo(&input) == FALSE) {
            return 0;
        }
        return GetTickCount() - input.dwTime;
    }

    static DWORD ForegroundPid() noexcept {
        HWND window = GetForegroundWindow();
        if (window == nullptr) {
            return 0;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        // UWP apps: the frame host owns the window, the app owns a child.
        wchar_t className[64]{};
        if (GetClassNameW(window, className, 64) > 0 && std::wcscmp(className, L"ApplicationFrameWindow") == 0) {
            struct Search {
                DWORD frame;
                DWORD app;
            } search{pid, 0};
            EnumChildWindows(
                window,
                [](HWND child, LPARAM param) -> BOOL {
                    auto* state = reinterpret_cast<Search*>(param);
                    DWORD childPid = 0;
                    GetWindowThreadProcessId(child, &childPid);
                    if (childPid != 0 && childPid != state->frame) {
                        state->app = childPid;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&search));
            if (search.app != 0) {
                pid = search.app;
            }
        }
        return pid;
    }

    static bool ForegroundIsFullscreen() noexcept {
        HWND window = GetForegroundWindow();
        if (window == nullptr || IsWindowVisible(window) == FALSE || IsIconic(window) != FALSE) {
            return false;
        }
        wchar_t className[64]{};
        if (GetClassNameW(window, className, 64) > 0 &&
            (std::wcscmp(className, L"Progman") == 0 || std::wcscmp(className, L"WorkerW") == 0 ||
                std::wcscmp(className, L"Shell_TrayWnd") == 0 ||
                std::wcscmp(className, L"Shell_SecondaryTrayWnd") == 0)) {
            return false;
        }
        RECT bounds{};
        if (GetWindowRect(window, &bounds) == FALSE) {
            return false;
        }
        HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (monitor == nullptr || GetMonitorInfoW(monitor, &info) == FALSE) {
            return false;
        }
        return bounds.left <= info.rcMonitor.left && bounds.top <= info.rcMonitor.top &&
            bounds.right >= info.rcMonitor.right && bounds.bottom >= info.rcMonitor.bottom;
    }

    void RefreshRegisteredGames(ULONGLONG now) {
        if (m_registeredLoaded && now - m_registeredAt < kRegisteredGamesRefreshMs) {
            return;
        }
        m_registeredLoaded = true;
        m_registeredAt = now;
        m_registeredGames.clear();
        HKEY children = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"System\\GameConfigStore\\Children", 0, KEY_READ, &children) !=
            ERROR_SUCCESS) {
            return;
        }
        for (DWORD index = 0;; ++index) {
            wchar_t sub[256]{};
            DWORD subLength = 256;
            if (RegEnumKeyExW(children, index, sub, &subLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
                break;
            }
            wchar_t path[MAX_PATH * 2]{};
            DWORD bytes = sizeof(path);
            if (RegGetValueW(children, sub, L"MatchedExeFullPath", RRF_RT_REG_SZ, nullptr, path, &bytes) ==
                    ERROR_SUCCESS &&
                path[0] != L'\0') {
                m_registeredGames.insert(Lower(path));
            }
        }
        RegCloseKey(children);
    }

    static void CloseProc(Proc& proc) noexcept {
        if (proc.handle != nullptr) {
            CloseHandle(proc.handle);
            proc.handle = nullptr;
        }
    }

    // Query rights for sampling; set rights only for processes being exempted.
    bool OpenProc(DWORD pid, Proc& proc, bool wantSet = false) {
        if (proc.handle != nullptr && (!wantSet || proc.canSet)) {
            return true;
        }
        if (wantSet && !proc.setFailed) {
            HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE, pid);
            if (handle != nullptr) {
                CloseProc(proc);
                proc.handle = handle;
                proc.canSet = true;
                proc.openFailed = false;
                return true;
            }
            proc.setFailed = true;
        }
        if (wantSet) {
            return false;
        }
        if (proc.openFailed) {
            return false;
        }
        proc.handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        proc.canSet = false;
        proc.openFailed = proc.handle == nullptr;
        return proc.handle != nullptr;
    }

    // First sighting of a pid: path and classification. The path comes from
    // the kernel by pid, so protected processes are classified too.
    void Classify(DWORD pid, Proc& proc) {
        proc.path = m_imagePaths.Lookup(pid);
        if (proc.path.empty() && OpenProc(pid, proc)) {
            wchar_t path[MAX_PATH * 2]{};
            DWORD length = MAX_PATH * 2;
            if (QueryFullProcessImageNameW(proc.handle, 0, path, &length) != FALSE) {
                proc.path = Lower(path);
            }
        }
        proc.kind = autopower::ClassifyApp(proc.exe, proc.path);
        proc.registeredGame = !proc.path.empty() && m_registeredGames.contains(proc.path);
    }

    bool IsAppRoot(DWORD pid, const Proc& proc) const {
        if (proc.kind == autopower::AppKind::Creative || proc.kind == autopower::AppKind::Game ||
            proc.kind == autopower::AppKind::Ai || proc.registeredGame || m_flagRoots.contains(pid)) {
            return true;
        }
        return pid == m_foregroundPid && proc.kind != autopower::AppKind::NeverGame;
    }

    // Parent a process folds into, or 0 (no live parent, a recycled pid, or a
    // launcher / shell whose children are separate apps).
    DWORD FoldParent(DWORD pid) const {
        const auto it = m_procs.find(pid);
        if (it == m_procs.end() || it->second.parent == 0) {
            return 0;
        }
        const auto parent = m_procs.find(it->second.parent);
        if (parent == m_procs.end() || parent->second.created == 0 ||
            parent->second.created > it->second.created ||
            std::find(std::begin(kNeverFoldInto), std::end(kNeverFoldInto), parent->second.exe) !=
                std::end(kNeverFoldInto)) {
            return 0;
        }
        return it->second.parent;
    }

    // Helpers (Premiere's dynamic link, Blender's render workers, a build's
    // compilers) belong to the app that launched them.
    DWORD RootOf(DWORD pid) const {
        DWORD current = pid;
        for (int depth = 0; depth < 6; ++depth) {
            const auto it = m_procs.find(current);
            if (it == m_procs.end()) {
                break;
            }
            if (IsAppRoot(current, it->second)) {
                return current;
            }
            const DWORD parent = FoldParent(current);
            if (parent == 0) {
                break;
            }
            current = parent;
        }
        return pid;
    }

    // Topmost ancestor a process folds into (a browser's audio service up to
    // the browser), so a signal on a helper is credited to the whole app.
    DWORD TopOf(DWORD pid) const {
        DWORD current = pid;
        for (int depth = 0; depth < 6; ++depth) {
            const DWORD parent = FoldParent(current);
            if (parent == 0) {
                break;
            }
            current = parent;
        }
        return current;
    }

    // CPU time (100 ns units) read at stamp (Now100ns): updates the share of
    // the machine and the logical processors kept busy. Too short a window (a
    // focus change right after a sample) is quantum noise: keep the reading.
    void UpdateCpu(Proc& proc, ULONGLONG cpu, ULONGLONG stamp) {
        if (proc.lastCpuStamp != 0 && stamp - proc.lastCpuStamp < 500000ULL) {
            return;
        }
        double cores = 0.0;
        if (proc.lastCpuStamp != 0 && cpu >= proc.lastCpu && stamp > proc.lastCpuStamp) {
            cores = static_cast<double>(cpu - proc.lastCpu) / static_cast<double>(stamp - proc.lastCpuStamp);
            cores = (std::min)(cores, static_cast<double>(m_cpuCount));
        }
        proc.lastCpu = cpu;
        proc.lastCpuStamp = stamp;
        proc.lastCores = cores;
        proc.lastPercent = 100.0 * cores / m_cpuCount;
    }

    void SampleLoads(ULONGLONG now) {
        m_lastLoadSample = now;
        RefreshRegisteredGames(now);

        if (!m_scanner.Scan(m_scan)) {
            if (!m_scanFailed) {
                m_scanFailed = true;
                Trace(L"Process information unavailable.");
            }
            return;
        }
        const ULONGLONG stamp = Now100ns();
        const DWORD self = GetCurrentProcessId();
        const unsigned generation = ++m_generation;
        for (const ScannedProcess& entry : m_scan) {
            const DWORD pid = entry.pid;
            if (pid == 0 || pid == 4 || pid == self || entry.exe.empty()) {
                continue;
            }
            // A recycled pid is a different process: start it over.
            auto it = m_procs.find(pid);
            if (it != m_procs.end() && it->second.created != entry.created) {
                CloseProc(it->second);
                m_procs.erase(it);
                it = m_procs.end();
            }
            if (it == m_procs.end()) {
                const std::wstring exe(entry.exe);
                Proc proc;
                proc.exe = Lower(exe);
                proc.display = DisplayName(exe.c_str());
                proc.parent = entry.parent;
                proc.created = entry.created;
                Classify(pid, proc);
                it = m_procs.emplace(pid, std::move(proc)).first;
            }
            it->second.parent = entry.parent;
            it->second.seenGeneration = generation;
            UpdateCpu(it->second, entry.cpu, stamp);
        }
        std::unordered_set<DWORD> alive;
        alive.reserve(m_procs.size());
        for (auto it = m_procs.begin(); it != m_procs.end();) {
            if (it->second.seenGeneration != generation) {
                CloseProc(it->second);
                it = m_procs.erase(it);
            } else {
                alive.insert(it->first);
                ++it;
            }
        }
        m_kmt.Forget(alive);

        // Audio, camera and microphone users become apps of their own (with
        // their helpers), so they can be exempted and credited.
        RefreshSignals(now);
        m_flagRoots.clear();
        for (const auto& [pid, proc] : m_procs) {
            if (proc.audio || proc.camera || proc.mic) {
                m_flagRoots.insert(TopOf(pid));
            }
        }

        // Group processes under their app roots; only roots get sampled.
        m_groups.clear();
        m_groupOfPid.clear();
        ++m_groupGeneration;
        for (auto& [pid, proc] : m_procs) {
            const DWORD root = RootOf(pid);
            const auto rootIt = m_procs.find(root);
            if (rootIt == m_procs.end() || !IsAppRoot(root, rootIt->second)) {
                continue;
            }
            const Proc& rootProc = rootIt->second;
            Group& group = m_groups[rootProc.exe];
            if (group.sample.key.empty()) {
                group.sample.key = rootProc.exe;
                group.sample.name = rootProc.display;
                group.sample.kind = rootProc.kind;
                group.sample.registeredGame = rootProc.registeredGame;
            }
            group.sample.audioActive = group.sample.audioActive || proc.audio;
            group.sample.cameraActive = group.sample.cameraActive || proc.camera;
            group.sample.micActive = group.sample.micActive || proc.mic;
            group.pids.push_back(pid);
            m_groupOfPid[pid] = rootProc.exe;
        }
        // The foreground app always gets a group, even a NeverGame one, so
        // the policy knows a game or tool has lost focus.
        if (m_foregroundPid != 0 && !m_groupOfPid.contains(m_foregroundPid)) {
            const auto it = m_procs.find(m_foregroundPid);
            if (it != m_procs.end()) {
                Group& group = m_groups[it->second.exe];
                if (group.sample.key.empty()) {
                    group.sample.key = it->second.exe;
                    group.sample.name = it->second.display;
                    group.sample.kind = it->second.kind;
                }
                group.pids.push_back(m_foregroundPid);
                m_groupOfPid[m_foregroundPid] = it->second.exe;
            }
        }

        SampleGpu(now, nullptr);
        for (auto& [key, group] : m_groups) {
            MeasureGroup(group, false);
        }

        // Handles are only kept for app groups and exempted processes; CPU
        // time for everything else comes from the next scan.
        for (auto& [pid, proc] : m_procs) {
            if (!proc.exempt && !m_groupOfPid.contains(pid)) {
                CloseProc(proc);
                proc.openFailed = false;
            }
        }
    }

    static bool UsesDevice(const CapabilityWatcher::InUse& inUse, const std::wstring& path) {
        if (inUse.Empty() || path.empty()) {
            return false;
        }
        if (inUse.paths.contains(path)) {
            return true;
        }
        for (const std::wstring& package : inUse.packages) {
            if (path.find(L"\\windowsapps\\" + package + L"_") != std::wstring::npos) {
                return true;
            }
        }
        return false;
    }

    void RefreshSignals(ULONGLONG now) {
        if (m_lastAudioSample == 0 || now - m_lastAudioSample >= kAudioSampleMs) {
            m_audio.Sample(m_audioPids);
            m_lastAudioSample = now;
        }
        static_cast<void>(m_capabilities.Refresh());
        const CapabilityWatcher::InUse& camera = m_capabilities.Camera();
        const CapabilityWatcher::InUse& microphone = m_capabilities.Microphone();
        for (auto& [pid, proc] : m_procs) {
            proc.audio = m_audioPids.contains(pid);
            proc.camera = UsesDevice(camera, proc.path);
            proc.mic = UsesDevice(microphone, proc.path);
        }
    }

    // Focused app without a known kind: fullscreen + 3D load could make it a game.
    bool ForegroundCouldBeUnknownGame() const {
        const auto foreground = m_groupOfPid.find(m_foregroundPid);
        if (foreground == m_groupOfPid.end()) {
            return false;
        }
        const auto group = m_groups.find(foreground->second);
        return group != m_groups.end() && group->second.sample.kind == autopower::AppKind::Other &&
            !group->second.sample.registeredGame;
    }

    // Focused app is (or may be) a game: worth watching the controllers.
    bool ForegroundGameish() {
        const auto foreground = m_groupOfPid.find(m_foregroundPid);
        if (foreground == m_groupOfPid.end()) {
            return false;
        }
        const auto group = m_groups.find(foreground->second);
        if (group == m_groups.end()) {
            return false;
        }
        const autopower::AppSample& sample = group->second.sample;
        return sample.kind == autopower::AppKind::Game || sample.registeredGame ||
            (ForegroundCouldBeUnknownGame() && ForegroundFullscreen());
    }

    // Foreground fullscreen state, read at most once per tick.
    bool ForegroundFullscreen() {
        if (m_fullscreenCache < 0) {
            m_fullscreenCache = ForegroundIsFullscreen() ? 1 : 0;
        }
        return m_fullscreenCache == 1;
    }

    // GPU load per pid. With kernel statistics (D3DKMT) the GPU is watched on
    // every sample: one call per engine while idle, per-process reads only
    // while it is busy. only = probe of a single group (the focused app).
    // Without them, PDH counters keep the old, sparser cadence.
    void SampleGpu(ULONGLONG now, Group* only) {
        if (m_kmt.Usable()) {
            std::vector<std::pair<DWORD, HANDLE>> procs;
            const auto add = [this, &procs](const Group& group) {
                for (const DWORD pid : group.pids) {
                    Proc& proc = m_procs[pid];
                    procs.emplace_back(pid, OpenProc(pid, proc) ? proc.handle : nullptr);
                }
            };
            if (only != nullptr) {
                add(*only);
            } else {
                m_gpuLoads.clear();
                for (const auto& [key, group] : m_groups) {
                    add(group);
                }
            }
            if (m_kmt.SampleGlobal() >= kGpuBusyGatePercent) {
                m_kmt.SampleProcesses(procs, m_gpuLoads);
            } else {
                for (const auto& [pid, handle] : procs) {
                    m_gpuLoads[pid] = {};
                }
            }
            m_lastGpuSample = now;
            return;
        }
        bool watchedRunning = false;
        for (const auto& [key, group] : m_groups) {
            watchedRunning = watchedRunning || group.sample.kind == autopower::AppKind::Creative ||
                group.sample.kind == autopower::AppKind::Ai;
        }
        const bool probe = ForegroundCouldBeUnknownGame() && ForegroundFullscreen();
        if (!probe && !watchedRunning) {
            m_gpuLoads.clear();
            return;
        }
        const ULONGLONG interval = !probe ? kGpuSampleMs
            : now < m_fullscreenProbeUntil ? static_cast<ULONGLONG>(kFullscreenProbeTickMs) - 50
                                           : kLoadSampleMs;
        if (m_lastGpuSample == 0 || now - m_lastGpuSample >= interval) {
            m_gpu.Sample(m_gpuLoads);
            m_lastGpuSample = now;
        }
    }

    // readCpu: re-read CPU time through the handles (probe ticks); a full
    // scan has already updated every process.
    void MeasureGroup(Group& group, bool readCpu) {
        double cpu = 0.0;
        double cores = 0.0;
        double gpu = 0.0;
        double gpu3d = 0.0;
        double compute = 0.0;
        const ULONGLONG stamp = Now100ns();
        for (const DWORD pid : group.pids) {
            Proc& proc = m_procs[pid];
            if (readCpu && OpenProc(pid, proc)) {
                FILETIME created{}, exited{}, kernel{}, user{};
                if (GetProcessTimes(proc.handle, &created, &exited, &kernel, &user) != FALSE) {
                    UpdateCpu(proc, FileTimeValue(kernel) + FileTimeValue(user), stamp);
                }
            }
            cpu += proc.lastPercent;
            cores += proc.lastCores;
            const auto load = m_gpuLoads.find(pid);
            if (load != m_gpuLoads.end()) {
                gpu = (std::max)(gpu, load->second.any);
                gpu3d = (std::max)(gpu3d, load->second.threeD);
                compute = (std::max)(compute, load->second.compute);
            }
        }
        group.sample.cpuPercent = (std::min)(cpu, 100.0);
        group.sample.cpuCores = cores;
        group.sample.gpuPercent = gpu;
        group.sample.gpu3dPercent = gpu3d;
        group.sample.gpuComputePercent = compute;
        group.sample.freshLoad = true;
    }

    // Probe tick: re-measure only the focused app from handles already open
    // (no process scan), plus its GPU load.
    void SampleForeground(ULONGLONG now) {
        const auto foreground = m_groupOfPid.find(m_foregroundPid);
        if (foreground == m_groupOfPid.end()) {
            return;
        }
        const auto group = m_groups.find(foreground->second);
        if (group == m_groups.end()) {
            return;
        }
        SampleGpu(now, &group->second);
        MeasureGroup(group->second, true);
    }

    void SetExempt(DWORD pid, Proc& proc, bool exempt) {
        if (proc.exempt == exempt) {
            return;
        }
        if (exempt) {
            if (!OpenProc(pid, proc, true)) {
                return;
            }
            proc.original = {};
            proc.original.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            proc.haveOriginal = GetProcessInformation(proc.handle, ProcessPowerThrottling, &proc.original,
                                    sizeof(proc.original)) != FALSE;
            PROCESS_POWER_THROTTLING_STATE state{};
            state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            // Control bits set with state bits clear: never throttle.
            state.ControlMask =
                PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
            state.StateMask = 0;
            proc.exempt =
                SetProcessInformation(proc.handle, ProcessPowerThrottling, &state, sizeof(state)) != FALSE;
            return;
        }
        if (proc.handle != nullptr && proc.canSet) {
            PROCESS_POWER_THROTTLING_STATE state{};
            state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            if (proc.haveOriginal) {
                state.ControlMask = proc.original.ControlMask;
                state.StateMask = proc.original.StateMask;
            }
            static_cast<void>(SetProcessInformation(proc.handle, ProcessPowerThrottling, &state, sizeof(state)));
        }
        proc.exempt = false;
        proc.haveOriginal = false;
    }

    void ApplyThrottling(const std::vector<std::wstring>& keys) {
        std::unordered_set<DWORD> wanted;
        for (const std::wstring& key : keys) {
            const auto it = m_groups.find(key);
            if (it != m_groups.end()) {
                wanted.insert(it->second.pids.begin(), it->second.pids.end());
            }
        }
        for (auto& [pid, proc] : m_procs) {
            SetExempt(pid, proc, wanted.contains(pid));
        }
    }

    void RestoreAllThrottling() {
        for (auto& [pid, proc] : m_procs) {
            SetExempt(pid, proc, false);
        }
    }

    Baseline m_baseline[kPlanSettingCount]{};
    Written m_written[kPlanSettingCount]{};
    std::optional<bool> m_planFull;
    bool m_engaged = false;
    DWORD m_cpuCount = 1;
    DWORD m_foregroundPid = 0;
    ULONGLONG m_lastLoadSample = 0;
    ULONGLONG m_lastGpuSample = 0;
    ULONGLONG m_probeUntil = 0;
    ULONGLONG m_fullscreenProbeUntil = 0;
    int m_fullscreenCache = -1;
    unsigned m_groupGeneration = 0;
    unsigned m_throttleGeneration = 0;
    std::vector<std::wstring> m_lastUnthrottle;
    unsigned m_generation = 0;
    bool m_lastFull = false;
    std::wstring m_lastApp;
    bool m_registeredLoaded = false;
    ULONGLONG m_registeredAt = 0;
    std::unordered_set<std::wstring> m_registeredGames;
    std::unordered_map<DWORD, Proc> m_procs;
    std::unordered_map<std::wstring, Group> m_groups;
    std::unordered_map<DWORD, std::wstring> m_groupOfPid;
    std::unordered_map<DWORD, GpuSampler::Load> m_gpuLoads;
    GpuSampler m_gpu;
    KmtGpuSampler m_kmt;
    ProcessScanner m_scanner;
    std::vector<ScannedProcess> m_scan;
    bool m_scanFailed = false;
    ImagePaths m_imagePaths;
    AudioSessions m_audio;
    std::unordered_set<DWORD> m_audioPids;
    ULONGLONG m_lastAudioSample = 0;
    CapabilityWatcher m_capabilities;
    std::unordered_set<DWORD> m_flagRoots;
    Gamepads m_gamepads;
    DWORD m_gamepadTick = 0;
    bool m_gamepadSeen = false;
    autopower::History m_history;
    bool m_historyLoaded = false;
    bool m_historyDirty = false;
    ULONGLONG m_historySavedAt = 0;
    std::wstring m_historyPath;
    unsigned m_switches = 0;
    double m_switchTotalMs = 0.0;
    double m_switchMaxMs = 0.0;
    int m_lastWrites = 0;
    double m_lastWriteMs = 0.0;
    autopower::Policy m_policy;
};

}  // namespace

bool RunPowercfg(const std::wstring& arguments) {
    wchar_t system[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    std::wstring commandLine = L"\"" + std::wstring(system) + L"\\powercfg.exe\" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
            &startup, &process) == FALSE) {
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

AutoPowerEngine::~AutoPowerEngine() {
    Stop();
}

bool AutoPowerEngine::Activate() {
    return EnsurePlan() && PowerSetActiveScheme(nullptr, &kAutoPlan) == ERROR_SUCCESS;
}

bool AutoPowerEngine::IsPlanActive() noexcept {
    return ActivePlanIs(kAutoPlan);
}

void AutoPowerEngine::Start(HWND notifyHwnd, UINT notifyMsg, bool adoptBalanced) {
    if (m_thread.joinable()) {
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_stop = false;
        m_woken = false;
        m_forceSample = false;
        m_commitWake = false;
        m_notifyHwnd = notifyHwnd;
        m_notifyMsg = notifyMsg;
        m_status = {};
    }
    m_thread = std::thread([this, adoptBalanced] {
        // The engine's own sampling is background work: let Windows run it
        // efficiently (EcoQoS) so it never competes with the apps it boosts.
        THREAD_POWER_THROTTLING_STATE throttle{};
        throttle.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        throttle.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        throttle.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        static_cast<void>(SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttle,
            sizeof(throttle)));
        if (adoptBalanced && ActivePlanIs(kBalancedPlan)) {
            // Hoverdock already showed Balanced as "Automatic"; make it real.
            Trace(Activate() ? L"Moved Balanced to Hoverdock Automatic." : L"Could not adopt Balanced.");
        }
        // Audio session queries are COM calls.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ThreadMain();
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
    });
}

void AutoPowerEngine::Stop() {
    {
        std::lock_guard lock(m_mutex);
        if (!m_thread.joinable()) {
            return;
        }
        m_stop = true;
    }
    m_wake.notify_all();
    m_thread.join();
}

void AutoPowerEngine::Wake() noexcept {
    Signal(true);
}

void AutoPowerEngine::NoteInput(bool commit) noexcept {
    // Every input is stamped: GetLastInputInfo lags behind hooks and raw input.
    m_hookInputTick.store(GetTickCount(), std::memory_order_relaxed);
    const bool any = m_wantInput.load(std::memory_order_relaxed) && m_wantInput.exchange(false);
    const bool starts = commit && m_wantCommit.load(std::memory_order_relaxed) && m_wantCommit.exchange(false);
    if (any || starts) {
        // Any wake the engine asked for probes the focused app: a creative
        // tool only counts as driven with real load behind the input.
        Signal(false, true);
    }
}

void AutoPowerEngine::SetKeyboardInputAvailable(bool available) noexcept {
    m_keyboardInput.store(available, std::memory_order_relaxed);
}

void AutoPowerEngine::Signal(bool forceSample, bool commit) noexcept {
    {
        std::lock_guard lock(m_mutex);
        m_woken = true;
        m_forceSample = m_forceSample || forceSample;
        m_commitWake = m_commitWake || commit;
    }
    m_wake.notify_all();
}

ULONG CALLBACK AutoPowerEngine::PlanChanged(PVOID context, ULONG type, PVOID setting) {
    auto* self = static_cast<AutoPowerEngine*>(context);
    const auto* change = static_cast<const POWERBROADCAST_SETTING*>(setting);
    if (self == nullptr || type != PBT_POWERSETTINGCHANGE || change == nullptr ||
        change->DataLength < sizeof(GUID) || IsEqualGUID(change->PowerSetting, kActiveSchemeSetting) == FALSE) {
        return ERROR_SUCCESS;
    }
    GUID scheme{};
    std::memcpy(&scheme, change->Data, sizeof(scheme));
    const int state = IsEqualGUID(scheme, kAutoPlan) != FALSE ? 1 : 2;
    // Re-activating our own plan to apply new values reports no change.
    if (self->m_planState.exchange(state) != state) {
        self->Wake();
    }
    return ERROR_SUCCESS;
}

AutoPowerStatus AutoPowerEngine::Status() const {
    std::lock_guard lock(m_mutex);
    return m_status;
}

void AutoPowerEngine::ThreadMain() {
    // Plan switches arrive as events, so a dormant engine sleeps until one
    // does. Without the subscription, fall back to polling the active plan.
    m_planState.store(0);
    PlanSubscription subscribe{&AutoPowerEngine::PlanChanged, this};
    HPOWERNOTIFY notify = nullptr;
    if (PowerSettingRegisterNotification(&kActiveSchemeSetting, kDeviceNotifyCallback, &subscribe, &notify) !=
        ERROR_SUCCESS) {
        notify = nullptr;
        Trace(L"Plan change notifications unavailable; polling.");
    } else {
        // Seed the state in case no initial notification is delivered; a
        // callback that already arrived wins.
        int unknown = 0;
        static_cast<void>(m_planState.compare_exchange_strong(unknown, ActivePlanIs(kAutoPlan) ? 1 : 2));
    }
    {
        Engine engine;
        bool forceSample = true;
        bool commitWake = false;
        for (;;) {
            const int planState = m_planState.load();
            const bool planActive = planState == 0 ? ActivePlanIs(kAutoPlan) : planState == 1;
            const TickResult result = engine.Tick(forceSample, planActive,
                m_hookInputTick.load(std::memory_order_relaxed), commitWake,
                m_keyboardInput.load(std::memory_order_relaxed));
            const AutoPowerStatus& status = result.status;
            m_wantInput.store(status.engaged && result.wantInput, std::memory_order_relaxed);
            m_wantCommit.store(status.engaged && result.wantCommit, std::memory_order_relaxed);
            HWND notifyHwnd = nullptr;
            UINT notifyMsg = 0;
            {
                std::lock_guard lock(m_mutex);
                if (status.engaged != m_status.engaged || status.fullPower != m_status.fullPower ||
                    status.reason != m_status.reason || status.appName != m_status.appName) {
                    m_status = status;
                    notifyHwnd = m_notifyHwnd;
                    notifyMsg = m_notifyMsg;
                }
            }
            if (notifyHwnd != nullptr) {
                PostMessageW(notifyHwnd, notifyMsg, 0, 0);
            }
            std::unique_lock lock(m_mutex);
            const auto ready = [this] { return m_stop || m_woken; };
            if (status.engaged) {
                m_wake.wait_for(lock, std::chrono::milliseconds(result.waitMs), ready);
            } else if (notify != nullptr && m_planState.load() != 0) {
                m_wake.wait(lock, ready);
            } else {
                m_wake.wait_for(lock, std::chrono::milliseconds(kDormantTickMs), ready);
            }
            if (m_stop) {
                break;
            }
            forceSample = m_forceSample;
            commitWake = m_commitWake;
            m_commitWake = false;
            m_forceSample = false;
            m_woken = false;
        }
        m_wantInput.store(false);
        m_wantCommit.store(false);
        if (notify != nullptr) {
            PowerSettingUnregisterNotification(notify);
        }
        // Engine destructor restores the calm plan and process throttling.
    }
}
