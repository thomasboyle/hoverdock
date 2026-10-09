#pragma once

#include <Windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

struct BluetoothDeviceInfo {
    std::wstring id;
    std::wstring name;
    std::wstring status;
    bool connected = false;
    bool busy = false;
};

struct BluetoothSnapshot {
    bool ready = false;
    bool radioPresent = false;
    bool radioOn = false;
    bool discovering = false;
    int hiddenPaired = 0;
    std::wstring hint;
    std::vector<BluetoothDeviceInfo> paired;
    std::vector<BluetoothDeviceInfo> discovered;
};

// Paired devices, radio state, and on-demand discovery for Quick Settings.
// WinRT and inquiry run on a worker thread. GetSnapshot is safe from the UI thread.
class BluetoothService {
public:
    BluetoothService();
    ~BluetoothService();

    BluetoothService(const BluetoothService&) = delete;
    BluetoothService& operator=(const BluetoothService&) = delete;

    void Start(HWND notifyWindow, UINT notifyMessage);
    void Stop() noexcept;

    // forcePaired bypasses the paired-list cache (BT page open / user action).
    void RequestRefresh(bool forcePaired = false);
    void SetRadioEnabled(bool enabled);
    void Connect(const std::wstring& id);
    void Disconnect(const std::wstring& id);
    void StartDiscovery();
    void StopDiscovery();
    void Pair(const std::wstring& id);

    [[nodiscard]] BluetoothSnapshot GetSnapshot() const;
    // HOVERDOCK_BT_DRYRUN=1: radio / connect / disconnect / pair jobs are
    // simulated on the worker (logged, state updated in-memory only) so the
    // UI paths can be exercised without touching real devices. Enumeration
    // and discovery stay real (read-only).
    [[nodiscard]] static bool DryRun() noexcept;
    // "<label> tid=<GetCurrentThreadId()>" for HOVERDOCK_PROFILE call-path audits.
    [[nodiscard]] static std::string ThreadTag(const char* label);
    [[nodiscard]] bool IsPairing() const noexcept;
    // Lets the next state change post another paint. Call before reading the snapshot.
    void AllowNextNotify() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
