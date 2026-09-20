#pragma once

#include <string>

// Manages the per-user "Launch at startup" state through two channels:
//   1. HKCU\Software\Microsoft\Windows\CurrentVersion\Run\Hoverdock
//   2. A per-user Task Scheduler logon task named "Hoverdock" (no delay).
//
// The Run key alone is slow: Explorer starts Run entries late in logon and
// Windows intentionally delays them (StartupDelayInMSec), which is the ~30 s
// "original taskbar visible" gap after login. A logon-triggered scheduled
// task with zero delay fires much earlier, alongside Explorer, so the dock
// can hide the native taskbar sooner. Both entries launch the same exe; the
// single-instance mutex makes the loser exit instantly, so keeping both is
// safe and gives a fallback when Task Scheduler is unavailable.
// Per-user (no elevation) so the dock itself can toggle it and the
// per-user NSIS installer can set it without UAC.
class Startup {
public:
    [[nodiscard]] static std::wstring CurrentExecutablePath();
    [[nodiscard]] static std::wstring StartupCommand();
    [[nodiscard]] static bool IsEnabled();
    static bool SetEnabled(bool enabled);
    // Reconciles the registry with the persisted config value. Called once at
    // startup so a moved/copied Dock.exe repairs a stale Run entry and an
    // installer-driven value does not fight the settings toggle.
    static void SyncWithConfig(bool configEnabled);

private:
    Startup() = delete;
};
