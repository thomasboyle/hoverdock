#include "AutoPower.h"

#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <powersetting.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <optional>
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
// plan (many Modern Standby laptops only ship Balanced). Display, sleep and
// other settings stay as in Balanced.
struct PlanSetting {
    const GUID* subgroup;
    GUID setting;
    DWORD highValue;
    DWORD lowFallback;
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
// GPU engine counters are the costliest read: renders run for minutes and
// are held 20 s, so a 4.5 s cadence is enough outside the fullscreen probe.
constexpr ULONGLONG kGpuSampleMs = 3 * kTickMs - 100;
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
    // commitWake: woken by a click or key (work may be starting);
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
        if (!m_engaged) {
            m_engaged = true;
            m_planFull.reset();
            for (Written& written : m_written) {
                written.valid = false;
            }
            forceLoadSample = true;
            Trace(L"Engaged.");
        }

        const ULONGLONG now = GetTickCount64();
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
            inputs.apps.push_back(std::move(app));
        }

        const autopower::Decision decision = m_policy.Update(inputs);
        for (auto& [key, group] : m_groups) {
            group.sample.freshLoad = false;
        }
        if (decision.fullPower) {
            // Already at high power: nothing left for the probe to find.
            m_probeUntil = 0;
        }
        ApplyPlan(decision.fullPower);
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
                group.sample.kind == autopower::AppKind::Game || group.sample.registeredGame;
        }
        if (!decision.fullPower) {
            // A click or key starts a probe, so arm that wake unless one runs.
            result.wantCommit = now >= m_probeUntil;
            if (creativeFocused) {
                // Driving a creative tool is any input, pointer motion too.
                result.wantInput = true;
                if (!keyboardInput) {
                    result.waitMs = kCreativeTickMs;
                }
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
            ApplyPlan(false);
        }
        if (m_engaged) {
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
        m_lastGpuSample = 0;
        m_probeUntil = 0;
        m_fullscreenProbeUntil = 0;
        m_lastUnthrottle.clear();
        ++m_groupGeneration;
        m_foregroundPid = 0;
        m_lastFull = false;
        m_lastApp.clear();
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
        ULONGLONG lastCpu = 0;
        ULONGLONG lastCpuTick = 0;
        double lastPercent = 0.0;
        unsigned seenGeneration = 0;
        bool exempt = false;
        bool haveOriginal = false;
        PROCESS_POWER_THROTTLING_STATE original{};
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

    void ApplyPlan(bool full) {
        if (m_planFull.has_value() && *m_planFull == full) {
            return;
        }
        // Each write is a power-service round trip: skip values already in the
        // plan (the 100% maximum state is the same in both modes).
        bool wrote = false;
        bool failed = false;
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
            written = {acOk && dcOk, ac, dc};
            wrote = wrote || acOk || dcOk;
            failed = failed || !acOk || !dcOk;
        }
        // Re-activating the active plan is what applies freshly written values.
        if (wrote && ActivePlanIs(kAutoPlan)) {
            static_cast<void>(PowerSetActiveScheme(nullptr, &kAutoPlan));
        }
        if (failed && !wrote) {
            Trace(L"Plan values could not be written; only throttling is managed.");
        }
        m_planFull = full;
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

    // First sighting of a pid: path, creation time and classification.
    void Classify(DWORD pid, Proc& proc) {
        if (OpenProc(pid, proc)) {
            wchar_t path[MAX_PATH * 2]{};
            DWORD length = MAX_PATH * 2;
            if (QueryFullProcessImageNameW(proc.handle, 0, path, &length) != FALSE) {
                proc.path = Lower(path);
            }
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetProcessTimes(proc.handle, &created, &exited, &kernel, &user) != FALSE) {
                proc.created = FileTimeValue(created);
            }
        }
        proc.kind = autopower::ClassifyApp(proc.exe, proc.path);
        proc.registeredGame = !proc.path.empty() && m_registeredGames.contains(proc.path);
    }

    bool IsAppRoot(DWORD pid, const Proc& proc) const {
        if (proc.kind == autopower::AppKind::Creative || proc.kind == autopower::AppKind::Game ||
            proc.registeredGame) {
            return true;
        }
        return pid == m_foregroundPid && proc.kind != autopower::AppKind::NeverGame;
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
            const auto parent = m_procs.find(it->second.parent);
            if (it->second.parent == 0 || parent == m_procs.end() || parent->second.created == 0 ||
                parent->second.created > it->second.created ||
                std::find(std::begin(kNeverFoldInto), std::end(kNeverFoldInto), parent->second.exe) !=
                    std::end(kNeverFoldInto)) {
                break;
            }
            current = it->second.parent;
        }
        return pid;
    }

    double CpuPercent(Proc& proc, ULONGLONG now) {
        // Too short an interval (a focus change right after a sample) is
        // quantum noise: keep the last reading until a fair window passes.
        if (proc.lastCpuTick != 0 && now - proc.lastCpuTick < 50) {
            return proc.lastPercent;
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        if (proc.handle == nullptr || GetProcessTimes(proc.handle, &created, &exited, &kernel, &user) == FALSE) {
            return 0.0;
        }
        const ULONGLONG cpu = FileTimeValue(kernel) + FileTimeValue(user);
        double percent = 0.0;
        if (proc.lastCpuTick != 0 && cpu >= proc.lastCpu) {
            // CPU time is in 100 ns units; wall time in ms.
            const double wall = static_cast<double>(now - proc.lastCpuTick) * 10000.0 * m_cpuCount;
            percent = (std::min)(100.0 * static_cast<double>(cpu - proc.lastCpu) / wall, 100.0);
        }
        proc.lastCpu = cpu;
        proc.lastCpuTick = now;
        proc.lastPercent = percent;
        return percent;
    }

    void SampleLoads(ULONGLONG now) {
        m_lastLoadSample = now;
        RefreshRegisteredGames(now);

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return;
        }
        const DWORD self = GetCurrentProcessId();
        const unsigned generation = ++m_generation;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry) != FALSE) {
            do {
                const DWORD pid = entry.th32ProcessID;
                if (pid == 0 || pid == 4 || pid == self) {
                    continue;
                }
                // Known pids compare in place; only new ones pay for a copy.
                auto it = m_procs.find(pid);
                if (it != m_procs.end() && _wcsicmp(it->second.exe.c_str(), entry.szExeFile) != 0) {
                    CloseProc(it->second);
                    m_procs.erase(it);
                    it = m_procs.end();
                }
                if (it == m_procs.end()) {
                    Proc proc;
                    proc.exe = Lower(entry.szExeFile);
                    proc.display = DisplayName(entry.szExeFile);
                    proc.parent = entry.th32ParentProcessID;
                    Classify(pid, proc);
                    it = m_procs.emplace(pid, std::move(proc)).first;
                }
                it->second.parent = entry.th32ParentProcessID;
                it->second.seenGeneration = generation;
            } while (Process32NextW(snapshot, &entry) != FALSE);
        }
        CloseHandle(snapshot);
        for (auto it = m_procs.begin(); it != m_procs.end();) {
            if (it->second.seenGeneration != generation) {
                CloseProc(it->second);
                it = m_procs.erase(it);
            } else {
                ++it;
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

        SampleGpu(now);
        for (auto& [key, group] : m_groups) {
            MeasureGroup(group, now);
        }

        // Handles are only kept for app groups and exempted processes.
        for (auto& [pid, proc] : m_procs) {
            if (!proc.exempt && !m_groupOfPid.contains(pid)) {
                CloseProc(proc);
                proc.openFailed = false;
                proc.lastCpuTick = 0;
                proc.lastPercent = 0.0;
            }
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

    // Foreground fullscreen state, read at most once per tick.
    bool ForegroundFullscreen() {
        if (m_fullscreenCache < 0) {
            m_fullscreenCache = ForegroundIsFullscreen() ? 1 : 0;
        }
        return m_fullscreenCache == 1;
    }

    // GPU engine counters are the costliest read: renders only need the slow
    // cadence; an unknown fullscreen app is probed fast right after focus.
    void SampleGpu(ULONGLONG now) {
        bool creativeRunning = false;
        for (const auto& [key, group] : m_groups) {
            creativeRunning = creativeRunning || group.sample.kind == autopower::AppKind::Creative;
        }
        const bool probe = ForegroundCouldBeUnknownGame() && ForegroundFullscreen();
        if (!probe && !creativeRunning) {
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

    void MeasureGroup(Group& group, ULONGLONG now) {
        double cpu = 0.0;
        double gpu = 0.0;
        double gpu3d = 0.0;
        for (const DWORD pid : group.pids) {
            Proc& proc = m_procs[pid];
            if (OpenProc(pid, proc)) {
                cpu += CpuPercent(proc, now);
            }
            const auto load = m_gpuLoads.find(pid);
            if (load != m_gpuLoads.end()) {
                gpu = (std::max)(gpu, load->second.any);
                gpu3d = (std::max)(gpu3d, load->second.threeD);
            }
        }
        group.sample.cpuPercent = (std::min)(cpu, 100.0);
        group.sample.gpuPercent = gpu;
        group.sample.gpu3dPercent = gpu3d;
        group.sample.freshLoad = true;
    }

    // Probe tick: re-measure only the focused app from handles already open
    // (no process scan), plus a GPU read when an unknown fullscreen app is due.
    void SampleForeground(ULONGLONG now) {
        const auto foreground = m_groupOfPid.find(m_foregroundPid);
        if (foreground == m_groupOfPid.end()) {
            return;
        }
        const auto group = m_groups.find(foreground->second);
        if (group == m_groups.end()) {
            return;
        }
        SampleGpu(now);
        MeasureGroup(group->second, now);
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
        ThreadMain();
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
        Signal(false, starts);
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
