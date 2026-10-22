#include "PowerProfiler.h"

#include <intrin.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powerbase.h>
#include <powrprof.h>
#include <sddl.h>
#include <setupapi.h>
#include <shellapi.h>
#include <winioctl.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _MSC_VER
// The shared benchmark helpers include a few this file does not use.
#pragma warning(disable : 4505)
#endif

namespace {

#include "EnergyBenchCommon.inl"

// ---- constants ----------------------------------------------------------------------------

const GUID kProcessorSubgroup = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
const GUID kPcieSubgroup = {0x501a4d13, 0x42af, 0x4429, {0x9f, 0xd1, 0xa8, 0x21, 0x8c, 0x26, 0x8e, 0x20}};
// Temporary copy of the engine's plan for the settings sweep (deleted afterwards).
const GUID kScratchPlan = {0x7f3c1e2a, 0x5b6d, 0x4c8e, {0x9a, 0x0b, 0x1c, 0x2d, 0x3e, 0x4f, 0x5a, 0x6b}};
constexpr wchar_t kScratchPlanName[] = L"Hoverdock Profiler (temporary)";
// Named event that switches on the engine's per-tick diagnostics (AutoPower.cpp).
constexpr wchar_t kProfilerEventName[] = L"Local\\HoverdockPowerProfiler";
// Set by Dock Settings to stop a running profile early.
constexpr wchar_t kStopEventName[] = L"Local\\HoverdockPowerProfilerStop";
constexpr char kEngineTracePrefix[] = "[Hoverdock AutoPower]";

struct SettingInfo {
    const char* key;
    const GUID* subgroup;
    GUID guid;
};

// The processor and PCIe settings the engine writes or depends on.
const SettingInfo kSettings[] = {
    {"min_state", &kProcessorSubgroup, {0x893dee8e, 0x2bef, 0x41e0, {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c}}},
    {"min_state_class1", &kProcessorSubgroup,
        {0x893dee8e, 0x2bef, 0x41e0, {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4d}}},
    {"max_state", &kProcessorSubgroup, {0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}}},
    {"max_state_class1", &kProcessorSubgroup,
        {0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xed}}},
    {"boost_mode", &kProcessorSubgroup, {0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}}},
    {"boost_policy", &kProcessorSubgroup,
        {0x45bcc044, 0xd885, 0x43e2, {0x86, 0x05, 0xee, 0x0e, 0xc6, 0xe9, 0x6b, 0x59}}},
    {"epp", &kProcessorSubgroup, {0x36687f9e, 0xe3a5, 0x4dbf, {0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63}}},
    {"epp_class1", &kProcessorSubgroup, {0x36687f9e, 0xe3a5, 0x4dbf, {0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x64}}},
    {"activity_window", &kProcessorSubgroup,
        {0xcfeda3d0, 0x7697, 0x4566, {0xa9, 0x22, 0xa9, 0x08, 0x6c, 0xd4, 0x9d, 0xfa}}},
    {"autonomous_mode", &kProcessorSubgroup,
        {0x8baa4a8a, 0x14c6, 0x4451, {0x8e, 0x8b, 0x14, 0xbd, 0xbd, 0x19, 0x75, 0x37}}},
    {"parking_min_cores", &kProcessorSubgroup,
        {0x0cc5b647, 0xc1df, 0x4637, {0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83}}},
    {"parking_min_cores_class1", &kProcessorSubgroup,
        {0x0cc5b647, 0xc1df, 0x4637, {0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x84}}},
    {"parking_max_cores", &kProcessorSubgroup,
        {0xea062031, 0x0e34, 0x4ff1, {0x9b, 0x6d, 0xeb, 0x10, 0x59, 0x33, 0x40, 0x28}}},
    {"latency_hint_perf", &kProcessorSubgroup,
        {0x619b7505, 0x003b, 0x4e82, {0xb7, 0xa6, 0x4d, 0xd2, 0x9c, 0x30, 0x09, 0x71}}},
    {"latency_hint_perf_class1", &kProcessorSubgroup,
        {0x619b7505, 0x003b, 0x4e82, {0xb7, 0xa6, 0x4d, 0xd2, 0x9c, 0x30, 0x09, 0x72}}},
    {"latency_hint_unpark", &kProcessorSubgroup,
        {0x616cdaa5, 0x695e, 0x4545, {0x97, 0xad, 0x97, 0xdc, 0x2d, 0x1b, 0xdd, 0x88}}},
    {"latency_hint_unpark_class1", &kProcessorSubgroup,
        {0x616cdaa5, 0x695e, 0x4545, {0x97, 0xad, 0x97, 0xdc, 0x2d, 0x1b, 0xdd, 0x89}}},
    {"idle_disable", &kProcessorSubgroup,
        {0x5d76a2ca, 0xe8c0, 0x402f, {0xa1, 0x33, 0x21, 0x58, 0x49, 0x2d, 0x58, 0xad}}},
    {"pcie_aspm", &kPcieSubgroup, {0xee12f906, 0xd277, 0x404b, {0xb6, 0xda, 0xe5, 0xfa, 0x1a, 0x57, 0x6d, 0xf5}}},
};

const SettingInfo* FindSetting(std::string_view key) {
    for (const SettingInfo& setting : kSettings) {
        if (key == setting.key) {
            return &setting;
        }
    }
    return nullptr;
}

bool ReadSetting(const GUID& plan, const SettingInfo& setting, bool ac, DWORD& value) {
    return (ac ? PowerReadACValueIndex(nullptr, &plan, setting.subgroup, &setting.guid, &value)
               : PowerReadDCValueIndex(nullptr, &plan, setting.subgroup, &setting.guid, &value)) == ERROR_SUCCESS;
}

std::string ValueText(bool ok, DWORD value) {
    return ok ? std::to_string(value) : std::string();
}

// ---- console and progress log -------------------------------------------------------------

HANDLE g_out = nullptr;
HANDLE g_cleanupDone = nullptr;
// The console window is closing, or Dock Settings stopped the run: exit without waiting for Enter.
std::atomic<bool> g_closing{false};
std::mutex g_sayMutex;
std::wstring g_progressLog;

void Say(const std::wstring& text) {
    const std::lock_guard lock(g_sayMutex);
    const std::wstring line = text + L"\r\n";
    if (g_out != nullptr && g_out != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteConsoleW(g_out, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
    if (!g_progressLog.empty()) {
        WriteFileText(g_progressLog, Utf8(text) + "\n", true);
    }
}

std::wstring Clock(double seconds) {
    const auto total = static_cast<long long>(std::max(0.0, std::ceil(seconds)));
    wchar_t text[32]{};
    swprintf_s(text, L"%lld:%02lld", total / 60, total % 60);
    return text;
}

// Ctrl+C / Ctrl+Break / closing the window: stop, restore the plan, then exit.
BOOL WINAPI OnConsoleControl(DWORD type) {
    g_abort.store(true);
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_closing.store(true);
        if (g_cleanupDone != nullptr) {
            WaitForSingleObject(g_cleanupDone, 4500);
        }
    }
    return TRUE;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ProcessImageName(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(process, 0, path, &size) != FALSE) {
        name = FileNameOf(std::wstring(path, size));
    }
    CloseHandle(process);
    return name;
}

// Power throttling Windows (or Hoverdock) has set on a process: "c<control>/s<state>"
// (1 = execution speed / EcoQoS, 4 = timer resolution ignored), "?" when unreadable.
std::string ThrottleText(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return "?";
    }
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    std::string out = "?";
    if (GetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state)) != FALSE) {
        char text[32]{};
        std::snprintf(text, sizeof(text), "c%lx/s%lx", static_cast<unsigned long>(state.ControlMask),
            static_cast<unsigned long>(state.StateMask));
        out = text;
    }
    CloseHandle(process);
    return out;
}

// Runs powercfg.exe hidden with stdout/stderr to a file (or discarded); true on exit code 0.
bool RunPowercfgTo(const std::wstring& arguments, const std::wstring& outputPath, DWORD timeoutMs) {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const HANDLE output = CreateFileW(outputPath.empty() ? L"NUL" : outputPath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        return false;
    }
    wchar_t systemDir[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(systemDir, MAX_PATH);
    const std::wstring exe =
        (length == 0 || length >= MAX_PATH ? std::wstring(L"C:\\Windows\\System32") : std::wstring(systemDir)) +
        L"\\powercfg.exe";
    std::wstring commandLine = L"\"" + exe + L"\" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = output;
    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &info);
    CloseHandle(output);
    if (created == FALSE) {
        return false;
    }
    CloseHandle(info.hThread);
    DWORD exitCode = 1;
    if (WaitForSingleObject(info.hProcess, timeoutMs) != WAIT_OBJECT_0) {
        TerminateProcess(info.hProcess, 1);
        WaitForSingleObject(info.hProcess, 2000);
    } else {
        GetExitCodeProcess(info.hProcess, &exitCode);
    }
    CloseHandle(info.hProcess);
    return exitCode == 0;
}

// Named objects the dock can reach whether it or the profiler runs elevated
// (everyone, low integrity).
class OpenSecurity {
public:
    OpenSecurity() {
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1,
                &m_descriptor, nullptr) == FALSE) {
            m_descriptor = nullptr;
        }
        m_attributes.nLength = sizeof(m_attributes);
        m_attributes.lpSecurityDescriptor = m_descriptor;
        m_attributes.bInheritHandle = FALSE;
    }
    ~OpenSecurity() {
        if (m_descriptor != nullptr) {
            LocalFree(m_descriptor);
        }
    }
    OpenSecurity(const OpenSecurity&) = delete;
    OpenSecurity& operator=(const OpenSecurity&) = delete;

    [[nodiscard]] SECURITY_ATTRIBUTES* Get() noexcept { return m_descriptor != nullptr ? &m_attributes : nullptr; }

private:
    PSECURITY_DESCRIPTOR m_descriptor = nullptr;
    SECURITY_ATTRIBUTES m_attributes{};
};

// ---- engine trace (OutputDebugString via DBWIN) -------------------------------------------

struct TraceLine {
    double at = 0.0;
    std::string text;
};

class DebugListener {
public:
    DebugListener() = default;
    ~DebugListener() { Stop(); }
    DebugListener(const DebugListener&) = delete;
    DebugListener& operator=(const DebugListener&) = delete;

    // note explains a partial or failed start.
    bool Start(const std::wstring& path, double origin, std::wstring& note) {
        m_path = path;
        m_origin = origin;
        OpenSecurity security;
        m_ready = CreateEventW(security.Get(), FALSE, FALSE, L"DBWIN_BUFFER_READY");
        const bool shared = m_ready != nullptr && GetLastError() == ERROR_ALREADY_EXISTS;
        m_data = CreateEventW(security.Get(), FALSE, FALSE, L"DBWIN_DATA_READY");
        m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, security.Get(), PAGE_READWRITE, 0, 4096, L"DBWIN_BUFFER");
        if (m_ready == nullptr || m_data == nullptr || m_mapping == nullptr) {
            note = L"could not listen to debug output (error " + std::to_wstring(GetLastError()) + L")";
            Close();
            return false;
        }
        m_view = static_cast<const BYTE*>(MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, 4096));
        if (m_view == nullptr) {
            note = L"could not map the debug output buffer";
            Close();
            return false;
        }
        if (shared) {
            note = L"another debug output listener (DebugView?) is running: close it, or engine.log may miss lines";
        }
        WriteFileText(m_path, "t_s,pid,text\n");
        m_stop.store(false);
        m_thread = std::thread([this] { Loop(); });
        return true;
    }

    void Stop() {
        m_stop.store(true);
        if (m_thread.joinable()) {
            m_thread.join();
        }
        Close();
    }

    [[nodiscard]] std::vector<TraceLine> Lines() const {
        const std::lock_guard lock(m_mutex);
        return m_lines;
    }

