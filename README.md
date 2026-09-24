# Hoverdock ✨

A macOS-style dock for Windows — with real liquid glass.
<img width="910" height="112" alt="image" src="https://github.com/user-attachments/assets/5a1e06a1-a306-4b05-844b-031db09cf969" />

Hoverdock hides the taskbar and replaces it with a centered, floating dock rendered in native C++ / Direct3D 12. No Electron, no WebView, no Acrylic. Just one fast `Dock.exe`.

Hover the bottom edge → it slides in (50 ms). Move away → it disappears.

## Why you'll like it

- **Gorgeous glass** — custom D3D12 shader with frosted blur, refraction, and rim light, sampled from your actual wallpaper
- **Fast** — single process, zero allocations during animations, 120 Hz friendly
- **Familiar** — pins, running apps, drag-to-reorder, right-click to pin / close / reveal location
- **Thoughtful** — auto-imports your taskbar pins on first run, restores the taskbar on exit/crash
- **Quiet** — per-user install, launch at startup, silent auto-updates from GitHub Releases

Plus: Quick Settings, Start/Search shortcuts, Per-Monitor V2 DPI, UWP support.

## Get it

Download `Hoverdock-Setup-*.exe` from [Releases](../../releases) and run it. No admin needed.

Or build it yourself (Windows 10 19041+, VS 2022, CMake 3.25+):

```bat
cmake -S . -B out -G "Visual Studio 17 2022" -A x64
cmake --build out --config Release
out\bin\Release\Dock.exe
```

Installer (optional, per-user, silent `/S` supported):

```bat
cmake --build out --config Release --target installer
```

That's it — no vcpkg, NuGet, or extra SDKs. Just the Windows SDK.

## Use it

- **Show:** touch the bottom edge of your primary monitor
- **Launch / focus:** click an icon
- **Pin / close / locate:** right-click an icon
- **Pin current app:** right-click empty glass
- **Reorder:** drag an icon onto another slot
- **Settings:** gear icon → startup, updates, version

Config lives at `%LOCALAPPDATA%\LiquidGlassDock\dock.ini`.

Stuck with a hidden taskbar after a crash? Just launch `Dock.exe` once and quit — it restores everything.

## Notes

- Primary monitor only (v1)
- D3D12 Feature Level 12_0+ required
- MIT licensed — see [LICENSE](LICENSE)
