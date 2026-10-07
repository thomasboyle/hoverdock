# UI stutter compare — HoverDock mouse-over (2026-10-07)

Primary user claim: **mouse-over stutter** (dock icon / QS hover path).

Artifacts under `.cursor/skills/verify-hoverdock/artifacts/`.

| Run | Artifact folder | Present gap median (ms) | P95 (ms) | Max (ms) | Capture SumMs | CPU median 1-core % | Notes |
|-----|-----------------|------------------------:|---------:|---------:|--------------:|--------------------:|-------|
| Baseline | `ui-stutter-baseline-20261007-145830` | **28.53** | 55.84 | 158.33 | **3995** | 32.51 | CaptureLiveBackdrop / CaptureBackdrop dominate |
| Unit2 self-dirt | `ui-stutter-postfix-selfdirt-20261007-153033` | 26.08 | 41.91 | 146.82 | 306 | 42.76 | SkipBackdropSelfDirt=1758, BitBlt=2 |
| Unit3 | `ui-stutter-postfix-unit3-20261007-154117` | 21.9 | 41.43 | 84.77 | ~354 | 32.43 | TickGlint settle label; BT pointer-hot; snapshot equality |
| Unit4 | `ui-stutter-postfix-unit4-20261007-160040` | **20.93** | 42.38 | 187.32 | ~302 | 36.01 | BT timer only on BT QS page; 12s/4s; CollectPaired cache |

## Headline (before → after)

- QS harness Present gap median: **28.53 ms → 20.93 ms**
- Capture SumMs: **3995 → ~354** (unit3; unit4 ~302)
- Dock-only Present gap median (unit4): **0.94 ms** (icon sweeps, no OpenPerfMenus). **Dock-only baseline was not captured** — label after-only.

## Unit1

Unit1 (`ProbeBackdropUnchanged` on dirt path) was **reverted** before unit2. Self-dirt filter (unit2) is the kept DXGI fix. Probe helpers remain on main unused by this path.

## Kept changes

1. **Unit2** — DXGI self-dirt filter in `Renderer.cpp` (`DirtyAffectsOutsideSelf` / skip dock-HWND-only dirt).
2. **Unit3** — TickGlint settle-only `UpdateHoverLabel`; BT pointer-hot skip; `ApplyBluetoothSnapshot` equality paint skip.
3. **Unit4** — BT timer only on Bluetooth QS page; 12s idle / 4s after BT UI; Publish skip when unchanged; CollectPaired 12s cache.

## Limiter progression

- Baseline: CaptureBackdrop SumMs ~4s
- After unit2+: Capture early-out; paint / BT were next
- After unit4: PaintOverflowPopup / PaintQuickSettings / Capture top SumMs; BT HandleRefresh down to ~209 ms / count=2 in QS harness
