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

// Glass recipe shared by the dock shader (GlassPS) and the Quick Settings
// popup CPU compositor. Calibrated so the face meters #e1e1e1 over white.
#define DOCK_GLASS_TINT 0.829f
#define DOCK_GLASS_MIX 0.85f
#define DOCK_GLASS_ALPHA 0.88f

// Clear-variant face mix for the GPU dock only (popups stay on
// DOCK_GLASS_MIX). 40% background signal instead of 15%, so refraction,
// dispersion and blur stay visible instead of hiding behind flat tint.
// Metered over white: face outputs ~0.909 pre-premult, compositing to
// #ebebeb at DOCK_GLASS_ALPHA. Over black the face lands ~#828282, so
// bright icons keep contrast (Apple Clear-variant behavior).
#define DOCK_GLASS_FACE_MIX 0.60f
