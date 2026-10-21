#pragma once

#include "AutoPowerPolicy.h"

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

// The Automatic energy mode. Choosing Automatic in Quick Settings activates
// the "Hoverdock Automatic" power plan (a copy of Balanced for display and
// sleep); while that plan is active this engine runs on a background thread
// and:
//   - samples the foreground app, user input (keyboard, mouse, gamepad),
//     per-app CPU load (one NtQuerySystemInformation call for every process)
//     and GPU load (kernel D3DKMT statistics, PDH as fallback), live audio
//     sessions, and camera / microphone use,
//   - asks autopower::Policy whether demanding work needs full power, trusting
//     apps it has learned are demanding (autopower-history.txt) sooner,
//   - switches the plan's processor policy between low power (the Power saver
//     plan's processor settings, as efficient as Hoverdock's Low Power) and
//     high power (minimum state 100%, EPP 0, aggressive boost, no core
//     parking, PCIe ASPM off, full-strength latency hints) on both AC and
//     battery,
//   - exempts games, creative tools, AI runtimes and apps using audio, the
//     camera or the microphone from Windows power throttling (EcoQoS) so a
//     minimized export, a call or a focused game is never slowed.
// Every plan switch is timed and traced (OutputDebugString).
// Picking Low or High Power (or another plan in Windows) makes the engine go
// dormant and restore everything it changed.
struct AutoPowerStatus {
    // Hoverdock Automatic plan is active and the engine is managing it.
    bool engaged = false;
    bool fullPower = false;
    autopower::BoostReason reason = autopower::BoostReason::None;
    std::wstring appName;
};

class AutoPowerEngine {
public:
    AutoPowerEngine() = default;
    ~AutoPowerEngine();

    AutoPowerEngine(const AutoPowerEngine&) = delete;
    AutoPowerEngine& operator=(const AutoPowerEngine&) = delete;

    // Starts the worker. notifyMsg is posted to notifyHwnd when Status changes.
    // adoptBalanced moves a machine on Balanced (which Hoverdock has always
    // shown as Automatic) onto the Hoverdock Automatic plan once.
    void Start(HWND notifyHwnd, UINT notifyMsg, bool adoptBalanced);
    // Stops the worker and restores the calm plan and process throttling.
    void Stop();
    // Re-evaluate now with a fresh load sample (foreground changed, energy
    // mode picked).
    void Wake() noexcept;
    // Called for every mouse (low-level hook) and keyboard (raw input) event;
    // commit = a click or key, the inputs that start work. Costs a few relaxed
    // atomic operations unless the engine asked to be woken: by a commit when
    // not at High Power, or by any input while a creative tool or a released
    // game is focused or the machine is parked. Each such wake starts a short
    // probe of the focused app's load.
    void NoteInput(bool commit) noexcept;
    // Keyboard input reaches NoteInput too (raw input registered), so no
    // keyboard fallback polling is needed.
    void SetKeyboardInputAvailable(bool available) noexcept;

    [[nodiscard]] AutoPowerStatus Status() const;

    // Create the Hoverdock Automatic plan when missing and make it active.
    [[nodiscard]] static bool Activate();
    [[nodiscard]] static bool IsPlanActive() noexcept;

private:
    void ThreadMain();
    void Signal(bool forceSample, bool commit = false) noexcept;
    // Active power plan change (GUID_ACTIVE_POWERSCHEME) from the power service.
    static ULONG CALLBACK PlanChanged(PVOID context, ULONG type, PVOID setting);

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_thread;
    bool m_stop = false;
    bool m_woken = false;
    bool m_forceSample = false;
    bool m_commitWake = false;
    std::atomic<bool> m_wantInput{false};
    std::atomic<bool> m_wantCommit{false};
    std::atomic<bool> m_keyboardInput{false};
    std::atomic<DWORD> m_hookInputTick{0};
    // 0 = unknown (poll the active plan), 1 = ours active, 2 = another plan.
    std::atomic<int> m_planState{0};
    HWND m_notifyHwnd = nullptr;
    UINT m_notifyMsg = 0;
    AutoPowerStatus m_status;
};

// Runs powercfg.exe hidden with the given arguments; true on exit code 0.
[[nodiscard]] bool RunPowercfg(const std::wstring& arguments);
