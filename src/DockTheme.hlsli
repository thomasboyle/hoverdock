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

// Glass tint/alpha shared by the dock shader (GlassPS) and the Quick
// Settings popup CPU compositor. Calibrated so the popup face meters
// #e1e1e1 over white. (The dock face itself uses conditional transmission
// tinting in GlassPS section 6 instead of a flat mix.)
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
// Bit 3 retired (was FrostBlur on/off); frost notches now ride bits 17-18.
#define DOCK_FX_TINT 16
#define DOCK_FX_SPECULAR 32
#define DOCK_FX_SHADOW 64
#define DOCK_FX_THICKNESS 128
#define DOCK_FX_FROSTB0 (1 << 17)
#define DOCK_FX_FROSTB1 (1 << 18)
#define DOCK_FX_ALL 255
