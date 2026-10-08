#include "DockTheme.hlsli"

// Refraction-only diagnostic (see GlassPS). 1 = raw warped backdrop,
// no material. Flip back to 0 after the warp check.
#define LIQUID_DEBUG_REFRACTION_ONLY 0

cbuffer FrameData : register(b0)
{
    float4 scene0; // output width, output height, glass alpha, dock scale
    float4 scene1; // fx bitmask | icon count, DPI scale, dev bounds, backdrop valid
    float4 scene2; // pointer glint x, y (output px), strength 0..1, unused
};

struct IconInstance
{
    float4 iconRect;
    float4 iconMeta;
};

StructuredBuffer<IconInstance> iconInstances : register(t0);
Texture2DArray iconTexture : register(t1);
Texture2D backdropTexture : register(t2);
Texture2D blurTemp : register(t3);
Texture2D blurTemp2 : register(t4);
SamplerState linearClamp : register(s0);

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint textureIndex : TEXCOORD1;
    nointerpolation uint instanceIndex : TEXCOORD2;
};

// ---------------------------------------------------------------------------
// 1. Core geometry: superellipse / squircle SDF (Apple-like continuous
//    curvature, n = 4) with analytic interior distance.
//
//    sd = (|qx|^n + |qy|^n)^(1/n) + min(max(qx,qy),0) - r,
//    q = |p| - (halfSize - r).
//
//    The `outside` term uses the L4 norm (sqrt(sqrt(x^4+y^4))) so corners have
//    continuous curvature instead of circular arcs. The `inside` term gives the
//    true negative distance deep in the interior (the old max(...,0) version
//    saturated at -r, which is fine for a thin bevel but wrong for height
//    profiles). Gradient magnitude is ~1 near the edge; surface normals come
//    from the SDF gradient as in CASDFLayer.
// ---------------------------------------------------------------------------
float SdSquircleBox(float2 position, float2 halfSize, float cornerRadius)
{
    float2 q = abs(position) - (halfSize - cornerRadius);
    float qx = max(q.x, 0.0);
    float qy = max(q.y, 0.0);
    float quartic = qx * qx * qx * qx + qy * qy * qy * qy;
    float outside = sqrt(sqrt(max(quartic, 0.0)));
    float inside = min(max(q.x, q.y), 0.0);
    return outside + inside - cornerRadius;
}

// Convex slab height vs. normalized edge distance x in [0,1] (0 = outer rim,
// 1 = flat interior). Superellipse n=4 profile: f(x) = (1-(1-x)^4)^(1/4).
// Thickness is highest in the center and tapers toward the rim; the slope is
// steepest at the rim, which is where refraction / Fresnel / dispersion peak.
float BevelHeight(float oneMinusX4)
{
    return sqrt(sqrt(saturate(1.0 - oneMinusX4)));
}

// Normalized slope df/dx = (1-x)^3 / (1-(1-x)^4)^(3/4), clamped to keep the
// rim tilt finite (true superellipse is vertical at x=0). Max ~3.5 is ~74 deg.
float BevelSlopeN(float oneMinusX, float oneMinusX4)
{
    float numer = oneMinusX * oneMinusX * oneMinusX;
    float denomBase = max(1.0 - oneMinusX4, 1e-4);
    // pow(x, 0.75) = sqrt(sqrt(x^3)); use pow for clarity, safe on [1e-4,1].
    float denom = pow(denomBase, 0.75);
    return min(numer / max(denom, 1e-3), 3.5);
}

// Soft asymptotic cap for refraction pull. Replaces hard clamp(d,0,lensMax)
// which left a C1 kink (visible contour) where the outer plateau met the
// natural falloff toward the dock center. Approaches lensMax as d grows so
// opposite-rim white fringe stays suppressed, without a hard edge.
float SoftLensCap(float d, float lensMax)
{
    const float m = max(lensMax, 1e-3);
    return d * rsqrt(1.0 + (d * d) / (m * m));
}

// Glass effect toggles packed in scene1.x (DOCK_FX_* bits, DockTheme.hlsli).
// scene1.x is an integer-valued float from a packed UINT that stays below
// 2^24, so every bit survives the constant-buffer store exactly. Bit tests
// go through uint — do not add 0.5 first: once frost occupies bits 16-23 the
// magnitude can reach 2^23 (ULP=1) and +0.5 rounds away low FX bits (rim,
// lens, etc.). High bits also carry halo count (8-13) and PANEL (14).
float FxEnabled(float packed, float bit)
{
    return (((uint)packed) & (uint)bit) != 0u ? 1.0 : 0.0;
}

// Bevel (lens band) width. Context panels refract as one full-face slab. The
// dock, and text panels (DOCK_FX_DOCK_FACE), confine the lens to an edge band
// so the center stays flat. Shared by GlassPS and ComputeFrostUVs.
float GlassBevelWidth(float2 halfSize, float cornerRadius, float dpi, bool isPanel)
{
    const float faceBevel = max(min(halfSize.x, halfSize.y) * 0.95, 8.0 * dpi);
    const float edgeBevel = clamp(cornerRadius * 1.1, 8.0 * dpi, faceBevel);
    if (!isPanel)
    {
        return edgeBevel;
    }
    const float labelFace = FxEnabled(scene1.x, (float)DOCK_FX_LABEL_FACE);
    if (labelFace > 0.5)
    {
        return edgeBevel;
    }
    const float textPanel = FxEnabled(scene1.x, (float)DOCK_FX_DOCK_FACE);
    const float textBand = clamp(DOCK_TEXT_PANEL_BEVEL_PT * dpi, 8.0 * dpi, faceBevel);
    return textPanel > 0.5 ? textBand : faceBevel;
}

