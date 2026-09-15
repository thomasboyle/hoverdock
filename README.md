# Liquid Glass Dock

Liquid Glass Dock is a single-process native Windows desktop overlay that hides the normal taskbar and replaces it with a centered macOS Dock-style launcher. It is written in C++ and Win32 with Direct3D 12, DXGI, DirectComposition, GDI capture, and HLSL only. The glass is a custom D3D12 pass; it does not use Acrylic, Mica, Fluent, WebView2, or third-party libraries.

`Dock.exe` is the only runtime artifact. HLSL is compiled to DXIL headers at build time and embedded into the executable.

## Requirements

- Windows 10 19041 or newer, with the latest Windows SDK installed
- Visual Studio 2022 with the Desktop development with C++ workload and MSVC x64 tools
- A GPU and driver exposing D3D12 Feature Level 12_0 or later and Shader Model 6.0 or later
- CMake 3.25 or later

No vcpkg, NuGet, Conan, package manager, Agility SDK, or runtime redistribution is required. This build uses inbox D3D12 and links only Windows SDK system libraries:

```text
d3d12 dxgi dcomp dwmapi shcore shell32 ole32 advapi32 windowscodecs gdi32
```

The binary probes feature levels in this exact order: 12_2, 12_1, then 12_0. It never assumes a feature level. It uses Shader Model 6.6 when the driver reports it; otherwise it uses the separately compiled SM 6.0 shaders for devices such as a GTX 1070 Ti-class GPU.

## Build

Open an x64 Native Tools Command Prompt for Visual Studio, from the repository root:

```bat
cmake -S . -B out -G "Visual Studio 17 2022" -A x64
cmake --build out --config Release
```

The executable is:

```text
out\bin\Release\Dock.exe
```

The CMake project asks MSVC for `/std:c++26` first and falls back to `/std:c++latest` only when the installed MSVC does not recognize the C++26 switch. `/permissive-`, `/W4`, `/WX`, `/EHsc`, `/Zc:__cplusplus`, and `/utf-8` are also enabled.

The Windows SDK `dxc.exe` is required at build time. CMake searches the SDK paths exposed by `WindowsSdkDir` and `WindowsSDKVersion`. If your SDK does not expose those variables, pass the exact SDK compiler path:

```bat
cmake -S . -B out -G "Visual Studio 17 2022" -A x64 ^
  -DDXC_EXECUTABLE="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe"
cmake --build out --config Release
```

DXC emits `vs_6_0`, `ps_6_0`, `vs_6_6`, and `ps_6_6` blobs into the build directory as generated C++ headers. The headers are compiled into `Dock.exe`; do not copy any generated shader files beside the EXE.

## Run

```bat
out\bin\Release\Dock.exe
```

The program is intentionally a single instance. It restores any previously hidden taskbar before it hides the current one, which is the failsafe for a prior unexpected process termination. It restores the taskbar on normal application shutdown and Windows session shutdown. If a hard crash or power loss leaves the taskbar hidden, start `Dock.exe` once and exit it normally to restore it.

Settings are written as UTF-8 to:

```text
%LOCALAPPDATA%\LiquidGlassDock\dock.ini
```

On first run, the dock imports taskbar pins in native order from the Windows 11 CloudStore taskbar store or Windows Taskband data, then uses the Windows 10 pinned-shortcut folder only to fill pins absent from those stores. Shell shortcuts are resolved through Windows Shell COM, including wrappers around AppsFolder AUMIDs, and recovered pins are persisted. Existing custom `dock.ini` files are preserved; an older file containing only the Explorer, Notepad, and Calculator defaults is upgraded from available OS pins. If no pin can be recovered, it falls back to File Explorer, Notepad, and Calculator. For UWP targets, launch is best effort through the AppsFolder shell target; running-window matching is intentionally limited to normal top-level desktop windows.

## Interaction

