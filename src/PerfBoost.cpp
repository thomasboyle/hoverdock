#include "PerfBoost.h"

#include <Psapi.h>
#include <TlHelp32.h>
#include <WtsApi32.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>

namespace {

constexpr size_t kMaxCandidates = 40;
constexpr size_t kCpuSampleCap = 60;
constexpr DWORD kCpuSampleDelayMs = 400;

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool EqualInsensitive(const std::wstring& left, const std::wstring& right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](wchar_t lhs, wchar_t rhs) {
            return std::towlower(lhs) == std::towlower(rhs);
        });
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring SystemRootDirectory() {
    std::wstring root(32768, L'\0');
    const UINT length = GetWindowsDirectoryW(root.data(), static_cast<UINT>(root.size()));
    if (length == 0 || length >= root.size()) {
        return {};
    }
    root.resize(length);
    return ToLower(root);
}

bool PathUnderDirectory(const std::wstring& lowerPath, const std::wstring& lowerDirectory) {
    if (lowerDirectory.empty() || lowerPath.size() <= lowerDirectory.size()) {
        return false;
    }
    if (lowerPath.compare(0, lowerDirectory.size(), lowerDirectory) != 0) {
        return false;
    }
    const wchar_t next = lowerPath[lowerDirectory.size()];
    return next == L'\\' || next == L'/';
}

// OS-critical, shell-hosting, security, and console processes. Anything here
// is never a boost candidate, regardless of what Jev says.
bool IsCriticalFileName(const std::wstring& lowerName) {
    static const wchar_t* const kCritical[] = {
        L"dock.exe", L"explorer.exe", L"dwm.exe", L"sihost.exe", L"ctfmon.exe",
        L"csrss.exe", L"smss.exe", L"wininit.exe", L"winlogon.exe", L"services.exe",
        L"lsass.exe", L"lsaiso.exe", L"fontdrvhost.exe", L"conhost.exe",
        L"dllhost.exe", L"rundll32.exe", L"taskhostw.exe", L"svchost.exe",
        L"shellexperiencehost.exe", L"startmenuexperiencehost.exe", L"searchhost.exe",
        L"searchapp.exe", L"textinputhost.exe", L"applicationframehost.exe",
        L"audiodg.exe", L"wudfhost.exe", L"smartscreen.exe", L"msmpeng.exe",
        L"nissrv.exe", L"spoolsv.exe", L"registry",
    };
    for (const wchar_t* critical : kCritical) {
        if (lowerName == critical) {
            return true;
        }
    }
    return false;
}

DWORD ForegroundProcessId() {
    const HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground != nullptr) {
        GetWindowThreadProcessId(foreground, &pid);
    }
    return pid;
}

std::wstring ProcessImagePath(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(process, 0, path.data(), &length) == FALSE) {
        return {};
    }
    path.resize(length);
    return path;
}

uint64_t ProcessAgeSeconds(HANDLE process) {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user) == FALSE) {
        return 0;
    }
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER start{};
    start.LowPart = created.dwLowDateTime;
    start.HighPart = created.dwHighDateTime;
    ULARGE_INTEGER current{};
    current.LowPart = now.dwLowDateTime;
    current.HighPart = now.dwHighDateTime;
    if (current.QuadPart <= start.QuadPart) {
        return 0;
    }
    return (current.QuadPart - start.QuadPart) / 10000000ULL;
}

uint64_t ProcessMemoryBytes(HANDLE process) {
    HANDLE memoryProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
        GetProcessId(process));
    if (memoryProcess == nullptr) {
        return 0;
    }
    PROCESS_MEMORY_COUNTERS counters{};
    const BOOL ok = GetProcessMemoryInfo(memoryProcess, &counters, sizeof(counters));
    CloseHandle(memoryProcess);
    return ok == FALSE ? 0 : static_cast<uint64_t>(counters.WorkingSetSize);
}

struct WindowBucket {
    bool hasWindow = false;
    std::wstring visibleTitle;
    std::wstring anyTitle;
};

struct WindowWalk {
    std::unordered_map<DWORD, WindowBucket>* buckets = nullptr;
};

BOOL CALLBACK CollectTopWindows(HWND window, LPARAM data) {
    auto* walk = reinterpret_cast<WindowWalk*>(data);
    if (GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    // Skip the dock's own windows; the dock PID is filtered separately, but
    // this keeps helper hosts out of other processes' buckets.
    wchar_t className[64]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0) {
        return TRUE;
    }
    auto& bucket = (*walk->buckets)[pid];
    bucket.hasWindow = true;
    const int length = GetWindowTextLengthW(window);
    if (length <= 0 || length > 512) {
        return TRUE;
    }
    std::wstring title(static_cast<size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(window, title.data(), length + 1);
    if (copied <= 0) {
        return TRUE;
    }
    title.resize(static_cast<size_t>(copied));
    if (bucket.anyTitle.empty()) {
        bucket.anyTitle = title;
    }
    if (bucket.visibleTitle.empty() && IsWindowVisible(window) != FALSE) {
        bucket.visibleTitle = title;
    }
    return TRUE;
}

uint64_t CpuTime100ns(HANDLE process) {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user) == FALSE) {
        return 0;
    }
    ULARGE_INTEGER kernelTime{};
    kernelTime.LowPart = kernel.dwLowDateTime;
    kernelTime.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER userTime{};
    userTime.LowPart = user.dwLowDateTime;
    userTime.HighPart = user.dwHighDateTime;
    return kernelTime.QuadPart + userTime.QuadPart;
}

}  // namespace

