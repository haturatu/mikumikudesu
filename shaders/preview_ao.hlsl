struct AoConstants {
    uint width;
    uint height;
    uint horizontal;
    uint padding;
};

[[vk::push_constant]] ConstantBuffer<AoConstants> ao;
[[vk::binding(0, 0)]] Texture2D<float> depthTexture;
[[vk::binding(1, 0)]] SamplerState aoSampler;
[[vk::binding(2, 0)]] Texture2D<float4> inputAo;
[[vk::binding(3, 0)]] RWTexture2D<float4> outputAo;
[[vk::binding(4, 0)]] Texture2D<float4> normalTexture;

[numthreads(8, 8, 1)]
void ComputeAO(uint3 id : SV_DispatchThreadID) {
    if (id.x >= ao.width || id.y >= ao.height)
        return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(ao.width, ao.height);
    const float center = depthTexture.SampleLevel(aoSampler, uv, 0);
    const float3 normal = normalize(normalTexture.SampleLevel(aoSampler, uv, 0).xyz * 2.0 - 1.0);
    if (center >= 0.9999) {
        outputAo[id.xy] = float4(1.0, 0.0, 0.0, 1.0);
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
        const float3 neighborNormal = normalize(normalTexture.SampleLevel(aoSampler, uv + offset, 0).xyz * 2.0 - 1.0);
        const float difference = center - neighbor;
        occlusion += saturate((difference - 0.0008) * 180.0) *
                     (1.0 - saturate(abs(difference) * 60.0)) *
                     saturate(dot(normal, neighborNormal) * 0.5 + 0.5);
    }
    outputAo[id.xy] = float4(saturate(1.0 - occlusion * 0.09), 0.0, 0.0, 1.0);
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
        total += inputAo.SampleLevel(aoSampler, position, 0).r * w;
        weight += w;
    }
    outputAo[id.xy] = float4(total / max(weight, 1e-5), 0.0, 0.0, 1.0);
}
