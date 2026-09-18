#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

// PerfBoost profiles running processes so Quick Settings can offer a
// "Boost performance" action: close idle, non-critical work to free memory
// for the apps the user actually cares about.
//
// Safety is layered, with deterministic rules in code owning every side
// effect (per the TypeSafe build guide: code owns control flow and side
// effects; Jev only supplies semantic judgments):
//   1. Enumeration skips PID 0/4, the dock itself, session-0 services,
//      anything under %SystemRoot%, and a denylist of critical executables.
//   2. The foreground process is never a candidate.
//   3. Windowless processes that share their executable with a windowed
//      process (helpers, renderers) are never terminated.
//   4. Jev judgments gate every close behind high thresholds; Noul values
//      near 0.5 (uncertain) never act.
//   5. Targets are re-validated immediately before closing in case the
//      foreground or process set changed during the Jev round-trip.

struct BoostProcess {
    DWORD pid = 0;
    std::wstring name;
    std::wstring exePath;
    std::wstring windowTitle;
    bool hasWindow = false;
    uint64_t memoryBytes = 0;
    double cpuPercent = 0.0;
    uint64_t ageSeconds = 0;
};

struct BoostCloseResult {
    int closed = 0;
    uint64_t freedBytes = 0;
};

class PerfBoost {
public:
    // Snapshot of closable-in-principle processes: non-critical, same user
    // session, not the foreground app, sorted by memory descending and
    // capped so a Jev request stays inside its token budget.
    [[nodiscard]] static std::vector<BoostProcess> EnumerateClosableCandidates();

    // Graceful close first (WM_CLOSE to top-level windows). Windowless
    // processes are terminated only when allowTerminateWindowless is true
    // (Jev path with high-confidence safe judgments); the heuristic
    // fallback passes false and only ever closes windows gracefully.
    [[nodiscard]] static BoostCloseResult CloseTargets(
        const std::vector<BoostProcess>& targets, bool allowTerminateWindowless);

    [[nodiscard]] static bool IsProtectedExecutable(const std::wstring& exePath) noexcept;
    [[nodiscard]] static std::wstring FormatMegabytes(uint64_t bytes);
};
