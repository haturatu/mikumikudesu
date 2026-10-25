#ifndef DAYO_PREVIEW_QUALITY
#define DAYO_PREVIEW_QUALITY
#include "preview_normal.hlsli"
struct QualityConstants {
    uint2 size;
    uint2 outputSize;
    float4 projection; // focal/ortho half span, aspect, near, far
    float4 light;      // view-space light direction, contact distance
    float4 effects;    // SSR, refraction, IOR, thickness
    float4 temporal;   // history weight, validity, perspective, trace steps
    float4 color;      // white balance gain, exposure multiplier
    float4 post;       // view transform, bloom amount, SSR distance, reserved
    float4 misc;       // phase, reserved
};
[[vk::push_constant]] ConstantBuffer<QualityConstants> quality;
[[vk::binding(0, 0)]] Texture2D<float4> qualityColor;
[[vk::binding(1, 0)]] Texture2D<float> qualityDepth;
[[vk::binding(2, 0)]] Texture2D<float2> qualityNormal;
[[vk::binding(3, 0)]] Texture2D<float4> qualitySurface;
[[vk::binding(4, 0)]] Texture2D<float4> qualityMotion;
[[vk::binding(5, 0)]] Texture2D<float4> qualityHistory;
[[vk::binding(6, 0)]] Texture2D<float4> qualityHistoryGeometry;
[[vk::binding(7, 0)]] Texture2D<float4> qualityAuxiliary;
[[vk::binding(8, 0)]] Texture2D<float4> qualityArea;
[[vk::binding(9, 0)]] Texture2D<float4> qualitySearch;
[[vk::binding(10, 0)]] SamplerState qualityLinearSampler;
[[vk::binding(11, 0)]] SamplerState qualityPointSampler;
#ifdef DAYO_GLSLC
[[spv::format_rgba16f]]
#else
[[vk::image_format("rgba16f")]]
#endif
[[vk::binding(12, 0)]] RWTexture2D<float4> qualityOutput;
#ifdef DAYO_GLSLC
[[spv::format_rgba16f]]
#else
[[vk::image_format("rgba16f")]]
#endif
[[vk::binding(13, 0)]] RWTexture2D<float4> qualityGeometryOutput;
float2 qualityUv(uint2 pixel) { return (float2(pixel) + 0.5) / float2(quality.outputSize); }
int2 qualityPixel(float2 uv) { return clamp(int2(uv * quality.size), 0, int2(quality.size) - 1); }
float qualityViewZ(float depth) {
    float n = quality.projection.z, f = quality.projection.w;
    return quality.temporal.z > 0.5 ? (f * n) / max(f - depth * (f - n), 1e-6) : n + depth * (f - n);
}
float qualityDepthAt(float2 uv) { return qualityViewZ(qualityDepth.Load(int3(qualityPixel(uv), 0))); }
float3 qualityPosition(float2 uv, float z) {
    float2 p = (uv * 2 - 1) * float2(quality.projection.y, -1);
    return float3(p * (quality.temporal.z > 0.5 ? z / quality.projection.x : quality.projection.x), z);
}
float2 qualityProject(float3 p) {
    float2 xy = p.xy / (quality.temporal.z > 0.5 ? max(p.z, 0.001) / quality.projection.x : quality.projection.x);
    return xy * float2(0.5 / quality.projection.y, -0.5) + 0.5;
}
bool qualityInside(float2 uv) { return all(uv > 0) && all(uv < 1); }
float4 qualitySurfaceAt(float2 uv) { return qualitySurface.Load(int3(qualityPixel(uv), 0)); }
float qualityReactive(float4 surface) { return float((uint(round(surface.a)) >> 8) & 7U) / 7.0; }
#endif
