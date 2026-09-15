cbuffer FrameData : register(b0)
{
    float4 scene0; // output width, output height, glass alpha, time
    float4 scene1; // slide progress, DPI scale, dev bounds, unused
};

struct IconInstance
{
    float4 iconRect;
    float4 iconMeta;
};

StructuredBuffer<IconInstance> iconInstances : register(t0);
Texture2DArray iconTexture : register(t1);
SamplerState linearClamp : register(s0);

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint textureIndex : TEXCOORD1;
};

float RoundedBoxSdf(float2 point, float2 halfSize, float radius)
{
    const float2 distance = abs(point) - halfSize + radius;
    return length(max(distance, 0.0)) + min(max(distance.x, distance.y), 0.0) - radius;
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
    return output;
}

float4 GlassPS(VertexOutput input) : SV_Target
{
    const float2 outputSize = scene0.xy;
    const float2 pixel = input.position.xy;
    const float radius = min(outputSize.y * 0.47, 43.0 * scene1.y);
    const float distance = RoundedBoxSdf(pixel - outputSize * 0.5, outputSize * 0.5 - 1.5, radius);
    const float mask = 1.0 - smoothstep(-1.0, 1.5, distance);

    const float2 uv = pixel / outputSize;
    const float travel = scene0.w * 0.32 + scene1.x * 0.17;
    const float ripple = sin((uv.x + uv.y * 1.9 + travel) * 18.0) * 0.004;
    const float2 refracted = uv + float2(ripple, -ripple * 0.7);
    const float3 background = lerp(float3(0.025, 0.04, 0.085), float3(0.15, 0.25, 0.36),
        saturate(refracted.y + 0.2 * sin(refracted.x * 8.0 + travel)));
    const float edge = saturate(1.0 - abs(distance) / max(radius, 1.0));
    const float fresnel = pow(1.0 - edge, 3.0);
    const float chromatic = sin((uv.x * 41.0 - uv.y * 23.0) + travel) * 0.012;
    float3 color = background + float3(chromatic, 0.008, -chromatic);
    color += float3(0.34, 0.48, 0.68) * (0.13 + fresnel * 0.28);
    color += float3(0.7, 0.9, 1.0) * smoothstep(0.0, 0.9, edge) * 0.10;

    if (scene1.z > 0.5)
    {
        const float outline = 1.0 - smoothstep(0.0, 1.0, abs(distance));
        color = lerp(color, float3(1.0, 0.18, 0.58), outline);
    }

    const float alpha = mask * scene0.z;
    return float4(color * alpha, alpha);
}

float4 IconPS(VertexOutput input) : SV_Target
{
    const float2 centered = input.uv - 0.5;
    const float plateDistance = RoundedBoxSdf(centered, float2(0.49, 0.49), 0.17);
    const float plate = 1.0 - smoothstep(-0.012, 0.016, plateDistance);
    const float2 lightVector = normalize(float2(-0.7, -1.0));
    const float highlight = saturate(dot(normalize(centered + 0.001), -lightVector) * 0.5 + 0.5);
    const float3 plateColor = lerp(float3(0.18, 0.28, 0.38), float3(0.48, 0.66, 0.82), highlight);
    const IconInstance icon = iconInstances[input.textureIndex];
    const float4 sampled = iconTexture.Sample(linearClamp, float3(input.uv, input.textureIndex));
    const float fallbackGlyph = 1.0 - smoothstep(0.17, 0.20, max(abs(centered.x), abs(centered.y)));
    const float iconAlpha = max(sampled.a, fallbackGlyph * 0.30);
    const float3 iconColor = sampled.a > 0.01 ? sampled.rgb : float3(0.9, 0.96, 1.0);
    const float plateAlpha = plate * (0.30 + icon.iconMeta.y * 0.12);
    const float alpha = saturate(plateAlpha + iconAlpha * (1.0 - plateAlpha));
    float3 color = plateColor * plateAlpha * (1.0 - iconAlpha) + iconColor * iconAlpha;

    if (icon.iconMeta.x > 0.5)
    {
        const float indicator = smoothstep(0.075, 0.045, length(input.uv - float2(0.5, 0.96)));
        color += float3(0.24, 0.76, 1.0) * indicator;
    }

    return float4(color, alpha);
}
