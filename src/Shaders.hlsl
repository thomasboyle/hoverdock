#include "DockTheme.hlsli"

// Refraction-only diagnostic (see GlassPS). 1 = raw warped backdrop,
// no material. Flip back to 0 after the warp check.
#define LIQUID_DEBUG_REFRACTION_ONLY 0

cbuffer FrameData : register(b0)
{
    float4 scene0; // output width, output height, glass alpha, dock scale
    float4 scene1; // fx bitmask | icon count, DPI scale, dev bounds, backdrop valid
    float4 scene2; // pointer glint x, y (output px), strength 0..1, unused
    float4 scene3; // dock: chrome ink darkness 0 (white) .. 1 (black); panels: y = corner radius px (0 = default)
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


// ---------------------------------------------------------------------------
// macOS 27 dock silhouette (measured spec, ratios of the icon body S):
// height 1.752 S, superellipse corners n = 2.78 with extent r = 0.738 S
// (not a pill, not a circular arc). The dock window is the pill plus the
// rounded shadow margin on every side (DockApp::RebuildLayout), so S falls
// out of the pill height and no extra constant has to be plumbed.
// ---------------------------------------------------------------------------
static const float kDock27HeightS = 1.752;
static const float kDock27CornerS = 0.738;
static const float kDock27CornerN = 2.78;
static const float kDock27RefS = 40.5; // spec px are measured at S = 40.5 px

float Dock27MarginPx(float dpi)
{
    // Same rounding as DockShadowMarginPx (std::lround) on the C++ side.
    return floor(DOCK_SHADOW_MARGIN_PT * dpi + 0.5);
}

// Superellipse rounded-rect SDF. Exact on the straight runs; in the corner
// the superellipse level is divided by its gradient length so the value is
// a first-order Euclidean distance near the boundary (needed for the 1 px
// rim core). nrm is the unit outward normal (y down, like pixels).
float SdSquircleRectN(float2 p, float2 halfSize, float r, float n, out float2 nrm)
{
    const float2 q = abs(p) - (halfSize - r);
    if (q.x > 0.0 && q.y > 0.0)
    {
        const float2 a = max(q, 1e-5);
        const float m = max(a.x, a.y);
        const float2 u = a / m;
        const float2 un = pow(u, n);
        const float sum = un.x + un.y;
        const float k = m * pow(sum, 1.0 / n);
        const float2 g = pow(u, n - 1.0) * pow(sum, 1.0 / n - 1.0);
        const float gl = max(length(g), 1e-4);
        nrm = sign(p) * g / gl;
        return (k - r) / gl;
    }
    nrm = (q.x > q.y) ? float2(sign(p.x), 0.0) : float2(0.0, sign(p.y));
    return max(q.x, q.y) - r;
}

void Dock27Shape(float2 pixel, float2 outputSize, float dpi,
    out float2 halfSize, out float iconS, out float cornerRadius, out float distance, out float2 nrm)
{
    halfSize = max(outputSize * 0.5 - Dock27MarginPx(dpi), float2(1.0, 1.0));
    iconS = 2.0 * halfSize.y / kDock27HeightS;
    cornerRadius = max(min(kDock27CornerS * iconS, halfSize.y), 1.0);
    distance = SdSquircleRectN(pixel - outputSize * 0.5, halfSize, cornerRadius, kDock27CornerN, nrm);
}

// Gaussian of width w integrated over a 1 px box footprint (approximated by
// widening to sqrt(w^2 + 1/12) at constant area) so the 0.4 px core cannot
// alias.
float PixelGauss(float x, float w)
{
    const float we = sqrt(w * w + 1.0 / 12.0);
    const float t = x / we;
    return (w / we) * exp(-0.5 * t * t);
}

// Rim light R(s, n) from the spec: s = depth inside the edge (px).
float Dock27Rim(float s, float2 nrm, float iconS)
{
    const float k = iconS / kDock27RefS;
    const float A = 0.0685 + 0.850 * nrm.y * nrm.y;
    const float core = 0.258 * PixelGauss(s - 1.29 * k, 0.404 * k);
    const float glow = 0.153 * exp(-max(s, 0.0) / (2.55 * k));
    return saturate(A * (core + glow));
}

// Faint dark hairline just outside the edge (alpha of black, premultiplied).
float Dock27OuterLine(float distance, float iconS)
{
    const float k = iconS / kDock27RefS;
    return 0.036 * PixelGauss(distance - 0.87 * k, 0.34 * k);
}

// Cool white tint composited over the (lightly blurred) backdrop.
static const float3 kDock27Tint = float3(165.0, 162.0, 193.0) / 255.0;
static const float kDock27TintAlpha = 0.29;

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

// Search panel: macOS Spotlight material (see DOCK_FX_SPOTLIGHT).
bool SpotlightPanel()
{
    return FxEnabled(scene1.x, (float)DOCK_FX_PANEL) > 0.5 && FxEnabled(scene1.x, (float)DOCK_FX_SPOTLIGHT) > 0.5;
}

// Bevel (lens band) width. Context panels refract as one full-face slab. The
// dock, and text panels (DOCK_FX_DOCK_FACE), confine the lens to an edge band
// so the center stays flat. Shared by GlassPS and ComputeFrostUVs.
// True for the dock itself and for dock-face panels (Quick Settings, Dock
// Settings, overflow): they share the dock's silhouette function, lens band,
// frost, tint, rim and shadow code. Charcoal context menus and hover labels
// keep the older panel path.
bool DockStyleFace()
{
    const float panelOn = FxEnabled(scene1.x, (float)DOCK_FX_PANEL);
    const float labelFace = panelOn * FxEnabled(scene1.x, (float)DOCK_FX_LABEL_FACE);
    const float textPanel = panelOn * FxEnabled(scene1.x, (float)DOCK_FX_DOCK_FACE) * (1.0 - labelFace);
        return panelOn < 0.5 || textPanel > 0.5;
}

// Dock-face panel geometry: same superellipse SDF + analytic normal as the
// dock (Dock27Shape), parameterised by the panel's half size and corner
// radius instead of the dock's icon-size ratios. The rim/outer-line scale S
// is the dock's reference size at this DPI so the rim reads identically.
void DockPanelShape(float2 pixel, float2 outputSize, float dpi, float2 halfSize,
    float cornerRadius, out float iconS, out float distance, out float2 nrm)
{
    iconS = kDock27RefS * dpi;
    distance = SdSquircleRectN(pixel - outputSize * 0.5, halfSize, cornerRadius, kDock27CornerN, nrm);
}

// Physical glass panel constants (see the model description at kLensGain's
// block below): fixed real-world lengths, 1 dp = 1/96 in at 100% scale.
static const float kPxPerMm = 96.0 / 25.4;      // dp per mm at 96 dpi (x dpi scale)
static const float kPanelBevelMm = 5.0;         // flat-to-rim bevel width
static const float kPanelSlabMm = 8.0;          // glass thickness at the flat interior
// Air gap between the slab's flat underside and the backdrop plane (the desktop
// sits behind the glass, it is not in contact). Design assumption, not measured:
// 2 mm = a pane floating just above the surface. 0 reproduces the old contact model.
static const float kPanelGapMm = 2.0;
static const float kPanelRimFadeDp = 1.0;       // AA coverage pixel only (mask is crisp)

float GlassBevelWidth(float2 halfSize, float cornerRadius, float dpi, bool isPanel)
{
    const float faceBevel = max(min(halfSize.x, halfSize.y) * 0.95, 8.0 * dpi);
    if (isPanel && DockStyleFace())
    {
        // Fixed physical edge width (never scaled with the panel); only a
        // panel smaller than the bevel itself clamps it.
        return min(kPanelBevelMm * kPxPerMm * dpi, faceBevel);
    }
    const float edgeBevel = clamp(cornerRadius * 1.1, 8.0 * dpi, faceBevel);
    if (!isPanel || DockStyleFace())
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

// The lens depends only on distance from the glass edge. There is deliberately no
// icon-proximity term: any slope/rim modulation near icons bends the glass edge.

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

// ---------------------------------------------------------------------------
// Physical glass panel lens (Quick Settings, Dock Settings, overflow: the
// DOCK_FX_PANEL | DOCK_FX_DOCK_FACE surfaces). The dock constants above are
// tuned for a ~74 px bar: its bevel follows the corner radius (~0.5 x bar
// height), its pull is boosted by kLensGain and soft-capped at kLensMaxPx.
// A large panel is a different object: the same physical edge, not a
// bar-relative one. It is modelled as a thin BK7 slab with a bevelled rim:
//   * bevel (edge) width and slab thickness are fixed physical lengths
//     (mm -> dp at 96 dpi), independent of the panel's pixel size;
//   * the pull is pure Snell geometry (see SlabPull): the top facet refracts the
//     view ray, it crosses the glass below the entry point, leaves through the
//     flat underside and crosses the air gap kPanelGapMm to the desktop:
//     d = T(x) tan(theta_s - theta_r) + g tan(asin(n sin(theta_s - theta_r))),
//     with no artistic gain and no cap;
//   * d is largest at the rim and falls to exactly 0 where the bevel ends,
//     so everything deeper than the bevel (the whole interior of a big
//     panel) is undistorted;
//   * dispersion is the per-channel BK7 index split run through the same
//     Snell step (kFringeBoost is the one shared display scale).
// ---------------------------------------------------------------------------

// True for dock-face panels (QS / Dock Settings / overflow). The dock itself
// and the charcoal / label panels keep the dock-tuned constants.
bool PhysicalGlassPanel(bool isPanel, bool dockStyle)
{
    return isPanel && dockStyle;
}

struct LensGeo
{
    float slopeScale;   // dH/dDist = slopeScale * f'(x)
    float thickScale;   // T(x) = thickPx * (0.35 + 0.65 f(x))
    float gain;
    float capPx;
    float rimFadePx;
    float gapPx;        // glass underside -> backdrop plane (0 = dock / contact)
};

LensGeo GetLensGeo(bool physical, float bevelWidth, float dpi)
{
    LensGeo g;
    if (physical)
    {
        const float slabPx = kPanelSlabMm * kPxPerMm * dpi;
        g.slopeScale = 0.65 * slabPx / max(bevelWidth, 1e-3);
        g.thickScale = slabPx;
        g.gain = 1.0;
        g.capPx = 1.0e4;
        g.rimFadePx = kPanelRimFadeDp * dpi;
        g.gapPx = kPanelGapMm * kPxPerMm * dpi;
    }
    else
    {
        g.slopeScale = 0.65;
        g.thickScale = bevelWidth;
        g.gain = kLensGain;
        g.capPx = kLensMaxPx * dpi;
        g.rimFadePx = kLensRimFadePx * dpi;
        g.gapPx = 0.0;
    }
    return g;
}
// Lateral pull of one view ray (view normal to the plate) at the backdrop plane:
//   top facet tilted thetaS: air->glass, sin(thetaR) = sin(thetaS)/n
//   ray leaves the facet tilted (thetaS - thetaR) from the screen normal and
//   crosses the glass thickness T below the entry point:  T * tan(thetaS - thetaR)
//   flat underside (parallel to the screen): glass->air, sin(phi) = n sin(thetaS - thetaR)
//   crosses the air gap g to the backdrop plane:          g * tan(phi)
// gapPx = 0 (dock, contact model) is exactly the first term. n sin(thetaS-thetaR)
// >= 1 is total internal reflection at the underside (thetaS > ~82 deg for BK7);
// the profile slope is clamped below that, the min() only guards the tan.
float SlabPull(float thicknessPx, float thetaS, float sinS, float n, float gapPx)
{
    const float a = max(thetaS - asin(clamp(sinS / n, 0.0, 0.999)), 0.0);
    float pull = thicknessPx * tan(a);
    if (gapPx > 0.0)
    {
        const float sinPhi = min(n * sin(a), 0.999);
        pull += gapPx * sinPhi * rsqrt(1.0 - sinPhi * sinPhi);
    }
    return pull;
}
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
    // Spotlight spreads the taps past 1 px so one pass reaches sigma = blurPx / 2.
    const float step = SpotlightPanel() ? blurPx / 16.0 : min(blurPx / 16.0, 1.0);
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

// ---------------------------------------------------------------------------
// macOS 26 Control Centre glass (Quick Settings). Shapes live in the panel
// instance buffer, two IconInstance slots per shape:
//   [2i].iconRect = centre x, centre y, half w, half h (output px)
//   [2i].iconMeta = corner radius, |A| refraction px, h bevel px, blur px
//   [2i+1].iconRect = W, B, fill alpha, fill black(1)/white(0)
// scene1.z carries 100 + shape count (the dock path uses scene1.z as a 0/1
// dev-bounds flag and its packed fx word overlaps every low bit, so the CC path
// is keyed on scene1.z alone). Shape 0 is the panel, later shapes sit on
// top. Lens depends only on distance from each shape's edge.
// ---------------------------------------------------------------------------
// CC blur axis: kGaussW has sigma = 8 taps (truncated at 2 sigma), so tap
// spacing = blurPx/16 gives sigma_px = blurPx/2 = the spec sigma (2 r pt).
float3 CcSampleAxis(Texture2D tex, float2 uv, float2 texel, float blurPx, float2 axis)
{
    const float2 lo = texel * 0.5;
    const float2 hi = 1.0 - texel * 0.5;
    const float step = max(blurPx * 0.0625, 0.25);
    float3 acc = tex.Sample(linearClamp, clamp(uv, lo, hi)).rgb * kGaussW[0];
    [unroll]
    for (int k = 1; k <= 16; ++k)
    {
        const float2 off = axis * (float(k) * step) * texel;
        acc += (tex.Sample(linearClamp, clamp(uv + off, lo, hi)).rgb +
            tex.Sample(linearClamp, clamp(uv - off, lo, hi)).rgb) * kGaussW[k];
    }
    return acc;
}
float SdRoundRectCc(float2 p, float2 halfSize, float r)
{
    const float2 q = abs(p) - halfSize + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

uint CcShapeCount()
{
    return min((uint)(scene1.z - 100.0 + 0.5), 48u);
}

// Blur width (px, SampleGlassAxis units) of the topmost shape under a pixel.
float CcBlurPxAt(float2 pixel)
{
    const uint n = CcShapeCount();
    float blurPx = 0.0;
    for (uint i = 0u; i < 48u; ++i)
    {
        if (i >= n)
        {
            break;
        }
        const IconInstance a = iconInstances[2u * i];
        const float s = SdRoundRectCc(pixel - a.iconRect.xy, a.iconRect.zw, a.iconMeta.x);
        if (i == 0u || s < 0.0)
        {
            blurPx = a.iconMeta.w;
        }
    }
    return max(blurPx, 1.0);
}

float4 CcGlass(float2 pixel, float2 outputSize, float dpi)
{
    const uint n = CcShapeCount();
    const float2 texel = 1.0 / outputSize;
    const float2 lo = texel * 0.5;
    const float2 hi = 1.0 - texel * 0.5;
    const bool hasBackdrop = scene1.w > 0.5;
    float3 accum = float3(0.0, 0.0, 0.0);
    float alpha = 0.0;
    float ring = 0.0;
    for (uint i = 0u; i < 48u; ++i)
    {
        if (i >= n)
        {
            break;
        }
        const IconInstance a = iconInstances[2u * i];
        const IconInstance b = iconInstances[2u * i + 1u];
        const float2 centre = a.iconRect.xy;
        const float2 halfSize = a.iconRect.zw;
        const float r = a.iconMeta.x;
        const float s = SdRoundRectCc(pixel - centre, halfSize, r);
        if (s > 1.6)
        {
            continue;
        }
        // One-pixel darkened ring just outside the shape (AA edge, no shadow).
        if (s > 0.0)
        {
            ring = max(ring, saturate(1.0 - abs(s - 0.9)) * 0.16);
        }
        const float cov = saturate(0.5 - s);
        if (cov <= 0.0)
        {
            continue;
        }
        const float d = max(-s, 0.0);
        // Outward normal from the SDF gradient (central differences).
        const float e = 0.5;
        const float2 g = float2(
            SdRoundRectCc(pixel + float2(e, 0.0) - centre, halfSize, r) -
                SdRoundRectCc(pixel - float2(e, 0.0) - centre, halfSize, r),
            SdRoundRectCc(pixel + float2(0.0, e) - centre, halfSize, r) -
                SdRoundRectCc(pixel - float2(0.0, e) - centre, halfSize, r));
        const float2 nOut = g / max(length(g), 1e-4);
        float3 color;
        if (hasBackdrop)
        {
            // Quarter-circle bevel: disp = |A| (1 - sqrt(1 - t^2)), t = 1 - d/h.
            const float t = saturate(1.0 - d / max(a.iconMeta.z, 1e-3));
            const float disp = a.iconMeta.y * (1.0 - sqrt(max(1.0 - t * t, 0.0))) *
                smoothstep(0.5 * dpi, 1.0 * dpi, d);
            const float2 uv = clamp((pixel - nOut * disp) * texel, lo, hi);
            const float3 c = CcSampleAxis(blurTemp, uv, texel, a.iconMeta.w, float2(0.0, 1.0));
            const float3 fillRgb = b.iconRect.w > 0.5 ? float3(0.0, 0.0, 0.0) : float3(1.0, 1.0, 1.0);
            // gain > 0: the face is a fixed colour plus the refraction signature
            // (displaced minus undisplaced blurred backdrop), so the lens stays
            // visible while uniform backdrops give an identical face.
            const float gain = b.iconMeta.x;
            float3 face = b.iconRect.y + (b.iconRect.x - b.iconRect.y) * c;
            if (gain > 0.0)
            {
                const float2 uv0 = clamp(pixel * texel, lo, hi);
                const float3 c0 = CcSampleAxis(blurTemp, uv0, texel, a.iconMeta.w, float2(0.0, 1.0));
                face += gain * (c - c0);
            }
            color = lerp(saturate(face), fillRgb, b.iconRect.z);
        }
        else
        {
            color = float3(0.8, 0.8, 0.82);
        }
        // Rim: peaks top-left / bottom-right, minima top-right / bottom-left.
        const float ang = atan2(-nOut.y, nOut.x);
        const float rimA = 0.18 + 0.545 * pow(abs(cos(ang - 2.356194)), 1.08);
        const float band = saturate(1.0 - d / dpi);
        color = lerp(color, float3(1.0, 1.0, 1.0), rimA * band);
        accum = color * cov + accum * (1.0 - cov);
        alpha = cov + alpha * (1.0 - cov);
    }
    accum *= (1.0 - ring);
    alpha = alpha + (1.0 - alpha) * ring;
    return float4(accum, alpha);
}
float4 GlassPS(VertexOutput input) : SV_Target
{
    if (scene1.z > 50.0)
    {
        return CcGlass(input.position.xy, scene0.xy, max(scene1.y, 1.0));
    }
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
    const bool dockStyle = !isPanel || textPanel > 0.5;
    const float marginDev = max(1.0 - panelOn, popupShadowOn) * DOCK_SHADOW_MARGIN_PT * dpi;
    // Panels: shared DOCK_CORNER_RADIUS_PT squircle (n=4). Dock: macOS 27
    // silhouette (Dock27Shape) with an analytic outward normal.
    float2 halfSize = max(outputSize * 0.5 - marginDev, float2(1.0, 1.0));
    float cornerRadius = max(min(DOCK_CORNER_RADIUS_PT * dpi, halfSize.y), 1.0);
    // Panels may override the radius (device px, scene3.y), e.g. Search uses the dock's own corner.
    if (isPanel && scene3.y > 0.0)
    {
        cornerRadius = max(min(scene3.y, halfSize.y), 1.0);
    }
    float distance = SdSquircleBox(pixel - outputSize * 0.5, halfSize, cornerRadius);
    float dockS = 1.0;
    float2 dockNrm = float2(0.0, 1.0);
    if (!isPanel)
    {
        Dock27Shape(pixel, outputSize, dpi, halfSize, dockS, cornerRadius, distance, dockNrm);
    }
    else if (dockStyle)
    {
        DockPanelShape(pixel, outputSize, dpi, halfSize, cornerRadius, dockS, distance, dockNrm);
    }
    const float aa = 1.35 * dpi;
    // Dock: crisp 1 px coverage (the spec edge is a 0.5-coverage crossing).
    const float mask = !dockStyle ? 1.0 - smoothstep(-aa, aa, distance) : saturate(0.5 - distance);
    // SDF gradient = 2D surface direction (CASDFLayer-style). ddx/ddy gives
    // screen-space analytic derivatives of the SDF field.
    const float2 screenGradient = float2(ddx(distance), ddy(distance));
    const float2 gradient = !dockStyle ? screenGradient : dockNrm;
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
        if (dockStyle)
        {
            // Dock: faint dark hairline just outside the edge, combined with
            // the downward contact shade when that effect is on.
            const float outerLine = Dock27OuterLine(distance, dockS) * rimGain;
            float shadowAlpha = 0.0;
            if (shadowOn > 0.5)
            {
                const float2 dockShadowCenter = outputSize * 0.5 + float2(0.0, 3.5) * dpi;
                float2 shadowNrm;
                const float dockShadowSdf = SdSquircleRectN(pixel - dockShadowCenter, halfSize,
                    cornerRadius, kDock27CornerN, shadowNrm);
                const float penumbra = 4.0 * dpi;
                const float facingDown = smoothstep(0.55, 0.98, dockNrm.y);
                const float s = 1.0 - saturate(max(dockShadowSdf, 0.0) / penumbra);
                shadowAlpha = s * s * (3.0 - 2.0 * s) * 0.14 * facingDown;
            }
            return float4(0.0, 0.0, 0.0, 1.0 - (1.0 - shadowAlpha) * (1.0 - outerLine));
        }
        // Only Quick Settings opts into a transparent popup margin. Other
        // panels retain their old no-shadow contract.
        if ((isPanel && popupShadowOn < 0.5) || shadowOn < 0.5)
        {
            return float4(0.0, 0.0, 0.0, 0.0);
        }
        // Dock: overhead light. The shade is the pill dropped a few pixels,
        // with a short penumbra, and only on the downward-facing edge. A ring
        // on the top and sides is not a cast shadow. Premultiplied black over
        // the desktop = drop shade via the DComp blend.
        if (!isPanel)
        {
            const float2 dockShadowCenter = outputSize * 0.5 + float2(0.0, 3.5) * dpi;
            const float dockShadowSdf = SdSquircleBox(pixel - dockShadowCenter, halfSize, cornerRadius);
            const float penumbra = 4.0 * dpi;
            const float2 dockOutward = gradient / max(length(gradient), 0.0001);
            // Only the downward face. 0.55 keeps the shade under the bar
            // instead of climbing the side corners.
            const float facingDown = smoothstep(0.55, 0.98, dockOutward.y);
            const float s = 1.0 - saturate(max(dockShadowSdf, 0.0) / penumbra);
            const float shadowAlpha = s * s * (3.0 - 2.0 * s) * 0.14 * facingDown;
            return float4(0.0, 0.0, 0.0, shadowAlpha);
        }
        // Panels keep a soft surround in their own margin.
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
    const LensGeo lens = GetLensGeo(PhysicalGlassPanel(isPanel, dockStyle), bevelWidth, dpi);
    float slopeMag = lens.slopeScale * slopeN;
    // Lens/rim depend only on edge distance (no icon-proximity modulation).
    const float rimLightDamp = 1.0;
    // Slab thickness T(x) = T0 + B*f(x), T0 = 0.35*bevel, B = 0.65*bevel.
    const float thicknessPx = (0.35 + 0.65 * height01) * lens.thickScale;
    const float bevelFactor = 1.0 - x; // 1 at rim, 0 in flat field

    // Frost radii: clear veil through mid, full mica on the right half.
    float frostRim;
    float frostCore;
    // Panels keep a minimum mica dissolve so busy wallpaper detail cannot
    // fight glyphs even when the user parks Frost near clear.
    // Dock-face panels follow the dock's Frost slider exactly (no floor).
    const float panelFrost = max(frostAmount,
        max(charcoalOn * DOCK_PANEL_FROST_BLUR_FLOOR,
            (dockStyle ? 0.0 : textPanel * DOCK_TEXT_PANEL_FROST_FLOOR)));
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
        float dR = SlabPull(thicknessPx, thetaS, sinS, nR, lens.gapPx) * lens.gain * lensOn;
        float dG = SlabPull(thicknessPx, thetaS, sinS, nG, lens.gapPx) * lens.gain * lensOn;
        float dB = SlabPull(thicknessPx, thetaS, sinS, nB, lens.gapPx) * lens.gain * lensOn;
        // Chromatic split (ratios physical, blue bends most); shared boost.
        dR = dG + (dR - dG) * kFringeBoost;
        dB = dG + (dB - dG) * kFringeBoost;
        // Dispersion off collapses all channels onto green (no split).
        dR = lerp(dG, dR, dispOn);
        dB = lerp(dG, dB, dispOn);
        // Soft-cap the drag (see SoftLensCap / kLensMaxPx): keep white-fringe
        // protection without a hard plateau kink toward the dock center.
        const float lensMax = lens.capPx;
        dR = SoftLensCap(dR, lensMax);
        dG = SoftLensCap(dG, lensMax);
        dB = SoftLensCap(dB, lensMax);
        const float lensRimFade = smoothstep(0.0, lens.rimFadePx, insideDistance);
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
        const float blurPx = SpotlightPanel() ? DOCK_SPOTLIGHT_BLUR_PX * dpi : lerp(frostRim, frostCore, height01) * dpi;
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

    // ---- macOS 27 dock material (dock + dock-face panels) -----------------
    // fill = cool white tint (165,162,193) at ~29% over the backdrop; rim is a
    // white additive overlay out = fill + R (1 - fill) with the spec's n_y^2
    // band light (crisp core + soft inner glow); faint dark line outside.
    if (dockStyle)
    {
        const float rimR = Dock27Rim(-distance, dockNrm, dockS) * rimGain;
        // Always on: the Tint toggle never fed the old dock face either.
        // Light mode (panel-only toggle, scene0.w) is the one parameter that
        // differs: it lifts the same tint layer toward a light plate so dark
        // ink stays legible. Dock and Light-off panels use the dock constants.
        const float lightP = isPanel ? saturate(scene0.w) : 0.0;
        float tintA = lerp(kDock27TintAlpha, 0.80, lightP);
        float3 tintC = lerp(kDock27Tint, float3(0.95, 0.96, 0.98), lightP);
        const bool spotlight = SpotlightPanel();
        if (spotlight)
        {
            // Spotlight: vibrant blurred backdrop under the system light/dark fill.
            tintA = lerp(DOCK_SPOTLIGHT_DARK_ALPHA, DOCK_SPOTLIGHT_LIGHT_ALPHA, lightP);
            tintC = lerp(DOCK_SPOTLIGHT_DARK_FILL, DOCK_SPOTLIGHT_LIGHT_FILL, lightP) * float3(1.0, 1.0, 1.0);
        }
        float3 inside;
        float insideAlpha;
        if (hasBackdrop)
        {
            float3 backdrop = frostedBackground;
            if (spotlight)
            {
                const float luma = dot(backdrop, float3(0.2126, 0.7152, 0.0722));
                backdrop = saturate(lerp(luma.xxx, backdrop, DOCK_SPOTLIGHT_SATURATION));
            }
            const float3 fill = lerp(backdrop, tintC, tintA);
            inside = fill + rimR * (spotlight ? 0.6 : 1.0) * (1.0 - fill);
            insideAlpha = 1.0;
        }
        else
        {
            // No live capture: composite the same two layers over the
            // desktop directly (tint at tintA, then white at R).
            insideAlpha = rimR + (1.0 - rimR) * tintA;
            inside = (rimR + (1.0 - rimR) * tintA * tintC) / max(insideAlpha, 1e-4);
        }
        const float glint = saturate(scene2.z) * specOn;
        if (glint > 0.0)
        {
            const float2 toGlint = pixel - scene2.xy;
            const float rimSigma = 48.0 * dpi;
            const float nearRim = exp(-dot(toGlint, toGlint) / (2.0 * rimSigma * rimSigma));
            inside += float3(0.85, 0.90, 1.0) * Dock27Rim(-distance, float2(0.0, 1.0), dockS) * 0.6 *
                nearRim * glint * rimGain;
        }
        if (scene1.z > 0.5)
        {
            const float outline = 1.0 - smoothstep(0.0, 1.0, abs(distance));
            inside = lerp(inside, float3(1.0, 0.18, 0.58), outline);
        }
        inside = saturate(inside);
        const float cov = saturate(mask * scene0.z * (hasBackdrop ? scene1.w : 1.0)) * insideAlpha;
        const float outerLine = Dock27OuterLine(distance, dockS) * rimGain;
        const float outA = cov + (1.0 - cov) * outerLine;
        return float4(inside * cov, outA);
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
    if (scene1.z > 50.0)
    {
        const float2 ccUv = clamp(pixel / outputSize, 0.5 / outputSize, 1.0 - 0.5 / outputSize);
        uvR = ccUv;
        uvG = ccUv;
        uvB = ccUv;
        blurPx = CcBlurPxAt(pixel);
        return;
    }
    // DOCK_FX_PANEL (menus): fill the surface; dock keeps the shadow margin ring.
    const float panelOn = FxEnabled(scene1.x, (float)DOCK_FX_PANEL);
    const float popupShadowOn = FxEnabled(scene1.x, (float)DOCK_FX_POPUP_SHADOW);
    const bool isPanel = panelOn > 0.5;
    const float labelFace = panelOn * FxEnabled(scene1.x, (float)DOCK_FX_LABEL_FACE);
    const float marginDev = max(1.0 - panelOn, popupShadowOn) * DOCK_SHADOW_MARGIN_PT * dpi;
    float2 halfSize = max(outputSize * 0.5 - marginDev, float2(1.0, 1.0));
    float cornerRadius = max(min(DOCK_CORNER_RADIUS_PT * dpi, halfSize.y), 1.0);
    // Panels may override the radius (device px, scene3.y), e.g. Search uses the dock's own corner.
    if (isPanel && scene3.y > 0.0)
    {
        cornerRadius = max(min(scene3.y, halfSize.y), 1.0);
    }
    float distance = SdSquircleBox(pixel - outputSize * 0.5, halfSize, cornerRadius);
    float dockS = 1.0;
    float2 dockNrm = float2(0.0, 1.0);
    const float textPanelF = panelOn * FxEnabled(scene1.x, (float)DOCK_FX_DOCK_FACE) * (1.0 - labelFace);
    const bool dockStyle = !isPanel || textPanelF > 0.5;
    if (!isPanel)
    {
        Dock27Shape(pixel, outputSize, dpi, halfSize, dockS, cornerRadius, distance, dockNrm);
    }
    else if (dockStyle)
    {
        DockPanelShape(pixel, outputSize, dpi, halfSize, cornerRadius, dockS, distance, dockNrm);
    }
    const float2 screenGradient = float2(ddx(distance), ddy(distance));
    const float2 gradient = !dockStyle ? screenGradient : dockNrm;
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
    const LensGeo lens = GetLensGeo(PhysicalGlassPanel(isPanel, dockStyle), bevelWidth, dpi);
    float slopeMag = lens.slopeScale * slopeN;
    const float fxBits = scene1.x;
    const float lensOn = FxEnabled(fxBits, 2.0);
    const float dispOn = FxEnabled(fxBits, 4.0);
    const float thicknessPx = (0.35 + 0.65 * height01) * lens.thickScale;
    const float thetaS = atan(slopeMag);
    const float sinS = sin(thetaS);
    const float sinRR = clamp(sinS / 1.5143, 0.0, 0.999);
    const float sinRG = clamp(sinS / 1.5168, 0.0, 0.999);
    const float sinRB = clamp(sinS / 1.5224, 0.0, 0.999);
    const float thetaRR = asin(sinRR);
    const float thetaRG = asin(sinRG);
    const float thetaRB = asin(sinRB);
    float dR = SlabPull(thicknessPx, thetaS, sinS, 1.5143, lens.gapPx) * lens.gain * lensOn;
    float dG = SlabPull(thicknessPx, thetaS, sinS, 1.5168, lens.gapPx) * lens.gain * lensOn;
    float dB = SlabPull(thicknessPx, thetaS, sinS, 1.5224, lens.gapPx) * lens.gain * lensOn;
    dR = dG + (dR - dG) * kFringeBoost;
    dB = dG + (dB - dG) * kFringeBoost;
    dR = lerp(dG, dR, dispOn);
    dB = lerp(dG, dB, dispOn);
    // Must stay identical to GlassPS above (see SoftLensCap / kLensMaxPx).
    const float lensMaxFrost = lens.capPx;
    dR = SoftLensCap(dR, lensMaxFrost);
    dG = SoftLensCap(dG, lensMaxFrost);
    dB = SoftLensCap(dB, lensMaxFrost);
    const float lensRimFadeFrost = smoothstep(0.0, lens.rimFadePx, insideDistance);
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
        (dockStyle ? 0.0 : textPanel * DOCK_TEXT_PANEL_FROST_FLOOR))), micaRim, micaCore);
    blurPx = SpotlightPanel() ? DOCK_SPOTLIGHT_BLUR_PX * dpi : lerp(micaRim, micaCore, height01) * dpi;
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
    const float3 h = (scene1.z > 50.0) ? CcSampleAxis(backdropTexture, uvR, texel, blurPx, float2(1.0, 0.0))
        : SampleGlassAxis(backdropTexture, uvR, uvG, uvB, texel, blurPx, float2(1.0, 0.0));
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
    const float3 v = (scene1.z > 50.0) ? CcSampleAxis(blurTemp, uvR, texel, blurPx, float2(0.0, 1.0))
        : SampleGlassAxis(blurTemp, uvR, uvG, uvB, texel, blurPx, float2(0.0, 1.0));
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
    const float3 h = (scene1.z > 50.0) ? CcSampleAxis(blurTemp2, uvR, texel, blurPx, float2(1.0, 0.0))
        : SampleGlassAxis(blurTemp2, uvR, uvG, uvB, texel, blurPx, float2(1.0, 0.0));
    return float4(h, 1.0);
}

// Chrome ink on the same face tonemap domain as GlassPS:
// face = lerp(OVER_BLACK, OVER_WHITE, backdropLuma); ink inverts that so glyphs
// stay contrasted on both dark and light plates.
// Adaptive chrome ink, macOS menu bar style: black over a light backdrop,
// white over a dark one. DockApp measures the desktop directly behind the
// tray + clock group, applies hysteresis and crossfades scene3.x over ~0.25 s.
float3 ChromeInk()
{
    const float3 darkInk = float3(DOCK_INK_R, DOCK_INK_G, DOCK_INK_B) / 255.0;
    const float3 lightInk = float3(0.96, 0.96, 0.97);
    const float t = saturate(scene3.x);
    return lerp(lightInk, darkInk, t * t * (3.0 - 2.0 * t));
}

float GlyphCoverage(float4 s)
{
    return max(s.a, max(s.r, max(s.g, s.b)));
}

static const float2 kShadowDirs[8] = {
    float2(1.0, 0.0), float2(0.7071, 0.7071), float2(0.0, 1.0), float2(-0.7071, 0.7071),
    float2(-1.0, 0.0), float2(-0.7071, -0.7071), float2(0.0, -1.0), float2(0.7071, -0.7071)
};

// macOS gives white menu bar items a faint soft shadow (none on black ones).
// Blurred coverage on a ring around the pixel, shifted so it falls ~1 px down.
float ShadowLoad(int2 texel, int layer, int2 size, float radius)
{
    float acc = 0.0;
    [unroll] for (int k = 0; k < 8; ++k)
    {
        const int2 t = texel + int2(round(kShadowDirs[k] * radius - float2(0.0, radius)));
        const bool inside = t.x >= 0 && t.y >= 0 && t.x < size.x && t.y < size.y;
        acc += inside ? GlyphCoverage(iconTexture.Load(int4(t, layer, 0))) : 0.0;
    }
    return acc / 8.0;
}

float ShadowSample(float2 uv, float layer, float side, float radius)
{
    float acc = 0.0;
    [unroll] for (int k = 0; k < 8; ++k)
    {
        const float2 t = uv + (kShadowDirs[k] * radius - float2(0.0, radius)) / side;
        const bool inside = all(t >= 0.0) && all(t <= 1.0);
        acc += inside ? GlyphCoverage(iconTexture.SampleLevel(linearClamp, float3(t, layer), 0)) : 0.0;
    }
    return acc / 8.0;
}

// Premultiplied black shadow under (rgb, a), faded out as the ink turns black.
void ApplyChromeShadow(inout float a, float blurred)
{
    const float shadow = saturate(blurred * 1.5) * 0.28 * (1.0 - saturate(scene3.x));
    a = a + shadow * (1.0 - a);
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
        // macOS 27 separator: neutral black at ~21% alpha, 1.28 S tall
        // (the slot height), ~0.049 S wide (2 px at 2x), ends fade over ~1 px.
        const float iconS = size.y / 1.28;
        const float halfWidth = max(0.0247 * iconS, 0.5);
        const float across = saturate(halfWidth + 0.5 - offset.x) - saturate(-halfWidth + 0.5 - offset.x);
        const float along = saturate(size.y * 0.5 - offset.y + 0.5);
        const float lineAlpha = saturate(across) * along * 0.21;
        return float4(0.0, 0.0, 0.0, lineAlpha);
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
            rgb = ChromeInk() * coverage;
            a = coverage;
            if (inside && scene3.x < 0.999)
            {
                ApplyChromeShadow(a, ShadowLoad(texel, (int)input.textureIndex, int2(icon.iconRect.zw),
                    max(1.0, icon.iconRect.w / 28.0)));
            }
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
        color = ChromeInk() * coverage * iconBrightness;
        float shaded = coverage;
        if (input.uv.y <= contentHeightRatio && scene3.x < 0.999)
        {
            // Content is square (iconRect.z px each way).
            const float side = max(icon.iconRect.z, 1.0);
            const float2 iconUv = saturate(float2(input.uv.x, input.uv.y / max(contentHeightRatio, 0.001)));
            ApplyChromeShadow(shaded, ShadowSample(iconUv, input.textureIndex, side, max(1.0, side / 18.0)));
        }
        alpha = shaded * iconBrightness;
    }

    if (icon.iconMeta.x > 0.5 && contentHeightRatio < 0.98 && input.uv.y > contentHeightRatio) {
        // macOS 27 running dot: diameter 0.091 S, centre 0.21 S below the
        // icon body (S = icon width), opaque near-black, 1 px AA.
        const float iconS = icon.iconRect.z;
        const float dotRadiusPx = max(0.0455 * iconS, 1.0);
        float2 dotOffset;
        dotOffset.x = (input.uv.x - 0.5) * icon.iconRect.z;
        dotOffset.y = input.uv.y * icon.iconRect.w - (iconS + 0.21 * iconS);
        const float dist = length(dotOffset);
        const float dotAlpha = saturate(dotRadiusPx + 0.5 - dist);
        // macOS running indicator: small black circle on dark, light, and photo bars.
        const float3 runningDot = float3(0.0, 0.0, 0.0);
        color = color * (1.0 - dotAlpha) + runningDot * dotAlpha;
        alpha = dotAlpha + alpha * (1.0 - dotAlpha);
    }

    return float4(color, alpha);
}
