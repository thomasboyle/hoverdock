#pragma once

#include <Windows.h>

#include <string>

// CPU / GPU temperatures for the Quick Settings home tile. All sensor I/O
// (WMI, PDH, NVML, nvidia-smi) runs on one background worker; the UI thread
// only takes a mutex to read the latest published values.
struct SystemTempsReading {
    // Whole degrees Celsius, or -1 when no trustworthy sensor is available.
    int cpuC = -1;
    int gpuC = -1;
    // Human-readable source of each value ("LibreHardwareMonitor", "NVML", ...).
    std::wstring cpuSource;
    std::wstring gpuSource;
    ULONGLONG stamp = 0;  // GetTickCount64 of the poll that produced this reading
};

// Keep the worker polling (~2 s cadence) for a few seconds after the last call.
// notifyMsg is posted to notifyHwnd whenever a poll changes a displayed value.
// Cheap: never blocks on sensor I/O.
void RequestSystemTemps(HWND notifyHwnd, UINT notifyMsg) noexcept;

// Copy the latest published reading. Returns false when nothing has been
// published yet.
bool LatestSystemTemps(SystemTempsReading& out);