private:
    void Loop() {
        while (!m_stop.load()) {
            SetEvent(m_ready);
            if (WaitForSingleObject(m_data, 200) != WAIT_OBJECT_0) {
                continue;
            }
            DWORD pid = 0;
            std::memcpy(&pid, m_view, sizeof(pid));
            const char* text = reinterpret_cast<const char*>(m_view + sizeof(DWORD));
            size_t length = 0;
            while (length < 4096 - sizeof(DWORD) && text[length] != '\0') {
                ++length;
            }
            std::string message(text, length);
            while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
                message.pop_back();
            }
            if (message.rfind(kEngineTracePrefix, 0) != 0) {
                continue;
            }
            message.erase(0, sizeof(kEngineTracePrefix));
            const double at = Now();
            WriteFileText(m_path, Number(at - m_origin, 3) + "," + std::to_string(pid) + "," + CsvField(message) + "\n",
                true);
            const std::lock_guard lock(m_mutex);
            m_lines.push_back({at, std::move(message)});
        }
    }

    void Close() {
        if (m_view != nullptr) {
            UnmapViewOfFile(m_view);
            m_view = nullptr;
        }
        for (HANDLE* handle : {&m_mapping, &m_ready, &m_data}) {
            if (*handle != nullptr) {
                CloseHandle(*handle);
                *handle = nullptr;
            }
        }
    }

    std::wstring m_path;
    double m_origin = 0.0;
    HANDLE m_ready = nullptr;
    HANDLE m_data = nullptr;
    HANDLE m_mapping = nullptr;
    const BYTE* m_view = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    mutable std::mutex m_mutex;
    std::vector<TraceLine> m_lines;
};

// Profile named in an engine "diag tick" line, or empty.
std::string TickProfile(const std::string& line) {
    if (line.rfind("diag tick ", 0) != 0) {
        return {};
    }
    const size_t at = line.find("profile=");
    if (at == std::string::npos) {
        return {};
    }
    const size_t end = line.find(' ', at);
    return line.substr(at + 8, end == std::string::npos ? std::string::npos : end - at - 8);
}

// ---- processes (NtQuerySystemInformation: CPU, cycles, context switches) ------------------

// SYSTEM_THREAD_INFORMATION / SYSTEM_PROCESS_INFORMATION in full (winternl.h hides most fields).
struct NtThread {
    LARGE_INTEGER kernelTime;
    LARGE_INTEGER userTime;
    LARGE_INTEGER createTime;
    ULONG waitTime;
    PVOID startAddress;
    HANDLE uniqueProcess;
    HANDLE uniqueThread;
    LONG priority;
    LONG basePriority;
    ULONG contextSwitches;
    ULONG threadState;
    ULONG waitReason;
};

struct NtProcess {
    ULONG nextEntryOffset;
    ULONG numberOfThreads;
    LARGE_INTEGER workingSetPrivateSize;
    ULONG hardFaultCount;
    ULONG numberOfThreadsHighWatermark;
    ULONGLONG cycleTime;
    LARGE_INTEGER createTime;
    LARGE_INTEGER userTime;
    LARGE_INTEGER kernelTime;
    UNICODE_STRING imageName;
    LONG basePriority;
    HANDLE uniqueProcessId;
    HANDLE inheritedFromUniqueProcessId;
    ULONG handleCount;
    ULONG sessionId;
    ULONG_PTR uniqueProcessKey;
    SIZE_T peakVirtualSize;
    SIZE_T virtualSize;
    ULONG pageFaultCount;
    SIZE_T peakWorkingSetSize;
    SIZE_T workingSetSize;
    SIZE_T quotaPeakPagedPoolUsage;
    SIZE_T quotaPagedPoolUsage;
    SIZE_T quotaPeakNonPagedPoolUsage;
    SIZE_T quotaNonPagedPoolUsage;
    SIZE_T pagefileUsage;
    SIZE_T peakPagefileUsage;
    SIZE_T privatePageCount;
    LARGE_INTEGER readOperationCount;
    LARGE_INTEGER writeOperationCount;
    LARGE_INTEGER otherOperationCount;
    LARGE_INTEGER readTransferCount;
    LARGE_INTEGER writeTransferCount;
    LARGE_INTEGER otherTransferCount;
};

#ifdef _WIN64
static_assert(sizeof(NtThread) == 0x50, "SYSTEM_THREAD_INFORMATION layout");
static_assert(sizeof(NtProcess) == 0x100, "SYSTEM_PROCESS_INFORMATION layout");
#endif

using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQueryTimerResolutionFn = LONG(NTAPI*)(PULONG, PULONG, PULONG);

template <typename Fn>
Fn NtdllFunction(const char* name) {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll == nullptr ? nullptr : reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(ntdll, name)));
}

// System timer resolution in milliseconds (coarsest, finest, current); false when unavailable.
bool TimerResolution(double& coarsest, double& finest, double& current) {
    static const auto query = NtdllFunction<NtQueryTimerResolutionFn>("NtQueryTimerResolution");
    ULONG maximum = 0;
    ULONG minimum = 0;
    ULONG now = 0;
    if (query == nullptr || query(&maximum, &minimum, &now) != 0) {
        return false;
    }
    coarsest = static_cast<double>(maximum) / 10000.0;
    finest = static_cast<double>(minimum) / 10000.0;
    current = static_cast<double>(now) / 10000.0;
    return true;
}

struct ProcessSnapshot {
    std::wstring name;
    LONGLONG created = 0;
    ULONGLONG cpu = 0;  // 100 ns
    ULONGLONG cycles = 0;
    ULONGLONG switches = 0;
    ULONG threads = 0;
};

class ProcessSampler {
public:
    bool Open() {
        m_query = NtdllFunction<NtQuerySystemInformationFn>("NtQuerySystemInformation");
        m_buffer.resize(1U << 20U);
        return m_query != nullptr;
    }

    bool Snapshot(std::unordered_map<DWORD, ProcessSnapshot>& out) {
        out.clear();
        if (m_query == nullptr) {
            return false;
        }
        constexpr ULONG kSystemProcessInformation = 5;
        constexpr ULONG kInfoLengthMismatch = 0xC0000004UL;
        bool ok = false;
        for (int attempt = 0; attempt < 5 && !ok; ++attempt) {
            ULONG needed = 0;
            const LONG status =
                m_query(kSystemProcessInformation, m_buffer.data(), static_cast<ULONG>(m_buffer.size()), &needed);
            if (status == 0) {
                ok = true;
            } else if (static_cast<ULONG>(status) == kInfoLengthMismatch) {
                m_buffer.resize(std::max(static_cast<size_t>(needed) + 65536U, m_buffer.size() * 2U));
            } else {
                return false;
            }
        }
        if (!ok) {
            return false;
        }
        size_t offset = 0;
        for (;;) {
            if (offset + sizeof(NtProcess) > m_buffer.size()) {
                break;
            }
            const auto* process = reinterpret_cast<const NtProcess*>(m_buffer.data() + offset);
            ProcessSnapshot entry;
            const auto pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(process->uniqueProcessId));
            entry.name = pid == 0 ? std::wstring(L"Idle")
                : process->imageName.Buffer != nullptr
                ? std::wstring(process->imageName.Buffer, process->imageName.Length / sizeof(wchar_t))
                : std::wstring(L"System");
            entry.created = process->createTime.QuadPart;
            entry.cpu = static_cast<ULONGLONG>(process->kernelTime.QuadPart) +
                static_cast<ULONGLONG>(process->userTime.QuadPart);
            entry.cycles = process->cycleTime;
            entry.threads = process->numberOfThreads;
            const auto* threads = reinterpret_cast<const NtThread*>(process + 1);
            const size_t available = (m_buffer.size() - offset - sizeof(NtProcess)) / sizeof(NtThread);
            const size_t count = std::min(static_cast<size_t>(process->numberOfThreads), available);
            for (size_t index = 0; index < count; ++index) {
                entry.switches += threads[index].contextSwitches;
            }
            out[pid] = std::move(entry);
            if (process->nextEntryOffset == 0) {
                break;
            }
            offset += process->nextEntryOffset;
        }
        return true;
    }

private:
    NtQuerySystemInformationFn m_query = nullptr;
    std::vector<BYTE> m_buffer;
};

// ---- processors (PDH "Processor Information") ---------------------------------------------

constexpr size_t kUtil = 0;
constexpr size_t kPerf = 1;
constexpr size_t kUtility = 2;
constexpr size_t kFreq = 3;
constexpr size_t kC1 = 4;
constexpr size_t kC2 = 5;
constexpr size_t kC3 = 6;
constexpr size_t kParked = 7;
constexpr size_t kLimitFlags = 8;
constexpr size_t kLimitPct = 9;
constexpr size_t kInterrupts = 10;
constexpr size_t kCoreFields = 11;
const wchar_t* const kCoreCounterNames[kCoreFields] = {L"% Processor Time", L"% Processor Performance",
    L"% Processor Utility", L"Processor Frequency", L"% C1 Time", L"% C2 Time", L"% C3 Time", L"Parking Status",
    L"Performance Limit Flags", L"% Performance Limit", L"Interrupts/sec"};
const char* const kCoreColumnNames[kCoreFields] = {"util_pct", "perf_pct", "utility_pct", "base_mhz", "c1_pct",
    "c2_pct", "c3_pct", "parked", "limit_flags", "limit_pct", "interrupts_s"};

struct CoreValues {
    std::wstring name;
    std::array<double, kCoreFields> values{};
};

struct CpuReading {
    std::vector<CoreValues> cores;
    CoreValues total;
    double contextSwitches = kNaN;
    double systemCalls = kNaN;
    double queue = kNaN;
};

class CpuCounters {
public:
    CpuCounters() = default;
    ~CpuCounters() {
        if (m_query != nullptr) {
            PdhCloseQuery(m_query);
        }
    }
    CpuCounters(const CpuCounters&) = delete;
    CpuCounters& operator=(const CpuCounters&) = delete;

    // missing lists counters this Windows build does not have.
    bool Open(std::string& missing) {
        if (PdhOpenQueryW(nullptr, 0, &m_query) != ERROR_SUCCESS) {
            m_query = nullptr;
            return false;
        }
        for (size_t index = 0; index < kCoreFields; ++index) {
            const std::wstring path = L"\\Processor Information(*)\\" + std::wstring(kCoreCounterNames[index]);
            if (PdhAddEnglishCounterW(m_query, path.c_str(), 0, &m_core[index]) != ERROR_SUCCESS) {
                m_core[index] = nullptr;
                missing += Utf8(kCoreCounterNames[index]) + "; ";
            }
        }
        const wchar_t* const systemPaths[3] = {
            L"\\System\\Context Switches/sec", L"\\System\\System Calls/sec", L"\\System\\Processor Queue Length"};
        for (size_t index = 0; index < 3; ++index) {
            if (PdhAddEnglishCounterW(m_query, systemPaths[index], 0, &m_system[index]) != ERROR_SUCCESS) {
                m_system[index] = nullptr;
            }
        }
        PdhCollectQueryData(m_query);
        return m_core[kUtil] != nullptr;
    }

