#include "EnergyBench.h"

#include "EnergyBenchStats.h"

#include <mmsystem.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powerbase.h>
#include <powrprof.h>
#include <setupapi.h>
#include <winioctl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

#include "EnergyBenchCommon.inl"

// ---- measurement window -----------------------------------------------------------------

struct Measurement {
    double seconds = 0.0;
    // Per EMI channel, in EmiMeter::Names() order; empty without EMI.
    std::vector<double> emiJoules;
    double pdhJoules = kNaN;
    // Integrated discharge rate, and the change in reported remaining capacity.
    double batteryJoules = kNaN;
    double batteryCapacityJoules = kNaN;
    double cpuPercent = 0.0;
    // The power source changed mid-run (charger plugged in or pulled).
    bool powerSourceChanged = false;
};

// Reads every available meter across one window: EMI at its ends, PDH every
// 200 ms and the battery every second (both integrated) on a sampler thread.
class Sampler {
public:
    Sampler(EmiMeter* emi, PdhPowerMeter* pdh, bool battery) : m_emi(emi), m_pdh(pdh), m_battery(battery) {}

    void Begin() {
        m_result = Measurement{};
        m_stop.store(false);
        BatteryReading battery;
        m_batteryOk = m_battery && ReadBattery(battery);
        m_startOnAc = battery.onAc;
        m_startRemainingMwh = battery.remainingMwh;
        m_pdhOk = m_pdh != nullptr;
        m_pdhJoules = 0.0;
        m_batteryJoules = 0.0;
        if (m_pdh != nullptr) {
            double ignored = 0.0;
            m_pdh->Read(ignored);  // starts a fresh PDH averaging interval
        }
        m_emiOk = m_emi != nullptr && m_emi->Read(m_emiStart);
        GetSystemTimes(&m_idle0, &m_kernel0, &m_user0);
        m_start = Now();
        m_thread = std::thread([this] { Loop(); });
    }

    Measurement End() {
        m_stop.store(true);
        if (m_thread.joinable()) {
            m_thread.join();
        }
        const double end = Now();
        // Close the integration with a final reading up to the end of the window.
        Integrate(end, true);
        FILETIME idle1{};
        FILETIME kernel1{};
        FILETIME user1{};
        GetSystemTimes(&idle1, &kernel1, &user1);
        std::vector<uint64_t> emiEnd;
        if (m_emiOk && m_emi->Read(emiEnd) && emiEnd.size() == m_emiStart.size()) {
            for (size_t i = 0; i < emiEnd.size(); ++i) {
                m_result.emiJoules.push_back(
                    static_cast<double>(emiEnd[i] - m_emiStart[i]) * kJoulesPerPicowattHour);
            }
        }
        m_result.seconds = end - m_start;
        m_result.cpuPercent = 100.0 * CpuBusyFraction(m_idle0, m_kernel0, m_user0, idle1, kernel1, user1);
        if (m_pdhOk && m_pdh != nullptr) {
            m_result.pdhJoules = m_pdhJoules;
        }
        BatteryReading battery;
        if (m_batteryOk && ReadBattery(battery)) {
            m_result.batteryJoules = m_batteryJoules;
            m_result.batteryCapacityJoules = (m_startRemainingMwh - battery.remainingMwh) * 3.6;
            m_result.powerSourceChanged = m_result.powerSourceChanged || battery.onAc != m_startOnAc;
        }
        return m_result;
    }

private:
    void Loop() {
        m_lastPdh = m_start;
        m_lastBattery = m_start;
        while (!m_stop.load()) {
            Sleep(200);
            Integrate(Now(), false);
        }
    }

    // PDH's Power is the average over the interval since the previous collect,
    // so value x elapsed is that interval's energy. The battery's rate updates
    // slowly, so it is read about once a second and once more at the end.
    void Integrate(double now, bool closing) {
        const std::lock_guard lock(m_mutex);
        if (m_pdhOk && m_pdh != nullptr && now > m_lastPdh) {
            double watts = 0.0;
            if (m_pdh->Read(watts)) {
                m_pdhJoules += watts * (now - m_lastPdh);
            } else {
                m_pdhOk = false;
            }
            m_lastPdh = now;
        }
        if (m_batteryOk && now > m_lastBattery && (closing || now - m_lastBattery >= 1.0)) {
            BatteryReading battery;
            if (ReadBattery(battery)) {
                m_batteryJoules += battery.dischargeWatts * (now - m_lastBattery);
                if (battery.onAc != m_startOnAc) {
                    m_result.powerSourceChanged = true;
                }
            }
            m_lastBattery = now;
        }
    }

