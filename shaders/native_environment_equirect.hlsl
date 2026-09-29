struct NativeEnvironmentConstants {
    uint faceSize;
    uint mipLevels;
    uint reserved0;
    uint reserved1;
};

[[vk::push_constant]] ConstantBuffer<NativeEnvironmentConstants> environment;
[[vk::binding(0, 0)]] Texture2D<float4> equirectangular;
[[vk::binding(1, 0)]]
#ifdef DAYO_GLSLC
[[spv::format_rgba16f]]
#else
[[vk::image_format("rgba16f")]]
#endif
RWTexture2DArray<float4> cube;
[[vk::binding(2, 0)]] SamplerState environmentSampler;

float3 faceDirection(uint face, float2 position) {
    switch (face) {
    case 0:
        return normalize(float3(1.0, -position.y, -position.x));
    case 1:
        return normalize(float3(-1.0, -position.y, position.x));
    case 2:
        return normalize(float3(position.x, 1.0, position.y));
    case 3:
        return normalize(float3(position.x, -1.0, -position.y));
    case 4:
        return normalize(float3(position.x, -position.y, 1.0));
    default:
        return normalize(float3(-position.x, -position.y, -1.0));
    }
}

[numthreads(8, 8, 1)]
void EquirectToCube(uint3 id : SV_DispatchThreadID) {
    if (id.x >= environment.faceSize || id.y >= environment.faceSize || id.z >= 6)
        return;
    const float2 uv = (float2(id.xy) + 0.5) / float(environment.faceSize);
    const float2 facePosition = uv * 2.0 - 1.0;
    const float3 direction = faceDirection(id.z, facePosition);
    const float longitude = atan2(direction.z, direction.x);
    const float latitude = asin(clamp(direction.y, -1.0, 1.0));
    const float2 sourceUv = float2(longitude / (2.0 * 3.14159265359) + 0.5,
                                   0.5 - latitude / 3.14159265359);
    cube[id] = equirectangular.SampleLevel(environmentSampler,
                                         float2(frac(sourceUv.x), saturate(sourceUv.y)), 0);
}
