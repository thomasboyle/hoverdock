# DockCpuTemp

Small .NET 8 helper that references **LibreHardwareMonitorLib** and publishes Intel
CPU package temperature (and package watts) for Hoverdock's Quick Settings tile.

Native `Dock.exe` cannot take a NuGet reference, so this process is auto-started
by `SystemTemps.cpp` and writes:

`%LOCALAPPDATA%\LiquidGlassDock\cpu-temp.bin`

## Privileges

Reading package DTS/MSR via PawnIO requires an elevated process on Windows
(`CreateFile \\.\Device\PawnIO` returns Access Denied at medium IL). One-time
UAC installs scheduled task `HoverDockCpuTemp` (ONLOGON, Run Level Highest).
Later Dock launches only need `schtasks /Run` (no UAC).

Requires **PawnIO** installed system-wide (already present if Libre Hardware
Monitor / FanControl / OpenRGB installed it) and the **.NET 8** desktop runtime.

## Build

Produced automatically by the Dock CMake build (`DockCpuTempHelper` target) into
`$<TARGET_FILE_DIR:Dock>/cpu-temp/`.