    bool Collect(CpuReading& out) {
        if (m_query == nullptr || PdhCollectQueryData(m_query) != ERROR_SUCCESS) {
            return false;
        }
        std::array<std::map<std::wstring, double>, kCoreFields> fields;
        for (size_t index = 0; index < kCoreFields; ++index) {
            if (m_core[index] != nullptr) {
                ReadArray(m_core[index], fields[index]);
            }
        }
        out.cores.clear();
        out.total = CoreValues{};
        out.total.name = L"_Total";
        out.total.values.fill(kNaN);
        std::vector<std::pair<std::pair<int, int>, std::wstring>> order;
        for (const auto& [name, value] : fields[kUtil]) {
            static_cast<void>(value);
            if (name.find(L"_Total") != std::wstring::npos) {
                continue;
            }
            const size_t comma = name.find(L',');
            const int group = comma == std::wstring::npos ? 0 : _wtoi(name.substr(0, comma).c_str());
            const int number = _wtoi(comma == std::wstring::npos ? name.c_str() : name.c_str() + comma + 1);
            order.push_back({{group, number}, name});
        }
        std::sort(order.begin(), order.end());
        const auto valuesOf = [&fields](const std::wstring& name, CoreValues& core) {
            core.name = name;
            for (size_t index = 0; index < kCoreFields; ++index) {
                const auto found = fields[index].find(name);
                core.values[index] = found != fields[index].end() ? found->second : kNaN;
            }
        };
        for (const auto& entry : order) {
            CoreValues core;
            valuesOf(entry.second, core);
            out.cores.push_back(std::move(core));
        }
        valuesOf(L"_Total", out.total);
        out.contextSwitches = ReadSingle(m_system[0]);
        out.systemCalls = ReadSingle(m_system[1]);
        out.queue = ReadSingle(m_system[2]);
        return true;
    }

private:
    static void ReadArray(PDH_HCOUNTER counter, std::map<std::wstring, double>& values) {
        DWORD size = 0;
        DWORD count = 0;
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr) !=
                static_cast<PDH_STATUS>(PDH_MORE_DATA) ||
            size == 0) {
            return;
        }
        std::vector<BYTE> buffer(size, 0);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) !=
            ERROR_SUCCESS) {
            return;
        }
        for (DWORD index = 0; index < count; ++index) {
            const PDH_FMT_COUNTERVALUE& value = items[index].FmtValue;
            if (items[index].szName != nullptr &&
                (value.CStatus == PDH_CSTATUS_VALID_DATA || value.CStatus == PDH_CSTATUS_NEW_DATA)) {
                values[items[index].szName] = value.doubleValue;
            }
        }
    }

    static double ReadSingle(PDH_HCOUNTER counter) {
        PDH_FMT_COUNTERVALUE value{};
        if (counter == nullptr ||
            PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &value) != ERROR_SUCCESS ||
            (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA)) {
            return kNaN;
        }
        return value.doubleValue;
    }

    PDH_HQUERY m_query = nullptr;
    std::array<PDH_HCOUNTER, kCoreFields> m_core{};
    std::array<PDH_HCOUNTER, 3> m_system{};
};

// Effective clock of a core while it runs: % Processor Performance (APERF/MPERF,
// over 100 in turbo) of its nominal frequency.
double CoreMhz(const CoreValues& core) {
    return core.values[kPerf] * core.values[kFreq] / 100.0;
}

// ---- timeline (one sample a second) -------------------------------------------------------

struct Sample {
    double at = 0.0;  // Now()
    std::string phase;
    std::string label;
    std::string plan;
    std::string foreground;
    double idleMs = kNaN;
    double util = kNaN;
    double perf = kNaN;
    double avgMhz = kNaN;
    double busyMhz = kNaN;
    double maxMhz = kNaN;
    double c1 = kNaN;
    double c2 = kNaN;
    double c3 = kNaN;
    double parked = kNaN;
    double limitFlags = kNaN;
    double contextSwitches = kNaN;
    double interrupts = kNaN;
    double systemCalls = kNaN;
    double queue = kNaN;
    double timerMs = kNaN;
    std::vector<double> emiWatts;
    double pdhWatts = kNaN;
    double batteryWatts = kNaN;
};

struct ProcessTotals {
    double cpuSeconds = 0.0;
    double switches = 0.0;
    double cycles = 0.0;
};

class Timeline {
public:
    Timeline() = default;
    ~Timeline() { Stop(); }
    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;

    void Start(const std::wstring& dir, double origin, EmiMeter* emi, PdhPowerMeter* pdh, CpuCounters* cpu,
        ProcessSampler* processes, bool battery) {
        m_dir = dir;
        m_origin = origin;
        m_emi = emi;
        m_pdh = pdh;
        m_cpu = cpu;
        m_processes = processes;
        m_battery = battery;
        std::string header = "t_s,phase,label,plan,foreground,idle_ms,util_pct,perf_pct,avg_mhz,busy_mhz,max_mhz,"
                             "c1_pct,c2_pct,c3_pct,parked_cores,limit_flags,context_switches_s,interrupts_s,"
                             "system_calls_s,queue,timer_ms";
        if (m_emi != nullptr) {
            for (const std::wstring& name : m_emi->Names()) {
                header += "," + CsvField("emi_" + Utf8(name) + "_w");
            }
        }
        header += ",pdh_w,battery_w\n";
        WriteFileText(m_dir + L"\\timeline.csv", header);
        std::string coreHeader = "t_s,phase,label,core,mhz";
        for (const char* column : kCoreColumnNames) {
            coreHeader += std::string(",") + column;
        }
        WriteFileText(m_dir + L"\\cores.csv", coreHeader + "\n");
        WriteFileText(m_dir + L"\\processes.csv",
            "t_s,phase,label,pid,exe,cpu_cores,gcycles_s,context_switches_s,threads,throttle\n");
        m_stop.store(false);
        m_thread = std::thread([this] { Loop(); });
    }

