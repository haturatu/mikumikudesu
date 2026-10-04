#include "preview_normal.hlsli"

struct AoConstants {
    uint width;
    uint height;
    uint horizontal;
    uint padding;
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

[numthreads(8, 8, 1)]
void ComputeAO(uint3 id : SV_DispatchThreadID) {
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
        occlusion += saturate((difference - 0.0008) * 180.0) *
                     (1.0 - saturate(abs(difference) * 60.0)) *
                     saturate(dot(normal, neighborNormal) * 0.5 + 0.5);
    }
    outputAo[id.xy] = saturate(1.0 - occlusion * 0.09);
}

[numthreads(8, 8, 1)]
void BlurAO(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ao.width || id.y >= ao.height)
        return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(ao.width, ao.height);
    const float centerDepth = depthTexture.SampleLevel(aoSampler, uv, 0);
    const float2 direction = ao.horizontal != 0 ? float2(1.0 / ao.width, 0.0)
                                                 : float2(0.0, 1.0 / ao.height);
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