    EmiMeter* m_emi;
    PdhPowerMeter* m_pdh;
    bool m_battery;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::mutex m_mutex;
    Measurement m_result;
    std::vector<uint64_t> m_emiStart;
    bool m_emiOk = false;
    bool m_pdhOk = false;
    bool m_batteryOk = false;
    bool m_startOnAc = true;
    double m_startRemainingMwh = 0.0;
    double m_pdhJoules = 0.0;
    double m_batteryJoules = 0.0;
    double m_start = 0.0;
    double m_lastPdh = 0.0;
    double m_lastBattery = 0.0;
    FILETIME m_idle0{};
    FILETIME m_kernel0{};
    FILETIME m_user0{};
};

// ---- workloads (child process side) -----------------------------------------------------

// The parent writes 'm' to stdin when measuring starts and closes stdin to stop.
std::atomic<bool> g_workerStop{false};
std::atomic<double> g_markTime{0.0};

void WatchParent() {
    std::thread([] {
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        char buffer[16]{};
        DWORD read = 0;
        while (ReadFile(input, buffer, sizeof(buffer), &read, nullptr) != FALSE && read > 0) {
            if (std::memchr(buffer, 'm', read) != nullptr) {
                g_markTime.store(Now());
            }
        }
        g_workerStop.store(true);
    }).detach();
}

bool Measuring(double time) {
    const double mark = g_markTime.load();
    return mark > 0.0 && time >= mark;
}

void Report(const std::string& line) {
    std::string out = line + "\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
}

std::string Metric(const char* key, double value) {
    char text[96]{};
    std::snprintf(text, sizeof(text), "%s=%.4f ", key, value);
    return text;
}

int WorkerIdle() {
    while (!g_workerStop.load()) {
        Sleep(100);
    }
    Report("");
    return 0;
}

// Absolute moves between two points 6 px apart near the primary monitor's centre,
// away from the dock, then the pointer goes back where it was.
int WorkerCursor() {
    POINT original{};
    GetCursorPos(&original);
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    const auto normalise = [](int value, int extent) {
        return static_cast<LONG>((static_cast<long long>(value) * 65535) / std::max(extent - 1, 1));
    };
    uint64_t moves = 0;
    uint64_t measured = 0;
    while (!g_workerStop.load()) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = normalise(width / 2 + ((moves & 1U) != 0 ? 6 : 0), width);
        input.mi.dy = normalise(height / 2, height);
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        SendInput(1, &input, sizeof(INPUT));
        ++moves;
        if (Measuring(Now())) {
            ++measured;
        }
        Sleep(16);
    }
    SetCursorPos(original.x, original.y);
    Report(Metric("moves", static_cast<double>(measured)));
    return 0;
}

int WorkerBursty(uint64_t burstIterations) {
    constexpr double kPeriod = 0.5;
    std::vector<double> burstMs;
    double next = Now();
    uint64_t seed = 1;
    while (!g_workerStop.load()) {
        const double wait = next - Now();
        if (wait > 0.0) {
            Sleep(static_cast<DWORD>(wait * 1000.0));
            continue;
        }
        const double start = Now();
        Spin(burstIterations, ++seed);
        const double end = Now();
        if (Measuring(start)) {
            burstMs.push_back((end - start) * 1000.0);
        }
        next += kPeriod;
        if (next < end) {
            next = end + kPeriod;
        }
    }
    Report(Metric("bursts", static_cast<double>(burstMs.size())) +
        Metric("p50_ms", energybench::Percentile(burstMs, 50.0)) +
        Metric("p95_ms", energybench::Percentile(burstMs, 95.0)));
    return 0;
}

int WorkerVideo(uint64_t frameIterations) {
    constexpr double kFrame = 1.0 / 60.0;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) {
        timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    }
    std::vector<double> workMs;
    uint64_t frames = 0;
    uint64_t missed = 0;
    uint64_t seed = 7;
    double next = Now() + kFrame;
    while (!g_workerStop.load()) {
        const double wait = next - Now();
        if (wait > 0.0 && timer != nullptr) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(wait * 1e7);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        } else if (wait > 0.0) {
            Sleep(1);
            continue;
        }
        const double start = Now();
        Spin(frameIterations, ++seed);
        const double end = Now();
        // A frame started at its vsync must be ready by the next one.
        const bool late = end > next + kFrame;
        if (Measuring(start)) {
            ++frames;
            missed += late ? 1U : 0U;
            workMs.push_back((end - start) * 1000.0);
        }
        next += kFrame;
        while (next + kFrame < end) {
            next += kFrame;
            if (Measuring(start)) {
                ++frames;
                ++missed;
            }
        }
    }
    if (timer != nullptr) {
        CloseHandle(timer);
    }
    Report(Metric("frames", static_cast<double>(frames)) +
        Metric("missed_pct", frames == 0 ? 0.0 : 100.0 * static_cast<double>(missed) / static_cast<double>(frames)) +
        Metric("p95_work_ms", energybench::Percentile(workMs, 95.0)));
    return 0;
}

LRESULT CALLBACK HiddenWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