    void Stop() {
        m_stop.store(true);
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    void SetLabel(const std::string& phase, const std::string& label) {
        const std::lock_guard lock(m_mutex);
        m_phase = phase;
        m_label = label;
    }

    [[nodiscard]] std::vector<Sample> Samples() const {
        const std::lock_guard lock(m_mutex);
        return m_samples;
    }

    // Per-phase totals by executable name, and how long each phase was sampled.
    [[nodiscard]] std::map<std::string, std::map<std::wstring, ProcessTotals>> ProcessesByPhase() const {
        const std::lock_guard lock(m_mutex);
        return m_totals;
    }
    [[nodiscard]] std::map<std::string, double> PhaseSeconds() const {
        const std::lock_guard lock(m_mutex);
        return m_phaseSeconds;
    }

private:
    void Loop() {
        CpuReading reading;
        m_cpu->Collect(reading);
        std::vector<uint64_t> lastEnergy;
        double lastEnergyAt = Now();
        if (m_emi != nullptr && !m_emi->Read(lastEnergy)) {
            lastEnergy.clear();
        }
        double lastSnapshotAt = Now();
        m_processes->Snapshot(m_lastSnapshot);
        double next = Now();
        unsigned count = 0;
        while (!m_stop.load()) {
            next += 1.0;
            while (!m_stop.load() && Now() < next) {
                Sleep(static_cast<DWORD>(std::clamp((next - Now()) * 1000.0, 1.0, 100.0)));
            }
            if (m_stop.load()) {
                break;
            }
            Sample sample;
            sample.at = Now();
            {
                const std::lock_guard lock(m_mutex);
                sample.phase = m_phase;
                sample.label = m_label;
            }
            FillCpu(sample, reading);
            if (m_emi != nullptr && !lastEnergy.empty()) {
                std::vector<uint64_t> energy;
                if (m_emi->Read(energy) && energy.size() == lastEnergy.size() && sample.at > lastEnergyAt) {
                    for (size_t index = 0; index < energy.size(); ++index) {
                        sample.emiWatts.push_back(static_cast<double>(energy[index] - lastEnergy[index]) *
                            kJoulesPerPicowattHour / (sample.at - lastEnergyAt));
                    }
                    lastEnergy = std::move(energy);
                    lastEnergyAt = sample.at;
                }
            }
            if (m_pdh != nullptr) {
                double watts = 0.0;
                if (m_pdh->Read(watts)) {
                    sample.pdhWatts = watts;
                }
            }
            BatteryReading battery;
            if (m_battery && ReadBattery(battery) && !battery.onAc) {
                sample.batteryWatts = battery.dischargeWatts;
            }
            FillSystem(sample);
            WriteSample(sample, reading);
            if (++count % 5 == 0) {
                const double at = Now();
                SampleProcesses(sample, at - lastSnapshotAt);
                lastSnapshotAt = at;
            }
            const std::lock_guard lock(m_mutex);
            m_phaseSeconds[sample.phase] += 1.0;
            m_samples.push_back(std::move(sample));
        }
    }

    void FillCpu(Sample& sample, CpuReading& reading) {
        if (!m_cpu->Collect(reading)) {
            return;
        }
        const CoreValues& total = reading.total;
        sample.util = total.values[kUtil];
        sample.perf = total.values[kPerf];
        sample.avgMhz = CoreMhz(total);
        sample.c1 = total.values[kC1];
        sample.c2 = total.values[kC2];
        sample.c3 = total.values[kC3];
        sample.interrupts = total.values[kInterrupts];
        double weighted = 0.0;
        double weight = 0.0;
        double fastest = kNaN;
        double parked = 0.0;
        unsigned flags = 0;
        bool haveParking = false;
        for (const CoreValues& core : reading.cores) {
            const double busy = core.values[kUtil];
            const double mhz = CoreMhz(core);
            if (std::isfinite(busy) && std::isfinite(mhz)) {
                weighted += busy * mhz;
                weight += busy;
                // A core that barely ran reports a meaningless clock.
                if (busy >= 2.0 && (!std::isfinite(fastest) || mhz > fastest)) {
                    fastest = mhz;
                }
            }
            if (std::isfinite(core.values[kParked])) {
                haveParking = true;
                parked += core.values[kParked] >= 0.5 ? 1.0 : 0.0;
            }
            if (std::isfinite(core.values[kLimitFlags])) {
                flags |= static_cast<unsigned>(core.values[kLimitFlags]);
            }
        }
        sample.busyMhz = weight > 0.01 ? weighted / weight : kNaN;
        sample.maxMhz = fastest;
        sample.parked = haveParking ? parked : kNaN;
        sample.limitFlags = static_cast<double>(flags);
        sample.contextSwitches = reading.contextSwitches;
        sample.systemCalls = reading.systemCalls;
        sample.queue = reading.queue;
    }

    void FillSystem(Sample& sample) {
        double coarsest = 0.0;
        double finest = 0.0;
        double current = 0.0;
        if (TimerResolution(coarsest, finest, current)) {
            sample.timerMs = current;
        }
        GUID active{};
        if (ActivePlan(active)) {
            const std::string key = GuidText(active);
            auto found = m_planNames.find(key);
            if (found == m_planNames.end()) {
                found = m_planNames.emplace(key, PlanName(active)).first;
            }
            sample.plan = found->second;
        }
        const HWND window = GetForegroundWindow();
        DWORD pid = 0;
        if (window != nullptr) {
            GetWindowThreadProcessId(window, &pid);
        }
        if (pid != m_foregroundPid) {
            m_foregroundPid = pid;
            m_foregroundName = pid == 0 ? std::string() : Utf8(ProcessImageName(pid));
        }
        sample.foreground = m_foregroundName;
        LASTINPUTINFO input{};
        input.cbSize = sizeof(input);
        if (GetLastInputInfo(&input) != FALSE) {
            sample.idleMs = static_cast<double>(GetTickCount() - input.dwTime);
        }
    }

    void WriteSample(const Sample& sample, const CpuReading& reading) {
        const std::string t = Number(sample.at - m_origin, 1);
        const std::string where = CsvField(sample.phase) + "," + CsvField(sample.label);
        std::string row = t + "," + where + "," + CsvField(sample.plan) + "," + CsvField(sample.foreground) + "," +
            Number(sample.idleMs, 0) + "," + Number(sample.util, 2) + "," + Number(sample.perf, 2) + "," +
            Number(sample.avgMhz, 0) + "," + Number(sample.busyMhz, 0) + "," + Number(sample.maxMhz, 0) + "," +
            Number(sample.c1, 2) + "," + Number(sample.c2, 2) + "," + Number(sample.c3, 2) + "," +
            Number(sample.parked, 0) + "," + Number(sample.limitFlags, 0) + "," + Number(sample.contextSwitches, 0) +
            "," + Number(sample.interrupts, 0) + "," + Number(sample.systemCalls, 0) + "," + Number(sample.queue, 0) +
            "," + Number(sample.timerMs, 4);
        const size_t channels = m_emi != nullptr ? m_emi->Names().size() : 0;
        for (size_t index = 0; index < channels; ++index) {
            row += "," + (index < sample.emiWatts.size() ? Number(sample.emiWatts[index], 3) : std::string());
        }
        row += "," + Number(sample.pdhWatts, 3) + "," + Number(sample.batteryWatts, 3) + "\n";
        WriteFileText(m_dir + L"\\timeline.csv", row, true);
        std::string cores;
        for (const CoreValues& core : reading.cores) {
            cores += t + "," + where + "," + CsvField(Utf8(core.name)) + "," + Number(CoreMhz(core), 0);
            for (size_t index = 0; index < kCoreFields; ++index) {
                cores += "," + Number(core.values[index], 2);
            }
            cores += "\n";
        }
        WriteFileText(m_dir + L"\\cores.csv", cores, true);
    }

    // Busiest processes by CPU and by context switches (wake-ups) since the
    // previous snapshot, plus per-phase totals for the summary.
    void SampleProcesses(const Sample& sample, double elapsed) {
        std::unordered_map<DWORD, ProcessSnapshot> current;
        if (elapsed <= 0.0 || !m_processes->Snapshot(current)) {
            return;
        }
        struct Delta {
            DWORD pid;
            const ProcessSnapshot* snapshot;
            double cores;
            double gcycles;
            double switches;
        };
        std::vector<Delta> deltas;
        std::map<std::wstring, ProcessTotals> phaseAdd;
        for (const auto& [pid, snapshot] : current) {
            const auto previous = m_lastSnapshot.find(pid);
            if (pid == 0 || previous == m_lastSnapshot.end() || previous->second.created != snapshot.created) {
                continue;
            }
            const double cpu = static_cast<double>(snapshot.cpu - std::min(snapshot.cpu, previous->second.cpu));
            const double cycles =
                static_cast<double>(snapshot.cycles - std::min(snapshot.cycles, previous->second.cycles));
            const double switches =
                static_cast<double>(snapshot.switches - std::min(snapshot.switches, previous->second.switches));
            deltas.push_back({pid, &snapshot, cpu / 1e7 / elapsed, cycles / 1e9 / elapsed, switches / elapsed});
            ProcessTotals& totals = phaseAdd[snapshot.name];
            totals.cpuSeconds += cpu / 1e7;
            totals.cycles += cycles;
            totals.switches += switches;
        }
        std::vector<size_t> pick;
        const auto top = [&deltas, &pick](auto key) {
            std::vector<size_t> order(deltas.size());
            for (size_t index = 0; index < order.size(); ++index) {
                order[index] = index;
            }
            std::sort(order.begin(), order.end(),
                [&deltas, &key](size_t a, size_t b) { return key(deltas[a]) > key(deltas[b]); });
            for (size_t index = 0; index < order.size() && index < 12; ++index) {
                if (std::find(pick.begin(), pick.end(), order[index]) == pick.end()) {
                    pick.push_back(order[index]);
                }
            }
        };
        top([](const Delta& delta) { return delta.cores; });
        top([](const Delta& delta) { return delta.switches; });
        const std::string prefix = Number(sample.at - m_origin, 1) + "," + CsvField(sample.phase) + "," +
            CsvField(sample.label) + ",";
        std::string rows;
        for (const size_t index : pick) {
            const Delta& delta = deltas[index];
            rows += prefix + std::to_string(delta.pid) + "," + CsvField(Utf8(delta.snapshot->name)) + "," +
                Number(delta.cores, 4) + "," + Number(delta.gcycles, 4) + "," + Number(delta.switches, 1) + "," +
                std::to_string(delta.snapshot->threads) + "," + ThrottleText(delta.pid) + "\n";
        }
        WriteFileText(m_dir + L"\\processes.csv", rows, true);
        {
            const std::lock_guard lock(m_mutex);
            std::map<std::wstring, ProcessTotals>& phase = m_totals[sample.phase];
            for (const auto& [name, add] : phaseAdd) {
                ProcessTotals& totals = phase[name];
                totals.cpuSeconds += add.cpuSeconds;
                totals.cycles += add.cycles;
                totals.switches += add.switches;
            }
        }
        m_lastSnapshot = std::move(current);
    }

    std::wstring m_dir;
    double m_origin = 0.0;
    EmiMeter* m_emi = nullptr;
    PdhPowerMeter* m_pdh = nullptr;
    CpuCounters* m_cpu = nullptr;
    ProcessSampler* m_processes = nullptr;
    bool m_battery = false;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    mutable std::mutex m_mutex;
    std::string m_phase = "start";
    std::string m_label;
    std::vector<Sample> m_samples;
    std::map<std::string, std::map<std::wstring, ProcessTotals>> m_totals;
    std::map<std::string, double> m_phaseSeconds;
    std::unordered_map<DWORD, ProcessSnapshot> m_lastSnapshot;
    std::map<std::string, std::string> m_planNames;
    DWORD m_foregroundPid = 0;
    std::string m_foregroundName;
};

// ---- inventory ----------------------------------------------------------------------------

// PROCESSOR_POWER_INFORMATION (documented, not declared by the SDK headers).
struct ProcessorPower {
    ULONG number;
    ULONG maxMhz;
    ULONG currentMhz;
    ULONG mhzLimit;
    ULONG maxIdleState;
    ULONG currentIdleState;
};

std::string YesNo(bool value) {
    return value ? "yes" : "no";
}

std::string CpuidText() {
    std::string out;
    int regs[4]{};
    __cpuid(regs, 0);
    const int maxLeaf = regs[0];
    char vendor[13]{};
    std::memcpy(vendor, &regs[1], 4);
    std::memcpy(vendor + 4, &regs[3], 4);
    std::memcpy(vendor + 8, &regs[2], 4);
    out += "- CPUID vendor: " + std::string(vendor) + "\n";
    if (maxLeaf >= 1) {
        __cpuid(regs, 1);
        const auto eax = static_cast<unsigned>(regs[0]);
        const unsigned family = (eax >> 8U) & 0xFU;
        const unsigned model = (eax >> 4U) & 0xFU;
        const unsigned family2 = family == 0xFU ? family + ((eax >> 20U) & 0xFFU) : family;
        const unsigned model2 = family == 6U || family == 0xFU ? (((eax >> 16U) & 0xFU) << 4U) + model : model;
        char text[96]{};
        std::snprintf(text, sizeof(text), "- Family / model / stepping: 0x%X / 0x%X / %u\n", family2, model2,
            eax & 0xFU);
        out += text;
    }
    if (maxLeaf >= 6) {
        __cpuid(regs, 6);
        const auto eax = static_cast<unsigned>(regs[0]);
        const auto ecx = static_cast<unsigned>(regs[2]);
        out += "- Intel HWP (Speed Shift): " + YesNo((eax & (1U << 7U)) != 0) +
            "; HWP energy performance preference: " + YesNo((eax & (1U << 10U)) != 0) +
            "; HWP activity window: " + YesNo((eax & (1U << 9U)) != 0) +
            "; HWP package-level request: " + YesNo((eax & (1U << 11U)) != 0) +
            "; energy performance bias: " + YesNo((ecx & (1U << 3U)) != 0) + "\n";
    }
    __cpuid(regs, static_cast<int>(0x80000000U));
    if (static_cast<unsigned>(regs[0]) >= 0x80000008U) {
        __cpuid(regs, static_cast<int>(0x80000008U));
        out += "- AMD CPPC: " + YesNo((static_cast<unsigned>(regs[1]) & (1U << 27U)) != 0) + "\n";
    }
    return out;
}

std::string TopologyText() {
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (length == 0) {
        return {};
    }
    std::vector<BYTE> buffer(length, 0);
    if (GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length) == FALSE) {
        return {};
    }
    unsigned cores = 0;
    unsigned smt = 0;
    std::map<unsigned, unsigned> classes;
    for (size_t offset = 0; offset + sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX) <= length;) {
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (entry->Size == 0) {
            break;
        }
        if (entry->Relationship == RelationProcessorCore) {
            ++cores;
            // PROCESSOR_RELATIONSHIP: Flags, then EfficiencyClass.
            const auto* bytes = reinterpret_cast<const BYTE*>(&entry->Processor);
            smt += (bytes[0] & 1U) != 0 ? 1U : 0U;
            ++classes[bytes[1]];
        }
        offset += entry->Size;
    }
    std::string out = "- Physical cores: " + std::to_string(cores) + " (" + std::to_string(smt) +
        " with SMT); logical processors: " + std::to_string(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) +
        "\n- Efficiency classes (class: cores):";
    for (const auto& [efficiency, count] : classes) {
        out += " " + std::to_string(efficiency) + ": " + std::to_string(count);
    }
    return out + "\n";
}

std::string ProcessorPowerText() {
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    std::vector<ProcessorPower> power(std::max<DWORD>(count, 1));
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, power.data(),
            static_cast<ULONG>(power.size() * sizeof(ProcessorPower))) != 0) {
        return "- Processor power information: unavailable\n";
    }
    std::string out = "- Per processor (number: max MHz / current MHz / MHz limit / deepest idle state):";
    for (const ProcessorPower& entry : power) {
        out += " " + std::to_string(entry.number) + ": " + std::to_string(entry.maxMhz) + "/" +
            std::to_string(entry.currentMhz) + "/" + std::to_string(entry.mhzLimit) + "/" +
            std::to_string(entry.maxIdleState) + ";";
    }
    return out + "\n";
}

std::string OverlayLabel(const std::string& guid) {
    if (guid == "961cc777-2547-4f9d-8174-7d86181b8a7a") {
        return "Best power efficiency";
    }
    if (guid == "00000000-0000-0000-0000-000000000000") {
        return "Balanced (no overlay)";
    }
    if (guid == "3af9b8d9-7c97-431d-ad78-34a8bfea439f") {
        return "Better performance";
    }
    if (guid == "ded574b5-45a0-4f42-8737-46345c09c238") {
        return "Best performance";
    }
    return "unknown";
}

// Settings > Power mode applies an overlay on top of the active plan.
std::string OverlayText() {
    using OverlayFn = DWORD(WINAPI*)(GUID*);
    HMODULE powrprof = GetModuleHandleW(L"powrprof.dll");
    if (powrprof == nullptr) {
        powrprof = LoadLibraryW(L"powrprof.dll");
    }
    if (powrprof == nullptr) {
        return "- Power mode overlay: unavailable\n";
    }
    std::string out;
    for (const char* name : {"PowerGetEffectiveOverlayScheme", "PowerGetActualOverlayScheme",
             "PowerGetUserConfiguredACPowerMode", "PowerGetUserConfiguredDCPowerMode"}) {
        const auto function = reinterpret_cast<OverlayFn>(reinterpret_cast<void*>(GetProcAddress(powrprof, name)));
        GUID overlay{};
        if (function != nullptr && function(&overlay) == ERROR_SUCCESS) {
            const std::string text = GuidText(overlay);
            out += "- " + std::string(name) + ": " + text + " (" + OverlayLabel(text) + ")\n";
        } else {
            out += "- " + std::string(name) + ": unavailable\n";
        }
    }
    return out;
}

std::vector<GUID> PowerSchemes() {
    std::vector<GUID> schemes;
    for (ULONG index = 0;; ++index) {
        GUID scheme{};
        DWORD size = sizeof(scheme);
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, index, reinterpret_cast<UCHAR*>(&scheme),
                &size) != ERROR_SUCCESS) {
            break;
        }
        schemes.push_back(scheme);
    }
    return schemes;
}

std::wstring SafeFileName(const std::string& name) {
    std::wstring out;
    for (const wchar_t c : Wide(name)) {
        out.push_back(iswalnum(c) != 0 || c == L'-' ? c : L'_');
    }
    return out.empty() ? std::wstring(L"plan") : out;
}

// Another Dock.exe (the running dock, whose engine manages the Automatic plan).
DWORD OtherDockPid(ProcessSampler& processes) {
    const std::wstring self = Lower(FileNameOf(ModulePath()));
    std::unordered_map<DWORD, ProcessSnapshot> snapshot;
    if (self.empty() || !processes.Snapshot(snapshot)) {
        return 0;
    }
    for (const auto& [pid, entry] : snapshot) {
        if (pid != GetCurrentProcessId() && Lower(entry.name) == self) {
            return pid;
        }
    }
    return 0;
}

