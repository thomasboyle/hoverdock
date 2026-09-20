#include "DockTheme.hlsli"

cbuffer FrameData : register(b0)
{
    float4 scene0; // output width, output height, glass alpha, time
    float4 scene1; // slide progress, DPI scale, dev bounds, backdrop valid
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
//    Radius is modulated by slab height f: thin rim ~1px, thick center ~3px
//    (Clear end of the range; the frosted look hid all detail). Rotation by IGN hides ring banding.
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
    const float2 halfSize = outputSize * 0.5 - 1.5 * dpi;
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
    if (mask <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float2 texel = 1.0 / outputSize;
    const float2 uv = pixel * texel;
    const float gradLen = length(gradient);
    const float2 outward = gradient / max(gradLen, 0.0001);
    const float insideDistance = max(-distance, 0.0);
    const float rim = exp(-insideDistance / max(2.6 * dpi, 1.75));
    const bool hasBackdrop = scene1.w > 0.5;
    // Glass tint shared with the Quick Settings popup (see DockTheme.hlsli),
    // but the dock face runs the Clear variant: DOCK_GLASS_FACE_MIX lets
    // 40% of the refracted background through (popups keep 15%). Metered
    // over white the face outputs ~0.909, compositing to #ebebeb at
    // DOCK_GLASS_ALPHA (0.909*0.88+0.12=0.920).
    const float3 glassTint = DOCK_GLASS_TINT;

    // ---- Bevel geometry ---------------------------------------------------
    // Wide optical bevel (~24pt) so the lensing ring spans ~10px instead of
    // hiding inside edge AA: the superellipse height profile concentrates
    // slope near the rim, and with a 14px bevel the whole bow lived in a
    // ~2px strip under the rim light (measured: zero pixel change). 24px
    // stays in the physical 8-48px range. The flat field is preserved
    // because SdSquircleBox returns TRUE interior depth (not saturated at
    // -r): x = saturate(depth/bevel) still reaches 1 wherever depth > 24px,
    // which holds across the dock's middle at any DPI.
    float bevelWidth = clamp(24.0 * dpi, 10.0 * dpi, max(min(halfSize.x, halfSize.y) * 0.9, 1.0));
    const float x = saturate(insideDistance / max(bevelWidth, 1e-3)); // 0 rim -> 1 flat
    const float oneMinusX = 1.0 - x;
    const float oneMinusX4 = oneMinusX * oneMinusX * oneMinusX * oneMinusX;
    const float height01 = BevelHeight(oneMinusX4); // f(x): 0 rim, 1 center
    const float slopeN = BevelSlopeN(oneMinusX, oneMinusX4);
    // True surface slope dH/dDist = (B/bevel) * f'(x), B = 0.75*bevel.
    const float slopeMag = 0.75 * slopeN;
    // Slab thickness T(x) = T0 + B*f(x), T0 = 0.45*bevel (0.3-0.5 range).
    const float thicknessPx = (0.45 + 0.75 * height01) * bevelWidth;
    const float bevelFactor = 1.0 - x; // 1 at rim, 0 in flat field

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
        // T_rim*tan(dtheta)*gain ~= 0.45*bevel*0.6*gain: with the 24px bevel
        // and gain 3.0 the silhouette pulls ~19px, decaying to 0 across the
        // band - an unmistakable liquid suck-in even on photographic content
        // with no straight edges to bend.
        const float lensGain = 3.0;
        float dR = thicknessPx * tan(max(thetaS - thetaRR, 0.0)) * lensGain;
        float dG = thicknessPx * tan(max(thetaS - thetaRG, 0.0)) * lensGain;
        float dB = thicknessPx * tan(max(thetaS - thetaRB, 0.0)) * lensGain;
        // Exaggerate the physical fringe ~18x around green for visibility;
        // ratios stay physical (blue bends most). Lands ~0.7px R-B split on
        // contrast boundaries: a faint spectral edge, not a rainbow overlay.
        const float fringeBoost = 18.0;
        dR = dG + (dR - dG) * fringeBoost;
        dB = dG + (dB - dG) * fringeBoost;

        const float2 lo = texel * 0.5;
        const float2 hi = 1.0 - texel * 0.5;
        const float2 uvR = clamp(uv - outward * dR * texel, lo, hi);
        const float2 uvG = clamp(uv - outward * dG * texel, lo, hi);
        const float2 uvB = clamp(uv - outward * dB * texel, lo, hi);

        // ---- 5. Scattering blur modulated by slab height ------------------
        // Lean hard toward Clear: 1px at the rim, 3px in the thick center.
        // Frosted mush is what hid the detail in the busy-background test;
        // legibility of icons comes from the compressive tint, not the blur.
        const float blurPx = (1.0 + height01 * 2.0) * dpi; // 1 rim .. 3 center
        frostedBackground = SampleChromaticGlass(uvR, uvG, uvB, texel, blurPx, pixel);
    }

    // ---- 6. Adaptive tint / luminosity / legibility -----------------------
    // Compressive mix toward the calibrated tint: over white the face
    // meters ~0.909 pre-premult (#ebebeb composited); over black it lands
    // ~0.51, so the full range breathes with content (Clear-variant
    // behavior) while icons drawn on top keep full contrast.
    // A small chroma bleed keeps the tint influenced by underlying content.
    const float backLuma = dot(frostedBackground, float3(0.299, 0.587, 0.114));
    const float3 backChroma = frostedBackground - backLuma;
    float3 color = lerp(frostedBackground, glassTint, DOCK_GLASS_FACE_MIX);
    color += backChroma * 0.08;
    color = lerp(color, color * float3(0.98, 0.985, 0.99) + glassTint * 0.08, 0.22);

    // ---- 4. Fresnel reflection + specular ---------------------------------
    // N = normalize(grad * slopeMag, 1): flat in the field, tilted outward on
    // the bevel. Schlick F0 = 0.04. Light is a virtual top-left key (desktop
    // has no gyroscope): speculars ride the bevel, strongest at the rim.
    const float3 surfN = normalize(float3(outward * slopeMag, 1.0));
    const float cosTheta = saturate(surfN.z);
    const float fresnel = 0.04 + 0.96 * pow(1.0 - cosTheta, 5.0);
    const float3 lightDir = normalize(float3(-0.35, -0.6, 0.7));
    const float ndl = saturate(dot(surfN, lightDir));
    const float specular = pow(ndl, 64.0) * bevelFactor;
    // Bevel-weighted so the flat field keeps the #e1e1e1 calibration exact;
    // the rim picks up the reflective veil + HDR-ish push (clamped by the
    // final saturate, LDR backbuffer).
    color += fresnel * float3(0.90, 0.95, 1.0) * 0.45 * bevelFactor;
    color += specular * float3(1.0, 1.0, 1.0) * 0.55;

    // Established edge treatment: faint thickness shading, bright rim
    // caustic, and top key sheen.
    color *= 1.0 - bevelFactor * bevelFactor * 0.03;
    color += glassTint * rim * 0.12;
    color += float3(1.0, 1.0, 1.0) * pow(rim, 4.0) * 0.18;
    const float topSheen = saturate(1.0 - pixel.y / max(11.0 * dpi, 7.0));
    color += float3(0.96, 0.97, 0.98) * topSheen * rim * 0.08;

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
        const float dotRadiusPx = min(stripHeightPx * 0.294, icon.iconRect.z * 0.042);
        float2 dotOffset;
        dotOffset.x = (input.uv.x - 0.5) * icon.iconRect.z;
        dotOffset.y = (input.uv.y - dotCenterY) * icon.iconRect.w;
        const float dist = length(dotOffset);
        const float edgeSoftness = max(0.75, dotRadiusPx * 0.3);
        const float dotAlpha = 1.0 - smoothstep(dotRadiusPx, dotRadiusPx + edgeSoftness, dist);
        const float3 runningDot = float3(1.0, 191.0 / 255.0, 0.0);
        color = color * (1.0 - dotAlpha) + runningDot * dotAlpha;
        alpha = dotAlpha + alpha * (1.0 - dotAlpha);
    }

    return float4(color, alpha);
}