// A chatty background app: one minimized window, 1 ms timer resolution, a
// little work every tick. Napping it (EcoQoS, timer requests ignored) shows
// as fewer ticks per second.
int WorkerHidden(uint64_t tickIterations) {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = HiddenWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"HoverdockEnergyBenchHidden";
    RegisterClassW(&windowClass);
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"Hoverdock energy benchmark background app",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 400, 300, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (window != nullptr) {
        ShowWindow(window, SW_SHOWMINNOACTIVE);
    }
    timeBeginPeriod(1);
    uint64_t ticks = 0;
    uint64_t seed = 3;
    while (!g_workerStop.load()) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
        Spin(tickIterations, ++seed);
        if (Measuring(Now())) {
            ++ticks;
        }
    }
    timeEndPeriod(1);
    const double mark = g_markTime.load();
    const double seconds = mark > 0.0 ? Now() - mark : 0.0;
    if (window != nullptr) {
        DestroyWindow(window);
    }
    Report(Metric("ticks_per_s", seconds > 0.0 ? static_cast<double>(ticks) / seconds : 0.0));
    return 0;
}

int WorkerSustained(uint64_t perThreadIterations) {
    const unsigned threads = std::max(1U, std::thread::hardware_concurrency());
    constexpr uint64_t kChunk = 1U << 20U;
    const double start = Now();
    std::atomic<bool> completed{true};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            for (uint64_t done = 0; done < perThreadIterations; done += kChunk) {
                if (g_workerStop.load()) {
                    completed.store(false);
                    return;
                }
                Spin(std::min(kChunk, perThreadIterations - done), t + 11U);
            }
        });
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
    Report(Metric("seconds", Now() - start) + Metric("completed", completed.load() ? 1.0 : 0.0) +
        Metric("threads", static_cast<double>(threads)));
    return 0;
}

int RunWorker(const std::vector<std::wstring>& args) {
    // args: <exe> --energy-bench-worker <name> [iterations]
    const std::wstring name = args.size() > 2 ? Lower(args[2]) : L"";
    const uint64_t iterations = args.size() > 3 ? std::wcstoull(args[3].c_str(), nullptr, 10) : 0;
    WatchParent();
    if (name == L"idle") {
        return WorkerIdle();
    }
    if (name == L"cursor") {
        return WorkerCursor();
    }
    if (name == L"bursty") {
        return WorkerBursty(iterations);
    }
    if (name == L"video") {
        return WorkerVideo(iterations);
    }
    if (name == L"hidden") {
        return WorkerHidden(iterations);
    }
    if (name == L"sustained") {
        return WorkerSustained(iterations);
    }
    return 2;
}


struct Scenario {
    const char* name;
    const char* description;
    // Runs a fixed job to completion (energy to finish) instead of a fixed time (average power).
    bool fixedWork;
    // Unmeasured lead-in after the worker starts.
    double preRollSeconds;
    // Worker metric shown next to energy; empty for none.
    const char* perfKey;
    const char* perfLabel;
};

// Order matters: the hot sustained job runs last in each round, followed by a cool-down.
const Scenario kScenarios[] = {
    {"idle", "Idle desktop with the dock running: rest profile and the engine's own overhead", false, 0.0, "", ""},
    {"cursor", "Pointer moving at 60 Hz over an idle desktop: input-boost handling", false, 0.0, "", ""},
    {"hidden", "Minimized app polling a 1 ms timer: App Nap / EcoQoS of hidden apps", false, 35.0, "ticks_per_s",
        "timer ticks/s"},
    {"bursty", "20 ms CPU bursts every 500 ms, like UI work: clocks vs. responsiveness", false, 0.0, "p95_ms",
        "p95 burst ms"},
    {"video", "60 fps frame loop, 4 ms of work per frame: media-style periodic load", false, 0.0, "missed_pct",
        "missed frames %"},
    {"sustained", "Fixed all-core job, like a build or export: energy to finish", true, 0.0, "seconds",
        "job seconds"},
};

struct Condition {
    std::string key;
    GUID plan;
};

enum class MeterKind { Battery, Emi, Pdh };

const char* MeterLabel(MeterKind kind) {
    return kind == MeterKind::Battery ? "battery discharge (whole system)"
        : kind == MeterKind::Emi      ? "EMI energy counter"
                                      : "PDH Energy Meter power (integrated)";
}

// One fixed configuration, about 30 minutes: 3 repetitions of each workload
// under each plan is the fewest that gives the t-test usable power.
struct Options {
    int reps = 3;
    double seconds = 30.0;
    double settle = 10.0;
    double jobSeconds = 10.0;
};

struct RunRecord {
    int rep = 0;
    size_t condition = 0;
    size_t scenario = 0;
    bool valid = true;
    std::string note;
    Measurement measurement;
    // Headline energy (J) and average power (W) from the chosen meter.
    double joules = kNaN;
    double watts = kNaN;
    std::string metrics;
    double perf = kNaN;
};

// ---- benchmark driver -------------------------------------------------------------------