// One markdown row per key setting for the given plans: "ac / dc".
std::string SettingsTable(const std::vector<std::pair<std::string, GUID>>& plans) {
    std::string md = "| Setting |";
    std::string rule = "|---|";
    for (const auto& plan : plans) {
        md += " " + plan.first + " |";
        rule += "---|";
    }
    md += "\n" + rule + "\n";
    for (const SettingInfo& setting : kSettings) {
        md += "| " + std::string(setting.key) + " |";
        for (const auto& plan : plans) {
            DWORD ac = 0;
            DWORD dc = 0;
            const bool acOk = ReadSetting(plan.second, setting, true, ac);
            const bool dcOk = ReadSetting(plan.second, setting, false, dc);
            md += " " + (acOk || dcOk ? ValueText(acOk, ac) + " / " + ValueText(dcOk, dc) : std::string("-")) + " |";
        }
        md += "\n";
    }
    return md;
}

void WritePlans(const std::wstring& dir) {
    CreateDirectoryW((dir + L"\\plans").c_str(), nullptr);
    RunPowercfgTo(L"/list", dir + L"\\plans\\list.txt", 20000);
    const std::vector<GUID> schemes = PowerSchemes();
    std::string csv = "setting";
    for (const GUID& scheme : schemes) {
        const std::string name = PlanName(scheme);
        csv += "," + CsvField(name + " AC") + "," + CsvField(name + " DC");
        RunPowercfgTo(L"/qh " + Wide(GuidText(scheme)), dir + L"\\plans\\" + SafeFileName(name) + L".txt", 30000);
    }
    csv += "\n";
    for (const SettingInfo& setting : kSettings) {
        csv += setting.key;
        for (const GUID& scheme : schemes) {
            DWORD ac = 0;
            DWORD dc = 0;
            const bool acOk = ReadSetting(scheme, setting, true, ac);
            const bool dcOk = ReadSetting(scheme, setting, false, dc);
            csv += "," + ValueText(acOk, ac) + "," + ValueText(dcOk, dc);
        }
        csv += "\n";
    }
    WriteFileText(dir + L"\\plans.csv", csv);
}

std::string SystemText(bool elevated, const std::string& meters, const std::string& missingCounters,
    const std::wstring& listenerNote, DWORD dockPid) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char date[64]{};
    std::snprintf(date, sizeof(date), "%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
        now.wMinute, now.wSecond);
    std::string out = "# Hoverdock power profile: system\n\n- Date: " + std::string(date) + "\n";
#ifdef DOCK_VERSION_STRING
    out += "- Hoverdock: " + std::string(DOCK_VERSION_STRING) + "\n";
#endif
    const wchar_t* const cpuKey = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    const wchar_t* const versionKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    out += "- CPU: " + RegistryText(HKEY_LOCAL_MACHINE, cpuKey, L"ProcessorNameString") + "\n";
    out += CpuidText() + TopologyText() + ProcessorPowerText();
    DWORD ubr = 0;
    DWORD ubrSize = sizeof(ubr);
    const bool haveUbr = RegGetValueW(HKEY_LOCAL_MACHINE, versionKey, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr,
                             &ubrSize) == ERROR_SUCCESS;
    out += "- Windows: " + RegistryText(HKEY_LOCAL_MACHINE, versionKey, L"DisplayVersion") + " build " +
        RegistryText(HKEY_LOCAL_MACHINE, versionKey, L"CurrentBuild") +
        (haveUbr ? "." + std::to_string(ubr) : std::string()) + "\n";
    out += "- Elevated: " + YesNo(elevated) + "\n";
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power) != FALSE) {
        const char* source = power.ACLineStatus == 1 ? "AC" : power.ACLineStatus == 0 ? "battery" : "unknown";
        out += "- Power source: " + std::string(source) + "; battery " +
            std::string((power.BatteryFlag & 128U) != 0 ? "none" : "present") + "\n";
    }
    GUID active{};
    if (ActivePlan(active)) {
        out += "- Active plan: " + PlanName(active) + " (" + GuidText(active) + ")\n";
    }
    out += OverlayText();
    double coarsest = 0.0;
    double finest = 0.0;
    double current = 0.0;
    if (TimerResolution(coarsest, finest, current)) {
        out += "- Timer resolution: current " + Number(current, 4) + " ms (range " + Number(finest, 4) + " to " +
            Number(coarsest, 4) + " ms)\n";
    }
    out += "- Energy meters: " + meters + "\n";
    out += "- Missing processor counters: " + (missingCounters.empty() ? std::string("none") : missingCounters) + "\n";
    out += "- Engine trace: " + (listenerNote.empty() ? std::string("listening") : Utf8(listenerNote)) + "\n";
    out += "- Running dock (engine host): " +
        (dockPid != 0 ? "pid " + std::to_string(dockPid) : std::string("not found")) + "\n";
    return out;
}

// ---- measured windows ---------------------------------------------------------------------

struct Workload {
    const char* name;  // worker scenario; nullptr = no worker (the machine as it is)
    double workMs;     // per burst / frame / job (sets the worker's iterations)
    bool fixed;        // runs a fixed job to completion
};

const Workload kNone{nullptr, 0.0, false};
const Workload kIdle{"idle", 0.0, false};
const Workload kCursor{"cursor", 0.0, false};
const Workload kBursty{"bursty", 20.0, false};
const Workload kVideo{"video", 4.0, false};
const Workload kSustained{"sustained", 6000.0, true};

struct Window {
    std::string phase;
    std::string config;
    std::string scenario;
    int rep = 0;
    bool valid = true;
    std::string note;
    double start = 0.0;
    double end = 0.0;
    std::vector<double> emiJoules;
    std::string metrics;
};

struct Stats {
    int samples = 0;
    double watts = kNaN;
    double util = kNaN;
    double avgMhz = kNaN;
    double busyMhz = kNaN;
    double maxMhz = kNaN;
    double c1 = kNaN;
    double c2 = kNaN;
    double c3 = kNaN;
    double parked = kNaN;
    double contextSwitches = kNaN;
    double interrupts = kNaN;
    double timerMs = kNaN;
    std::string profiles;
};

class Average {
public:
    void Add(double value) {
        if (std::isfinite(value)) {
            m_sum += value;
            ++m_count;
        }
    }
    [[nodiscard]] double Get() const { return m_count == 0 ? kNaN : m_sum / m_count; }

private:
    double m_sum = 0.0;
    int m_count = 0;
};

struct Context {
    std::wstring dir;
    double origin = 0.0;
    EmiMeter* emi = nullptr;
    size_t emiPrimary = 0;
    Timeline* timeline = nullptr;
    DebugListener* listener = nullptr;
    std::wstring workerExe;
    double iterationsPerMs = 0.0;
    std::vector<Window> windows;
};

Stats Measure(const Context& context, const Window& window) {
    Stats stats;
    const std::vector<Sample> samples = context.timeline->Samples();
    Average util;
    Average avgMhz;
    Average busyMhz;
    Average maxMhz;
    Average c1;
    Average c2;
    Average c3;
    Average parked;
    Average switches;
    Average interrupts;
    Average timer;
    Average pdh;
    // A sample covers the second before it.
    for (const Sample& sample : samples) {
        if (sample.at <= window.start + 0.5 || sample.at > window.end + 0.25) {
            continue;
        }
        ++stats.samples;
        util.Add(sample.util);
        avgMhz.Add(sample.avgMhz);
        busyMhz.Add(sample.busyMhz);
        maxMhz.Add(sample.maxMhz);
        c1.Add(sample.c1);
        c2.Add(sample.c2);
        c3.Add(sample.c3);
        parked.Add(sample.parked);
        switches.Add(sample.contextSwitches);
        interrupts.Add(sample.interrupts);
        timer.Add(sample.timerMs);
        pdh.Add(sample.pdhWatts);
    }
    stats.util = util.Get();
    stats.avgMhz = avgMhz.Get();
    stats.busyMhz = busyMhz.Get();
    stats.maxMhz = maxMhz.Get();
    stats.c1 = c1.Get();
    stats.c2 = c2.Get();
    stats.c3 = c3.Get();
    stats.parked = parked.Get();
    stats.contextSwitches = switches.Get();
    stats.interrupts = interrupts.Get();
    stats.timerMs = timer.Get();
    const double seconds = window.end - window.start;
    stats.watts = context.emiPrimary < window.emiJoules.size() && seconds > 0.0
        ? window.emiJoules[context.emiPrimary] / seconds
        : pdh.Get();
    if (context.listener != nullptr) {
        std::map<std::string, int> counts;
        for (const TraceLine& line : context.listener->Lines()) {
            if (line.at >= window.start && line.at <= window.end) {
                const std::string profile = TickProfile(line.text);
                if (!profile.empty()) {
                    ++counts[profile];
                }
            }
        }
        for (const auto& [profile, count] : counts) {
            stats.profiles += (stats.profiles.empty() ? "" : " ") + profile + ":" + std::to_string(count);
        }
    }
    return stats;
}

std::string RunsHeader(const Context& context) {
    std::string header = "phase,config,scenario,rep,valid,note,seconds,watts,util_pct,avg_mhz,busy_mhz,max_mhz,c1_pct,"
                         "c2_pct,c3_pct,parked_cores,context_switches_s,interrupts_s,timer_ms,engine_ticks";
    if (context.emi != nullptr) {
        for (const std::wstring& name : context.emi->Names()) {
            header += "," + CsvField("emi_" + Utf8(name) + "_w");
        }
    }
    return header + ",metrics\n";
}

void RecordWindow(Context& context, Window window) {
    const Stats stats = Measure(context, window);
    const double seconds = window.end - window.start;
    std::string row = CsvField(window.phase) + "," + CsvField(window.config) + "," + CsvField(window.scenario) + "," +
        std::to_string(window.rep) + "," + (window.valid ? "1" : "0") + "," + CsvField(window.note) + "," +
        Number(seconds, 2) + "," + Number(stats.watts, 3) + "," + Number(stats.util, 2) + "," +
        Number(stats.avgMhz, 0) + "," + Number(stats.busyMhz, 0) + "," + Number(stats.maxMhz, 0) + "," +
        Number(stats.c1, 2) + "," + Number(stats.c2, 2) + "," + Number(stats.c3, 2) + "," + Number(stats.parked, 1) +
        "," + Number(stats.contextSwitches, 0) + "," + Number(stats.interrupts, 0) + "," + Number(stats.timerMs, 4) +
        "," + CsvField(stats.profiles);
    if (context.emi != nullptr) {
        for (size_t index = 0; index < context.emi->Names().size(); ++index) {
            row += "," + (index < window.emiJoules.size() && seconds > 0.0
                                 ? Number(window.emiJoules[index] / seconds, 3)
                                 : std::string());
        }
    }
    row += "," + CsvField(window.metrics) + "\n";
    WriteFileText(context.dir + L"\\runs.csv", row, true);
    Say(L"    " + Wide(window.config) + L" \u00B7 " + Wide(window.scenario) + L": " +
        (window.valid ? Wide(Number(stats.watts, 2)) + L" W, busy clock " + Wide(Number(stats.busyMhz, 0)) +
                L" MHz" + (window.metrics.empty() ? L"" : L", " + Wide(window.metrics))
                      : L"rejected (" + Wide(window.note) + L")"));
    context.windows.push_back(std::move(window));
}