// Icon-calm halos: 1 on/near an icon rect, 0 beyond a 10px feather. Live
// count rides bits 8-13 of scene1.x; lookups capped at 64.
float IconHaloCalm(float2 pixel, float dpi)
{
    const uint haloCount = min(((uint)scene1.x >> 8) & 63u, 64u);
    float minIconDist = 1e9;
    for (uint haloIndex = 0u; haloIndex < 64u; ++haloIndex)
    {
        if (haloIndex >= haloCount)
        {
            break;
        }
        const float4 haloRect = iconInstances[haloIndex].iconRect; // l,t,w,h px
        const float2 haloCenter = haloRect.xy + haloRect.zw * 0.5;
        const float2 haloQ = abs(pixel - haloCenter) - haloRect.zw * 0.5;
        minIconDist = min(minIconDist, length(max(haloQ, 0.0)));
    }
    return 1.0 - smoothstep(0.0, 10.0 * dpi, minIconDist);
}

// Slope damping from the halos. The dock's lens band already sits outside
// the icon field, so it only needs a light calm; panels keep the strong one.
float IconHaloDamp(float haloCalm, bool isPanel)
{
    return 1.0 - haloCalm * (isPanel ? 0.9 : 0.35);
}

VertexOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0),
    };
    const float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0),
    };

    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    output.textureIndex = 0;
    output.instanceIndex = 0;
    return output;
}

