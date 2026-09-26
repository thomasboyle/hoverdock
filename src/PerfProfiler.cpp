#include "PerfProfiler.h"

#include "Version.h"

#include <Psapi.h>

#include <TlHelp32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

constexpr DWORD kSampleIntervalMs = 200;

uint64_t NowUnixMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    // FILETIME epoch is 1601-01-01; Unix is 1970-01-01 (11644473600s).
    constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;
    if (value.QuadPart < kEpochDiff100ns) {
        return 0;
    }
    return (value.QuadPart - kEpochDiff100ns) / 10000ULL;
}

void FormatLocalStamp(wchar_t* buffer, size_t capacity) {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    swprintf_s(buffer, capacity, L"%04u%02u%02u-%02u%02u%02u", local.wYear, local.wMonth,
        local.wDay, local.wHour, local.wMinute, local.wSecond);
}

void FormatLocalIso(char* buffer, size_t capacity) {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    sprintf_s(buffer, capacity, "%04u-%02u-%02u %02u:%02u:%02u.%03u", local.wYear, local.wMonth,
        local.wDay, local.wHour, local.wMinute, local.wSecond, local.wMilliseconds);
}

}  // namespace

uint64_t PerfProfiler::FileTimeToU100ns(const FILETIME& ft) noexcept {
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

double PerfProfiler::FileTimeToSeconds(const FILETIME& ft) noexcept {
    return static_cast<double>(FileTimeToU100ns(ft)) / 10000000.0;
}

DWORD PerfProfiler::QueryThreadCount(DWORD pid) noexcept {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W entry{sizeof(entry)};
    DWORD count = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more != FALSE;
         more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == pid) {
            count = entry.cntThreads;
            break;
        }
    }
    CloseHandle(snapshot);
    return count;
}

uint64_t PerfProfiler::QueryVirtualSize(HANDLE process) noexcept {
    // Walk the VA space; cheap enough at 200ms for a dock-sized process.
    uint64_t total = 0;
    MEMORY_BASIC_INFORMATION info{};
    unsigned char* address = nullptr;
    while (VirtualQueryEx(process, address, &info, sizeof(info)) == sizeof(info)) {
        if (info.State != MEM_FREE) {
            total += static_cast<uint64_t>(info.RegionSize);
        }
        unsigned char* next = static_cast<unsigned char*>(info.BaseAddress) + info.RegionSize;
        if (next <= address) {
            break;
        }
        address = next;
    }
    return total;
}

