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

float SquircleBoxSdf(float2 position, float2 halfSize, float cornerRadius, float exponent)
{
    position = abs(position);
    position -= halfSize - cornerRadius;
    position = max(position, 0.0);
    return pow(pow(position.x, exponent) + pow(position.y, exponent), 1.0 / exponent) - cornerRadius;
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

float3 SampleFrostedBackdrop(float2 uv, float2 texel, float2 normal, float rim, float dpi)
{
    const float2 centerUv = clamp(uv + normal * rim * 1.6 * texel, texel * 0.5, 1.0 - texel * 0.5);
    const float noise = InterleavedGradientNoise(uv / max(texel, 1e-6));
    float sine;
    float cosine;
    sincos(noise * 6.2831853, sine, cosine);
    const float2x2 rot = float2x2(cosine, -sine, sine, cosine);
    // Apple-like frosted blur: wide kernel in texel units, scaled by display
    // factor so the blur stays proportional to logical size. Same 17 taps,
    // so no extra GPU cost.
    const float radius = (20.0 + rim * 6.0) * dpi;

    float3 acc = backdropTexture.Sample(linearClamp, centerUv).rgb * 0.16;
    float weight = 0.16;
    const float2 taps[16] = {
        float2(0.15, 0.00), float2(-0.15, 0.00), float2(0.00, 0.15), float2(0.00, -0.15),
        float2(0.28, 0.28), float2(-0.28, 0.28), float2(0.28, -0.28), float2(-0.28, -0.28),
        float2(0.42, 0.10), float2(-0.42, 0.10), float2(0.10, 0.42), float2(-0.10, 0.42),
        float2(0.42, -0.10), float2(-0.42, -0.10), float2(0.10, -0.42), float2(-0.10, -0.42)
    };
    [unroll]
    for (int i = 0; i < 16; ++i)
    {
        const float2 offset = mul(taps[i] * 2.4, rot) * radius * texel;
        const float w = 0.0525;
        acc += backdropTexture.Sample(linearClamp,
            clamp(centerUv + offset, texel * 0.5, 1.0 - texel * 0.5)).rgb * w;
        weight += w;
    }
    return acc / weight;
}

float4 GlassPS(VertexOutput input) : SV_Target
{
    const float2 outputSize = scene0.xy;
    const float2 pixel = input.position.xy;
    const float dpi = max(scene1.y, 1.0);
    const float2 halfSize = outputSize * 0.5 - 1.5 * dpi;
    // Shared DOCK_CORNER_RADIUS_PT (see DockTheme.hlsli): same radius as the
    // Quick Settings popup. Exponent 2.0 = true circular arcs, matching the
    // popup's GDI RoundRect geometry exactly (higher exponents look squarer).
    const float cornerRadius = max(min(DOCK_CORNER_RADIUS_PT * dpi, halfSize.y), 1.0);
    const float distance = SquircleBoxSdf(pixel - outputSize * 0.5, halfSize, cornerRadius, 2.0);
    const float aa = 1.35 * dpi;
    const float mask = 1.0 - smoothstep(-aa, aa, distance);
    const float2 gradient = float2(ddx(distance), ddy(distance));
    if (mask <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float2 texel = 1.0 / outputSize;
    const float2 uv = pixel * texel;
    const float2 normal = gradient / max(length(gradient), 0.0001);
    const float insideDistance = max(-distance, 0.0);
    const float rim = exp(-insideDistance / max(2.6 * dpi, 1.75));
    const bool hasBackdrop = scene1.w > 0.5;
    // Glass recipe shared with the Quick Settings popup (see DockTheme.hlsli).
    // Calibrated so the dock face meters #e1e1e1 over a white backdrop at
    // DOCK_GLASS_ALPHA: shader must output 0.8663 so that 0.8663*0.88+0.12=0.8824.
    const float3 glassTint = DOCK_GLASS_TINT;
    const float3 frostedBackground = hasBackdrop
        ? SampleFrostedBackdrop(uv, texel, normal, rim, dpi)
        : glassTint;

    float3 color = lerp(frostedBackground, glassTint, DOCK_GLASS_MIX);
    color = lerp(color, color * float3(0.98, 0.985, 0.99) + glassTint * 0.08, 0.22);
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
