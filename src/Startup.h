#pragma once

#include <string>

// Manages the per-user "Launch at startup" Run key:
//   HKCU\Software\Microsoft\Windows\CurrentVersion\Run\Hoverdock
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
