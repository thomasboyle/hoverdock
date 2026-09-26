#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// Background process counter sampler for Dock Settings "Performance profile".
// Samples on a dedicated thread (never the Present/UI path). Start/Stop from
// the UI thread; Stop (and the destructor) flush a readable CSV log next to
// Dock.exe under %LOCALAPPDATA%\Programs\Hoverdock\.
class PerfProfiler {
public:
    PerfProfiler() = default;
    ~PerfProfiler();

    PerfProfiler(const PerfProfiler&) = delete;
    PerfProfiler& operator=(const PerfProfiler&) = delete;

    // Opens the log and starts the sampler. False if already running or the
    // log could not be created (no admin required; install dir is per-user).
    [[nodiscard]] bool Start();

    // Signals the sampler to stop, joins it, and finalizes the log footer.
    // Safe when idle. Keeps running if Dock Settings closes mid-profile.
    void Stop();

    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] std::wstring LogPath() const;

private:
    struct Sample {
        double wallSeconds = 0.0;
        double elapsedMs = 0.0;
        double cpuUserSeconds = 0.0;
        double cpuKernelSeconds = 0.0;
        double cpuTotalSeconds = 0.0;
        double cpuIntervalPct = 0.0;
        uint64_t cycleTime = 0;
        uint64_t cycleDelta = 0;
        uint64_t workingSet = 0;
        uint64_t peakWorkingSet = 0;
        uint64_t privateBytes = 0;
        uint64_t virtualSize = 0;
        uint64_t pageFaults = 0;
        DWORD handleCount = 0;
        DWORD threadCount = 0;
        DWORD gdiCount = 0;
        DWORD userCount = 0;
        uint64_t ioReadBytes = 0;
        uint64_t ioWriteBytes = 0;
        uint64_t ioOtherBytes = 0;
        uint64_t ioReadOps = 0;
        uint64_t ioWriteOps = 0;
        uint64_t ioOtherOps = 0;
    };

    struct Summary {
        uint64_t samples = 0;
        double cpuSum = 0.0;
        double cpuPeak = 0.0;
        uint64_t wsPeak = 0;
        uint64_t privatePeak = 0;
        uint64_t ioReadTotal = 0;
        uint64_t ioWriteTotal = 0;
        uint64_t ioReadOpsTotal = 0;
        uint64_t ioWriteOpsTotal = 0;
        double durationMs = 0.0;
    };

    void ThreadMain();
    [[nodiscard]] static bool CollectSample(HANDLE process, DWORD pid, double wallSeconds,
        double elapsedMs, const Sample* previous, Sample& out) noexcept;
    static void WriteHeader(FILE* file, const wchar_t* exePath, DWORD pid);
    static void WriteSample(FILE* file, const Sample& sample);
    static void WriteFooter(FILE* file, const Summary& summary);
    [[nodiscard]] static std::wstring InstallDirectory();
    [[nodiscard]] static std::wstring MakeLogPath();
    [[nodiscard]] static uint64_t FileTimeToU100ns(const FILETIME& ft) noexcept;
    [[nodiscard]] static double FileTimeToSeconds(const FILETIME& ft) noexcept;
    [[nodiscard]] static DWORD QueryThreadCount(DWORD pid) noexcept;
    [[nodiscard]] static uint64_t QueryVirtualSize(HANDLE process) noexcept;

    mutable std::mutex m_mutex;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_thread;
    std::wstring m_logPath;
};