- Move the pointer into the full-width, 2-pixel bottom edge of the primary monitor to show the dock.
- The dock slides from below the monitor to its resting position over exactly 50 ms of QPC time.
- While the dock is showing or visible, moving the pointer above its resting top edge immediately starts the 200 ms hide animation. This also applies when the first post-hot-edge pointer sample is already above that edge.
- Leaving through the left or right side does not hide the dock. The pointer remains unrestricted inside the resting dock bounds, which makes edge-to-edge icon targeting practical.
- Start and Search are always the first two nonpersistent items. Their **Open** action sends the corresponding Windows keyboard shortcut; they cannot be pinned, closed, opened in Explorer, or reordered.
- Click a running app to focus it; Windows foreground policy remains authoritative. If Windows rejects focus, the app is flashed instead of using an input-attachment foreground-stealing workaround.
- Click a stopped app to launch it with `ShellExecuteEx`.
- Right-click an application icon for Focus/Open, Open location, Close, and Unpin. Transient running applications offer Pin instead. Right-click empty dock glass to pin the current foreground desktop application. UWP pinning and location are best effort.
- Drag a persistent icon and release it over another slot to persist a left-to-right reorder.
- Right-click empty glass and select **Show developer bounds** to draw the capsule edge. `F12` toggles the same option when the window has keyboard input.

The D3D12/DirectComposition renderer is intentionally hit-transparent. A separate titleless, topmost, 1-alpha layered input window follows the DPI-scaled capsule region, never activates the process, and receives dock clicks while it is visible. The dock is always on top, owns no taskbar button, uses Per-Monitor V2 DPI, and handles 100%, 125%, and 150% scaling. It is primary-monitor only in v1.

## Rendering and timing

The overlay is a transparent DirectComposition visual backed by a D3D12 `FLIP_DISCARD` composition swap chain with three buffers and `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`. It has one D3D12 device, one DIRECT queue, reusable frame allocators and command list, persistently mapped per-frame CBVs, a persistently mapped instanced-icon buffer, and a static shader-visible SRV heap for an icon texture array and desktop backdrop. No buffer, command allocator, descriptor layout, icon texture, or backdrop resource is allocated during the 50 ms show or 200 ms hide path. Presentation is synchronized and does not opt into tearing: tearing is not appropriate for this transparent composition overlay.

The render loop uses `QueryPerformanceCounter` for elapsed time and the DXGI frame-latency waitable object while the state machine is animating. It presents synchronized frames and targets the display cadence, up to 120 FPS on a 120 Hz display. A literal “120 unique frames in 50 ms at 120 Hz” cannot occur: 50 ms at 120 Hz contains 6 display intervals. This implementation produces one QPC-derived unique animation sample per available presentation interval, so the show interval has up to 6 unique visible positions and the 200 ms hide interval has up to 24 on a 120 Hz panel.

The capsule’s slide distance is its current DPI-scaled height plus a 10-DIP margin. Its easing is cubic smoothstep (`t²(3−2t)`) for `t = elapsed / duration`, where duration is 0.05 seconds while showing and 0.2 seconds while hiding. The window is content-sized from display item count, icon size, gaps, and horizontal padding; it is centered rather than monitor-width.

Before each hidden-to-show transition, a preallocated GDI DIB captures the dock rectangle with `BitBlt` and `CAPTUREBLT` while both dock windows are hidden. A precreated D3D12 command allocator and command list upload that snapshot into a reusable backdrop texture. The glass shader samples the real desktop with multi-tap frosted blur, edge-normal refraction, and Fresnel/specular rim highlights at a 0.6 alpha tint; capture failure keeps the prior snapshot or a transparent fallback. This remains one D3D12 device and one DIRECT queue, with no D3D11 desktop duplication. Instanced icon quads are sampled from high-resolution Windows shell images (or transparent Start/Search glyphs) with their native alpha; only the glass pass contributes a background behind app icons.

The native taskbar remains part of the primary monitor’s normal work area. The dock is deliberately positioned against the physical primary-monitor bottom edge after it hides the taskbar; it does not modify the system work area or replace Explorer.

## Project layout

```text
CMakeLists.txt        MSVC/DXC build definition
dock.default.ini      Default persisted pin order and settings
src/
  main.cpp            Win32 entry point
  DockApp.*           Overlay, input, state machine, taskbar safety, UI actions
  DockConfig.*        In-tree UTF-8 INI parser, taskbar import, and writer
  WindowCatalog.*     Window enumeration, launch, focus, close, pin actions
  Renderer.*          D3D12/DXGI/DirectComposition renderer, backdrop, and icon uploads
  Shaders.hlsl        Custom liquid-glass and icon HLSL
```

## v1 non-goals

Win+X, a full system tray, Task View, Copilot, Widgets, Explorer replacement, Mission Control, and Stage Manager are intentionally outside this version.
