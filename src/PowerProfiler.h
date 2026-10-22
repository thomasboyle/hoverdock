#pragma once

#include <Windows.h>

#include <string>

// Dock.exe --power-profile [--quick] [--live <minutes>] [--skip-plans] [--skip-sweep] [--no-energy-report]
//
// Collects everything needed to tune the Automatic energy engine on this
// machine, into a Hoverdock-profile-<stamp> folder next to Dock.exe (or under
// %LOCALAPPDATA%\Hoverdock\Profiles when that is not writable):
//   system.txt      processor (incl. HWP / EPP / activity-window support),
//                   Windows build, power source, power mode overlay, timer
//                   resolution, whether the dock's engine is running
//   plans.csv       the processor settings the engine relies on, for every
//                   power plan (AC and DC); plans\*.txt full powercfg dumps
//   timeline.csv    once a second: package / core / GPU power (RAPL via EMI or
//                   PDH), utilisation, effective clock (average, busy-weighted,
//                   fastest core), C-state residency, parked cores, context
//                   switches, interrupts, timer resolution, active plan,
//                   foreground app, input idle time
//   cores.csv       the same per logical processor
//   processes.csv   every 5 s, the busiest processes by CPU and by context
//                   switches (wake-ups), with cycles and power throttling
//   engine.log      the running dock's engine trace, with per-tick decisions
//                   and every plan value it writes (diagnostics are switched
//                   on while the profiler runs)
//   runs.csv        every measured window: plan or configuration, workload,
//                   energy, clocks, C-states and workload metrics
//   summary.md      the headline tables
//   energy-report.html, requests.txt  powercfg /energy and /requests (elevated only)
// Phases (hands off the keyboard and mouse): the machine as it is now; Balanced,
// Power saver and Automatic under the energy benchmark's workloads; then a sweep
// of the engine's rest-profile settings on a temporary copy of its plan. The
// original plan is restored and the temporary plan deleted when it finishes or
// is cancelled (Ctrl+C). --live records only the timeline, processes and engine
// trace while the machine is used normally.
int RunPowerProfiler(int argc, LPWSTR* argv);

// Dock Settings "Run power profiler": starts Dock.exe --power-profile in its own
// console, elevated when the user allows it (the energy counters and
// powercfg /energy need it), and stops it through a named event so the
// profiler still puts the power plan back.
class PowerProfilerLauncher {
public:
    PowerProfilerLauncher() = default;
    ~PowerProfilerLauncher();

    PowerProfilerLauncher(const PowerProfilerLauncher&) = delete;
    PowerProfilerLauncher& operator=(const PowerProfilerLauncher&) = delete;

    // elevated reports whether the user allowed elevation; false when it could not start at all.
    [[nodiscard]] bool Start(bool& elevated);
    // Asks the profiler to stop early; it restores the plan and writes what it measured.
    void RequestStop() noexcept;
    [[nodiscard]] bool IsRunning() const noexcept;
    [[nodiscard]] bool IsStopping() const noexcept { return m_stopping; }
    // Results folder of the run that just finished, or empty.
    [[nodiscard]] std::wstring LastResult() const;

private:
    void Close() noexcept;

    HANDLE m_process = nullptr;
    HANDLE m_stopEvent = nullptr;
    bool m_stopping = false;
};

// Where a finished profile records its results folder for the dock to find.
std::wstring PowerProfileResultMarker();
