struct NativeEnvironmentConstants {
    uint faceSize;
    uint mipLevels;
    uint mipLevel;
    uint sampleCount;
};

[[vk::push_constant]] ConstantBuffer<NativeEnvironmentConstants> environment;
[[vk::binding(0, 0)]] TextureCube<float4> sourceCube;
[[vk::binding(1, 0)]]
#ifdef DAYO_GLSLC
[[spv::format_rgba16f]]
#else
[[vk::image_format("rgba16f")]]
#endif
RWTexture2DArray<float4> prefilteredCube;
[[vk::binding(2, 0)]] SamplerState cubeSampler;

static const float PI = 3.14159265359;

float3 faceDirection(uint face, float2 position) {
    switch (face) {
    case 0: return normalize(float3(1.0, -position.y, -position.x));
    case 1: return normalize(float3(-1.0, -position.y, position.x));
    case 2: return normalize(float3(position.x, 1.0, position.y));
    case 3: return normalize(float3(position.x, -1.0, -position.y));
    case 4: return normalize(float3(position.x, -position.y, 1.0));
    default: return normalize(float3(-position.x, -position.y, -1.0));
    }
}

float radicalInverse(uint bits) {
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555) << 1) | ((bits & 0xaaaaaaaa) >> 1);
    bits = ((bits & 0x33333333) << 2) | ((bits & 0xcccccccc) >> 2);
    bits = ((bits & 0x0f0f0f0f) << 4) | ((bits & 0xf0f0f0f0) >> 4);
    bits = ((bits & 0x00ff00ff) << 8) | ((bits & 0xff00ff00) >> 8);
    return float(bits) * 2.3283064365386963e-10;
}

float3 importanceSampleGGX(float2 xi, float3 normal, float roughness) {
    const float alpha = max(roughness * roughness, 0.001);
    const float a2 = alpha * alpha;
    const float phi = 2.0 * PI * xi.x;
    const float cosine = sqrt((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y));
    const float sine = sqrt(max(1.0 - cosine * cosine, 0.0));
    const float3 tangent = normalize(cross(abs(normal.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0), normal));
    const float3 bitangent = cross(normal, tangent);
    return normalize(tangent * (cos(phi) * sine) + bitangent * (sin(phi) * sine) + normal * cosine);
}

[numthreads(8, 8, 1)]
void PrefilterCube(uint3 id : SV_DispatchThreadID) {
    const uint size = max(environment.faceSize >> environment.mipLevel, 1U);
    if (id.x >= size || id.y >= size || id.z >= 6)
        return;
    const float2 facePosition = (float2(id.xy) + 0.5) * (2.0 / float(size)) - 1.0;
    const float3 normal = faceDirection(id.z, facePosition);
    if (environment.mipLevel == 0) {
        prefilteredCube[id] = sourceCube.SampleLevel(cubeSampler, normal, 0);
        return;
    }
    const float roughness = float(environment.mipLevel) / float(max(environment.mipLevels - 1U, 1U));
    float3 radiance = 0;
    float weight = 0;
    [loop]
    for (uint sampleIndex = 0; sampleIndex < environment.sampleCount; ++sampleIndex) {
        const float2 xi = float2(float(sampleIndex) / float(environment.sampleCount), radicalInverse(sampleIndex));
        const float3 halfVector = importanceSampleGGX(xi, normal, roughness);
        const float3 light = normalize(2.0 * dot(normal, halfVector) * halfVector - normal);
        const float noL = saturate(dot(normal, light));
        if (noL > 0.0) {
            radiance += sourceCube.SampleLevel(cubeSampler, light, 0).rgb * noL;
            weight += noL;
        }
    }
    prefilteredCube[id] = float4(radiance / max(weight, 1e-5), 1.0);
}