std::wstring PerfProfiler::InstallDirectory() {
    wchar_t module[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    std::wstring path(module, length);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    return path.substr(0, slash + 1);
}

std::wstring PerfProfiler::MakeLogPath() {
    const std::wstring dir = InstallDirectory();
    if (dir.empty()) {
        return {};
    }
    wchar_t stamp[32]{};
    FormatLocalStamp(stamp, std::size(stamp));
    return dir + L"Hoverdock-perf-" + stamp + L".log";
}

PerfProfiler::~PerfProfiler() {
    Stop();
}

bool PerfProfiler::IsRunning() const noexcept {
    return m_running.load(std::memory_order_acquire);
}

std::wstring PerfProfiler::LogPath() const {
    const std::lock_guard lock(m_mutex);
    return m_logPath;
}

bool PerfProfiler::Start() {
    if (m_running.load(std::memory_order_acquire)) {
        return false;
    }
    // Join a previous finished thread so Start can be called again.
    if (m_thread.joinable()) {
        m_thread.join();
    }

    const std::wstring path = MakeLogPath();
    if (path.empty()) {
        return false;
    }

    {
        const std::lock_guard lock(m_mutex);
        m_logPath = path;
    }
    m_stopRequested.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    try {
        m_thread = std::thread([this]() { ThreadMain(); });
    } catch (...) {
        m_running.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void PerfProfiler::Stop() {
    m_stopRequested.store(true, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false, std::memory_order_release);
}

void PerfProfiler::WriteHeader(FILE* file, const wchar_t* exePath, DWORD pid) {
    char started[64]{};
    FormatLocalIso(started, sizeof(started));
    char exeUtf8[MAX_PATH * 4]{};
    WideCharToMultiByte(CP_UTF8, 0, exePath != nullptr ? exePath : L"", -1, exeUtf8,
        static_cast<int>(sizeof(exeUtf8)), nullptr, nullptr);

    std::fprintf(file,
        "# Hoverdock performance profile\n"
        "# version=%s\n"
        "# pid=%lu\n"
        "# path=%s\n"
        "# started=%s\n"
        "# sample_interval_ms=%lu\n"
        "# cpu_interval_pct is normalized by logical processor count (Task Manager style)\n"
        "#\n"
        "wall_time,elapsed_ms,cpu_user_s,cpu_kernel_s,cpu_total_s,cpu_interval_pct,"
        "cycle_time,cycle_delta,ws_bytes,peak_ws_bytes,private_bytes,virtual_bytes,"
        "page_faults,handles,threads,gdi,user,"
        "io_read_bytes,io_write_bytes,io_other_bytes,io_read_ops,io_write_ops,io_other_ops\n",
        DockVersion::kVersion, static_cast<unsigned long>(pid), exeUtf8, started,
        static_cast<unsigned long>(kSampleIntervalMs));
}

void PerfProfiler::WriteSample(FILE* file, const Sample& sample) {
    char wall[64]{};
    FormatLocalIso(wall, sizeof(wall));
    std::fprintf(file,
        "%s,%.3f,%.6f,%.6f,%.6f,%.3f,"
        "%llu,%llu,%llu,%llu,%llu,%llu,"
        "%llu,%lu,%lu,%lu,%lu,"
        "%llu,%llu,%llu,%llu,%llu,%llu\n",
        wall, sample.elapsedMs, sample.cpuUserSeconds, sample.cpuKernelSeconds,
        sample.cpuTotalSeconds, sample.cpuIntervalPct,
        static_cast<unsigned long long>(sample.cycleTime),
        static_cast<unsigned long long>(sample.cycleDelta),
        static_cast<unsigned long long>(sample.workingSet),
        static_cast<unsigned long long>(sample.peakWorkingSet),
        static_cast<unsigned long long>(sample.privateBytes),
        static_cast<unsigned long long>(sample.virtualSize),
        static_cast<unsigned long long>(sample.pageFaults),
        static_cast<unsigned long>(sample.handleCount),
        static_cast<unsigned long>(sample.threadCount),
        static_cast<unsigned long>(sample.gdiCount),
        static_cast<unsigned long>(sample.userCount),
        static_cast<unsigned long long>(sample.ioReadBytes),
        static_cast<unsigned long long>(sample.ioWriteBytes),
        static_cast<unsigned long long>(sample.ioOtherBytes),
        static_cast<unsigned long long>(sample.ioReadOps),
        static_cast<unsigned long long>(sample.ioWriteOps),
        static_cast<unsigned long long>(sample.ioOtherOps));
}

void PerfProfiler::WriteFooter(FILE* file, const Summary& summary) {
    char ended[64]{};
    FormatLocalIso(ended, sizeof(ended));
    const double avgCpu = summary.samples > 0 ? summary.cpuSum / static_cast<double>(summary.samples)
                                              : 0.0;
    std::fprintf(file,
        "#\n"
        "# summary\n"
        "# ended=%s\n"
        "# duration_ms=%.3f\n"
        "# samples=%llu\n"
        "# avg_cpu_pct=%.3f\n"
        "# peak_cpu_pct=%.3f\n"
        "# peak_ws_bytes=%llu\n"
        "# peak_private_bytes=%llu\n"
        "# total_io_read_bytes=%llu\n"
        "# total_io_write_bytes=%llu\n"
        "# total_io_read_ops=%llu\n"
        "# total_io_write_ops=%llu\n",
        ended, summary.durationMs, static_cast<unsigned long long>(summary.samples), avgCpu,
        summary.cpuPeak, static_cast<unsigned long long>(summary.wsPeak),
        static_cast<unsigned long long>(summary.privatePeak),
        static_cast<unsigned long long>(summary.ioReadTotal),
        static_cast<unsigned long long>(summary.ioWriteTotal),
        static_cast<unsigned long long>(summary.ioReadOpsTotal),
        static_cast<unsigned long long>(summary.ioWriteOpsTotal));
}

bool PerfProfiler::CollectSample(HANDLE process, DWORD pid, double wallSeconds, double elapsedMs,
    const Sample* previous, Sample& out) noexcept {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user) == FALSE) {
        return false;
    }

    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory)) == FALSE) {
        return false;
    }

    IO_COUNTERS io{};
    if (GetProcessIoCounters(process, &io) == FALSE) {
        std::memset(&io, 0, sizeof(io));
    }

    DWORD handles = 0;
    GetProcessHandleCount(process, &handles);

    ULONG64 cycles = 0;
    QueryProcessCycleTime(process, &cycles);

    out = {};
    out.wallSeconds = wallSeconds;
    out.elapsedMs = elapsedMs;
    out.cpuUserSeconds = FileTimeToSeconds(user);
    out.cpuKernelSeconds = FileTimeToSeconds(kernel);
    out.cpuTotalSeconds = out.cpuUserSeconds + out.cpuKernelSeconds;
    out.cycleTime = cycles;
    out.workingSet = memory.WorkingSetSize;
    out.peakWorkingSet = memory.PeakWorkingSetSize;
    out.privateBytes = memory.PrivateUsage;
    out.virtualSize = QueryVirtualSize(process);
    out.pageFaults = memory.PageFaultCount;
    out.handleCount = handles;
    out.threadCount = QueryThreadCount(pid);
    out.gdiCount = GetGuiResources(process, GR_GDIOBJECTS);
    out.userCount = GetGuiResources(process, GR_USEROBJECTS);
    out.ioReadBytes = io.ReadTransferCount;
    out.ioWriteBytes = io.WriteTransferCount;
    out.ioOtherBytes = io.OtherTransferCount;
    out.ioReadOps = io.ReadOperationCount;
    out.ioWriteOps = io.WriteOperationCount;
    out.ioOtherOps = io.OtherOperationCount;

    if (previous != nullptr) {
        const double deltaCpu = out.cpuTotalSeconds - previous->cpuTotalSeconds;
        const double deltaWall = (elapsedMs - previous->elapsedMs) / 1000.0;
        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        const double processors = static_cast<double>(std::max(1UL, systemInfo.dwNumberOfProcessors));
        if (deltaWall > 0.0 && deltaCpu >= 0.0) {
            out.cpuIntervalPct = (deltaCpu / deltaWall) * 100.0 / processors;
        }
        if (out.cycleTime >= previous->cycleTime) {
            out.cycleDelta = out.cycleTime - previous->cycleTime;
        }
    }
    return true;
}

