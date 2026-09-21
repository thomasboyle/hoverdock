#include "DockTheme.hlsli"

// Refraction-only diagnostic (see GlassPS). 1 = raw warped backdrop,
// no material. Flip back to 0 after the warp check.
#define LIQUID_DEBUG_REFRACTION_ONLY 0

cbuffer FrameData : register(b0)
{
    float4 scene0; // output width, output height, glass alpha, dock scale
    float4 scene1; // fx bitmask | icon count, DPI scale, dev bounds, backdrop valid
};

struct IconInstance
{
    float4 iconRect;
    float4 iconMeta;
};

StructuredBuffer<IconInstance> iconInstances : register(t0);
Texture2DArray iconTexture : register(t1);
Texture2D backdropTexture : register(t2);
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

// Glass effect toggles packed in scene1.x (DOCK_FX_* bits, DockTheme.hlsli).
// All values < 256 survive the float trip exactly; callers add 0.5 to guard
// truncation at bit boundaries. High bits (value >> 8) carry the live icon
// count for icon-calm halos; total stays far below 2^24 (exactly kept).
float FxEnabled(float packed, float bit)
{
    return fmod(floor(packed / bit), 2.0);
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
// 5. Frosted / scattering blur AFTER refraction, per chromatic channel.
//    9 taps per channel (center + 8 rotated ring) = 27 backdrop fetches.
//    Radius is modulated by slab height f: thin rim ~0.5px, thick center
//    ~1.5px (near-clear; blur must not hide the warp). Rotation by IGN
//    hides ring banding.
//    Each channel is blurred around its own Snell-displaced UV so dispersion
//    survives the frosted lobe instead of being averaged away.
// ---------------------------------------------------------------------------
float3 SampleChromaticGlass(float2 uvR, float2 uvG, float2 uvB, float2 texel,
    float blurPx, float2 pixel)
{
    const float noise = InterleavedGradientNoise(pixel);
    float sine;
    float cosine;
    sincos(noise * 6.2831853, sine, cosine);
    const float2x2 rot = float2x2(cosine, -sine, sine, cosine);

    const float2 dirs[8] = {
        float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0),
        float2(0.70710678, 0.70710678), float2(-0.70710678, 0.70710678),
        float2(0.70710678, -0.70710678), float2(-0.70710678, -0.70710678)
    };

    const float wCenter = 0.18;
    const float wRing = 0.1025; // 0.18 + 8*0.1025 = 1.0
    const float2 lo = texel * 0.5;
    const float2 hi = 1.0 - texel * 0.5;

    float3 centerR = backdropTexture.Sample(linearClamp, clamp(uvR, lo, hi)).rgb;
    float3 centerG = backdropTexture.Sample(linearClamp, clamp(uvG, lo, hi)).rgb;
    float3 centerB = backdropTexture.Sample(linearClamp, clamp(uvB, lo, hi)).rgb;
    float3 acc = float3(centerR.r, centerG.g, centerB.b) * wCenter;

    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        const float2 offset = mul(dirs[i], rot) * blurPx * texel;
        acc.r += backdropTexture.Sample(linearClamp, clamp(uvR + offset, lo, hi)).r * wRing;
        acc.g += backdropTexture.Sample(linearClamp, clamp(uvG + offset, lo, hi)).g * wRing;
        acc.b += backdropTexture.Sample(linearClamp, clamp(uvB + offset, lo, hi)).b * wRing;
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
    const float marginDev = DOCK_SHADOW_MARGIN_PT * dpi;
    const float2 halfSize = outputSize * 0.5 - marginDev - 1.5 * dpi;
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
    const float fxBits = scene1.x + 0.5;
    const float rimGain = FxEnabled(fxBits, 1.0);
    const float lensOn = FxEnabled(fxBits, 2.0);
    const float dispOn = FxEnabled(fxBits, 4.0);
    const float tintOn = FxEnabled(fxBits, 16.0);
    const float specOn = FxEnabled(fxBits, 32.0);
    const float shadowOn = FxEnabled(fxBits, 64.0);
    const float thickOn = FxEnabled(fxBits, 128.0);
    if (mask <= 0.0)
    {
        if (shadowOn < 0.5)
        {
            return float4(0.0, 0.0, 0.0, 0.0);
        }
        // Soft outer contact shadow: the pill SDF re-evaluated with a small
        // down-right offset (top-left key light), quadratic falloff over a
        // 14px band. Premultiplied black over the desktop = drop shade via
        // the DComp blend. Fits inside the layout margin (18px) with room.
        const float2 shadowCenter = outputSize * 0.5 + float2(2.0, 5.0) * dpi;
        const float shadowSdf = SdSquircleBox(pixel - shadowCenter, halfSize, cornerRadius);
        const float shadowWidth = 14.0 * dpi;
        if (shadowSdf < shadowWidth)
        {
            float s = 1.0 - max(shadowSdf, 0.0) / shadowWidth;
            const float shadowAlpha = s * s * 0.22;
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
    // Gray tint token shared with the Quick Settings popup; the dock face
    // itself runs adaptive transmission (section 1 below), so no fixed mix
    // applies here.
    const float3 glassTint = DOCK_GLASS_TINT;

    // ---- Bevel geometry: one slab -----------------------------------------
    // The bevel spans the short half-axis so the whole face refracts as one
    // convex lens: gentle magnification in the field, steep warp at the rim,
    // exact calm only at the center point. minHalf already carries dpi and
    // dock scale via the layout, so the lens stays proportional at every
    // size. x = saturate(depth/bevel) still reaches 1 at the center because
    // the SDF returns true interior depth.
    float bevelWidth = max(min(halfSize.x, halfSize.y) * 0.95, 8.0 * dpi);
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
    // are untouched - only the visible warp calms. Live count rides the high
    // bits of scene1.x; lookups capped at 64.
    const uint fxU = (uint)fxBits;
    const uint haloCount = min((fxU >> 8) & 63u, 64u);
    // Frost notches ride bits 17-18: 0 clear, 1 balanced, 2 full frost.
    const uint frostLevel = min((fxU >> 17) & 3u, 2u);
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
    const float haloCalm = 1.0 - smoothstep(0.0, 10.0 * dpi, minIconDist);
    const float haloDamp = 1.0 - haloCalm * 0.9;
    slopeMag *= haloDamp;
    // Slab thickness T(x) = T0 + B*f(x), T0 = 0.35*bevel, B = 0.65*bevel.
    const float thicknessPx = (0.35 + 0.65 * height01) * bevelWidth;
    const float bevelFactor = 1.0 - x; // 1 at rim, 0 in flat field

    // Frost notch presets (settings slider, bits 17-18): blur radii in px
    // at 1x plus multiplicative veil endpoints. Function scope: both the
    // backdrop branch and the tint section below consume them.
    float frostRim;
    float frostCore;
    float frostTintLo;
    float frostTintHi;
    if (frostLevel == 0u)
    {
        frostRim = 0.0;
        frostCore = 0.0;
        frostTintLo = 0.0;
        frostTintHi = 0.0;
    }
    else if (frostLevel == 1u)
    {
        frostRim = 2.0;
        frostCore = 6.0;
        frostTintLo = 0.5;
        frostTintHi = 0.85;
    }
    else
    {
        frostRim = 8.0;
        frostCore = 18.0;
        frostTintLo = 0.8;
        frostTintHi = 0.95;
    }

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
        // Artistic gain on the physical shape. Rim displacement is
        // T_rim*tan(dtheta)*gain ~= 0.35*bevel*0.56*gain: with the full-span
        // bevel (~35px here) gain 2.3 lands ~16px at the silhouette, decaying
        // smoothly to a calm center - one continuous slab, not edge trim.
        const float lensGain = 2.3;
        float dR = thicknessPx * tan(max(thetaS - thetaRR, 0.0)) * lensGain * lensOn;
        float dG = thicknessPx * tan(max(thetaS - thetaRG, 0.0)) * lensGain * lensOn;
        float dB = thicknessPx * tan(max(thetaS - thetaRB, 0.0)) * lensGain * lensOn;
        // Exaggerate the physical fringe ~24x around green for visibility;
        // ratios stay physical (blue bends most). Lands ~1.3px R-B split on
        // contrast boundaries: saturated edge color like the reference.
        const float fringeBoost = 24.0;
        dR = dG + (dR - dG) * fringeBoost;
        dB = dG + (dB - dG) * fringeBoost;
        // Dispersion off collapses all channels onto green (no split).
        dR = lerp(dG, dR, dispOn);
        dB = lerp(dG, dB, dispOn);

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
        // Lean hard toward Clear: sharp in the thin rim, light frost in the
        // thick center. A sharp refracted image is what makes the warp
        // readable; frosted mush hides it. Icon legibility still comes from
        // the tint backing, not the blur.
        // Notch radii/veil come from the function-scope ladder above.
        const float blurPx = lerp(frostRim, frostCore, height01) * dpi;
        // Frost off samples each channel once at its (possibly refracted)
        // UV instead of the 9-tap ring: same result as a zero-radius blur
        // with a ninth of the fetches.
        // Notch 0 samples each channel once (no frost); 1-2 run the ring.
        if (frostLevel == 0u)
        {
            frostedBackground = float3(backdropTexture.Sample(linearClamp, clamp(uvR, lo, hi)).r,
                backdropTexture.Sample(linearClamp, clamp(uvG, lo, hi)).g,
                backdropTexture.Sample(linearClamp, clamp(uvB, lo, hi)).b);
        }
        else
        {
            frostedBackground = SampleChromaticGlass(uvR, uvG, uvB, texel, blurPx, pixel);
        }
    }

    // ---- 1. Fixed transmission tint -------------------------------------
    // Multiplicative, modulated by slab height only - no backdrop-driven
    // adaptation. Nearly clear at the rim (refraction + caustic carry the
    // edge), light veil over the flat field where icons need backing.
    // veil amounts come from the frost ladder above.
    const float tintAmount = lerp(frostTintLo, frostTintHi, height01) * tintOn;
    float3 color = lerp(frostedBackground, frostedBackground * glassTint, tintAmount);
    color = lerp(color, color * float3(0.98, 0.985, 0.99) + glassTint * 0.08, 0.22 * tintOn);

    // ---- 4. Fresnel reflection + specular ---------------------------------
    // N = normalize(grad * slopeMag, 1): flat in the field, tilted outward on
    // the bevel. Schlick F0 = 0.04. Light is a virtual top-left key (desktop
    // has no gyroscope): speculars ride the bevel, strongest at the rim.
    const float3 surfN = normalize(float3(outward * slopeMag, 1.0));
    const float cosTheta = saturate(surfN.z);
    const float fresnel = 0.04 + 0.96 * pow(1.0 - cosTheta, 5.0);
    const float3 lightDir = normalize(float3(-0.35, -0.6, 0.7));
    const float ndl = saturate(dot(surfN, lightDir));
    const float specular = pow(ndl, 80.0) * (bevelFactor * 0.7 + rim * 0.5);
    // Cool bounce fill from the opposite side so highlights travel instead
    // of sitting in one static lobe.
    const float3 fillDir = normalize(float3(0.55, 0.6, 0.45));
    const float fillSpec = pow(saturate(dot(surfN, fillDir)), 24.0) * bevelFactor;
    // Tight Fresnel veil at the rim (sharper optical edge definition) on
    // top of the faint broad veil below; final saturate keeps LDR range.
    // Narrowed (rim^1.5) so the border never reads as a milky frame: the
    // crisp caustic line underneath carries the edge instead.
    color += fresnel * float3(0.90, 0.95, 1.0) * 0.45 * pow(rim, 1.5) * rimGain * haloDamp;
    color += specular * float3(1.0, 1.0, 1.0) * 0.55 * specOn;
    color += fillSpec * float3(0.75, 0.85, 1.0) * 0.18 * specOn;

    // Established edge treatment: faint thickness shading, bright rim
    // caustic (the focused edge-lensing highlight, following the key light
    // around the squircle via NdotL rather than a uniform ring), and top
    // key sheen.
    // Edge thickness shading: darkens just inside the silhouette (peaks at
    // the rim, gone by 0.6 bevel) so the bright caustic sits against a
    // grounded edge. True outer shadow is drawn outside the mask below.
    const float thicknessShade = 1.0 - 0.07 * saturate(1.0 - insideDistance / max(bevelWidth * 0.6, 1e-3));
    color *= lerp(1.0, thicknessShade, thickOn);
    color += glassTint * rim * 0.05 * rimGain * haloDamp;
    color += float3(1.0, 1.0, 1.0) * pow(rim, 5.0) * 0.34 * (0.55 + 0.45 * ndl) * rimGain * haloDamp;
    const float topSheen = saturate(1.0 - pixel.y / max(11.0 * dpi, 7.0));
    color += float3(0.96, 0.97, 0.98) * topSheen * rim * 0.08 * rimGain * haloDamp;

    if (scene1.z > 0.5)
    {
        const float outline = 1.0 - smoothstep(0.0, 1.0, abs(distance));
        color = lerp(color, float3(1.0, 0.18, 0.58), outline);
    }

    const float dither = (InterleavedGradientNoise(pixel) - 0.5) / 255.0;
    color = saturate(color + dither);
    const float alpha = saturate(mask * scene0.z * (hasBackdrop ? scene1.w : 1.0) + dither);
    return float4(color * alpha, alpha);
}

float4 IconPS(VertexOutput input) : SV_Target
{
    const IconInstance icon = iconInstances[input.instanceIndex];
    if (icon.iconMeta.y > 1.5 && icon.iconMeta.y < 3.5)
    {
        const float2 size = max(icon.iconRect.zw, 1.0);
        const float2 offset = abs(input.uv - 0.5) * size;
        const float halfThickness = 0.75;
        const float cap = 2.0;
        const float lineAlpha = smoothstep(halfThickness + 0.75, halfThickness, offset.x) *
            smoothstep(size.y * 0.5, max(size.y * 0.5 - cap, 0.0), offset.y) * 0.80;
        return float4(float3(0.32, 0.32, 0.34) * lineAlpha, lineAlpha);
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
        return float4(sampledClock.rgb * clockBrightness, sampledClock.a * clockBrightness);
    }

    const float contentHeightRatio = saturate(icon.iconRect.z / max(icon.iconRect.w, 1.0));
    float4 sampled = float4(0.0, 0.0, 0.0, 0.0);
    if (input.uv.y <= contentHeightRatio) {
        const float2 iconUv = float2(input.uv.x, input.uv.y / max(contentHeightRatio, 0.001));
        sampled = iconTexture.Sample(linearClamp, float3(saturate(iconUv), input.textureIndex));
    }

    const float iconBrightness = icon.iconMeta.y > 0.5 && icon.iconMeta.z < 0.5 ? 0.5 : 1.0;
    float3 color = sampled.rgb * iconBrightness;
    float alpha = sampled.a;

    if (icon.iconMeta.x > 0.5 && contentHeightRatio < 0.98 && input.uv.y > contentHeightRatio) {
        const float stripHeightPx = icon.iconRect.w * (1.0 - contentHeightRatio);
        const float dotCenterY = contentHeightRatio + (1.0 - contentHeightRatio) * 0.8;
        const float dotRadiusPx = max(min(stripHeightPx * 0.294, icon.iconRect.z * 0.042), 2.25);
        float2 dotOffset;
        dotOffset.x = (input.uv.x - 0.5) * icon.iconRect.z;
        dotOffset.y = (input.uv.y - dotCenterY) * icon.iconRect.w;
        const float dist = length(dotOffset);
        const float edgeSoftness = max(0.6, dotRadiusPx * 0.22);
        const float dotAlpha = 1.0 - smoothstep(dotRadiusPx, dotRadiusPx + edgeSoftness, dist);
        const float3 runningDot = float3(1.0, 191.0 / 255.0, 0.0);
        color = color * (1.0 - dotAlpha) + runningDot * dotAlpha;
        alpha = dotAlpha + alpha * (1.0 - dotAlpha);
    }

    return float4(color, alpha);
}