// One measured window: settle, start the workload, measure, stop it.
void RunWindow(Context& context, const std::string& phase, const std::string& config, const Workload& workload,
    int rep, const GUID* expectPlan, double settle, double seconds) {
    Window window;
    window.phase = phase;
    window.config = config;
    window.scenario = workload.name != nullptr ? workload.name : "as-is";
    window.rep = rep;
    const auto finish = [&context, &window](std::string note) {
        if (!note.empty()) {
            window.valid = false;
            window.note = std::move(note);
        }
        RecordWindow(context, std::move(window));
    };
    context.timeline->SetLabel(phase, config + " settle");
    if (!SleepFor(settle)) {
        finish("aborted");
        return;
    }
    WorkerProcess worker;
    if (workload.name != nullptr) {
        const auto iterations = static_cast<uint64_t>(std::max(0.0, context.iterationsPerMs * workload.workMs));
        const std::wstring arguments =
            L"--energy-bench-worker " + Wide(workload.name) + L" " + std::to_wstring(iterations);
        std::string error;
        if (!worker.Start(context.workerExe, arguments, error)) {
            finish("worker failed: " + error);
            return;
        }
        if (!workload.fixed && !SleepFor(2.0)) {
            finish("aborted");
            return;
        }
    }
    context.timeline->SetLabel(phase, config + " " + window.scenario);
    std::vector<uint64_t> before;
    const bool haveEnergy = context.emi != nullptr && context.emi->Read(before);
    window.start = Now();
    worker.Mark();
    const bool completed =
        workload.fixed ? worker.WaitExit(workload.workMs / 1000.0 * 20.0 + 60.0) : SleepFor(seconds);
    window.end = Now();
    std::vector<uint64_t> after;
    if (haveEnergy && context.emi->Read(after) && after.size() == before.size()) {
        for (size_t index = 0; index < after.size(); ++index) {
            window.emiJoules.push_back(static_cast<double>(after[index] - before[index]) * kJoulesPerPicowattHour);
        }
    }
    context.timeline->SetLabel(phase, config + " done");
    if (workload.name != nullptr) {
        if (!workload.fixed) {
            worker.RequestStop();
            worker.WaitExit(10.0);
        }
        window.metrics = worker.Output();
        worker.Close();
    }
    if (!completed) {
        finish(g_abort.load() ? "aborted" : "timed out");
        return;
    }
    GUID active{};
    if (expectPlan != nullptr && (!ActivePlan(active) || !IsEqualGUID(active, *expectPlan))) {
        finish("power plan changed during the run");
        return;
    }
    finish({});
}

// ---- rest-profile sweep -------------------------------------------------------------------

// Resolved to the Power saver plan's value (the engine's rest EPP before this release).
constexpr DWORD kPowerSaverValue = 0xFFFFFFFFUL;

struct SweepConfig {
    std::string name;
    std::string description;
    std::vector<std::pair<std::string, DWORD>> overrides;
    bool quick;
};

std::vector<SweepConfig> SweepConfigs() {
    using Overrides = std::vector<std::pair<std::string, DWORD>>;
    const auto epp = [](DWORD value) { return Overrides{{"epp", value}, {"epp_class1", value}}; };
    const auto join = [](Overrides first, const Overrides& second) {
        first.insert(first.end(), second.begin(), second.end());
        return first;
    };
    const Overrides window0{{"activity_window", 0}};
    return {
        {"rest", "the engine's rest profile as it is now", {}, true},
        {"epp33", "energy preference 33 (Balanced's usual)", epp(33), true},
        {"epp50", "energy preference 50", epp(50), true},
        {"epp70", "energy preference 70", epp(70), true},
        {"epp85", "energy preference 85", epp(85), false},
        {"boost-efficient", "boost mode Efficient Enabled (3)", {{"boost_mode", 3}}, true},
        {"boost-efficient-aggressive", "boost mode Efficient Aggressive (4)", {{"boost_mode", 4}}, false},
        {"boost-enabled", "boost mode Enabled (1)", {{"boost_mode", 1}}, false},
        {"rest-repeat-1", "the rest profile again (drift check)", {}, false},
        {"window-default", "activity window 0 (hardware default)", window0, true},
        {"window-5ms", "activity window 5 ms", {{"activity_window", 5000}}, false},
        {"window-20ms", "activity window 20 ms", {{"activity_window", 20000}}, false},
        {"no-parking", "no core parking (100% unparked)",
            {{"parking_min_cores", 100}, {"parking_min_cores_class1", 100}}, false},
        {"pcie-off", "PCIe link power management off", {{"pcie_aspm", 0}}, false},
        {"combo-epp50-effaggr-w0", "EPP 50 + Efficient Aggressive + default window",
            join(join(epp(50), {{"boost_mode", 4}}), window0), true},
        {"combo-balanced-like", "EPP 33 + Aggressive + default window",
            join(join(epp(33), {{"boost_mode", 2}}), window0), false},
        {"previous-rest", "the previous release: Power saver EPP + Efficient Enabled + default window",
            join(join(epp(kPowerSaverValue), {{"boost_mode", 3}}), window0), true},
        {"rest-repeat-2", "the rest profile again (drift check)", {}, true},
    };
}

bool CreateScratchPlan(const GUID& source) {
    if (PlanExists(kScratchPlan)) {
        GUID active{};
        if (ActivePlan(active) && IsEqualGUID(active, kScratchPlan)) {
            SetPlan(kBalancedPlan);
        }
        PowerDeleteScheme(nullptr, &kScratchPlan);
    }
    RunPowercfgTo(L"/duplicatescheme " + Wide(GuidText(source)) + L" " + Wide(GuidText(kScratchPlan)), L"", 20000);
    if (!PlanExists(kScratchPlan)) {
        return false;
    }
    std::wstring name = kScratchPlanName;
    PowerWriteFriendlyName(nullptr, &kScratchPlan, nullptr, nullptr, reinterpret_cast<UCHAR*>(name.data()),
        static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)));
    return true;
}

struct BaselineValue {
    const SettingInfo* setting;
    bool acOk;
    DWORD ac;
    bool dcOk;
    DWORD dc;
};

// Writes the scratch plan's baseline, then a configuration's overrides, and
// re-activates it; returns the values read back ("key=ac/dc ...").
std::string ApplySweepConfig(const SweepConfig& config, const std::vector<BaselineValue>& baseline,
    DWORD powerSaverEpp) {
    for (const BaselineValue& value : baseline) {
        if (value.acOk) {
            PowerWriteACValueIndex(nullptr, &kScratchPlan, value.setting->subgroup, &value.setting->guid, value.ac);
        }
        if (value.dcOk) {
            PowerWriteDCValueIndex(nullptr, &kScratchPlan, value.setting->subgroup, &value.setting->guid, value.dc);
        }
    }
    std::string applied;
    for (const auto& [key, wanted] : config.overrides) {
        const SettingInfo* setting = FindSetting(key);
        if (setting == nullptr) {
            continue;
        }
        const DWORD value = wanted == kPowerSaverValue ? powerSaverEpp : wanted;
        const bool acOk =
            PowerWriteACValueIndex(nullptr, &kScratchPlan, setting->subgroup, &setting->guid, value) == ERROR_SUCCESS;
        const bool dcOk =
            PowerWriteDCValueIndex(nullptr, &kScratchPlan, setting->subgroup, &setting->guid, value) == ERROR_SUCCESS;
        if (!acOk || !dcOk) {
            applied += key + "=write-failed ";
        }
    }
    SetPlan(kScratchPlan);
    for (const BaselineValue& value : baseline) {
        DWORD ac = 0;
        DWORD dc = 0;
        const bool acOk = ReadSetting(kScratchPlan, *value.setting, true, ac);
        const bool dcOk = ReadSetting(kScratchPlan, *value.setting, false, dc);
        applied += std::string(value.setting->key) + "=" + ValueText(acOk, ac) + "/" + ValueText(dcOk, dc) + " ";
    }
    return applied;
}

// ---- summary ------------------------------------------------------------------------------

std::string Cell(double value, int decimals) {
    return std::isfinite(value) ? Number(value, decimals) : std::string("-");
}

const char* MetricKey(const std::string& scenario) {
    return scenario == "bursty" ? "p95_ms"
        : scenario == "video"   ? "missed_pct"
        : scenario == "sustained" ? "seconds"
        : scenario == "cursor"    ? "moves"
                                  : "";
}

struct Cellset {
    Average watts;
    Average busyMhz;
    Average maxMhz;
    Average c3;
    Average parked;
    Average switches;
    Average metric;
    std::string profiles;
};

// Groups valid windows of a phase by (config, scenario) in first-seen order.
std::vector<std::pair<std::pair<std::string, std::string>, Cellset>> Group(const Context& context,
    const std::string& phase) {
    std::vector<std::pair<std::pair<std::string, std::string>, Cellset>> groups;
    for (const Window& window : context.windows) {
        if (window.phase != phase || !window.valid) {
            continue;
        }
        const std::pair<std::string, std::string> key{window.config, window.scenario};
        auto found =
            std::find_if(groups.begin(), groups.end(), [&key](const auto& entry) { return entry.first == key; });
        if (found == groups.end()) {
            groups.push_back({key, Cellset{}});
            found = std::prev(groups.end());
        }
        const Stats stats = Measure(context, window);
        Cellset& cells = found->second;
        cells.watts.Add(stats.watts);
        cells.busyMhz.Add(stats.busyMhz);
        cells.maxMhz.Add(stats.maxMhz);
        cells.c3.Add(stats.c3);
        cells.parked.Add(stats.parked);
        cells.switches.Add(stats.contextSwitches);
        const char* metric = MetricKey(window.scenario);
        if (metric[0] != '\0') {
            cells.metric.Add(MetricValue(window.metrics, metric));
        }
        if (cells.profiles.empty()) {
            cells.profiles = stats.profiles;
        }
    }
    return groups;
}

const Cellset* FindCells(const std::vector<std::pair<std::pair<std::string, std::string>, Cellset>>& groups,
    const std::string& config, const std::string& scenario) {
    for (const auto& entry : groups) {
        if (entry.first.first == config && entry.first.second == scenario) {
            return &entry.second;
        }
    }
    return nullptr;
}

std::string TopProcesses(const std::map<std::wstring, ProcessTotals>& totals, double seconds) {
    if (totals.empty() || seconds <= 0.0) {
        return "(no process samples)\n";
    }
    std::vector<std::pair<std::wstring, ProcessTotals>> sorted(totals.begin(), totals.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second.switches > b.second.switches; });
    std::string md =
        "| Process | Wake-ups/s (context switches) | CPU (% of one core) | Gcycles/s |\n|---|---|---|---|\n";
    for (size_t index = 0; index < sorted.size() && index < 15; ++index) {
        const ProcessTotals& entry = sorted[index].second;
        md += "| " + Utf8(sorted[index].first) + " | " + Number(entry.switches / seconds, 0) + " | " +
            Number(entry.cpuSeconds / seconds * 100.0, 2) + " | " + Number(entry.cycles / seconds / 1e9, 3) + " |\n";
    }
    return md;
}