bool PerfBoost::IsProtectedExecutable(const std::wstring& exePath) noexcept {
    try {
        if (exePath.empty()) {
            return true;
        }
        const std::wstring lower = ToLower(exePath);
        if (IsCriticalFileName(FileNameOf(lower))) {
            return true;
        }
        static const std::wstring systemRoot = SystemRootDirectory();
        return PathUnderDirectory(lower, systemRoot);
    } catch (...) {
        return true;
    }
}

std::wstring PerfBoost::FormatMegabytes(uint64_t bytes) {
    const uint64_t megabytes = (bytes + 524288ULL) / 1048576ULL;
    // Quick Settings tiles are ~70px wide (~12 chars at 11px Segoe UI), so a
    // 4-digit MB value ("Freed 5790 MB", 13 chars) ellipsizes to "Freed 5790 ...".
    // Switch to GB at 1000 MB to stay inside the tile: 5790 MB -> "5.7 GB".
    // "%.0f" above 9.95 GB keeps "10 GB" (5 chars) instead of "10.0 GB" (6).
    if (megabytes >= 1000ULL) {
        const double gigabytes = static_cast<double>(bytes) / 1073741824.0;
        wchar_t buffer[32]{};
        if (gigabytes >= 9.95) {
            std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%.0f GB", gigabytes);
        } else {
            std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%.1f GB", gigabytes);
        }
        return buffer;
    }
    return std::to_wstring(megabytes) + L" MB";
}

std::vector<BoostProcess> PerfBoost::EnumerateClosableCandidates() {
    const DWORD selfPid = GetCurrentProcessId();
    DWORD selfSession = 0;
    ProcessIdToSessionId(selfPid, &selfSession);
    const DWORD foregroundPid = ForegroundProcessId();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return {};
    }

    std::vector<BoostProcess> processes;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL more = Process32FirstW(snapshot, &entry); more != FALSE;
         more = Process32NextW(snapshot, &entry)) {
        const DWORD pid = entry.th32ProcessID;
        if (pid == 0 || pid == 4 || pid == selfPid || pid == foregroundPid) {
            continue;
        }
        DWORD session = 0;
        if (ProcessIdToSessionId(pid, &session) == FALSE || session != selfSession) {
            continue;  // Session 0 services and other users are never touched.
        }
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr) {
            continue;
        }
        const std::wstring exePath = ProcessImagePath(process);
        const uint64_t age = ProcessAgeSeconds(process);
        const uint64_t memory = ProcessMemoryBytes(process);
        CloseHandle(process);
        if (IsProtectedExecutable(exePath)) {
            continue;
        }
        BoostProcess candidate;
        candidate.pid = pid;
        candidate.name = FileNameOf(exePath);
        candidate.exePath = exePath;
        candidate.memoryBytes = memory;
        candidate.ageSeconds = age;
        processes.push_back(std::move(candidate));
    }
    CloseHandle(snapshot);

    if (processes.empty()) {
        return {};
    }

    // Attach top-level window state so Jev can tell interactive apps from
    // background work.
    std::unordered_map<DWORD, WindowBucket> buckets;
    WindowWalk walk{&buckets};
    EnumWindows(&CollectTopWindows, reinterpret_cast<LPARAM>(&walk));
    for (BoostProcess& candidate : processes) {
        const auto bucket = buckets.find(candidate.pid);
        if (bucket == buckets.end() || !bucket->second.hasWindow) {
            continue;
        }
        candidate.hasWindow = true;
        candidate.windowTitle = !bucket->second.visibleTitle.empty()
            ? bucket->second.visibleTitle
            : bucket->second.anyTitle;
    }

    // Biggest reclaim first; only the head is worth a Jev round-trip.
    std::ranges::sort(processes, std::greater{}, &BoostProcess::memoryBytes);
    if (processes.size() > kCpuSampleCap) {
        processes.resize(kCpuSampleCap);
    }

    // Short double-sample of kernel+user time so Jev can separate busy apps
    // from idle ones. Runs on the boost worker thread, never the UI thread.
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const DWORD processorCount = std::max(1UL, systemInfo.dwNumberOfProcessors);
    std::unordered_map<DWORD, uint64_t> firstSample;
    for (const BoostProcess& candidate : processes) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, candidate.pid);
        if (process == nullptr) {
            continue;
        }
        firstSample.emplace(candidate.pid, CpuTime100ns(process));
        CloseHandle(process);
    }
    Sleep(kCpuSampleDelayMs);
    for (BoostProcess& candidate : processes) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, candidate.pid);
        if (process == nullptr) {
            continue;
        }
        const uint64_t second = CpuTime100ns(process);
        CloseHandle(process);
        const auto first = firstSample.find(candidate.pid);
        if (first == firstSample.end() || second < first->second) {
            continue;
        }
        const double deltaMs = static_cast<double>(second - first->second) / 10000.0;
        candidate.cpuPercent = deltaMs / static_cast<double>(kCpuSampleDelayMs) * 100.0 /
            static_cast<double>(processorCount);
    }

    std::ranges::sort(processes, std::greater{}, &BoostProcess::memoryBytes);
    if (processes.size() > kMaxCandidates) {
        processes.resize(kMaxCandidates);
    }
    return processes;
}

