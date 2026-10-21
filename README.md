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
- 🔴 **Notification badges.** Unread counts from Teams, Outlook, WhatsApp and any other app that badges its Windows taskbar icon appear on its dock icon, just like macOS. Follows the Windows *Show badges on taskbar apps* setting.
- 🛟 **Safe by design.** It auto-imports your existing taskbar pins on first run, and restores your taskbar on exit or crash. You can't get locked out.
- 🔎 **Spotlight for Windows.** Press **Ctrl + Space** and a glass search panel appears above the dock. Apps, files and folders match as you type, straight from the NTFS Master File Table, and stay current through the change journal. Results arrive in milliseconds, even on drives with millions of files.
- ✨ **Ask AI from Search.** Type a question and press **Ctrl + Enter** for a streamed answer from Groq (Qwen 3.8 27B by default) with formatted text, tables, highlighted code, links and images. When it searches the web, a **Sources** badge lists every page it used.
- 🤫 **Stays out of your way.** Per-user install (no admin), launch at startup, and silent auto-updates from GitHub Releases.

**And more:** Quick Settings, a Start shortcut, Per-Monitor V2 DPI awareness, UWP app support, and a weather widget.

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
| Search apps, files and folders | Press **Ctrl + Space** (or click the Search tile), type, then **Enter** |
| Show a result in File Explorer | **Shift + Enter** (Ctrl + click) |
| Copy a result's path | **Ctrl + C** |
| Ask AI about what you typed | **Ctrl + Enter** (Esc goes back to results) |
| Attach an image for AI | **Ctrl + V** an image, or click 📎 in the search bar |

### Search

Search reads file names straight from the Master File Table of every fixed NTFS drive, then follows the USN change journal so new, renamed and deleted files show up within about a second. Nothing is crawled or re-scanned.

Reading the MFT needs administrator rights, and Hoverdock never runs elevated. Until you opt in, Search covers your user folders. Choose **Search this entire PC** in the results to approve, once, a small on-demand task. Approving copies the indexer to `%ProgramFiles%\Hoverdock` (so nothing user-writable ever runs elevated). When Hoverdock starts indexing, the task runs it with `--mft-dump`, it streams the file names to the dock over a private pipe, and then exits. Following the change journal doesn't need elevation.

Ctrl+Space is claimed system-wide while the dock runs, so apps that use it (code-completion in editors, some IME on/off toggles) won't see it. If you rely on that, set `SearchHotkey=0` under `[Dock]` in `dock.ini` and open Search from the dock tile.

### Ask AI

Type a question into Search and press **Ctrl + Enter**. The answer streams into the panel as rich text: headings, lists, tables, syntax-highlighted code blocks (with a **Copy** button), clickable links and images. Scroll with the mouse wheel or the arrow keys, and press **Ctrl + C** to copy the whole answer. When the model looks something up online, a **Sources** badge appears at the bottom; click it to see, and open, every page it used.

To ask about a picture, paste it with **Ctrl + V** (a screenshot, a copied image, or image files copied in Explorer) or click the 📎 button in the search bar to pick files. Up to 3 images ride along with your question as thumbnails under the bar; click a thumbnail's × or press **Backspace** on an empty query to remove one. Press **Ctrl + Enter** with no text to just have the images described. Images are downscaled and sent only to Groq with your question.

Answers come from [Groq](https://console.groq.com) using your own API key (creating one is free):

1. Open Quick Settings, click the gear, and choose **Groq API key** under the AI section.
2. Paste your key and press **Enter**. It's stored in `dock.ini` encrypted for your Windows account (DPAPI). A `GROQ_API_KEY` environment variable takes priority if set.
3. Optionally change **AI model** (default `qwen/qwen3.8-27b`) to any Groq chat model.

Web search uses Groq's built-in browser search on the same key, so nothing else needs setting up.

Your config lives at `%LOCALAPPDATA%\LiquidGlassDock\dock.ini`.

> **Taskbar still hidden after a crash?** Launch `Dock.exe` once and quit it. Everything is restored.

### Automatic energy mode

Quick Settings → **Power** → **Energy Mode** works like the macOS menu. **Low Power** uses Windows' Power saver plan and **High Power** uses Ultimate Performance. **Automatic** switches to a *Hoverdock Automatic* power plan, and while that plan is active Hoverdock manages it for you:

- Most of the time it runs the processor exactly like **Low Power** (Windows' Power saver processor settings), so general use is as efficient as possible. Display and sleep settings stay as in Balanced.
- When an app needs full power, it switches the processor to **High Power** (no down-clocking, performance-first energy preference, aggressive boost, no core parking, PCIe link power saving off), on mains or battery. High Power kicks in for:
  - **Games in focus**: anything in a Steam, Epic, GOG, Xbox, Riot, Ubisoft, EA or Rockstar library, games Windows Game Bar recognises, exclusive fullscreen apps, and borderless fullscreen apps with real 3D load.
  - **Creative tools you're driving**, such as Premiere Pro, After Effects, DaVinci Resolve, Blender, Cinema 4D, Unreal, Unity, Photoshop or Ableton. Scrubbing a timeline, orbiting a viewport or painting keeps them at full power.
  - **Renders and exports**: a creative tool doing sustained CPU or GPU work, even when it's minimised.
  - **Heavy work in the focused app**, such as a compile.
- Games and creative tools are never throttled by Windows' background power saving (EcoQoS) while they run.

The Automatic row shows **High Power** while it's boosting an app and **Low Power** the rest of the time. It reacts in milliseconds: switching to a game or touching the mouse or keyboard in a creative tool goes to High Power straight away, and work you start (an export, a render, a build) is confirmed within about a third of a second. Each boost is held a few seconds after the work stops, so power doesn't flicker on and off. Picking Low or High Power hands control back to Windows and undoes everything Automatic changed. If you were on Balanced (which Hoverdock already showed as Automatic), it moves you to the Automatic plan once. Uninstalling Hoverdock switches you back to Balanced and removes the plan.

## Build it yourself

You need Windows 10 (19041+), Visual Studio 2022, and CMake 3.25+. No vcpkg, no NuGet, no extra SDKs. Just the Windows SDK.

```bat
cmake -S . -B out -G "Visual Studio 17 2022" -A x64
cmake --build out --config Release
out\bin\Release\Dock.exe
```

Search engine tests and benchmark: configure with `-DDOCK_BUILD_TESTS=ON`, then run `FileSearchIndexTests.exe`, `FileSearchIndexBench.exe` and, from an elevated prompt, `FileSearchServiceTests.exe`. The portable index tests also build anywhere: `c++ -std=c++20 -O2 -I src tests/FileSearchIndexTests.cpp src/FileSearchIndex.cpp`.

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
