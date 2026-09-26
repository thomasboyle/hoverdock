# Hoverdock 1.1.45 CPU cut verification

Captured 2026-09-26 Europe/London (BST). Target: interactive peak ≤1.0% all-cores (PerfProfiler-style cpu_interval_pct / logical processors).

## Meta

| Field | Value |
|---|---|
| Machine | Thomas-PC |
| Dock PID | **18484** |
| Version | **1.1.45** (`HoverdockVersion=1.1.45`) |
| Exe | `%LOCALAPPDATA%\Programs\Hoverdock\Dock.exe` |
| CheckForUpdates | **0** |
| Logical processors | 16 |
| Sampler | External PerfProfiler-equivalent (GetProcessTimes, 200ms, all-cores %) |
| Scenario | `Hoverdock.OpenPerfMenus` → QS + Dock Settings confirmed on-screen; 30s still + 20s slow hover |

## Result

| Metric | Value |
|---|---:|
| Samples (drop first 2) | 248 |
| **Peak cpu_interval_pct** | **0.488%** |
| **Avg cpu_interval_pct** | **0.006%** |
| Samples > 1.0% | **0** |

**PASS** vs ≤1.0% all-cores.

Prior baseline peak was ~4.37% all-cores (1.1.41-era interactive profile).

## What changed (1.1.42 → 1.1.45)

1. **Exact-size GDI capture DIB pool (3 slots)** + `SetICMMode(ICM_OFF)` — stops CreateDIBSection+ICM thrash when QS/settings/context round-robin different WxH.
2. **DWM + content-hash skip** for live panel BitBlt/GPU; per-size `dwmAtHash`.
3. **Adaptive backdrop timer** no longer forces 120 Hz solely because menus are open.
4. **Backdrop change serial gate** — live menu glass BitBlt/GPU only when dock backdrop detects desktop change (or 2s forced refresh). Preserves live glass over moving wallpaper; idle menus stay quiet.
5. **Skip Apply/Present** when glass bake bytes are identical (`memcmp`); hover fast-path only runs when Apply changed pixels.
6. Hidden `RegisterWindowMessage(L"Hoverdock.OpenPerfMenus")` / `ClosePerfMenus` for agent verification (no visual change).

## Artifacts

- `D:\C++\hoverdock\perf\verify-1142\Hoverdock-perf-menus-confirmed-20260926-181609.log`
- `D:\C++\hoverdock\perf\verify-1142\cpu-summary-menus-confirmed.txt`
- Matching PDB: `out\bin\Release\Dock.pdb` (also installed beside exe)

## Visual

No shader/frost/lens/rim changes. Glass still updates when desktop content under menus moves (backdrop serial advances). Static desktop: no 120 Hz rebake.
