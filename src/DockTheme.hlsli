#pragma once

// Single source of truth for corner radii, shared by the D3D glass shader
// (compiled with DXC) and the C++ host. Units are points (px at 1x); both
// sides multiply by the display scale factor.
#define DOCK_CORNER_RADIUS_PT 20.0f

// Ink color for dock/popup glyphs and light-surface text: #212224.
// Consumers pick channel slots explicitly: popup DIB buffers are BGR-ordered,
// the GPU icon atlas is RGB-ordered.
#define DOCK_INK_R 33
#define DOCK_INK_G 34
#define DOCK_INK_B 36

// Dock-face chrome (Start/Search/clock/tray glyphs) is baked LIGHT and remapped
// in IconPS from wallpaper luma so it stays readable on dark glass. Quick /
// Dock Settings flyout text uses the same AdaptiveChromeInk cut via
// Renderer::SampleAdaptiveChromeInk.
#define DOCK_CHROME_INK_R 245
#define DOCK_CHROME_INK_G 245
#define DOCK_CHROME_INK_B 247

// Dock face tone map (linear lift), calibrated from solid swatches:
//   backdrop #000000 -> face #3a3a3a
//   backdrop #1f1f1f -> face #4e4e4e   (fit err < 0.5 LSB)
//   backdrop #ffffff -> face #e1e1e1
// face = lerp(OVER_BLACK, OVER_WHITE, blurredBackdrop). Shared by GlassPS and
// the CPU popup baker (Quick Settings / Dock Settings / context).
#define DOCK_FACE_OVER_BLACK (58.0f / 255.0f)
#define DOCK_FACE_OVER_WHITE (225.0f / 255.0f)
// Alias used by rim/specular accents (highlight body = over-white plate).
#define DOCK_FROST_OVER_WHITE DOCK_FACE_OVER_WHITE

// Legacy popup alpha floor. FrostAmount lerps toward 1 so the plate is opaque
// at full frost (same rule as the dock). Tint/mix kept for any residual refs.
#define DOCK_GLASS_TINT 0.829f
#define DOCK_GLASS_MIX 0.85f
#define DOCK_GLASS_ALPHA 0.97f

// Panel (Quick Settings / Dock Settings / context) - neutral dark glass.
// Charcoal plate (pre-sage Concept A+D) so live capture+blur reads without a
// sage wash. Dock bar keeps DOCK_FACE_* above; GlassPS branches on DOCK_FX_PANEL.
// Alpha 1.0: layered ULW must be fully opaque so the one-shot/live bake is the
// only backdrop (alpha < 1 composited a frozen snapshot OVER live desktop).
#define DOCK_PANEL_FACE_OVER_BLACK (26.0f / 255.0f)  // ~#1a1a1a charcoal
#define DOCK_PANEL_FACE_OVER_WHITE (68.0f / 255.0f)  // ~#444444 over light WP
// Light chrome ink on dark plate (forest sage ink is unreadable here).
#define DOCK_PANEL_INK_R DOCK_CHROME_INK_R
#define DOCK_PANEL_INK_G DOCK_CHROME_INK_G
#define DOCK_PANEL_INK_B DOCK_CHROME_INK_B
// Legacy icon-plate (unused by QS tiles since 1.1.57; kept for reference).
#define DOCK_PANEL_ICON_R 94
#define DOCK_PANEL_ICON_G 127
#define DOCK_PANEL_ICON_B 108
// Toggles + frost slider / QS tile + action accent (#98A869 olive sage).
#define DOCK_PANEL_TOGGLE_R 152
#define DOCK_PANEL_TOGGLE_G 168
#define DOCK_PANEL_TOGGLE_B 105
// Milk floor for charcoal plate readability (same as 1.1.52 dark glass).
#define DOCK_PANEL_PLATE_MIX_FLOOR 0.62f
// Floor frost for mica radii / blur so wallpaper detail dissolves under glyphs.
#define DOCK_PANEL_FROST_BLUR_FLOOR 0.45f
// Fully opaque plate: live look comes only from TickLivePopupGlass rebakes.
#define DOCK_PANEL_GLASS_ALPHA 1.0f

// Drop-shadow margin around the dock pill, shared by layout (window
// inflation, input-region inset, popup anchors) and the glass shader (pill
// inset, shadow band). Device px at 1x; both sides scale by display DPI.
#define DOCK_SHADOW_MARGIN_PT 18.0f

// Glass effect toggles packed into scene1.x (float-stored bitmask).
// DockRenderState composes these from the DockConfig bools; GlassPS
// decodes with FxEnabled(). All on = current look.
// Layout: bits 0-7 FX toggles, 8-13 halo icon count, 14 PANEL, 16-23 frost.
// Total stays below 2^24 so every bit survives the CPU float store exactly.
#define DOCK_FX_RIM 1
#define DOCK_FX_LENS 2
#define DOCK_FX_DISPERSION 4
#define DOCK_FX_BLUR 8
#define DOCK_FX_TINT 16
#define DOCK_FX_SPECULAR 32
#define DOCK_FX_SHADOW 64
#define DOCK_FX_THICKNESS 128
#define DOCK_FX_ALL 255
// Menu/popup plates: same GlassPS path, but no dock shadow margin ring.
// Bit 14 (not bit 24): bit 24 pushed the packed float into ULP=2 range and
// destroyed rim/lens/specular flags on every popup bake.
#define DOCK_FX_PANEL 0x4000u
