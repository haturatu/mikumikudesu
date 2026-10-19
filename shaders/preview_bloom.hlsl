// Bloom chain used by the HDR preview path.
//
// A single compute entry point drives the whole chain so the renderer only has
// to create one pipeline and one descriptor set layout:
//
//   mode 0 - threshold the full resolution HDR image while downsampling it to
//            half resolution (the "prefilter").
//   mode 1 - horizontal separable Gaussian on the half resolution image.
//   mode 2 - vertical separable Gaussian on the half resolution image.
//
// The renderer ping-pongs between two half resolution images, so the source and
// destination descriptors are rebuilt per dispatch.

struct BloomConstants {
    uint width;    // destination width
    uint height;   // destination height
    uint mode;     // 0 prefilter, 1 horizontal blur, 2 vertical blur
    uint reserved;
    float4 params; // x = threshold, y = reserved, z = reserved, w = reserved
};
[[vk::push_constant]] ConstantBuffer<BloomConstants> bloom;

[[vk::binding(0, 0)]] Texture2D<float4> bloomSource;
[[vk::binding(1, 0)]] SamplerState bloomSampler;
[[vk::binding(2, 0)]]
#ifdef DAYO_GLSLC
[[spv::format_rgba16f]]
#else
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> bloomOutput;

static const float bloomWeights[5] = {0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216};

float3 thresholdSample(float3 value) {
    const float luminance = dot(value, float3(0.2126, 0.7152, 0.0722));
    const float threshold = bloom.params.x;
    const float knee = max(threshold * 0.5, 1e-3);
    float soft = saturate((luminance - threshold + knee) / (2.0 * knee));
    soft = soft * soft * knee;
    const float contribution = max(luminance - threshold, 0.0) + soft;
    const float weight = luminance > 1e-5 ? contribution / luminance : 0.0;
    return value * weight;
}

[numthreads(8, 8, 1)]
void BloomFilter(uint3 id : SV_DispatchThreadID) {
    if (id.x >= bloom.width || id.y >= bloom.height)
        return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(bloom.width, bloom.height);
    if (bloom.mode == 0) {
        // The destination is exactly half the source, so one destination texel
        // spans two source texels; a bilinear box gives a stable downsample.
        const float2 texel = 0.5 / float2(bloom.width, bloom.height);
        float3 total = 0.0;
        [unroll]
        for (int y = 0; y < 2; ++y) {
            [unroll]
            for (int x = 0; x < 2; ++x)
                total += thresholdSample(bloomSource.SampleLevel(bloomSampler, uv + (float2(x, y) - 0.5) * texel, 0).rgb);
        }
        bloomOutput[id.xy] = float4(total * 0.25, 1.0);
        return;
    }
    const float2 direction =
        bloom.mode == 1 ? float2(1.0 / bloom.width, 0.0) : float2(0.0, 1.0 / bloom.height);
    float3 result = bloomSource.SampleLevel(bloomSampler, uv, 0).rgb * bloomWeights[0];
    [unroll]
    for (int tap = 1; tap < 5; ++tap) {
        const float2 offset = direction * float(tap);
        result += bloomSource.SampleLevel(bloomSampler, uv + offset, 0).rgb * bloomWeights[tap];
        result += bloomSource.SampleLevel(bloomSampler, uv - offset, 0).rgb * bloomWeights[tap];
    }
    bloomOutput[id.xy] = float4(result, 1.0);
}
