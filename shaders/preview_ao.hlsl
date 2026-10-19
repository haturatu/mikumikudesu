// Reference/adaptation notices: docs/preview/THIRD_PARTY_NOTICES.md
#include "preview_normal.hlsli"

// GTAO uses view-space horizon searches and the analytic cosine-weighted
// slice integral from Jimenez et al., Practical Real-Time Strategies for
// Accurate Indirect Occlusion. See docs/preview/render-effects.md for sources.
// ComputeLegacy retains the previous estimator; both use BlurAo.

struct AoConstants {
    uint width;
    uint height;
    uint horizontal;
    uint mode;
    // x = focal length (perspective) or orthographic extent, y = aspect, z = near, w = far
    float4 projection;
    // x = intensity, y = radius scale, z = 1 for perspective, w = reserved
    float4 params;
};

[[vk::push_constant]] ConstantBuffer<AoConstants> ao;
[[vk::binding(0, 0)]] Texture2D<float> depthTexture;
[[vk::binding(1, 0)]] SamplerState aoSampler;
[[vk::binding(2, 0)]] Texture2D<float> inputAo;
[[vk::binding(3, 0)]]
#ifdef DAYO_GLSLC
#ifdef DAYO_AO_R32
[[spv::format_r32f]]
#elif defined(DAYO_AO_R16)
[[spv::format_r16f]]
#else
[[spv::format_r8]]
#endif
#else
#ifdef DAYO_AO_R32
[[vk::image_format("r32f")]]
#elif defined(DAYO_AO_R16)
[[vk::image_format("r16f")]]
#else
[[vk::image_format("r8")]]
#endif
#endif
RWTexture2D<float> outputAo;
[[vk::binding(4, 0)]] Texture2D<float2> normalTexture;

// Octahedral coordinates cannot be interpolated across their folded seams.
// Fetch one encoded normal and decode it before computing angular weights.
float3 sampleNormal(float2 uv) {
    uint width, height;
    normalTexture.GetDimensions(width, height);
    const int2 pixel = clamp(int2(uv * float2(width, height)), int2(0, 0), int2(width, height) - 1);
    return decodeOctNormal(normalTexture.Load(int3(pixel, 0)));
}

float interleavedGradientNoise(float2 position) {
    return frac(52.9829189 * frac(0.06711056 * position.x + 0.00583715 * position.y));
}

// Inverse of makeVertex's projection in preview.hlsl. preview.hlsl flips
// clip-space Y, so view space Y grows upwards while uv.y grows downwards.
float3 viewPositionFromDepth(float2 uv, float depth) {
    const float2 ndc = uv * 2.0 - 1.0;
    const float aspect = max(ao.projection.y, 1e-5);
    if (ao.params.z > 0.5) {
        const float nearPlane = max(ao.projection.z, 1e-5);
        const float farPlane = max(ao.projection.w, nearPlane + 1e-3);
        // clipDepth = far / (far - near) - far * near / ((far - near) * viewZ)
        const float scale = farPlane / (farPlane - nearPlane);
        const float offset = farPlane * nearPlane / (farPlane - nearPlane);
        const float viewZ = offset / max(scale - depth, 1e-6);
        const float focal = max(ao.projection.x, 1e-5);
        return float3(ndc.x * aspect * viewZ / focal, -ndc.y * viewZ / focal, viewZ);
    }
    const float viewZ = depth * (ao.projection.w - ao.projection.z) + ao.projection.z;
    const float extent = max(ao.projection.x, 1e-5);
    return float3(ndc.x * aspect * extent, -ndc.y * extent, viewZ);
}

float2 projectToUv(float3 viewPosition) {
    const float aspect = max(ao.projection.y, 1e-5);
    float2 ndc;
    if (ao.params.z > 0.5) {
        const float focal = max(ao.projection.x, 1e-5);
        const float invZ = focal / max(viewPosition.z, 1e-4);
        ndc = float2(viewPosition.x * invZ / aspect, -viewPosition.y * invZ);
    } else {
        const float extent = max(ao.projection.x, 1e-5);
        ndc = float2(viewPosition.x / (aspect * extent), -viewPosition.y / extent);
    }
    return ndc * 0.5 + 0.5;
}

// Screen pixels covered by one view-space unit at the given depth. Used to skip
// pixels whose occlusion radius is smaller than the sampling grid, where the
// marcher can only produce self-intersections.
float pixelsPerViewUnit(float viewZ) {
    if (ao.params.z > 0.5)
        return 0.5 * float(ao.height) * max(ao.projection.x, 1e-5) / max(viewZ, 1e-3);
    return 0.5 * float(ao.height) / max(ao.projection.x, 1e-5);
}