std::string Summary(const Context& context, const std::string& systemText, const std::string& settingsTable,
    const std::string& sweepBaseline, const std::vector<std::string>& notes) {
    std::string md = "# Hoverdock power profile\n\n";
    md += "Share this whole folder (zip it): every CSV is needed, not just this summary.\n\n";
    if (!notes.empty()) {
        md += "## Notes\n\n";
        for (const std::string& note : notes) {
            md += "- " + note + "\n";
        }
        md += "\n";
    }
    md += systemText + "\n## Key processor settings (AC / DC)\n\n" + settingsTable + "\n";
    md += "Clocks: busy = average clock of the cores that ran, weighted by how busy each was; max = the fastest "
          "running core. Watts: the processor package (RAPL) unless noted. C3 = time in the deepest idle state "
          "Windows reports.\n\n";

    const auto processes = context.timeline->ProcessesByPhase();
    const auto seconds = context.timeline->PhaseSeconds();
    for (const char* phase : {"asis", "live"}) {
        const auto groups = Group(context, phase);
        if (groups.empty()) {
            continue;
        }
        md += std::string("## ") + (std::string(phase) == "asis" ? "The machine as it was (hands off)" : "Live use") +
            "\n\n| Plan | Watts | Busy MHz | Max MHz | C3 % | Parked cores | Wake-ups/s | Engine ticks |\n"
            "|---|---|---|---|---|---|---|---|\n";
        for (const auto& [key, cells] : groups) {
            md += "| " + key.first + " | " + Cell(cells.watts.Get(), 2) + " | " + Cell(cells.busyMhz.Get(), 0) +
                " | " + Cell(cells.maxMhz.Get(), 0) + " | " + Cell(cells.c3.Get(), 1) + " | " +
                Cell(cells.parked.Get(), 1) + " | " + Cell(cells.switches.Get(), 0) + " | " + cells.profiles + " |\n";
        }
        const auto found = processes.find(phase);
        const auto length = seconds.find(phase);
        md += "\nBusiest processes by wake-ups:\n\n" +
            TopProcesses(found != processes.end() ? found->second : std::map<std::wstring, ProcessTotals>{},
                length != seconds.end() ? length->second : 0.0) +
            "\n";
    }

    const auto plans = Group(context, "plans");
    if (!plans.empty()) {
        md += "## Plans under the benchmark workloads\n\n| Plan | Workload | Watts | Busy MHz | Max MHz | C3 % | "
              "Parked | Metric | Engine ticks |\n|---|---|---|---|---|---|---|---|---|\n";
        for (const auto& [key, cells] : plans) {
            const std::string metric = MetricKey(key.second);
            md += "| " + key.first + " | " + key.second + " | " + Cell(cells.watts.Get(), 2) + " | " +
                Cell(cells.busyMhz.Get(), 0) + " | " + Cell(cells.maxMhz.Get(), 0) + " | " + Cell(cells.c3.Get(), 1) +
                " | " + Cell(cells.parked.Get(), 1) + " | " +
                (metric.empty() ? std::string("-") : metric + " " + Cell(cells.metric.Get(), 2)) + " | " +
                cells.profiles + " |\n";
        }
        md += "\n";
    }

    const auto sweep = Group(context, "sweep");
    if (!sweep.empty()) {
        md += "## Rest-profile sweep (temporary plan, engine not involved)\n\n" + sweepBaseline +
            "\n| Config | Idle W | Idle busy MHz | Idle C3 % | Bursty W | Bursty p95 ms | Video W | Video missed % | "
            "Video busy MHz | What changed |\n|---|---|---|---|---|---|---|---|---|---|\n";
        for (const SweepConfig& config : SweepConfigs()) {
            const Cellset* idle = FindCells(sweep, config.name, "idle");
            const Cellset* bursty = FindCells(sweep, config.name, "bursty");
            const Cellset* video = FindCells(sweep, config.name, "video");
            if (idle == nullptr && bursty == nullptr && video == nullptr) {
                continue;
            }
            const auto cell = [](const Cellset* cells, double (*pick)(const Cellset&), int decimals) {
                return cells != nullptr ? Cell(pick(*cells), decimals) : std::string("-");
            };
            md += "| " + config.name + " | " + cell(idle, [](const Cellset& c) { return c.watts.Get(); }, 2) + " | " +
                cell(idle, [](const Cellset& c) { return c.busyMhz.Get(); }, 0) + " | " +
                cell(idle, [](const Cellset& c) { return c.c3.Get(); }, 1) + " | " +
                cell(bursty, [](const Cellset& c) { return c.watts.Get(); }, 2) + " | " +
                cell(bursty, [](const Cellset& c) { return c.metric.Get(); }, 2) + " | " +
                cell(video, [](const Cellset& c) { return c.watts.Get(); }, 2) + " | " +
                cell(video, [](const Cellset& c) { return c.metric.Get(); }, 2) + " | " +
                cell(video, [](const Cellset& c) { return c.busyMhz.Get(); }, 0) + " | " + config.description + " |\n";
        }
        md += "\nExact values applied per configuration: sweep-configs.csv.\n";
    }
    return md;
}

// ---- driver -------------------------------------------------------------------------------

struct ProfileOptions {
    bool quick = false;
    double liveMinutes = 0.0;
    bool skipPlans = false;
    bool skipSweep = false;
    bool energyReport = true;
};

bool WritableDirectory(const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring probe = dir + L"\\profiler.log";
    return WriteFileText(probe, "");
}

