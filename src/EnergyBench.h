#pragma once

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

// Dock Settings "Energy benchmark": measures whether the Automatic energy
// engine (AutoPowerEngine) changes how much energy this machine uses.
//
// It runs the same scripted workloads under Windows Balanced (the engine is
// dormant) and under the Hoverdock Automatic plan (the engine is engaged),
// alternating which goes first over several repetitions so drift lands on
// both sides, and compares each workload with a Welch t-test: a saving or a
// cost is only reported when its whole 95% confidence interval is on one side
// of zero. Workloads run in child processes of a renamed copy of Dock.exe
// (HoverdockBenchWorker.exe, --energy-bench-worker): the engine never naps
// Dock.exe, and a worker must look like any other app to it.
//   idle       nothing but the dock: the rest profile plus the engine's own cost
//   cursor     the pointer moving at 60 Hz over an idle desktop (input boost)
//   hidden     a minimized app polling a 1 ms timer (App Nap / EcoQoS), measured
//              after the engine's 30 s nap delay
//   bursty     20 ms CPU bursts every 500 ms, like UI work; p95 burst time
//   video      a 60 fps frame loop with 4 ms of work per frame; missed frames
//   sustained  a fixed all-core job (a build or export); energy to finish it
// Energy comes from the battery (whole system, when unplugged), else the
// processor's RAPL counters (EMI when readable, else PDH Energy Meter).
// Results (runs.csv, summary.md) go to a Hoverdock-energy-<stamp> folder next
// to Dock.exe. The user's power plan is restored when it finishes or stops.
struct EnergyBenchStatus {
    bool running = false;
    bool stopping = false;
    // Estimated time until the run finishes (counts down live).
    double secondsLeft = 0.0;
    // What is happening now, e.g. "Run 5 of 36 · automatic · bursty".
    std::wstring step;
    // After a run: a one-line outcome (verdict, or why it could not start).
    std::wstring result;
    bool failed = false;
    // After a run: summary.md, when written.
    std::wstring summaryPath;
};

class EnergyBenchmark {
public:
    EnergyBenchmark() = default;
    ~EnergyBenchmark();

    EnergyBenchmark(const EnergyBenchmark&) = delete;
    EnergyBenchmark& operator=(const EnergyBenchmark&) = delete;

    // Starts a run on a background thread; false when one is already running.
    [[nodiscard]] bool Start();
    // Asks the run to stop (it restores the power plan); returns at once.
    void RequestStop() noexcept;
    // Stops and waits for the run thread (dock shutdown).
    void Stop();

    [[nodiscard]] bool IsRunning() const noexcept { return m_running.load(); }
    [[nodiscard]] EnergyBenchStatus Status() const;

private:
    void ThreadMain();

    mutable std::mutex m_mutex;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopping{false};
    // Time left = what remains of the current step's estimate + every later step's.
    double m_stepStart = 0.0;
    double m_stepExpected = 0.0;
    double m_laterExpected = 0.0;
    std::wstring m_step;
    std::wstring m_result;
    bool m_failed = false;
    std::wstring m_summaryPath;

};

// Dock.exe --energy-bench-worker <scenario> <iterations>: one workload, run
// until the benchmark closes its stdin (or a fixed job finishes).
int RunEnergyBenchWorker(int argc, LPWSTR* argv);