struct Bench {
    Options options;
    std::wstring exe;
    std::vector<size_t> scenarios;
    std::vector<Condition> conditions;
    MeterKind meter = MeterKind::Pdh;
    size_t emiPrimary = 0;
    std::vector<std::wstring> emiNames;
    std::wstring pdhInstance;
    double iterationsPerMs = 0.0;
    std::vector<RunRecord> runs;
};

uint64_t ScenarioIterations(const Bench& bench, const Scenario& scenario) {
    const double perMs = bench.iterationsPerMs;
    const std::string name = scenario.name;
    const double ms = name == "bursty" ? 20.0
        : name == "video"              ? 4.0
        : name == "hidden"             ? 0.05
        : name == "sustained"          ? bench.options.jobSeconds * 1000.0
                                       : 0.0;
    return static_cast<uint64_t>(std::max(0.0, perMs * ms));
}

RunRecord RunOne(Bench& bench, Sampler& sampler, int rep, size_t conditionIndex, size_t scenarioIndex,
    double settleSeconds) {
    const Scenario& scenario = kScenarios[scenarioIndex];
    const Condition& condition = bench.conditions[conditionIndex];
    RunRecord record;
    record.rep = rep;
    record.condition = conditionIndex;
    record.scenario = scenarioIndex;
    const auto fail = [&](std::string note) {
        record.valid = false;
        record.note = std::move(note);
        return record;
    };
    if (!SetPlan(condition.plan)) {
        return fail("could not switch power plan");
    }
    if (!SleepFor(settleSeconds)) {
        return fail("aborted");
    }
    std::wstring arguments = L"--energy-bench-worker ";
    for (const char* c = scenario.name; *c != '\0'; ++c) {
        arguments.push_back(static_cast<wchar_t>(*c));
    }
    arguments += L" " + std::to_wstring(ScenarioIterations(bench, scenario));
    WorkerProcess worker;
    std::string error;
    Measurement measurement;
    if (scenario.fixedWork) {
        sampler.Begin();
        if (!worker.Start(bench.exe, arguments, error)) {
            sampler.End();
            return fail("worker failed to start: " + error);
        }
        worker.Mark();
        const bool finished = worker.WaitExit(bench.options.jobSeconds * 20.0 + 60.0);
        measurement = sampler.End();
        if (!finished) {
            worker.Close();
            return fail(g_abort.load() ? "aborted" : "job timed out");
        }
    } else {
        if (!worker.Start(bench.exe, arguments, error)) {
            return fail("worker failed to start: " + error);
        }
        if (!SleepFor(scenario.preRollSeconds)) {
            return fail("aborted");
        }
        sampler.Begin();
        worker.Mark();
        const bool completed = SleepFor(bench.options.seconds);
        measurement = sampler.End();
        worker.RequestStop();
        worker.WaitExit(10.0);
        if (!completed) {
            return fail("aborted");
        }
    }
    record.metrics = worker.Output();
    worker.Close();
    record.measurement = measurement;
    record.perf = MetricValue(record.metrics, scenario.perfKey);

    GUID active{};
    if (!ActivePlan(active) || !IsEqualGUID(active, condition.plan)) {
        return fail("power plan changed during the run");
    }
    if (measurement.powerSourceChanged) {
        return fail("power source changed during the run");
    }
    if (scenario.fixedWork && MetricValue(record.metrics, "completed") != 1.0) {
        return fail("job did not finish");
    }
    switch (bench.meter) {
    case MeterKind::Battery:
        record.joules = measurement.batteryJoules;
        break;
    case MeterKind::Emi:
        record.joules = bench.emiPrimary < measurement.emiJoules.size() ? measurement.emiJoules[bench.emiPrimary]
                                                                        : kNaN;
        break;
    case MeterKind::Pdh:
        record.joules = measurement.pdhJoules;
        break;
    }
    if (!std::isfinite(record.joules) || measurement.seconds <= 0.0) {
        return fail("the energy meter returned no data");
    }
    record.watts = record.joules / measurement.seconds;
    return record;
}

std::string CsvHeader(const Bench& bench) {
    std::string header = "run,rep,condition,plan,scenario,valid,note,seconds,energy_j,avg_w,cpu_pct,pdh_j,battery_j,"
                         "battery_capacity_j";
    for (const std::wstring& name : bench.emiNames) {
        header += "," + CsvField("emi_" + Utf8(name) + "_j");
    }
    return header + ",metrics\n";
}

std::string CsvRow(const Bench& bench, size_t index, const RunRecord& run) {
    const Measurement& m = run.measurement;
    std::string row = std::to_string(index + 1) + "," + std::to_string(run.rep + 1) + "," +
        CsvField(bench.conditions[run.condition].key) + "," + GuidText(bench.conditions[run.condition].plan) + "," +
        kScenarios[run.scenario].name + "," + (run.valid ? "1" : "0") + "," + CsvField(run.note) + "," +
        Number(m.seconds) + "," + Number(run.joules) + "," + Number(run.watts, 4) + "," + Number(m.cpuPercent, 2) +
        "," + Number(m.pdhJoules) + "," + Number(m.batteryJoules) + "," + Number(m.batteryCapacityJoules);
    for (size_t i = 0; i < bench.emiNames.size(); ++i) {
        row += "," + (i < m.emiJoules.size() ? Number(m.emiJoules[i]) : std::string());
    }
    return row + "," + CsvField(run.metrics) + "\n";
}