[numthreads(8, 8, 1)]
void ComputeGtao(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ao.width || id.y >= ao.height)
        return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(ao.width, ao.height);
    const float centerDepth = depthTexture.SampleLevel(aoSampler, uv, 0);
    if (centerDepth >= 0.9999) {
        outputAo[id.xy] = 1.0;
        return;
    }
    const float3 position = viewPositionFromDepth(uv, centerDepth);
    const float3 normal = sampleNormal(uv);
    const float3 view = ao.params.z > 0.5 ? normalize(-position) : float3(0.0, 0.0, -1.0);
    if (dot(normal, view) <= 0.02) {
        outputAo[id.xy] = 1.0;
        return;
    }
    const float radius = max(ao.params.y, 1e-3);
    const float screenRadius = radius * pixelsPerViewUnit(position.z);
    if (screenRadius < 1.5 || ao.params.x <= 0.0) {
        outputAo[id.xy] = 1.0;
        return;
    }
    const float noise = interleavedGradientNoise(float2(id.xy));
    float visibility = 0.0;
    [loop]
    for (uint slice = 0; slice < 4; ++slice) {
        const float phi = (float(slice) + noise) * 0.78539816339;
        const float3 direction = float3(cos(phi), sin(phi), 0.0);
        const float3 orthogonal = normalize(direction - dot(direction, view) * view);
        const float3 axis = normalize(cross(orthogonal, view));
        const float3 projected = normal - axis * dot(normal, axis);
        const float projectedLength = length(projected);
        if (projectedLength < 1e-5) continue;
        const float cosNormal = saturate(dot(projected, view) / projectedLength);
        const float normalAngle = (dot(orthogonal, projected) < 0.0 ? -1.0 : 1.0) * acos(cosNormal);
        float2 horizon = float2(cos(normalAngle + 1.57079632679), cos(normalAngle - 1.57079632679));
        const float2 minimumHorizon = horizon;
        [loop]
        for (uint step = 0; step < 5; ++step) {
            const float fraction = (float(step) + 0.5 + 0.5 * noise) / 5.0;
            const float pixelDistance = max(1.5, screenRadius * fraction * fraction);
            const float2 offset = float2(cos(phi), -sin(phi)) * pixelDistance / float2(ao.width, ao.height);
            [unroll]
            for (int side = 0; side < 2; ++side) {
                const float2 sampleUv = uv + offset * (side == 0 ? 1.0 : -1.0);
                if (any(sampleUv < 0.0) || any(sampleUv > 1.0)) continue;
                const float sampleDepth = depthTexture.SampleLevel(aoSampler, sampleUv, 0);
                if (sampleDepth >= 0.9999) continue;
                const float3 delta = viewPositionFromDepth(sampleUv, sampleDepth) - position;
                const float distance = length(delta);
                if (distance < 1e-4) continue;
                const float falloff = saturate(2.0 - 2.0 * distance / radius);
                const float candidate = dot(delta / distance, view);
                horizon[side] = max(horizon[side], lerp(minimumHorizon[side], candidate, falloff));
            }
        }
        const float lower = -acos(clamp(horizon.y, -1.0, 1.0));
        const float upper = acos(clamp(horizon.x, -1.0, 1.0));
        const float integralLower = (cosNormal + 2.0 * lower * sin(normalAngle) - cos(2.0 * lower - normalAngle)) * 0.25;
        const float integralUpper = (cosNormal + 2.0 * upper * sin(normalAngle) - cos(2.0 * upper - normalAngle)) * 0.25;
        visibility += projectedLength * (integralLower + integralUpper);
    }
    outputAo[id.xy] = saturate(1.0 - (1.0 - saturate(visibility * 0.25)) * ao.params.x);
}

// Original neighbour-difference heuristic. Kept for the "SSAO (legacy)" mode.
[numthreads(8, 8, 1)]
void ComputeLegacy(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ao.width || id.y >= ao.height)
        return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(ao.width, ao.height);
    const float center = depthTexture.SampleLevel(aoSampler, uv, 0);
    const float3 normal = sampleNormal(uv);
    if (center >= 0.9999) {
        outputAo[id.xy] = 1.0;
        return;
    }
    const float2 pixel = 1.0 / float2(ao.width, ao.height);
    float occlusion = 0.0;
    [unroll]
    for (uint i = 0; i < 8; ++i) {
        const float angle = (float(i) + 0.5) * 0.78539816339;
        const float2 direction = float2(cos(angle), sin(angle));
        const float2 offset = direction * pixel * 6.0;
        const float neighbor = depthTexture.SampleLevel(aoSampler, uv + offset, 0);
        const float3 neighborNormal = sampleNormal(uv + offset);
        const float difference = center - neighbor;
        occlusion += saturate((difference - 0.0008) * 180.0) * (1.0 - saturate(abs(difference) * 60.0)) *
                     saturate(dot(normal, neighborNormal) * 0.5 + 0.5);
    }
    outputAo[id.xy] = saturate(1.0 - occlusion * 0.09 * ao.params.x);
}

[numthreads(8, 8, 1)]
void BlurAo(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ao.width || id.y >= ao.height)
        return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(ao.width, ao.height);
    const float centerDepth = depthTexture.SampleLevel(aoSampler, uv, 0);
    const float2 direction =
        ao.horizontal != 0 ? float2(1.0 / ao.width, 0.0) : float2(0.0, 1.0 / ao.height);
    float total = 0.0;
    float weight = 0.0;
    [unroll]
    for (int i = -2; i <= 2; ++i) {
        const float2 position = uv + direction * float(i);
        const float depth = depthTexture.SampleLevel(aoSampler, position, 0);
        const float bilateral = exp(-abs(depth - centerDepth) * 250.0);
        const float spatial = i == 0 ? 1.0 : (abs(i) == 1 ? 0.6 : 0.3);
        const float w = bilateral * spatial;
        total += inputAo.SampleLevel(aoSampler, position, 0) * w;
        weight += w;
    }
    outputAo[id.xy] = total / max(weight, 1e-5);
}
