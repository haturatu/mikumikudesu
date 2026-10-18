#include "preview_color.hlsli"

// Tone mapping pass for the HDR preview path.
//
// It samples the scene-referred half float buffer (plus the bloom chain and the
// depth prepass), applies exposure and white balance, the selected view
// transform, depth of field and FXAA, and writes display-referred premultiplied
// colour. The sRGB swapchain performs the transfer encode.

struct FullscreenVertex {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

struct TonemapConstants {
    // x = view transform (DAYO_VIEW_*), y = exposure EV, z = white balance K, w = bloom intensity
    float4 options;
    // x = bloom, y = FXAA, z = depth of field, w = depth of field intensity
    float4 flags;
    // x = focal length or orthographic extent, y = aspect, z = near, w = far
    float4 projection;
    // x = focus distance, y = aperture, z = maximum circle of confusion, w = viewport height
    float4 depthOfField;
    // xy = viewport size in pixels
    float4 viewport;
};
[[vk::push_constant]] ConstantBuffer<TonemapConstants> tonemap;

[[vk::binding(0, 0)]] Texture2D<float4> hdrColor;
[[vk::binding(1, 0)]] SamplerState colorSampler;
[[vk::binding(2, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(3, 0)]] Texture2D<float4> bloomColor;
[[vk::binding(4, 0)]] Texture2D<float4> skinDiffuse;

FullscreenVertex VS(uint vertexId : SV_VertexID) {
    const float2 position = vertexId == 0 ? float2(-1.0, -1.0)
                            : vertexId == 1 ? float2(3.0, -1.0)
                                            : float2(-1.0, 3.0);
    FullscreenVertex output;
    output.position = float4(position, 0.0, 1.0);
    output.uv = position * 0.5 + 0.5;
    return output;
}

float3 sceneAt(float2 uv) {
    const float4 sample = hdrColor.Sample(colorSampler, uv);
    return sample.rgb / max(saturate(sample.a), 1e-5);
}

// preview.hlsl flips clip space Y and uses a [0, 1] Vulkan depth range, so the
// stored depth is clipDepth = far / (far - near) - far * near / ((far - near) * viewZ).
float reconstructViewZ(float depth) {
    const float nearPlane = max(tonemap.projection.z, 1e-5);
    const float farPlane = max(tonemap.projection.w, nearPlane + 1e-3);
    if (tonemap.viewport.z < 0.5) return nearPlane + depth * (farPlane - nearPlane);
    const float scale = farPlane / (farPlane - nearPlane);
    const float offset = farPlane * nearPlane / (farPlane - nearPlane);
    return offset / max(scale - depth, 1e-6);
}

// Edge-aware screen-space diffusion operates on skin diffuse only. Specular,
// sheen, hair and non-skin materials remain in the original HDR color.
float3 scatterSkin(float2 uv, float3 color) {
    if (tonemap.viewport.w <= 0.0) return color;
    const float4 center = skinDiffuse.SampleLevel(colorSampler, uv, 0);
    if (center.a <= 0.01) return color;
    const float centerZ = reconstructViewZ(sceneDepth.SampleLevel(colorSampler, uv, 0));
    const float pixelsPerUnit = tonemap.viewport.z > 0.5
        ? 0.5 * tonemap.viewport.y * tonemap.projection.x / max(centerZ, 0.05)
        : 0.5 * tonemap.viewport.y / max(tonemap.projection.x, 0.01);
    const float radius = clamp(pixelsPerUnit * 0.025 * tonemap.viewport.w, 0.0, 24.0);
    if (radius < 0.5) return color;
    float3 total = center.rgb;
    float3 weights = 1.0.xxx;
    const float2 texel = 1.0 / max(tonemap.viewport.xy, 1.0.xx);
    [loop]
    for (uint tap = 0; tap < 16; ++tap) {
        const float fraction = (float(tap) + 0.5) / 16.0;
        const float r = sqrt(fraction);
        const float angle = float(tap) * 2.39996322973;
        const float2 sampleUv = uv + float2(cos(angle), sin(angle)) * r * radius * texel;
        const float4 sample = skinDiffuse.SampleLevel(colorSampler, sampleUv, 0);
        const float sampleZ = reconstructViewZ(sceneDepth.SampleLevel(colorSampler, sampleUv, 0));
        const float edge = exp(-abs(sampleZ - centerZ) / 0.02) * saturate(sample.a);
        // Red travels furthest, blue least: approximate RGB diffusion profile.
        const float3 weight = exp(-fraction * float3(2.0, 5.0, 10.0)) * edge;
        total += sample.rgb * weight;
        weights += weight;
    }
    const float blend = saturate(center.a * tonemap.viewport.w);
    return max(color + (total / weights - center.rgb) * blend, 0.0);
}

float3 sceneAtWithBloom(float2 uv) {
    float3 color = scatterSkin(uv, sceneAt(uv));
    if (tonemap.flags.x > 0.5)
        color += bloomColor.Sample(colorSampler, uv).rgb * tonemap.options.w;
    return color;
}

float displayLuma(float3 color) {
    return dot(color, float3(0.299, 0.587, 0.114));
}

// 24 tap ring blur with a smooth circle of confusion. Cheap enough for the
// preview resolution and stable because the sample pattern is fixed.
float3 depthOfFieldSample(float2 uv, float3 center, float viewZ) {
    const float focus = max(tonemap.depthOfField.x, 1e-3);
    const float coc = clamp(abs(viewZ - focus) / focus * tonemap.depthOfField.y, 0.0, 1.0) *
                      tonemap.depthOfField.z * max(tonemap.flags.w, 0.0);
    if (coc < 0.75)
        return center;
    const float2 texel = 1.0 / max(tonemap.viewport.xy, 1.0.xx);
    float3 total = center;
    float weight = 1.0;
    [unroll]
    for (int ring = 1; ring <= 3; ++ring) {
        const float radius = coc * float(ring) / 3.0;
        [unroll]
        for (int tap = 0; tap < 8; ++tap) {
            const float angle = (float(tap) + 0.5) * 0.7853981634 + float(ring) * 0.3;
            const float2 sampleUv = uv + float2(cos(angle), sin(angle)) * radius * texel;
            const float sampleZ = reconstructViewZ(sceneDepth.SampleLevel(colorSampler, sampleUv, 0).r);
            const float sampleWeight = exp(-abs(sampleZ - viewZ) / max(0.1 * viewZ, 0.01));
            total += sceneAtWithBloom(sampleUv) * sampleWeight;
            weight += sampleWeight;
        }
    }
    return total / weight;
}

float3 displayAt(float2 uv) {
    float3 color = sceneAtWithBloom(uv);
    if (tonemap.flags.z > 0.5) {
        const float viewZ = reconstructViewZ(sceneDepth.SampleLevel(colorSampler, uv, 0).r);
        color = depthOfFieldSample(uv, color, viewZ);
    }
    return applyViewTransform(applyExposureAndWhiteBalance(color, tonemap.options.y, tonemap.options.z),
                              uint(tonemap.options.x));
}

// Keep emissive bloom outside geometry as additive RGB with zero coverage.
// The One / OneMinusSrcAlpha blend then adds the halo to the display background
// without darkening it or clipping the glow to the model silhouette.
float4 outputAt(float2 uv) {
    const float alpha = saturate(hdrColor.SampleLevel(colorSampler, uv, 0).a);
    float3 color = displayAt(uv) * alpha;
    if (tonemap.flags.x > 0.5 && alpha < 1.0) {
        const float3 bloom = bloomColor.SampleLevel(colorSampler, uv, 0).rgb * tonemap.options.w;
        color += applyViewTransform(applyExposureAndWhiteBalance(bloom, tonemap.options.y, tonemap.options.z),
                                    uint(tonemap.options.x)) * (1.0 - alpha);
    }
    return float4(color, alpha);
}

// FXAA3 "quality lite" over the already transformed neighbours.
float4 fxaa(float2 uv, float4 center) {
    const float2 texel = 1.0 / max(tonemap.viewport.xy, 1.0.xx);
    const float4 nw = outputAt(uv + float2(-1.0, -1.0) * texel);
    const float4 ne = outputAt(uv + float2(1.0, -1.0) * texel);
    const float4 sw = outputAt(uv + float2(-1.0, 1.0) * texel);
    const float4 se = outputAt(uv + float2(1.0, 1.0) * texel);
    const float lumaM = displayLuma(center.rgb);
    const float lumaNW = displayLuma(nw.rgb);
    const float lumaNE = displayLuma(ne.rgb);
    const float lumaSW = displayLuma(sw.rgb);
    const float lumaSE = displayLuma(se.rgb);
    const float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    const float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    if (lumaMax - lumaMin < max(0.0312, lumaMax * 0.125))
        return center;
    float2 direction;
    direction.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    direction.y = (lumaNW + lumaSW) - (lumaNE + lumaSE);
    const float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 0.0078125);
    const float rcpMin = 1.0 / (min(abs(direction.x), abs(direction.y)) + reduce);
    direction = clamp(direction * rcpMin, -8.0, 8.0) * texel;
    const float4 rgbA = 0.5 * (outputAt(uv + direction * (1.0 / 3.0 - 0.5)) +
                               outputAt(uv + direction * (2.0 / 3.0 - 0.5)));
    const float4 rgbB =
        rgbA * 0.5 + 0.25 * (outputAt(uv + direction * -0.5) + outputAt(uv + direction * 0.5));
    const float lumaB = displayLuma(rgbB.rgb);
    return (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
}

float4 PS(FullscreenVertex input) : SV_Target0 {
    float4 display = outputAt(input.uv);
    if (tonemap.flags.y > 0.5)
        display = fxaa(input.uv, display);
    return float4(saturate(display.rgb), saturate(display.a));
}
