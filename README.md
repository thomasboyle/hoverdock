<h1 align="center">Hoverdock</h1>

<p align="center">
  <b>The liquid glass dock your Windows desktop has been missing.</b><br>
  Native C++ · Direct3D 12 · One tiny <code>Dock.exe</code>
</p>

<p align="center">
  <img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-blue">
  <img alt="Platform: Windows 10+" src="https://img.shields.io/badge/platform-Windows%2010%2B-0078D6">
  <img alt="Renderer: Direct3D 12" src="https://img.shields.io/badge/renderer-Direct3D%2012-8A2BE2">
</p>

<p align="center">
  <img width="867" height="98" alt="Hoverdock floating glass dock" src="https://github.com/user-attachments/assets/beab2db1-44ce-4fe2-b770-e97ed119394a" />
</p>
<p align="center">
  <img width="1004" height="98" alt="Hoverdock sliding in over the desktop" src="https://github.com/user-attachments/assets/f30daee5-0351-4000-933a-89997eb8a3c2" />
</p>

<p align="center">
  <a href="../../releases"><b>⬇️ Download the latest release</b></a>
</p>

---

## Your taskbar, but beautiful

Hoverdock hides the Windows taskbar and replaces it with a centered, floating dock made of gorgeous glass. Touch the bottom edge of your screen and it slides in within **50 ms**. Move away and it's gone, leaving your whole screen to your work.

No Electron. No WebView. No Acrylic hacks. Just a single, fast native executable that draws everything itself with Direct3D 12.

## Why you'll love it

- 🪟 **Real liquid glass.** A custom D3D12 shader delivers frosted blur, refraction, and rim light, sampled from your *actual* wallpaper, so the dock always matches your desktop.
- ⚡ **Genuinely fast.** Single process, zero allocations during animations, and smooth at 120 Hz.
- 🧭 **Instantly familiar.** Pinned apps, running apps, drag-to-reorder, and a right-click menu to pin, close, or reveal an app's location.
- 🛟 **Safe by design.** It auto-imports your existing taskbar pins on first run, and restores your taskbar on exit or crash. You can't get locked out.
- 🤫 **Stays out of your way.** Per-user install (no admin), launch at startup, and silent auto-updates from GitHub Releases.

**And more:** Quick Settings, Start and Search shortcuts, Per-Monitor V2 DPI awareness, UWP app support, and a weather widget.

## Get started in 30 seconds

1. Download **`Hoverdock-Setup-*.exe`** from the [Releases page](../../releases).
2. Run it. No admin rights needed.
3. Move your mouse to the bottom edge of your screen. That's it.

Your taskbar pins are imported automatically, so your dock is ready the first time you see it.

## How to use it

| I want to... | Do this |
| --- | --- |
| Show the dock | Touch the bottom edge of your monitor |
| Launch or focus an app | Click its icon |
| Pin, close, or locate an app | Right-click its icon |
| Reorder icons | Drag an icon onto another slot |
| Change startup, updates, or check version | Click the gear icon in Quick Settings |

Your config lives at `%LOCALAPPDATA%\LiquidGlassDock\dock.ini`.

> **Taskbar still hidden after a crash?** Launch `Dock.exe` once and quit it. Everything is restored.

## Build it yourself

You need Windows 10 (19041+), Visual Studio 2022, and CMake 3.25+. No vcpkg, no NuGet, no extra SDKs. Just the Windows SDK.

```bat
cmake -S . -B out -G "Visual Studio 17 2022" -A x64
cmake --build out --config Release
out\bin\Release\Dock.exe
```

Want to build the installer too? It's per-user and supports silent installs with `/S`:

```bat
cmake --build out --config Release --target installer
```

## Requirements

- Windows 10 (build 19041) or newer
- A GPU supporting Direct3D 12 Feature Level 12_0+

## Contributing

Found a bug, have an idea, or made something pretty? Issues and pull requests are welcome. If Hoverdock makes your desktop nicer, a ⭐ helps others find it.

## License

MIT. See [LICENSE](LICENSE).

## Credits

- **Weather icons:** [Meteocons](https://meteocons.com) by Bas Milius (MIT). Fill-style static frames are vendored under `assets/meteocons/`.
- **Weather data:** [Open-Meteo](https://open-meteo.com) forecast API (no API key needed).