std::wstring OutputDirectory() {
    const std::wstring name = L"Hoverdock-profile-" + LocalStamp();
    const std::wstring exe = ModulePath();
    const size_t slash = exe.find_last_of(L"\\/");
    if (slash != std::wstring::npos && WritableDirectory(exe.substr(0, slash + 1) + name)) {
        return exe.substr(0, slash + 1) + name;
    }
    wchar_t local[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    std::wstring dir = std::wstring(local) + L"\\Hoverdock";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\Profiles";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\" + name;
    return WritableDirectory(dir) ? dir : std::wstring();
}

// Restores the user's plan, removes the temporary plan and worker, and lets
// the display sleep again, however the run ends.
struct Restore {
    bool haveOriginal = false;
    GUID original{};
    bool scratch = false;
    std::wstring worker;

    void Undo() {
        if (haveOriginal) {
            SetPlan(original);
        }
        if (scratch) {
            PowerDeleteScheme(nullptr, &kScratchPlan);
            scratch = false;
        }
        if (!worker.empty()) {
            DeleteFileW(worker.c_str());
            worker.clear();
        }
        SetThreadExecutionState(ES_CONTINUOUS);
    }
    Restore() = default;
    Restore(const Restore&) = delete;
    Restore& operator=(const Restore&) = delete;
    ~Restore() { Undo(); }
};

int Profile(const ProfileOptions& options) {
    Context context;
    context.origin = Now();
    context.dir = OutputDirectory();
    if (context.dir.empty()) {
        Say(L"Could not create an output folder next to Dock.exe or under %LOCALAPPDATA%\\Hoverdock.");
        return 1;
    }
    g_progressLog = context.dir + L"\\profiler.log";
    Say(L"Hoverdock power profiler");
    Say(L"Results folder: " + context.dir);
    std::vector<std::string> notes;
    const bool elevated = IsElevated();
    if (!elevated) {
        notes.push_back("Not elevated: the processor's energy counters may be unreadable and powercfg /energy is "
                        "skipped. Run from an elevated terminal for complete data.");
    }

    EmiMeter emi;
    std::string emiError;
    const bool emiOk = emi.Open(emiError);
    PdhPowerMeter pdh;
    const bool pdhOk = pdh.Open();
    BatteryReading battery;
    const bool batteryOk = ReadBattery(battery);
    std::string meters;
    if (emiOk) {
        context.emi = &emi;
        int best = -1;
        meters = "EMI";
        for (size_t index = 0; index < emi.Names().size(); ++index) {
            meters += " " + Utf8(emi.Names()[index]);
            const int score = ChannelScore(emi.Names()[index]);
            if (score > best) {
                best = score;
                context.emiPrimary = index;
            }
        }
        meters += " (headline: " + Utf8(emi.Names()[context.emiPrimary]) + ")";
    } else {
        meters = "EMI unavailable (" + emiError + ")";
    }
    meters += pdhOk ? "; PDH Energy Meter " + Utf8(pdh.Instance()) : "; PDH Energy Meter unavailable";
    if (batteryOk) {
        meters += battery.onAc ? "; battery present, on AC" : "; on battery";
    }
    if (!emiOk && !pdhOk) {
        notes.push_back("No energy meter is readable: power columns are empty (clocks and C-states still count).");
    }
    CpuCounters cpu;
    std::string missing;
    if (!cpu.Open(missing)) {
        Say(L"Windows' processor performance counters are unavailable, so there is nothing to profile.");
        return 1;
    }
    ProcessSampler processes;
    if (!processes.Open()) {
        notes.push_back("Process sampling is unavailable: processes.csv is empty.");
    }

    // Engine diagnostics and its trace.
    OpenSecurity security;
    HANDLE diagEvent = CreateEventW(security.Get(), TRUE, TRUE, kProfilerEventName);
    if (diagEvent == nullptr) {
        diagEvent = CreateEventW(nullptr, TRUE, TRUE, kProfilerEventName);
    }
    DebugListener listener;
    std::wstring listenerNote;
    if (listener.Start(context.dir + L"\\engine.log", context.origin, listenerNote)) {
        context.listener = &listener;
    }
    if (!listenerNote.empty()) {
        notes.push_back(Utf8(listenerNote));
    }
    const DWORD dockPid = OtherDockPid(processes);
    if (dockPid == 0) {
        notes.push_back("The dock was not running, so nothing managed the Automatic plan: its runs were skipped and "
                        "engine.log is empty. Start Hoverdock, choose Automatic, and run the profiler again.");
    }

    Say(L"Collecting system and power plan information\u2026");
    const std::string systemText = SystemText(elevated, meters, missing, listenerNote, dockPid);
    WriteFileText(context.dir + L"\\system.txt", systemText);
    WritePlans(context.dir);
    std::vector<std::pair<std::string, GUID>> keyPlans;
    for (const auto& plan : std::vector<std::pair<std::string, GUID>>{
             {"Balanced", kBalancedPlan}, {"Power saver", kPowerSaverPlan}, {"Hoverdock Automatic", kAutoPlan}}) {
        if (PlanExists(plan.second)) {
            keyPlans.push_back(plan);
        }
    }
    const std::string settingsTable = SettingsTable(keyPlans);
    std::string sweepBaseline;

    Timeline timeline;
    timeline.Start(context.dir, context.origin, context.emi, pdhOk ? &pdh : nullptr, &cpu, &processes, batteryOk);
    context.timeline = &timeline;
    WriteFileText(context.dir + L"\\runs.csv", RunsHeader(context));

    Restore restore;
    restore.haveOriginal = ActivePlan(restore.original);

    if (options.liveMinutes > 0.0) {
        Say(L"Recording for " + Wide(Number(options.liveMinutes, 0)) +
            L" min. Use the PC as you normally do (Ctrl+C stops early and keeps what was recorded).");
        Window window;
        window.phase = "live";
        window.config = restore.haveOriginal ? PlanName(restore.original) : "current";
        window.scenario = "live";
        window.rep = 1;
        timeline.SetLabel("live", "live");
        window.start = Now();
        std::vector<uint64_t> before;
        const bool haveEnergy = context.emi != nullptr && context.emi->Read(before);
        const double end = window.start + options.liveMinutes * 60.0;
        while (!g_abort.load() && Now() < end) {
            SleepFor(std::min(60.0, end - Now()));
            Say(L"  " + Clock(std::max(0.0, end - Now())) + L" left");
        }
        window.end = Now();
        std::vector<uint64_t> after;
        if (haveEnergy && context.emi->Read(after) && after.size() == before.size()) {
            for (size_t index = 0; index < after.size(); ++index) {
                window.emiJoules.push_back(static_cast<double>(after[index] - before[index]) * kJoulesPerPicowattHour);
            }
        }
        RecordWindow(context, std::move(window));
    } else {
        const double asisSeconds = options.quick ? 60.0 : 120.0;
        const double measure = options.quick ? 20.0 : 30.0;
        const double sweepMeasure = options.quick ? 15.0 : 25.0;
        const int reps = options.quick ? 1 : 2;
        std::vector<std::pair<std::string, GUID>> conditions;
        if (PlanExists(kBalancedPlan)) {
            conditions.push_back({"Balanced", kBalancedPlan});
        }
        if (PlanExists(kPowerSaverPlan)) {
            conditions.push_back({"Power saver", kPowerSaverPlan});
        }
        if (PlanExists(kAutoPlan) && dockPid != 0) {
            conditions.push_back({"Automatic", kAutoPlan});
        }
        std::vector<std::pair<Workload, int>> workloads = {{kIdle, reps}, {kBursty, reps}, {kVideo, reps}};
        if (!options.quick) {
            workloads.push_back({kCursor, 1});
            workloads.push_back({kSustained, 1});
        }
        std::vector<SweepConfig> configs;
        for (SweepConfig& config : SweepConfigs()) {
            if (!options.quick || config.quick) {
                configs.push_back(std::move(config));
            }
        }
        double estimate = asisSeconds + 15.0;
        if (!options.skipPlans) {
            for (const auto& entry : workloads) {
                estimate += static_cast<double>(conditions.size()) * entry.second *
                    (entry.first.fixed ? 40.0 : 12.0 + measure);
            }
        }
        if (!options.skipSweep) {
            estimate += 10.0 + static_cast<double>(configs.size()) * 3.0 * (9.0 + sweepMeasure);
        }
        if (elevated && options.energyReport) {
            estimate += 75.0;
        }
        Say(L"");
        Say(L"About " + Wide(Number(estimate / 60.0, 0)) +
            L" minutes, hands off the keyboard and mouse throughout: input changes what the engine does.");
        Say(L"Plug in a laptop's charger, close what you don't need, and leave the window open.");
        Say(L"Ctrl+C stops at any point; your power plan is put back either way.");
        for (int second = 10; second > 0 && !g_abort.load(); --second) {
            Say(L"  starting in " + std::to_wstring(second) + L"\u2026");
            SleepFor(1.0);
        }
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);

        if (!g_abort.load()) {
            Say(L"Phase 1: the machine as it is (" + Wide(Number(asisSeconds, 0)) + L" s)");
            RunWindow(context, "asis", restore.haveOriginal ? PlanName(restore.original) : "current", kNone, 1,
                nullptr, 3.0, asisSeconds);
        }

        // Workloads run from a renamed copy of Dock.exe, like the energy benchmark's.
        bool workerReady = false;
        if (!g_abort.load() && (!options.skipPlans || !options.skipSweep)) {
            SetPlan(kBalancedPlan);
            SleepFor(3.0);
            timeline.SetLabel("calibrate", "calibrate");
            context.iterationsPerMs = CalibrateIterationsPerMs();
            wchar_t temp[MAX_PATH]{};
            const DWORD tempLength = GetTempPathW(MAX_PATH, temp);
            const std::wstring workerDir =
                (tempLength == 0 || tempLength >= MAX_PATH ? std::wstring() : std::wstring(temp)) +
                L"Hoverdock-PowerProfile";
            CreateDirectoryW(workerDir.c_str(), nullptr);
            context.workerExe = workerDir + L"\\HoverdockProfileWorker.exe";
            workerReady = CopyFileW(ModulePath().c_str(), context.workerExe.c_str(), FALSE) != FALSE;
            if (workerReady) {
                restore.worker = context.workerExe;
            } else {
                notes.push_back(
                    "Could not copy the worker program to " + Utf8(workerDir) + ": workload phases skipped.");
            }
        }

        if (workerReady && !options.skipPlans && !g_abort.load()) {
            Say(L"Phase 2: Balanced, Power saver and Automatic under the benchmark workloads");
            bool previousHot = false;
            for (int rep = 1; rep <= reps && !g_abort.load(); ++rep) {
                for (size_t w = 0; w < workloads.size() && !g_abort.load(); ++w) {
                    if (rep > workloads[w].second) {
                        continue;
                    }
                    const bool reverse = (static_cast<size_t>(rep) + w) % 2 == 0;
                    for (size_t k = 0; k < conditions.size() && !g_abort.load(); ++k) {
                        const auto& condition = conditions[reverse ? conditions.size() - 1 - k : k];
                        SetPlan(condition.second);
                        RunWindow(context, "plans", condition.first, workloads[w].first, rep, &condition.second,
                            previousHot ? 20.0 : 8.0, measure);
                        previousHot = workloads[w].first.fixed;
                    }
                }
            }
        }

        if (workerReady && !options.skipSweep && !g_abort.load()) {
            Say(L"Phase 3: rest-profile settings sweep on a temporary copy of the Automatic plan");
            // On another plan the engine goes dormant and leaves its plan at rest values.
            SetPlan(kBalancedPlan);
            SleepFor(4.0);
            const GUID source = PlanExists(kAutoPlan) ? kAutoPlan : kBalancedPlan;
            if (!PlanExists(kAutoPlan)) {
                notes.push_back("The Automatic plan does not exist: the sweep started from Balanced instead.");
            }
            if (CreateScratchPlan(source)) {
                restore.scratch = true;
                std::vector<BaselineValue> baseline;
                std::vector<std::string> touched;
                for (const SweepConfig& config : configs) {
                    for (const auto& entry : config.overrides) {
                        if (std::find(touched.begin(), touched.end(), entry.first) == touched.end()) {
                            touched.push_back(entry.first);
                        }
                    }
                }
                for (const std::string& key : touched) {
                    const SettingInfo* setting = FindSetting(key);
                    if (setting != nullptr) {
                        BaselineValue value{setting, false, 0, false, 0};
                        value.acOk = ReadSetting(kScratchPlan, *setting, true, value.ac);
                        value.dcOk = ReadSetting(kScratchPlan, *setting, false, value.dc);
                        baseline.push_back(value);
                    }
                }
                DWORD powerSaverEpp = 80;
                const SettingInfo* epp = FindSetting("epp");
                if (epp != nullptr && !ReadSetting(kPowerSaverPlan, *epp, true, powerSaverEpp)) {
                    powerSaverEpp = 80;
                }
                sweepBaseline = SettingsTable({{"Sweep baseline (engine rest values)", kScratchPlan}});
                WriteFileText(context.dir + L"\\sweep-configs.csv", "config,description,applied\n");
                for (const SweepConfig& config : configs) {
                    if (g_abort.load()) {
                        break;
                    }
                    const std::string applied = ApplySweepConfig(config, baseline, powerSaverEpp);
                    WriteFileText(context.dir + L"\\sweep-configs.csv",
                        CsvField(config.name) + "," + CsvField(config.description) + "," + CsvField(applied) + "\n",
                        true);
                    bool first = true;
                    for (const Workload& workload : {kIdle, kBursty, kVideo}) {
                        if (g_abort.load()) {
                            break;
                        }
                        RunWindow(context, "sweep", config.name, workload, 1, &kScratchPlan, first ? 8.0 : 5.0,
                            sweepMeasure);
                        first = false;
                    }
                }
            } else {
                notes.push_back("Could not create the temporary plan: the settings sweep was skipped.");
            }
        }

        restore.Undo();
        if (!g_abort.load() && elevated && options.energyReport) {
            Say(L"Phase 4: powercfg /energy under your own plan (60 s, hands off)");
            timeline.SetLabel("energy-report", "powercfg /energy");
            SleepFor(5.0);
            RunPowercfgTo(L"/energy /duration 60 /output \"" + context.dir + L"\\energy-report.html\"",
                context.dir + L"\\energy-report.log", 180000);
            RunPowercfgTo(L"/requests", context.dir + L"\\requests.txt", 20000);
        }
    }

    restore.Undo();
    timeline.Stop();
    listener.Stop();
    if (diagEvent != nullptr) {
        CloseHandle(diagEvent);
    }
    if (g_abort.load()) {
        notes.push_back("The run was stopped early: tables cover what was measured.");
    }
    WriteFileText(context.dir + L"\\summary.md", Summary(context, systemText, settingsTable, sweepBaseline, notes));
    // Tells Dock Settings where the results are.
    const std::wstring marker = PowerProfileResultMarker();
    if (!marker.empty()) {
        CreateDirectoryW(marker.substr(0, marker.find_last_of(L'\\')).c_str(), nullptr);
        WriteFileText(marker, Utf8(context.dir));
    }
    Say(L"");
    Say(L"Finished. Results: " + context.dir);
    Say(L"Zip that whole folder and share it.");
    ShellExecuteW(nullptr, L"open", context.dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return g_abort.load() ? 2 : 0;
}

ProfileOptions ParseOptions(int argc, LPWSTR* argv) {
    ProfileOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::wstring arg = Lower(argv[index]);
        if (arg == L"--quick") {
            options.quick = true;
        } else if (arg == L"--live" && index + 1 < argc) {
            options.liveMinutes = std::clamp(std::wcstod(argv[++index], nullptr), 1.0, 600.0);
        } else if (arg == L"--skip-plans") {
            options.skipPlans = true;
        } else if (arg == L"--skip-sweep") {
            options.skipSweep = true;
        } else if (arg == L"--no-energy-report") {
            options.energyReport = false;
        }
    }
    return options;
}

}  // namespace

int RunPowerProfiler(int argc, LPWSTR* argv) {
    // A GUI-subsystem exe has no console: open one for progress and Ctrl+C.
    AllocConsole();
    g_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, 0, nullptr);
    SetConsoleTitleW(L"Hoverdock power profiler");
    g_cleanupDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(&OnConsoleControl, TRUE);
    // Dock Settings' Stop button (the event exists already when the dock started us).
    HANDLE stopEvent = nullptr;
    {
        OpenSecurity security;
        stopEvent = CreateEventW(security.Get(), TRUE, FALSE, kStopEventName);
        if (stopEvent == nullptr) {
            stopEvent = OpenEventW(SYNCHRONIZE, FALSE, kStopEventName);
        }
    }
    std::thread watcher;
    if (stopEvent != nullptr && g_cleanupDone != nullptr) {
        watcher = std::thread([stopEvent] {
            const HANDLE waits[2] = {stopEvent, g_cleanupDone};
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
                g_closing.store(true);
                g_abort.store(true);
                Say(L"Stop requested from Dock Settings: restoring your power plan and saving what was measured\u2026");
            }
        });
    }
    int code = 1;
    try {
        code = Profile(ParseOptions(argc, argv));
    } catch (...) {
        Say(L"The profiler stopped after an unexpected error; your power plan was restored.");
    }
    if (g_cleanupDone != nullptr) {
        SetEvent(g_cleanupDone);
    }
    if (watcher.joinable()) {
        watcher.join();
    }
    if (stopEvent != nullptr) {
        CloseHandle(stopEvent);
    }
    if (!g_closing.load()) {
        Say(L"Press Enter to close this window.");
        const HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (input != INVALID_HANDLE_VALUE) {
            wchar_t buffer[8]{};
            DWORD read = 0;
            ReadConsoleW(input, buffer, 8, &read, nullptr);
            CloseHandle(input);
        }
    }
    return code;
}

std::wstring PowerProfileResultMarker() {
    wchar_t local[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::wstring(local) + L"\\Hoverdock\\last-power-profile.txt";
}

PowerProfilerLauncher::~PowerProfilerLauncher() {
    // A running profile carries on (and restores the plan) without the dock.
    Close();
}

bool PowerProfilerLauncher::Start(bool& elevated) {
    elevated = false;
    if (IsRunning()) {
        return false;
    }
    Close();
    m_stopping = false;
    const std::wstring marker = PowerProfileResultMarker();
    if (!marker.empty()) {
        DeleteFileW(marker.c_str());
    }
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
    if (m_stopEvent != nullptr) {
        ResetEvent(m_stopEvent);
    }
    const std::wstring exe = ModulePath();
    if (exe.empty()) {
        return false;
    }
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = L"--power-profile";
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info) != FALSE) {
        elevated = true;
    } else {
        // Elevation declined: run anyway, with less data (no RAPL counters or energy report).
        info.lpVerb = nullptr;
        if (ShellExecuteExW(&info) == FALSE) {
            return false;
        }
    }
    m_process = info.hProcess;
    return m_process != nullptr;
}

void PowerProfilerLauncher::RequestStop() noexcept {
    if (m_stopEvent != nullptr && m_process != nullptr) {
        SetEvent(m_stopEvent);
        m_stopping = true;
    }
}

bool PowerProfilerLauncher::IsRunning() const noexcept {
    return m_process != nullptr && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
}

std::wstring PowerProfilerLauncher::LastResult() const {
    const std::wstring marker = PowerProfileResultMarker();
    FILE* file = nullptr;
    if (marker.empty() || _wfopen_s(&file, marker.c_str(), L"rb") != 0 || file == nullptr) {
        return {};
    }
    std::string text;
    char buffer[512];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, read);
    }
    std::fclose(file);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return Wide(text);
}

void PowerProfilerLauncher::Close() noexcept {
    if (m_process != nullptr) {
        CloseHandle(m_process);
        m_process = nullptr;
    }
    if (m_stopEvent != nullptr) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}
