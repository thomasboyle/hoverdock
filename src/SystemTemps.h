#pragma once

#include <Windows.h>

#include <string>

// CPU / GPU temperatures and package/board watts for the Quick Settings home
// tile. All sensor I/O (WMI, PDH, NVML, nvidia-smi, shared memory) runs on one
// background worker; the UI thread only takes a mutex to read the latest
// published values.
//
// Task Manager (Windows 11 Performance) shows GPU temperature via the WDDM /
// graphics-driver path (same family as NVML) but does NOT expose a CPU package
// Celsius reading -- confirmed on 25H2 build 26200 (taskmgr has gpuTemperature*
// strings only). There is no public WinRT / CallNtPowerInformation / documented
// Kernel Performance API for Intel package die temp. ACPI Thermal Zone counters
// (the Performance Monitor "Thermal Zone Information" set) are often a fixed
// motherboard placeholder on desktop boards.
//
// CPU temperature (prefer real package/die sensors; skip the fixed ACPI
// 27.85 C / 301.0 K placeholder many Z390 boards report):
//   1. LibreHardwareMonitor / OpenHardwareMonitor WMI (needs LHM/OHM GUI running).
//   2. Bundled DockCpuTemp helper (LibreHardwareMonitorLib NuGet) writing
//      %LOCALAPPDATA%\LiquidGlassDock\cpu-temp.bin. Auto-started by Dock.
//      Reads Intel "CPU Package" via PawnIO; one-time elevation installs the
//      HoverDockCpuTemp scheduled task so later launches need no UAC.
//   3. Core Temp shared memory ("CoreTempMappingObject") when Core Temp is
//      running -- max core reading, converted from F / delta-to-TjMax.
//   4. HWiNFO shared memory ("Global\HWiNFO_SENS_SM2") when Shared Memory
//      Support is enabled -- prefers "CPU Package" / Tctl/Tdie labels.
//   5. MSAcpi_ThermalZoneTemperature (often admin-only; rejected if stuck at
//      the ACPI placeholder).
//   6. PDH Thermal Zone Information (same ACPI data, no admin).
//
// Without the helper (or LHM/Core Temp/HWiNFO), Intel package °C is unavailable
// on this class of hardware -- Dock will show watts-only (RAPL) rather than a
// fake 27.85 C.
//
// CPU package watts (RAPL), undelevated when possible:
//   1. LHM/OHM SensorType=Power ("CPU Package") when running.
//   2. PDH Energy Meter RAPL_Package*_PKG\Power (milliwatts -> W). Available
//      without admin on this Z390 + 9900K class hardware.
//
// GPU temperature: NVML, then LHM/OHM, then nvidia-smi.
// GPU board watts: NVML nvmlDeviceGetPowerUsage (mW). On pre-Turing cards
// (GTX 1070 Ti / Pascal) this is entire-board draw -- the same figure
// nvidia-smi reports as power.draw ("entire board"). Newer MODULE/scope APIs
// are used when present; otherwise this legacy reading is documented as board
// total for Pascal.
struct SystemTempsReading {
    // Whole degrees Celsius, or -1 when no trustworthy sensor is available.
    int cpuC = -1;
    int gpuC = -1;
    // Whole watts (package / board), or -1 when unavailable.
    int cpuW = -1;
    int gpuW = -1;
    // Human-readable source of each value ("LibreHardwareMonitor", "NVML", ...).
    std::wstring cpuSource;
    std::wstring gpuSource;
    std::wstring cpuPowerSource;
    std::wstring gpuPowerSource;
    ULONGLONG stamp = 0;  // GetTickCount64 of the poll that produced this reading
};

// Keep the worker polling (~2 s cadence) for a few seconds after the last call.
// notifyMsg is posted to notifyHwnd whenever a poll changes a displayed value.
// Cheap: never blocks on sensor I/O.
void RequestSystemTemps(HWND notifyHwnd, UINT notifyMsg) noexcept;

// Copy the latest published reading. Returns false when nothing has been
// published yet.
bool LatestSystemTemps(SystemTempsReading& out);