// Timed scenarios compare average power; the fixed job compares energy to finish.
double Score(const Scenario& scenario, const RunRecord& run) {
    return scenario.fixedWork ? run.joules : run.watts;
}

const char* VerdictText(energybench::Verdict verdict) {
    switch (verdict) {
    case energybench::Verdict::Lower:
        return "LESS energy (significant)";
    case energybench::Verdict::Higher:
        return "MORE energy (significant)";
    case energybench::Verdict::NoDifference:
        return "no detectable difference";
    case energybench::Verdict::NotEnoughData:
        break;
    }
    return "not enough valid runs";
}

std::string MeanSd(const std::vector<double>& values, int decimals) {
    if (values.empty()) {
        return "-";
    }
    return Number(energybench::Mean(values), decimals) + " \xC2\xB1 " + Number(energybench::StdDev(values), decimals);
}

std::string Summarize(const Bench& bench, const std::string& machine) {
    std::string md = "# Hoverdock power engine energy benchmark\n\n" + machine + "\n";
    md += "Baseline: **" + bench.conditions[0].key + "** (" + PlanName(bench.conditions[0].plan) + ").\n";
    md += "Headline meter: **" + std::string(MeterLabel(bench.meter)) + "**";
    if (bench.meter == MeterKind::Emi && bench.emiPrimary < bench.emiNames.size()) {
        md += " (" + Utf8(bench.emiNames[bench.emiPrimary]) + ")";
    } else if (bench.meter == MeterKind::Pdh) {
        md += " (" + Utf8(bench.pdhInstance) + ")";
    }
    md += bench.meter == MeterKind::Battery ? ".\n\n"
                                            : ". This covers the processor package, not the display, storage or "
                                              "the rest of the system; run on battery for whole-system energy.\n\n";
    md += "Timed scenarios compare average power (W) over the same measured time; the fixed job compares energy "
          "(J) to finish it. Change is relative to the baseline with a 95% confidence interval (Welch's t-test); "
          "a difference is only called when the whole interval is on one side of zero.\n\n";
    for (size_t c = 1; c < bench.conditions.size(); ++c) {
        const Condition& candidate = bench.conditions[c];
        md += "## " + candidate.key + " vs " + bench.conditions[0].key + "\n\n";
        md += "| Scenario | Measure | " + bench.conditions[0].key + " | " + candidate.key +
            " | Change | 95% CI | Verdict | Performance (" + bench.conditions[0].key + " \xE2\x86\x92 " +
            candidate.key + ") |\n";
        md += "|---|---|---|---|---|---|---|---|\n";
        int lower = 0;
        int higher = 0;
        int same = 0;
        for (const size_t s : bench.scenarios) {
            const Scenario& scenario = kScenarios[s];
            std::vector<double> base;
            std::vector<double> cand;
            std::vector<double> basePerf;
            std::vector<double> candPerf;
            for (const RunRecord& run : bench.runs) {
                if (!run.valid || run.scenario != s) {
                    continue;
                }
                if (run.condition == 0) {
                    base.push_back(Score(scenario, run));
                    if (std::isfinite(run.perf)) {
                        basePerf.push_back(run.perf);
                    }
                } else if (run.condition == c) {
                    cand.push_back(Score(scenario, run));
                    if (std::isfinite(run.perf)) {
                        candPerf.push_back(run.perf);
                    }
                }
            }
            const energybench::Comparison cmp = energybench::Compare(base, cand);
            lower += cmp.verdict == energybench::Verdict::Lower ? 1 : 0;
            higher += cmp.verdict == energybench::Verdict::Higher ? 1 : 0;
            same += cmp.verdict == energybench::Verdict::NoDifference ? 1 : 0;
            const char* unit = scenario.fixedWork ? "J" : "W";
            std::string perf = "-";
            if (scenario.perfKey[0] != '\0' && !basePerf.empty() && !candPerf.empty()) {
                perf = std::string(scenario.perfLabel) + ": " + Number(energybench::Mean(basePerf), 2) +
                    " \xE2\x86\x92 " + Number(energybench::Mean(candPerf), 2);
            }
            const bool judged = cmp.verdict != energybench::Verdict::NotEnoughData;
            md += "| " + std::string(scenario.name) + " | " + (scenario.fixedWork ? "energy (J)" : "power (W)") +
                " | " + MeanSd(base, scenario.fixedWork ? 1 : 3) + " " + unit + " | " +
                MeanSd(cand, scenario.fixedWork ? 1 : 3) + " " + unit + " | " +
                (base.empty() || cand.empty() ? "-" : (cmp.percent >= 0 ? "+" : "") + Number(cmp.percent, 1) + "%") +
                " | " +
                (judged ? Number(cmp.percentCiLow, 1) + "% \xE2\x80\xA6 " + Number(cmp.percentCiHigh, 1) + "%"
                        : std::string("-")) +
                " | " + VerdictText(cmp.verdict) + " | " + perf + " |\n";
        }
        md += "\n**Verdict:** " + candidate.key + " used significantly less energy in " + std::to_string(lower) +
            ", significantly more in " + std::to_string(higher) + ", and no detectable difference in " +
            std::to_string(same) + " of " + std::to_string(bench.scenarios.size()) + " scenarios.\n\n";
    }
    md += "## Scenarios\n\n";
    for (const size_t s : bench.scenarios) {
        md += "- **" + std::string(kScenarios[s].name) + "**: " + kScenarios[s].description + "\n";
    }
    int invalid = 0;
    for (const RunRecord& run : bench.runs) {
        invalid += run.valid ? 0 : 1;
    }
    md += "\n" + std::to_string(bench.runs.size()) + " runs, " + std::to_string(invalid) +
        " rejected (see runs.csv for every reading, every meter and the reason for each rejection).\n";
    return md;
}