VertexOutput IconVS(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const float2 corners[6] = {
        float2(0.0, 0.0),
        float2(0.0, 1.0),
        float2(1.0, 0.0),
        float2(1.0, 0.0),
        float2(0.0, 1.0),
        float2(1.0, 1.0),
    };

    const IconInstance icon = iconInstances[instanceId];
    const float2 pixelPosition = icon.iconRect.xy + corners[vertexId] * icon.iconRect.zw;
    const float2 normalized = pixelPosition / scene0.xy;
    VertexOutput output;
    output.position = float4(normalized * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = corners[vertexId];
    output.textureIndex = (uint)icon.iconMeta.w;
    output.instanceIndex = instanceId;
    return output;
}

float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// ---------------------------------------------------------------------------
// 5. Separable Gaussian frost, split across two passes (smooth by
//    construction: dense axis coverage, fixed taps, no rotation, no radius
//    jitter, no dither - nothing in the blur can add grain or bands).
//    True 1D Gaussian, sigma = blurPx/2 over taps -8..+8 (see kGaussW).
//    Tap spacing is blurPx/8 so the kernel stays dense (<=2px gaps at 1x,
//    bilinear-filtered) at every radius; the old -4..+4 kernel spaced taps
//    blurPx/4 apart and skipped whole pixels, which read as blocky
//    pixelation on text/edges behind the dock. Radius is modulated by slab
//    height f. Each channel is blurred around its own Snell-displaced UV so
//    dispersion survives both axes.
// ---------------------------------------------------------------------------
// Optical constants shared by both separable passes (GlassPS + BlurHPS).
// Both passes must compute identical UVs or the split is invalid.
static const float kLensGain = 2.55;
static const float kFringeBoost = 14.0;
// Soft max refraction pull (px at 1x, scaled by dpi). Without a cap the
// full-span slab drags content from 20-40px away to the opposite rim, so a
// white window overlapping only the top paints a white fringe along the
// bottom edge over a dark backdrop. SoftLensCap approaches this limit
// without a hard clamp kink (which read as a cutoff line toward center).
static const float kLensMaxPx = 10.0;
// Refraction fades to zero across this rim band (px at 1x, scaled by dpi) so
// the AA edge (mask < 1) samples the true backdrop behind instead of a
// refracted sample from the interior. Otherwise the blended fringe reads as
// a bright line over dark content.
static const float kLensRimFadePx = 3.0;
// Frost mica radii: clear-glass veil (left/mid) vs full dissolve (right).
// FrostAmount lerps between these; C++ also ramps H/V pass count above mid.
static const float kMicaClearRim = 5.0;
static const float kMicaClearCore = 4.0;
static const float kMicaFullRim = 16.0;
static const float kMicaFullCore = 16.0;
// Legacy aliases (ComputeFrostUVs / SampleGlassAxis call sites).
static const float kMicaBlurRim = kMicaFullRim;
static const float kMicaBlurCore = kMicaFullCore;

// Map frostAmount to per-axis mica radii.
// 0: none; ~0.5: clear-glass veil (5/4); 1: full frost (16/16).
void FrostMicaRadii(float frostAmount, out float rim, out float core)
{
    const float t = saturate(frostAmount);
    const float upper = saturate((t - 0.5) * 2.0); // 0 at mid, 1 at full
    const float maxRim = lerp(kMicaClearRim, kMicaFullRim, upper);
    const float maxCore = lerp(kMicaClearCore, kMicaFullCore, upper);
    const float reach = saturate(t * 2.0); // 0 at 0, 1 at >=0.5
    rim = maxRim * reach;
    core = maxCore * reach;
}

// Face plate milk: clear Apple mix at 0, full #3a..#e1 tone-map at mid+.
float FrostPlateMix(float frostAmount)
{
    const float t = saturate(frostAmount);
    return t <= 0.5 ? lerp(0.10, 1.0, t * 2.0) : 1.0;
}
// Dense 33-tap Gaussian, sigma = 8, taps at 1-pixel multiples.
// w(k) = exp(-k^2 / (2*sigma^2)), normalized with mirrors.
static const float kGaussW[17] = {
    0.051893, 0.051489, 0.050297, 0.048370, 0.045796, 0.042686, 0.039171,
    0.035388, 0.031475, 0.027560, 0.023758, 0.020164, 0.016847, 0.013858,
    0.011223, 0.008948, 0.007023
};

float3 SampleGlassAxis(Texture2D tex, float2 uvR, float2 uvG, float2 uvB,
    float2 texel, float blurPx, float2 axis)
{
    const float2 lo = texel * 0.5;
    const float2 hi = 1.0 - texel * 0.5;
    // Unit-pixel steps (never sparse). blurPx scales how far the lobe
    // reaches; with blurPx == 16, step == 1 and all 33 taps are used.
    const float step = min(blurPx / 16.0, 1.0);
    float3 acc = float3(tex.Sample(linearClamp, clamp(uvR, lo, hi)).r,
        tex.Sample(linearClamp, clamp(uvG, lo, hi)).g,
        tex.Sample(linearClamp, clamp(uvB, lo, hi)).b) * kGaussW[0];
    [unroll]
    for (int k = 1; k <= 16; ++k)
    {
        const float2 off = axis * (float(k) * step) * texel;
        const float w = kGaussW[k];
        acc.r += (tex.Sample(linearClamp, clamp(uvR + off, lo, hi)).r +
            tex.Sample(linearClamp, clamp(uvR - off, lo, hi)).r) * w;
        acc.g += (tex.Sample(linearClamp, clamp(uvG + off, lo, hi)).g +
            tex.Sample(linearClamp, clamp(uvG - off, lo, hi)).g) * w;
        acc.b += (tex.Sample(linearClamp, clamp(uvB + off, lo, hi)).b +
            tex.Sample(linearClamp, clamp(uvB - off, lo, hi)).b) * w;
    }
    return acc;
}

float4 GlassPS(VertexOutput input) : SV_Target
{
    const float2 outputSize = scene0.xy;
    const float2 pixel = input.position.xy;
    const float dpi = max(scene1.y, 1.0);
    // Pill geometry: the window carries a shadow margin ring (shared
    // DOCK_SHADOW_MARGIN_PT); the glass sits inset, the shader draws the
    // drop shade into the margin outside the mask.
    // DOCK_FX_PANEL (menus): fill the surface; dock keeps the shadow margin ring.
    const float panelOn = FxEnabled(scene1.x, (float)DOCK_FX_PANEL);
    const float popupShadowOn = FxEnabled(scene1.x, (float)DOCK_FX_POPUP_SHADOW);
    const bool isPanel = panelOn > 0.5;
    // Hover labels share the dock bar face (LABEL_FACE): skip charcoal and
    // text-panel crush. QS/Settings keep DOCK_FACE text-panel faces.
    const float labelFace = panelOn * FxEnabled(scene1.x, (float)DOCK_FX_LABEL_FACE);
    const float textPanel = panelOn * FxEnabled(scene1.x, (float)DOCK_FX_DOCK_FACE) * (1.0 - labelFace);
    const float charcoalOn = panelOn * (1.0 - FxEnabled(scene1.x, (float)DOCK_FX_DOCK_FACE)) * (1.0 - labelFace);
    const float marginDev = max(1.0 - panelOn, popupShadowOn) * DOCK_SHADOW_MARGIN_PT * dpi;
    const float2 halfSize = max(outputSize * 0.5 - marginDev - 1.5 * dpi * (1.0 - panelOn), float2(1.0, 1.0));
    // Shared DOCK_CORNER_RADIUS_PT (see DockTheme.hlsli). Squircle n=4 gives
    // Apple-like continuous curvature (slightly fuller corners than the true
    // circular arcs of GDI RoundRect used for the input region; the few-px
    // corner difference is outside the interactive icon area).
    const float cornerRadius = max(min(DOCK_CORNER_RADIUS_PT * dpi, halfSize.y), 1.0);
    const float distance = SdSquircleBox(pixel - outputSize * 0.5, halfSize, cornerRadius);
    const float aa = 1.35 * dpi;
    const float mask = 1.0 - smoothstep(-aa, aa, distance);
    // SDF gradient = 2D surface direction (CASDFLayer-style). ddx/ddy gives
    // screen-space analytic derivatives of the SDF field.
    const float2 gradient = float2(ddx(distance), ddy(distance));
    // Per-effect master switches (dock settings, default all on). Decoded
    // from the scene1.x bitmask; each gates exactly one pipeline stage.
    // Declared before the shadow early-out so every branch can use them.
    const float fxBits = scene1.x;
    const float rimGain = FxEnabled(fxBits, 1.0);
    const float lensOn = FxEnabled(fxBits, 2.0);
    const float dispOn = FxEnabled(fxBits, 4.0);
    const float frostOn = FxEnabled(fxBits, 8.0);
    // Continuous mica strength from bits 16-23 (0..255). Toggle off/on used
    // to be the endpoints; the settings slider lands anywhere between.
    const float frostAmount = saturate(((uint)fxBits >> 16) / 255.0);
    const float tintOn = FxEnabled(fxBits, 16.0);
    const float specOn = FxEnabled(fxBits, 32.0);
    const float shadowOn = FxEnabled(fxBits, 64.0);
    const float thickOn = FxEnabled(fxBits, 128.0);
    if (mask <= 0.0)
    {
        // Only Quick Settings opts into a transparent popup margin. Other
        // panels retain their old no-shadow contract.
        if ((isPanel && popupShadowOn < 0.5) || shadowOn < 0.5)
        {
            return float4(0.0, 0.0, 0.0, 0.0);
        }
        // Soft outer contact shadow: the pill SDF re-evaluated with a small
        // downward offset, falloff over a 16px band. Premultiplied black over
        // the desktop = drop shade via the DComp blend. Fits inside the layout
        // margin (18px). Dock: lighter, centered ambient float (Apple glass).
        const float2 shadowOffset = float2(0.0, 4.0);
        const float2 shadowCenter = outputSize * 0.5 + shadowOffset * dpi;
        const float shadowSdf = SdSquircleBox(pixel - shadowCenter, halfSize, cornerRadius);
        const float shadowWidth = 16.0 * dpi;
        if (shadowSdf < shadowWidth)
        {
            float s = 1.0 - max(shadowSdf, 0.0) / shadowWidth;
            const float shadowAlpha = s * s * (3.0 - 2.0 * s) * 0.20;
            return float4(0.0, 0.0, 0.0, shadowAlpha);
        }
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float2 texel = 1.0 / outputSize;
    const float2 uv = pixel * texel;
    const float gradLen = length(gradient);
    const float2 outward = gradient / max(gradLen, 0.0001);
    const float insideDistance = max(-distance, 0.0);
    // Soft Fresnel-like edge falloff: near-cubic (2.8) curve over an 8px
    // rim band, so veil/caustic/sheen decay naturally and the rim reads
    // softer, driven by shape rather than a hard glow.
    const float rim = pow(saturate(1.0 - insideDistance / max(8.0 * dpi, 2.0)), 2.8);
    const bool hasBackdrop = scene1.w > 0.5;
    // Face accents use the over-white plate; the tone map below sets the body.
    // Rim/specular accents still reference this as the glass body color.
    const float3 glassTint = DOCK_FROST_OVER_WHITE;

    // ---- Bevel geometry ---------------------------------------------------
    // Panels: the bevel spans the short half-axis so the whole face refracts
    // as one convex lens. Dock: an edge band (see GlassBevelWidth) - steep
    // warp at the rim, flat undistorted center. halfSize already carries dpi
    // and dock scale via the layout, so the lens stays proportional at every
    // size. x = saturate(depth/bevel) reaches 1 inside the band because the
    // SDF returns true interior depth.
    float bevelWidth = GlassBevelWidth(halfSize, cornerRadius, dpi, isPanel);
    const float x = saturate(insideDistance / max(bevelWidth, 1e-3)); // 0 rim -> 1 flat
    const float oneMinusX = 1.0 - x;
    const float oneMinusX4 = oneMinusX * oneMinusX * oneMinusX * oneMinusX;
    const float height01 = BevelHeight(oneMinusX4); // f(x): 0 rim, 1 center
    const float slopeN = BevelSlopeN(oneMinusX, oneMinusX4);
    // True surface slope dH/dDist = (B/bevel) * f'(x), B = 0.65*bevel.
    float slopeMag = 0.65 * slopeN;
    // ---- Icon-calm halos --------------------------------------------------
    // Lensing excludes icons: within a feather of any icon rect the slab
    // relaxes toward flat, so refraction peaks in the background gaps
    // instead of crowding glyphs. (Icons are opaque and drawn undisplaced
    // on top regardless; this stills the glass around them.) Tint and frost
    // are untouched - only the visible warp calms.
    const float haloDamp = IconHaloDamp(IconHaloCalm(pixel, dpi), isPanel);
    slopeMag *= haloDamp;
    // Panels also calm rim lights near glyphs; the dock keeps its rim
    // highlight continuous around the silhouette.
    const float rimLightDamp = isPanel ? haloDamp : 1.0;
    // Slab thickness T(x) = T0 + B*f(x), T0 = 0.35*bevel, B = 0.65*bevel.
    const float thicknessPx = (0.35 + 0.65 * height01) * bevelWidth;
    const float bevelFactor = 1.0 - x; // 1 at rim, 0 in flat field

    // Frost radii: clear veil through mid, full mica on the right half.
    float frostRim;
    float frostCore;
    // Panels keep a minimum mica dissolve so busy wallpaper detail cannot
    // fight glyphs even when the user parks Frost near clear.
    const float panelFrost = max(frostAmount,
        max(charcoalOn * DOCK_PANEL_FROST_BLUR_FLOOR,
            textPanel * DOCK_TEXT_PANEL_FROST_FLOOR));
    FrostMicaRadii(panelFrost, frostRim, frostCore);

    float3 frostedBackground;
    if (!hasBackdrop)
    {
        frostedBackground = glassTint;
    }
    else
    {
        // ---- 2. Snell refraction through the convex slab ------------------
        // theta_s = atan(slope) = incidence (view is normal to the plate).
        // theta_r = asin(sin(theta_s)/n) per wavelength, BK7 crown glass.
        // d(x) = T(x) * tan(theta_s - theta_r), peak at the rim.
        // uv' = uv - outward * d (inward pull => magnified look).
        const float thetaS = atan(slopeMag);
        const float sinS = sin(thetaS);
        // 3. Chromatic dispersion: real BK7 Fraunhofer indices (d/F/C lines).
        // Red 656nm 1.5143, Green 588nm 1.5168, Blue 486nm 1.5224.
        const float nR = 1.5143;
        const float nG = 1.5168;
        const float nB = 1.5224;
        const float sinRR = clamp(sinS / nR, 0.0, 0.999);
        const float sinRG = clamp(sinS / nG, 0.0, 0.999);
        const float sinRB = clamp(sinS / nB, 0.0, 0.999);
        const float thetaRR = asin(sinRR);
        const float thetaRG = asin(sinRG);
        const float thetaRB = asin(sinRB);
        // Gain lives in kLensGain above (shared with the horizontal pass):
        // calm center, one continuous slab; pull soft-capped to kLensMaxPx with
        // a rim fade so a high-contrast edge above the dock cannot paint the
        // opposite (dark) rim.
        float dR = thicknessPx * tan(max(thetaS - thetaRR, 0.0)) * kLensGain * lensOn;
        float dG = thicknessPx * tan(max(thetaS - thetaRG, 0.0)) * kLensGain * lensOn;
        float dB = thicknessPx * tan(max(thetaS - thetaRB, 0.0)) * kLensGain * lensOn;
        // Chromatic split (ratios physical, blue bends most); shared boost.
        dR = dG + (dR - dG) * kFringeBoost;
        dB = dG + (dB - dG) * kFringeBoost;
        // Dispersion off collapses all channels onto green (no split).
        dR = lerp(dG, dR, dispOn);
        dB = lerp(dG, dB, dispOn);
        // Soft-cap the drag (see SoftLensCap / kLensMaxPx): keep white-fringe
        // protection without a hard plateau kink toward the dock center.
        const float lensMax = kLensMaxPx * dpi;
        dR = SoftLensCap(dR, lensMax);
        dG = SoftLensCap(dG, lensMax);
        dB = SoftLensCap(dB, lensMax);
        const float lensRimFade = smoothstep(0.0, kLensRimFadePx * dpi, insideDistance);
        dR *= lensRimFade;
        dG *= lensRimFade;
        dB *= lensRimFade;

        const float2 lo = texel * 0.5;
        const float2 hi = 1.0 - texel * 0.5;
        const float2 uvR = clamp(uv - outward * dR * texel, lo, hi);
        const float2 uvG = clamp(uv - outward * dG * texel, lo, hi);
        const float2 uvB = clamp(uv - outward * dB * texel, lo, hi);

#if LIQUID_DEBUG_REFRACTION_ONLY
        // Refraction-only diagnostic: raw backdrop at the refracted UV, no
        // tint/blur/dispersion/Fresnel/lights. If straight edges behind the
        // dock don't kink at the rim here, displacement is zero and material
        // tuning is pointless. Production builds set this to 0.
        {
            float3 dbg = backdropTexture.Sample(linearClamp, clamp(uvG, lo, hi)).rgb;
            return float4(dbg * mask, mask);
        }
#endif

        // ---- 5. Scattering blur modulated by slab height ------------------
        // Heavy mica: soft rim (refraction still reads), near-opaque frost
        // in the thick center. Iterated separable passes compound radii.
        // Icon legibility comes from blur + the #e1e1e1 plate, not sharpness.
        const float blurPx = lerp(frostRim, frostCore, height01) * dpi;
        // Frost off samples each channel once at its (possibly refracted)
        // UV instead of the 17-tap ring: same result as a zero-radius blur
        // with a seventeenth of the fetches.
        // Frost off samples each channel once (no frost); on runs the ring.
        if (frostOn < 0.5)
        {
            frostedBackground = float3(backdropTexture.Sample(linearClamp, clamp(uvR, lo, hi)).r,
                backdropTexture.Sample(linearClamp, clamp(uvG, lo, hi)).g,
                backdropTexture.Sample(linearClamp, clamp(uvB, lo, hi)).b);
        }
        else
        {
            // Frost on: vertical separable axis from the temp target (pass 1
            // blurred horizontal). True Gaussian finish, zero stochastic
            // elements anywhere in the frost.
            frostedBackground = SampleGlassAxis(blurTemp, uvR, uvG, uvB, texel, blurPx, float2(0.0, 1.0));
        }
    }

    // ---- 1. Face plate (Frost slider) -----------------------------------
    // Dock: vibrancy (saturation boost) then a #4b..#e1 veil (clear
    // Apple mix -> milky glass) so the backdrop keeps its color and contrast.
    // Panels (DOCK_FX_PANEL): neutral charcoal plate (pre-sage Concept A+D)
    // with milk floor — live capture+blur, no sage wash on the dock bar.
    if (charcoalOn < 0.5 && textPanel < 0.5 && hasBackdrop)
    {
        const float backdropLuma = dot(frostedBackground, float3(0.2126, 0.7152, 0.0722));
        frostedBackground = saturate(lerp(backdropLuma.xxx, frostedBackground, DOCK_VIBRANCY));
    }
    float faceLo = lerp(DOCK_FACE_OVER_BLACK, DOCK_PANEL_FACE_OVER_BLACK, charcoalOn);
    float faceHi = lerp(DOCK_FACE_OVER_WHITE, DOCK_PANEL_FACE_OVER_WHITE, charcoalOn);
    if (textPanel > 0.5)
    {
        const float lightPlate = saturate(scene0.w);
        faceLo = lerp(DOCK_TEXT_PANEL_FACE_OVER_BLACK, DOCK_TEXT_PANEL_LIGHT_OVER_BLACK, lightPlate);
        faceHi = lerp(DOCK_TEXT_PANEL_FACE_OVER_WHITE, DOCK_TEXT_PANEL_LIGHT_OVER_WHITE, lightPlate);
    }
    const float3 toneMapped = lerp(faceLo, faceHi, saturate(frostedBackground));
    float plateMix = FrostPlateMix(max(frostAmount, textPanel * DOCK_TEXT_PANEL_FROST_FLOOR));
    plateMix = max(plateMix, charcoalOn * DOCK_PANEL_PLATE_MIX_FLOOR);
    plateMix = max(plateMix, textPanel * DOCK_TEXT_PANEL_PLATE_MIX_FLOOR);
    float3 color = lerp(frostedBackground, toneMapped, plateMix);

    if (labelFace > 0.5)
    {
        color = (247.0 / 255.0).xxx;
    }

    // ---- 4. Soft edge + pointer-reactive glint (no corner speculars) ------
    // Removed the sharp top-left key specular (pow(ndl, 110)) and NdotL-boosted
    // white rimFacing that painted bright white corner pixels on the dock and
    // labels. Idle: uniform soft rim only. Pointer: local rim flare + bloom.
    const float3 surfN = normalize(float3(outward * slopeMag, 1.0));
    const float cosTheta = saturate(surfN.z);
    const float fresnel = 0.04 + 0.96 * pow(1.0 - cosTheta, 5.0);
    // Labels stay a flat speech bubble; the dock keeps the soft rim.
    const float edgeGain = rimGain * (1.0 - labelFace);
    // Cool, low Fresnel veil — no white hotspot.
    color += fresnel * float3(0.88, 0.92, 0.98) * 0.12 * pow(rim, 2.8) * edgeGain * rimLightDamp;

    const float thicknessShade = 1.0 - 0.07 * saturate(1.0 - insideDistance / max(bevelWidth * 0.6, 1e-3));
    color *= lerp(1.0, thicknessShade, thickOn * (1.0 - labelFace));
    color += glassTint * rim * 0.02 * edgeGain * rimLightDamp;

    // Uniform soft rim (no NdotL / dual-corner lobes).
    float rimStrength = lerp(0.28, 0.22, charcoalOn);
    rimStrength = lerp(rimStrength, 0.34, textPanel);
    const float3 rimEdge = lerp(float3(0.78, 0.82, 0.88), float3(0.70, 0.72, 0.76), charcoalOn);
    color += rimEdge * pow(rim, 8.0) * rimStrength * edgeGain * rimLightDamp;

    // Pointer glint on the dock bar only. Labels are a flat tooltip.
    const float glintStrength = (isPanel ? 0.0 : 1.0) * saturate(scene2.z) * specOn;
    if (glintStrength > 0.0)
    {
        const float2 glintPos = scene2.xy;
        float2 glintAxis = float2(-0.70710678, -0.70710678);
        const float2 fromCenter = (glintPos - outputSize * 0.5) / max(halfSize, float2(1.0, 1.0));
        const float fromLen = length(fromCenter);
        const float2 toward = fromCenter / max(fromLen, 1e-4);
        const float swing = glintStrength * saturate(fromLen * 2.0);
        const float2 swung = lerp(glintAxis, toward, swing);
        glintAxis = swung / max(length(swung), 1e-4);
        const float facing = dot(outward, glintAxis);
        const float pointerRim = pow(saturate(facing), 3.0);
        const float2 toGlint = pixel - glintPos;
        const float glintDist2 = dot(toGlint, toGlint);
        const float rimSigma = 48.0 * dpi;
        const float bloomSigma = 70.0 * dpi;
        const float nearRim = exp(-glintDist2 / (2.0 * rimSigma * rimSigma));
        const float bloom = exp(-glintDist2 / (2.0 * bloomSigma * bloomSigma));
        // Soft cool flare — avoid pure-white corner dots.
        color += float3(0.85, 0.90, 1.0) * pow(rim, 5.0) * 0.28 * nearRim * pointerRim * glintStrength * rimGain;
        color += float3(0.80, 0.88, 1.0) * pow(rim, 2.5) * 0.08 * nearRim * glintStrength * rimGain;
        color += float3(0.90, 0.94, 1.0) * 0.03 * bloom * glintStrength;
    }

    if (scene1.z > 0.5)
    {
        const float outline = 1.0 - smoothstep(0.0, 1.0, abs(distance));
        color = lerp(color, float3(1.0, 0.18, 0.58), outline);
    }

    color = saturate(color);
    float alpha = saturate(mask * scene0.z * (hasBackdrop ? scene1.w : 1.0));
    if (labelFace > 0.5)
    {
        alpha = mask * 0.8;
    }
    return float4(color * alpha, alpha);
}

// ---------------------------------------------------------------------------
// Separable frost (BlurHPS + GlassPS vertical finish). Every pass
// must compute identical lens UVs (same SDF, bevel, halos, Snell, fringe):
// equal inputs yield equal UVs, and that equality is what makes the split
// valid, so all passes share ComputeFrostUVs below. Iterating moderate
// dense kernels compounds into a heavy blur (per-axis sigma grows ~x1.4
// per H/V pair) without ever opening sparse tap gaps, which is what a
// single wide kernel does (blocky pixelation on text/edges). Passes after
// the first only run when frost is on and the backdrop is live (C++ skips
// them otherwise), so the mica radii are unconditional. GlassPS finishes
// the final vertical axis from temp.
// ---------------------------------------------------------------------------
// Shared lens-UV computation for every frost pass. Must stay identical to
// the UV block in GlassPS above.
void ComputeFrostUVs(float2 pixel, float2 outputSize, float dpi,
    out float2 uvR, out float2 uvG, out float2 uvB, out float blurPx)
{
    // DOCK_FX_PANEL (menus): fill the surface; dock keeps the shadow margin ring.
    const float panelOn = FxEnabled(scene1.x, (float)DOCK_FX_PANEL);
    const float popupShadowOn = FxEnabled(scene1.x, (float)DOCK_FX_POPUP_SHADOW);
    const bool isPanel = panelOn > 0.5;
    const float labelFace = panelOn * FxEnabled(scene1.x, (float)DOCK_FX_LABEL_FACE);
    const float marginDev = max(1.0 - panelOn, popupShadowOn) * DOCK_SHADOW_MARGIN_PT * dpi;
    const float2 halfSize = max(outputSize * 0.5 - marginDev - 1.5 * dpi * (1.0 - panelOn), float2(1.0, 1.0));
    const float cornerRadius = max(min(DOCK_CORNER_RADIUS_PT * dpi, halfSize.y), 1.0);
    const float distance = SdSquircleBox(pixel - outputSize * 0.5, halfSize, cornerRadius);
    const float2 gradient = float2(ddx(distance), ddy(distance));
    const float2 texel = 1.0 / outputSize;
    const float2 uv = pixel * texel;
    const float2 outward = gradient / max(length(gradient), 0.0001);
    const float insideDistance = max(-distance, 0.0);
    float bevelWidth = GlassBevelWidth(halfSize, cornerRadius, dpi, isPanel);
    const float x = saturate(insideDistance / max(bevelWidth, 1e-3)); // 0 rim -> 1 flat
    const float oneMinusX = 1.0 - x;
    const float oneMinusX4 = oneMinusX * oneMinusX * oneMinusX * oneMinusX;
    const float height01 = BevelHeight(oneMinusX4); // f(x): 0 rim, 1 center
    const float slopeN = BevelSlopeN(oneMinusX, oneMinusX4);
    float slopeMag = 0.65 * slopeN;
    const float fxBits = scene1.x;
    const float lensOn = FxEnabled(fxBits, 2.0);
    const float dispOn = FxEnabled(fxBits, 4.0);
    slopeMag *= IconHaloDamp(IconHaloCalm(pixel, dpi), isPanel);
    const float thicknessPx = (0.35 + 0.65 * height01) * bevelWidth;
    const float thetaS = atan(slopeMag);
    const float sinS = sin(thetaS);
    const float sinRR = clamp(sinS / 1.5143, 0.0, 0.999);
    const float sinRG = clamp(sinS / 1.5168, 0.0, 0.999);
    const float sinRB = clamp(sinS / 1.5224, 0.0, 0.999);
    const float thetaRR = asin(sinRR);
    const float thetaRG = asin(sinRG);
    const float thetaRB = asin(sinRB);
    float dR = thicknessPx * tan(max(thetaS - thetaRR, 0.0)) * kLensGain * lensOn;
    float dG = thicknessPx * tan(max(thetaS - thetaRG, 0.0)) * kLensGain * lensOn;
    float dB = thicknessPx * tan(max(thetaS - thetaRB, 0.0)) * kLensGain * lensOn;
    dR = dG + (dR - dG) * kFringeBoost;
    dB = dG + (dB - dG) * kFringeBoost;
    dR = lerp(dG, dR, dispOn);
    dB = lerp(dG, dB, dispOn);
    // Must stay identical to GlassPS above (see SoftLensCap / kLensMaxPx).
    const float lensMaxFrost = kLensMaxPx * dpi;
    dR = SoftLensCap(dR, lensMaxFrost);
    dG = SoftLensCap(dG, lensMaxFrost);
    dB = SoftLensCap(dB, lensMaxFrost);
    const float lensRimFadeFrost = smoothstep(0.0, kLensRimFadePx * dpi, insideDistance);
    dR *= lensRimFadeFrost;
    dG *= lensRimFadeFrost;
    dB *= lensRimFadeFrost;
    const float2 lo = texel * 0.5;
    const float2 hi = 1.0 - texel * 0.5;
    uvR = clamp(uv - outward * dR * texel, lo, hi);
    uvG = clamp(uv - outward * dG * texel, lo, hi);
    uvB = clamp(uv - outward * dB * texel, lo, hi);
    const float frostAmt = saturate(((uint)scene1.x >> 16) / 255.0);
    float micaRim;
    float micaCore;
    const float textPanel = panelOn * FxEnabled(scene1.x, (float)DOCK_FX_DOCK_FACE) * (1.0 - labelFace);
    const float charcoalOn = panelOn * (1.0 - FxEnabled(scene1.x, (float)DOCK_FX_DOCK_FACE)) * (1.0 - labelFace);
    FrostMicaRadii(max(frostAmt, max(charcoalOn * DOCK_PANEL_FROST_BLUR_FLOOR,
        textPanel * DOCK_TEXT_PANEL_FROST_FLOOR)), micaRim, micaCore);
    blurPx = lerp(micaRim, micaCore, height01) * dpi;
}


// Pass 1: horizontal Gaussian axis from the live backdrop into temp.
float4 BlurHPS(VertexOutput input) : SV_Target
{
    const float2 outputSize = scene0.xy;
    const float2 texel = 1.0 / outputSize;
    float2 uvR;
    float2 uvG;
    float2 uvB;
    float blurPx;
    ComputeFrostUVs(input.position.xy, outputSize, max(scene1.y, 1.0), uvR, uvG, uvB, blurPx);
    const float3 h = SampleGlassAxis(backdropTexture, uvR, uvG, uvB, texel, blurPx, float2(1.0, 0.0));
    return float4(h, 1.0);
}

// Pass 2: vertical Gaussian axis from temp into temp2.
float4 BlurVPS(VertexOutput input) : SV_Target
{
    const float2 outputSize = scene0.xy;
    const float2 texel = 1.0 / outputSize;
    float2 uvR;
    float2 uvG;
    float2 uvB;
    float blurPx;
    ComputeFrostUVs(input.position.xy, outputSize, max(scene1.y, 1.0), uvR, uvG, uvB, blurPx);
    const float3 v = SampleGlassAxis(blurTemp, uvR, uvG, uvB, texel, blurPx, float2(0.0, 1.0));
    return float4(v, 1.0);
}

// Pass 3: horizontal Gaussian axis from temp2 back into temp; GlassPS
// finishes the final vertical axis from temp.
float4 BlurHPS2(VertexOutput input) : SV_Target
{
    const float2 outputSize = scene0.xy;
    const float2 texel = 1.0 / outputSize;
    float2 uvR;
    float2 uvG;
    float2 uvB;
    float blurPx;
    ComputeFrostUVs(input.position.xy, outputSize, max(scene1.y, 1.0), uvR, uvG, uvB, blurPx);
    const float3 h = SampleGlassAxis(blurTemp2, uvR, uvG, uvB, texel, blurPx, float2(1.0, 0.0));
    return float4(h, 1.0);
}

// Chrome ink on the same face tonemap domain as GlassPS:
// face = lerp(OVER_BLACK, OVER_WHITE, backdropLuma); ink inverts that so glyphs
// stay contrasted on both dark and light plates.
float3 AdaptiveChromeInk(float2 outputSize)
{
    // Hard cut on wallpaper luma (ignore frosted plate — it sits mid-grey and
    // wrongly pulled ink dark on black desks). Dark desk -> light chrome.
    const float3 darkInk = float3(DOCK_INK_R, DOCK_INK_G, DOCK_INK_B) / 255.0;
    const float3 lightInk = float3(0.96, 0.96, 0.97);
    if (scene1.w < 0.5)
    {
        return lightInk;
    }
    // Average a few samples across the dock window so one bright pixel cannot
    // tip the whole chrome set dark.
    float acc = 0.0;
    acc += dot(backdropTexture.SampleLevel(linearClamp, float2(0.20, 0.50), 0).rgb, float3(0.2126, 0.7152, 0.0722));
    acc += dot(backdropTexture.SampleLevel(linearClamp, float2(0.50, 0.50), 0).rgb, float3(0.2126, 0.7152, 0.0722));
    acc += dot(backdropTexture.SampleLevel(linearClamp, float2(0.80, 0.50), 0).rgb, float3(0.2126, 0.7152, 0.0722));
    const float wallpaperLuma = saturate(acc / 3.0);
    return wallpaperLuma < 0.50 ? lightInk : darkInk;
}

float4 IconPS(VertexOutput input) : SV_Target
{
    const IconInstance icon = iconInstances[input.instanceIndex];
    const uint metaZ = (uint)(icon.iconMeta.z + 0.5);
    const bool dragged = (metaZ & 1u) != 0u;
    const bool adaptiveInk = (metaZ & 2u) != 0u;
    const float2 outputSize = max(scene0.xy, float2(1.0, 1.0));

    if (icon.iconMeta.y > 1.5 && icon.iconMeta.y < 3.5)
    {
        const float2 size = max(icon.iconRect.zw, 1.0);
        const float2 offset = abs(input.uv - 0.5) * size;
        const float halfThickness = 0.75;
        const float cap = 2.0;
        const float lineAlpha = smoothstep(halfThickness + 0.75, halfThickness, offset.x) *
            smoothstep(size.y * 0.5, max(size.y * 0.5 - cap, 0.0), offset.y) * 0.80;
        const float3 ink = adaptiveInk ? AdaptiveChromeInk(outputSize)
                                       : float3(0.32, 0.32, 0.34);
        return float4(ink * lineAlpha, lineAlpha);
    }

    if (icon.iconMeta.y > 3.5)
    {
        const int2 texel = int2(floor(input.position.xy - icon.iconRect.xy));
        const bool inside = texel.x >= 0 && texel.y >= 0 &&
            texel.x < (int)icon.iconRect.z && texel.y < (int)icon.iconRect.w;
        float4 sampledClock = inside
            ? iconTexture.Load(int4(texel, (int)input.textureIndex, 0))
            : 0.0;
        const float clockBrightness = icon.iconMeta.y > 4.5 ? 0.5 : 1.0;
        float3 rgb = sampledClock.rgb;
        float a = sampledClock.a;
        if (adaptiveInk)
        {
            const float coverage = max(a, max(rgb.r, max(rgb.g, rgb.b)));
            const float3 ink = AdaptiveChromeInk(outputSize);
            rgb = ink * coverage;
            a = coverage;
        }
        return float4(rgb * clockBrightness, a * clockBrightness);
    }

    const float contentHeightRatio = saturate(icon.iconRect.z / max(icon.iconRect.w, 1.0));
    float4 sampled = float4(0.0, 0.0, 0.0, 0.0);
    if (input.uv.y <= contentHeightRatio) {
        const float2 iconUv = float2(input.uv.x, input.uv.y / max(contentHeightRatio, 0.001));
        sampled = iconTexture.Sample(linearClamp, float3(saturate(iconUv), input.textureIndex));
    }

    const float iconBrightness = icon.iconMeta.y > 0.5 && !dragged ? 0.5 : 1.0;
    float3 color = sampled.rgb * iconBrightness;
    float alpha = sampled.a;

    if (adaptiveInk)
    {
        const float coverage = max(alpha, max(sampled.r, max(sampled.g, sampled.b)));
        const float3 ink = AdaptiveChromeInk(outputSize);
        color = ink * coverage * iconBrightness;
        alpha = coverage * iconBrightness;
    }

    if (icon.iconMeta.x > 0.5 && contentHeightRatio < 0.98 && input.uv.y > contentHeightRatio) {
        const float stripHeightPx = icon.iconRect.w * (1.0 - contentHeightRatio);
        // Sit in the lower part of the strip, under the icon, clear of the slot edge.
        const float dotCenterY = contentHeightRatio + (1.0 - contentHeightRatio) * 0.72;
        const float dotRadiusPx = 0.75 * max(min(stripHeightPx * 0.42, icon.iconRect.z * 0.055), 2.8);
        float2 dotOffset;
        dotOffset.x = (input.uv.x - 0.5) * icon.iconRect.z;
        dotOffset.y = (input.uv.y - dotCenterY) * icon.iconRect.w;
        const float dist = length(dotOffset);
        const float edgeSoftness = max(0.28, dotRadiusPx * 0.2);
        const float dotAlpha = 1.0 - smoothstep(dotRadiusPx, dotRadiusPx + edgeSoftness, dist);
        // macOS running indicator: small black circle on dark, light, and photo bars.
        const float3 runningDot = float3(0.0, 0.0, 0.0);
        color = color * (1.0 - dotAlpha) + runningDot * dotAlpha;
        alpha = dotAlpha + alpha * (1.0 - dotAlpha);
    }

    return float4(color, alpha);
}