void PerfProfiler::ThreadMain() {
    std::wstring path;
    {
        const std::lock_guard lock(m_mutex);
        path = m_logPath;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        m_running.store(false, std::memory_order_release);
        return;
    }
    // UTF-8 BOM so Notepad recognizes the CSV cleanly.
    static const unsigned char kBom[] = {0xEF, 0xBB, 0xBF};
    fwrite(kBom, 1, sizeof(kBom), file);

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const DWORD pid = GetCurrentProcessId();
    HANDLE process = GetCurrentProcess();

    WriteHeader(file, exePath, pid);
    std::fflush(file);

    LARGE_INTEGER frequency{};
    LARGE_INTEGER origin{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&origin);

    Sample previous{};
    bool havePrevious = false;
    Summary summary{};
    uint64_t firstIoRead = 0;
    uint64_t firstIoWrite = 0;
    uint64_t firstIoReadOps = 0;
    uint64_t firstIoWriteOps = 0;
    bool haveIoBase = false;

    while (!m_stopRequested.load(std::memory_order_acquire)) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsedMs =
            static_cast<double>(now.QuadPart - origin.QuadPart) * 1000.0 /
            static_cast<double>(frequency.QuadPart);
        const double wallSeconds = static_cast<double>(NowUnixMs()) / 1000.0;

        Sample sample{};
        if (CollectSample(process, pid, wallSeconds, elapsedMs,
                havePrevious ? &previous : nullptr, sample)) {
            WriteSample(file, sample);
            // Flush periodically so a crash still leaves useful data.
            if ((summary.samples % 5ULL) == 0ULL) {
                std::fflush(file);
            }

            summary.samples += 1;
            summary.cpuSum += sample.cpuIntervalPct;
            summary.cpuPeak = std::max(summary.cpuPeak, sample.cpuIntervalPct);
            summary.wsPeak = std::max(summary.wsPeak, sample.workingSet);
            summary.privatePeak = std::max(summary.privatePeak, sample.privateBytes);
            summary.durationMs = elapsedMs;
            if (!haveIoBase) {
                firstIoRead = sample.ioReadBytes;
                firstIoWrite = sample.ioWriteBytes;
                firstIoReadOps = sample.ioReadOps;
                firstIoWriteOps = sample.ioWriteOps;
                haveIoBase = true;
            }
            summary.ioReadTotal = sample.ioReadBytes - firstIoRead;
            summary.ioWriteTotal = sample.ioWriteBytes - firstIoWrite;
            summary.ioReadOpsTotal = sample.ioReadOps - firstIoReadOps;
            summary.ioWriteOpsTotal = sample.ioWriteOps - firstIoWriteOps;

            previous = sample;
            havePrevious = true;
        }

        // Sleep in short slices so Stop responds quickly.
        for (DWORD waited = 0; waited < kSampleIntervalMs; waited += 20) {
            if (m_stopRequested.load(std::memory_order_acquire)) {
                break;
            }
            Sleep(20);
        }
    }

    // Final sample on stop for an accurate footer duration / last counters.
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsedMs =
            static_cast<double>(now.QuadPart - origin.QuadPart) * 1000.0 /
            static_cast<double>(frequency.QuadPart);
        const double wallSeconds = static_cast<double>(NowUnixMs()) / 1000.0;
        Sample sample{};
        if (CollectSample(process, pid, wallSeconds, elapsedMs,
                havePrevious ? &previous : nullptr, sample)) {
            WriteSample(file, sample);
            summary.samples += 1;
            summary.cpuSum += sample.cpuIntervalPct;
            summary.cpuPeak = std::max(summary.cpuPeak, sample.cpuIntervalPct);
            summary.wsPeak = std::max(summary.wsPeak, sample.workingSet);
            summary.privatePeak = std::max(summary.privatePeak, sample.privateBytes);
            summary.durationMs = elapsedMs;
            if (haveIoBase) {
                summary.ioReadTotal = sample.ioReadBytes - firstIoRead;
                summary.ioWriteTotal = sample.ioWriteBytes - firstIoWrite;
                summary.ioReadOpsTotal = sample.ioReadOps - firstIoReadOps;
                summary.ioWriteOpsTotal = sample.ioWriteOps - firstIoWriteOps;
            }
        }
    }

    WriteFooter(file, summary);
    std::fflush(file);
    std::fclose(file);
    m_running.store(false, std::memory_order_release);
}
