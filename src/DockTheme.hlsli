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
#define DOCK_GLASS_ALPHA 0.88f

// Drop-shadow margin around the dock pill, shared by layout (window
// inflation, input-region inset, popup anchors) and the glass shader (pill
// inset, shadow band). Device px at 1x; both sides scale by display DPI.
#define DOCK_SHADOW_MARGIN_PT 18.0f

// Glass effect toggles packed into scene1.x (float-stored bitmask, all
// values < 256 exact). DockRenderState composes these from the DockConfig
// bools; GlassPS decodes with FxEnabled(). All on = current look.
// High bits of the same word carry the live icon count for icon-calm
// halos (count << 8 | mask); total stays far below 2^24 (exactly kept).
#define DOCK_FX_RIM 1
#define DOCK_FX_LENS 2
#define DOCK_FX_DISPERSION 4
#define DOCK_FX_BLUR 8
#define DOCK_FX_TINT 16
#define DOCK_FX_SPECULAR 32
#define DOCK_FX_SHADOW 64
#define DOCK_FX_THICKNESS 128
#define DOCK_FX_ALL 255