namespace {

struct CloseWalk {
    DWORD pid = 0;
    std::vector<HWND>* windows = nullptr;
};

BOOL CALLBACK CollectPidWindows(HWND window, LPARAM data) {
    auto* walk = reinterpret_cast<CloseWalk*>(data);
    if (GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == walk->pid) {
        walk->windows->push_back(window);
    }
    return TRUE;
}

bool ProcessStillAlive(DWORD pid, const std::unordered_set<DWORD>& livePids) {
    return livePids.find(pid) != livePids.end();
}

}  // namespace

BoostCloseResult PerfBoost::CloseTargets(const std::vector<BoostProcess>& targets,
    bool allowTerminateWindowless) {
    BoostCloseResult result;
    if (targets.empty()) {
        return result;
    }

    const DWORD selfPid = GetCurrentProcessId();
    DWORD selfSession = 0;
    ProcessIdToSessionId(selfPid, &selfSession);
    const DWORD foregroundPid = ForegroundProcessId();

    // Executables that still own a window elsewhere on screen: their
    // windowless processes are helpers/renderers and are never terminated.
    std::unordered_set<std::wstring> exesWithWindows;
    for (const BoostProcess& target : targets) {
        if (target.hasWindow && !target.exePath.empty()) {
            exesWithWindows.insert(ToLower(target.exePath));
        }
    }

    struct Attempted {
        DWORD pid = 0;
        uint64_t memoryBytes = 0;
    };
    std::vector<Attempted> attempted;

    for (const BoostProcess& target : targets) {
        if (target.pid == 0 || target.pid == 4 || target.pid == selfPid ||
            target.pid == foregroundPid) {
            continue;
        }
        // Freshness re-check: resolve the live process and re-apply every
        // deterministic rule in case the set changed during the Jev call.
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid);
        if (process == nullptr) {
            // Already gone: its memory is already reclaimed.
            if (!target.exePath.empty()) {
                result.closed += 1;
                result.freedBytes += target.memoryBytes;
            }
            continue;
        }
        const std::wstring livePath = ProcessImagePath(process);
        DWORD liveSession = 0;
        ProcessIdToSessionId(target.pid, &liveSession);
        const bool sameImage =
            !livePath.empty() && EqualInsensitive(livePath, target.exePath);
        CloseHandle(process);
        if (!sameImage || liveSession != selfSession || IsProtectedExecutable(livePath)) {
            continue;  // PID reused by something else, or now protected.
        }

        std::vector<HWND> windows;
        CloseWalk walk{target.pid, &windows};
        EnumWindows(&CollectPidWindows, reinterpret_cast<LPARAM>(&walk));

        if (!windows.empty()) {
            for (HWND window : windows) {
                if (IsWindow(window) != FALSE) {
                    PostMessageW(window, WM_CLOSE, 0, 0);
                }
            }
            attempted.push_back({target.pid, target.memoryBytes});
            continue;
        }

        if (!allowTerminateWindowless) {
            continue;  // Heuristic path only ever closes windows gracefully.
        }
        if (exesWithWindows.find(ToLower(livePath)) != exesWithWindows.end()) {
            continue;  // Helper/renderer of a windowed app: leave it alone.
        }
        HANDLE terminable = OpenProcess(PROCESS_TERMINATE, FALSE, target.pid);
        if (terminable == nullptr) {
            continue;
        }
        const BOOL ended = TerminateProcess(terminable, 1);
        CloseHandle(terminable);
        if (ended != FALSE) {
            attempted.push_back({target.pid, target.memoryBytes});
        }
    }

    if (attempted.empty()) {
        return result;
    }

    // Give graceful WM_CLOSE a moment, then confirm who actually exited so
    // the reported count reflects reality instead of intent.
    Sleep(1000);
    std::unordered_set<DWORD> livePids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{sizeof(entry)};
        for (BOOL more = Process32FirstW(snapshot, &entry); more != FALSE;
             more = Process32NextW(snapshot, &entry)) {
            livePids.insert(entry.th32ProcessID);
        }
        CloseHandle(snapshot);
    }
    for (const Attempted& attempt : attempted) {
        if (!ProcessStillAlive(attempt.pid, livePids)) {
            result.closed += 1;
            result.freedBytes += attempt.memoryBytes;
        }
    }
    return result;
}