std::string MachineDescription(const Bench& bench, const BatteryReading& battery, bool batteryOk) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char date[64]{};
    std::snprintf(date, sizeof(date), "%04u-%02u-%02u %02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
        now.wMinute);
    const std::string cpu =
        RegistryText(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    const std::string version =
        RegistryText(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");
    const std::string build =
        RegistryText(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild");
    std::string out = "- Date: " + std::string(date) + "\n- CPU: " + (cpu.empty() ? "unknown" : cpu) + " (" +
        std::to_string(std::max(1U, std::thread::hardware_concurrency())) + " threads)\n- Windows: " + version +
        " build " + build + "\n- Power source: " +
        (!batteryOk ? "AC (no battery)" : battery.onAc ? "AC (battery present)" : "battery") + "\n";
    out += "- Runs: " + std::to_string(bench.options.reps) + " repetitions, " + Number(bench.options.seconds, 0) +
        " s measured per timed scenario, " + Number(bench.options.settle, 0) + " s settle after each plan switch\n";
    out += "- Calibration: " + Number(bench.iterationsPerMs / 1000.0, 1) + " M work units per ms (one thread)\n";
    return out;
}

// Planned length of one run, for the countdown; the fixed job uses the
// measured length of earlier jobs once there is one.
double PlannedSeconds(const Bench& bench, const Scenario& scenario, double settle, double jobSeconds) {
    return settle + scenario.preRollSeconds + (scenario.fixedWork ? jobSeconds : bench.options.seconds) + 1.0;
}

struct PlannedRun {
    int rep;
    size_t condition;
    size_t scenario;
    double settle;
};

energybench::Comparison CompareScenario(const Bench& bench, size_t scenario, size_t condition) {
    std::vector<double> base;
    std::vector<double> candidate;
    for (const RunRecord& run : bench.runs) {
        if (!run.valid || run.scenario != scenario) {
            continue;
        }
        if (run.condition == 0) {
            base.push_back(Score(kScenarios[scenario], run));
        } else if (run.condition == condition) {
            candidate.push_back(Score(kScenarios[scenario], run));
        }
    }
    return energybench::Compare(base, candidate);
}

void Trace(const std::wstring& text) {
    OutputDebugStringW((L"[EnergyBench] " + text + L"\n").c_str());
}

}  // namespace

EnergyBenchmark::~EnergyBenchmark() {
    Stop();
}

bool EnergyBenchmark::Start() {
    if (m_running.exchange(true)) {
        return false;
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    g_abort.store(false);
    m_stopping.store(false);
    {
        const std::lock_guard lock(m_mutex);
        m_step = L"Preparing\u2026";
        m_stepStart = Now();
        m_stepExpected = 0.0;
        m_laterExpected = 0.0;
        m_result.clear();
        m_failed = false;
        m_summaryPath.clear();
    }
    try {
        m_thread = std::thread([this] {
            try {
                ThreadMain();
            } catch (...) {
                {
                    const std::lock_guard lock(m_mutex);
                    m_result = L"The benchmark stopped after an unexpected error.";
                    m_failed = true;
                    m_step.clear();
                }
                SetThreadExecutionState(ES_CONTINUOUS);
                m_stopping.store(false);
                m_running.store(false);
            }
        });
    } catch (...) {
        m_running.store(false);
        return false;
    }
    return true;
}

void EnergyBenchmark::RequestStop() noexcept {
    if (m_running.load()) {
        m_stopping.store(true);
        g_abort.store(true);
    }
}

void EnergyBenchmark::Stop() {
    RequestStop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

EnergyBenchStatus EnergyBenchmark::Status() const {
    EnergyBenchStatus status;
    status.running = m_running.load();
    status.stopping = m_stopping.load();
    const std::lock_guard lock(m_mutex);
    status.step = m_step;
    status.result = m_result;
    status.failed = m_failed;
    status.summaryPath = m_summaryPath;
    if (status.running) {
        status.secondsLeft = std::max(0.0, m_stepExpected - (Now() - m_stepStart)) + m_laterExpected;
    }
    return status;
}

void EnergyBenchmark::ThreadMain() {
    const auto setStep = [this](std::wstring text, double expected, double later) {
        const std::lock_guard lock(m_mutex);
        m_step = std::move(text);
        m_stepStart = Now();
        m_stepExpected = expected;
        m_laterExpected = later;
    };
    const auto finish = [this](std::wstring result, bool failed, std::wstring summary) {
        Trace(result);
        {
            const std::lock_guard lock(m_mutex);
            m_result = std::move(result);
            m_failed = failed;
            m_summaryPath = std::move(summary);
            m_step.clear();
            m_stepExpected = 0.0;
            m_laterExpected = 0.0;
        }
        m_stopping.store(false);
        m_running.store(false);
    };

    Bench bench;
    for (size_t s = 0; s < std::size(kScenarios); ++s) {
        bench.scenarios.push_back(s);
    }
    bench.conditions = {{"Balanced", kBalancedPlan}, {"Automatic", kAutoPlan}};
    if (!PlanExists(kAutoPlan)) {
        finish(L"Choose Automatic once in Quick Settings \u203A Energy Mode first: that creates the plan this "
               L"benchmark compares.",
            true, {});
        return;
    }
    if (!PlanExists(kBalancedPlan)) {
        finish(L"Windows' Balanced plan is missing, so there is nothing to compare Automatic against.", true, {});
        return;
    }

    // Schedule: alternate which plan goes first (ABBA) so drift cancels out,
    // with a cool-down after each hot sustained job.
    std::vector<PlannedRun> schedule;
    bool previousWasHot = false;
    for (int rep = 0; rep < bench.options.reps; ++rep) {
        for (size_t i = 0; i < bench.scenarios.size(); ++i) {
            const bool reverse = (static_cast<size_t>(rep) + i) % 2 == 1;
            for (size_t k = 0; k < bench.conditions.size(); ++k) {
                const size_t c = reverse ? bench.conditions.size() - 1 - k : k;
                const double settle = bench.options.settle + (previousWasHot ? bench.options.settle * 2.0 : 0.0);
                schedule.push_back({rep, c, bench.scenarios[i], settle});
                previousWasHot = kScenarios[bench.scenarios[i]].fixedWork;
            }
        }
    }
    double jobSeconds = bench.options.jobSeconds * 1.5;
    const auto plannedFrom = [&](size_t first) {
        double total = 0.0;
        for (size_t j = first; j < schedule.size(); ++j) {
            total += PlannedSeconds(bench, kScenarios[schedule[j].scenario], schedule[j].settle, jobSeconds);
        }
        return total;
    };
    constexpr double kPrepareSeconds = 6.0;
    constexpr double kCountdownSeconds = 10.0;
    setStep(L"Preparing\u2026", kPrepareSeconds, kCountdownSeconds + plannedFrom(0));

    // Meters: the battery when unplugged (whole system), else RAPL via EMI, else PDH.
    EmiMeter emi;
    std::string emiError;
    const bool emiOk = emi.Open(emiError);
    PdhPowerMeter pdh;
    const bool pdhOk = pdh.Open();
    BatteryReading battery;
    const bool batteryOk = ReadBattery(battery);
    if (batteryOk && !battery.onAc) {
        bench.meter = MeterKind::Battery;
    } else if (emiOk) {
        bench.meter = MeterKind::Emi;
    } else if (pdhOk) {
        bench.meter = MeterKind::Pdh;
    } else {
        finish(L"No energy meter: this processor exposes no energy counters to Windows. Unplug the charger to "
               L"measure from the battery instead.",
            true, {});
        return;
    }
    if (emiOk) {
        bench.emiNames = emi.Names();
        int best = -1;
        for (size_t i = 0; i < bench.emiNames.size(); ++i) {
            const int score = ChannelScore(bench.emiNames[i]);
            if (score > best) {
                best = score;
                bench.emiPrimary = i;
            }
        }
    }
    bench.pdhInstance = pdh.Instance();
    bench.iterationsPerMs = CalibrateIterationsPerMs();

    // Workers run from a renamed copy: the engine never naps Dock.exe.
    const std::wstring dockExe = ModulePath();
    wchar_t tempPath[MAX_PATH]{};
    const DWORD tempLength = GetTempPathW(MAX_PATH, tempPath);
    const std::wstring workerDir =
        (tempLength == 0 || tempLength >= MAX_PATH ? std::wstring() : std::wstring(tempPath)) +
        L"Hoverdock-EnergyBench";
    CreateDirectoryW(workerDir.c_str(), nullptr);
    bench.exe = workerDir + L"\\HoverdockBenchWorker.exe";
    if (dockExe.empty() || CopyFileW(dockExe.c_str(), bench.exe.c_str(), FALSE) == FALSE) {
        finish(L"Could not prepare the benchmark's worker program in " + workerDir + L".", true, {});
        return;
    }

    const size_t slash = dockExe.find_last_of(L"\\/");
    const std::wstring outDir =
        (slash == std::wstring::npos ? std::wstring() : dockExe.substr(0, slash + 1)) + L"Hoverdock-energy-" +
        LocalStamp();
    CreateDirectoryW(outDir.c_str(), nullptr);
    const std::wstring csvPath = outDir + L"\\runs.csv";
    const std::wstring summaryPath = outDir + L"\\summary.md";
    if (!WriteFileText(csvPath, CsvHeader(bench))) {
        DeleteFileW(bench.exe.c_str());
        finish(L"Could not write results to " + outDir + L".", true, {});
        return;
    }
    GUID originalPlan{};
    if (!ActivePlan(originalPlan)) {
        DeleteFileW(bench.exe.c_str());
        finish(L"Could not read the active power plan.", true, {});
        return;
    }

    // A moment to let go of the mouse: input wakes the engine.
    setStep(L"Starting soon \u00B7 hands off the keyboard and mouse until it finishes", kCountdownSeconds,
        plannedFrom(0));
    SleepFor(kCountdownSeconds);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    Sampler sampler(emiOk ? &emi : nullptr, pdhOk ? &pdh : nullptr, batteryOk);
    std::vector<double> jobTimes;
    for (size_t i = 0; i < schedule.size() && !g_abort.load(); ++i) {
        const PlannedRun& planned = schedule[i];
        const Scenario& scenario = kScenarios[planned.scenario];
        const std::wstring label = L"Run " + std::to_wstring(i + 1) + L" of " + std::to_wstring(schedule.size()) +
            L" \u00B7 " + Wide(bench.conditions[planned.condition].key) + L" \u00B7 " + Wide(scenario.name) +
            L" \u00B7 hands off";
        setStep(label, PlannedSeconds(bench, scenario, planned.settle, jobSeconds), plannedFrom(i + 1));
        RunRecord run = RunOne(bench, sampler, planned.rep, planned.condition, planned.scenario, planned.settle);
        if (run.note == "aborted") {
            break;
        }
        if (scenario.fixedWork && run.valid) {
            jobTimes.push_back(run.measurement.seconds);
            jobSeconds = energybench::Mean(jobTimes);
        }
        WriteFileText(csvPath, CsvRow(bench, bench.runs.size(), run), true);
        Trace(label + L": " +
            (run.valid ? Wide(Number(run.joules, 1)) + L" J, " + Wide(Number(run.watts, 3)) + L" W"
                       : L"rejected (" + Wide(run.note) + L")"));
        bench.runs.push_back(std::move(run));
    }
    SetThreadExecutionState(ES_CONTINUOUS);
    SetPlan(originalPlan);
    const bool aborted = g_abort.load();
    {
        const std::lock_guard lock(m_mutex);
        m_step = L"Writing results\u2026";
    }
    // The last worker may still be exiting.
    for (int attempt = 0; attempt < 20 && DeleteFileW(bench.exe.c_str()) == FALSE &&
                          GetLastError() != ERROR_FILE_NOT_FOUND;
         ++attempt) {
        Sleep(250);
    }
    RemoveDirectoryW(workerDir.c_str());

    if (bench.runs.empty()) {
        finish(aborted ? L"Stopped before any test finished." : L"No test finished.", !aborted, {});
        return;
    }
    WriteFileText(summaryPath, Summarize(bench, MachineDescription(bench, battery, batteryOk)));
    int lower = 0;
    int higher = 0;
    int same = 0;
    for (const size_t s : bench.scenarios) {
        const energybench::Verdict verdict = CompareScenario(bench, s, 1).verdict;
        lower += verdict == energybench::Verdict::Lower ? 1 : 0;
        higher += verdict == energybench::Verdict::Higher ? 1 : 0;
        same += verdict == energybench::Verdict::NoDifference ? 1 : 0;
    }
    std::wstring result = aborted ? L"Stopped early, partial results. " : L"";
    if (lower + higher + same == 0) {
        result += L"Not enough tests finished to compare.";
    } else {
        result += L"Automatic used less energy in " + std::to_wstring(lower) + L", more in " +
            std::to_wstring(higher) + L", no clear difference in " + std::to_wstring(same) + L" of " +
            std::to_wstring(bench.scenarios.size()) + L" tests.";
    }
    finish(result, false, summaryPath);
}

int RunEnergyBenchWorker(int argc, LPWSTR* argv) {
    std::vector<std::wstring> args;
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return RunWorker(args);
}
