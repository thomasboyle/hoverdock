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
SamplerState iconPointClamp : register(s1);

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint textureIndex : TEXCOORD1;
};

float RoundedBoxSdf(float2 boxPosition, float2 halfSize, float radius)
{
    const float2 distance = abs(boxPosition) - halfSize + radius;
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

float3 SampleFrostedBackdrop(float2 uv, float2 texel, float2 normal, float edge)
{
    const float2 refracted = normal * (1.2 + edge * 4.0) * texel;
    const float2 centerUv = clamp(uv + refracted, texel * 0.5, 1.0 - texel * 0.5);
    const float2 blur = texel * (1.25 + edge * 2.5);
    const float3 center = backdropTexture.Sample(linearClamp, centerUv).rgb;
    const float3 samples =
        backdropTexture.Sample(linearClamp, centerUv + float2(blur.x, 0.0)).rgb +
        backdropTexture.Sample(linearClamp, centerUv - float2(blur.x, 0.0)).rgb +
        backdropTexture.Sample(linearClamp, centerUv + float2(0.0, blur.y)).rgb +
        backdropTexture.Sample(linearClamp, centerUv - float2(0.0, blur.y)).rgb +
        backdropTexture.Sample(linearClamp, centerUv + blur).rgb +
        backdropTexture.Sample(linearClamp, centerUv - blur).rgb +
        backdropTexture.Sample(linearClamp, centerUv + float2(blur.x, -blur.y)).rgb +
        backdropTexture.Sample(linearClamp, centerUv + float2(-blur.x, blur.y)).rgb;
    return center * 0.20 + samples * 0.10;
}

float4 GlassPS(VertexOutput input) : SV_Target
{
    const float2 outputSize = scene0.xy;
    const float2 pixel = input.position.xy;
    const float radius = min(outputSize.y * 0.47, 43.0 * scene1.y);
    const float distance = RoundedBoxSdf(pixel - outputSize * 0.5, outputSize * 0.5 - 1.5, radius);
    const float mask = 1.0 - smoothstep(-1.0, 1.5, distance);
    const float2 texel = 1.0 / outputSize;
    const float2 uv = pixel * texel;

    const float2 gradient = float2(ddx(distance), ddy(distance));
    const float2 normal = gradient / max(length(gradient), 0.0001);
    const float insideDistance = max(-distance, 0.0);
    const float edge = 1.0 - smoothstep(0.0, max(radius * 0.35, 1.0), insideDistance);
    const float3 frostedBackground = SampleFrostedBackdrop(uv, texel, normal, edge);

    const float3 coolTint = float3(0.72, 0.84, 1.0);
    float3 color = lerp(frostedBackground, frostedBackground + coolTint * (0.10 + edge * 0.08),
        0.26);
    const float fresnel = pow(saturate(edge), 2.2);
    const float3 lightDirection = normalize(float3(-0.45, -0.85, 0.55));
    const float3 surfaceNormal = normalize(float3(normal, 0.55));
    const float specular = pow(saturate(dot(surfaceNormal, lightDirection)), 18.0) *
        (0.24 + fresnel * 0.76);
    color += float3(0.34, 0.54, 0.82) * fresnel * 0.17;
    color += float3(0.84, 0.94, 1.0) * specular * 0.24;

    if (scene1.z > 0.5)
    {
        const float outline = 1.0 - smoothstep(0.0, 1.0, abs(distance));
        color = lerp(color, float3(1.0, 0.18, 0.58), outline);
    }

    const float alpha = mask * scene0.z * scene1.w;
    return float4(color * alpha, alpha);
}

float4 IconPS(VertexOutput input) : SV_Target
{
    const IconInstance icon = iconInstances[input.textureIndex];
    const float4 sampled = iconTexture.Sample(iconPointClamp, float3(input.uv, input.textureIndex));
    const float indicator = icon.iconMeta.x > 0.5
        ? smoothstep(0.075, 0.045, length(input.uv - float2(0.5, 0.96)))
        : 0.0;
    const float alpha = saturate(sampled.a + indicator * (1.0 - sampled.a));
    const float3 color = sampled.rgb * sampled.a +
        float3(0.24, 0.76, 1.0) * indicator * (1.0 - sampled.a);
    return float4(color, alpha);
}
