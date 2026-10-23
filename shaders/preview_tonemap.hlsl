#include "preview_color.hlsli"

// Tone mapping pass for the HDR preview path.
//
// It samples the prepared scene-referred SSS/DoF image and bloom chain, applies
// exposure, white balance, the selected view transform and FXAA, and writes
// display-referred premultiplied colour. The sRGB swapchain encodes the transfer.

struct FullscreenVertex {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

struct TonemapConstants {
    // x = view transform (DAYO_VIEW_*), yz = reserved, w = bloom intensity
    float4 options;
    // x = bloom, y = FXAA, zw = reserved
    float4 flags;
    // xyz = precomputed white-balance gain, w = exposure multiplier
    float4 colorAdjustment;
    // Reserved ABI words. Spatial effects run in preview_dof.hlsl.
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

float3 sceneAtWithBloom(float2 uv) {
    float3 color = sceneAt(uv);
    if (tonemap.flags.x > 0.5)
        color += bloomColor.Sample(colorSampler, uv).rgb * tonemap.options.w;
    return color;
}

float displayLuma(float3 color) {
    return dot(color, float3(0.299, 0.587, 0.114));
}

float3 adjustedColor(float3 color) {
    return color * tonemap.colorAdjustment.w * tonemap.colorAdjustment.xyz;
}

float3 displayAt(float2 uv) {
    float3 color = sceneAtWithBloom(uv);
    return applyViewTransform(adjustedColor(color), uint(tonemap.options.x));
}

// Keep emissive bloom outside geometry as additive RGB with zero coverage.
// The One / OneMinusSrcAlpha blend then adds the halo to the display background
// without darkening it or clipping the glow to the model silhouette.
float4 outputAt(float2 uv) {
    const float alpha = saturate(hdrColor.SampleLevel(colorSampler, uv, 0).a);
    float3 color = displayAt(uv) * alpha;
    if (tonemap.flags.x > 0.5 && alpha < 1.0) {
        const float3 bloom = bloomColor.SampleLevel(colorSampler, uv, 0).rgb * tonemap.options.w;
        color += applyViewTransform(adjustedColor(bloom), uint(tonemap.options.x)) * (1.0 - alpha);
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
