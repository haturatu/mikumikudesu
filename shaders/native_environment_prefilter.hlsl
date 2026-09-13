struct NativeEnvironmentConstants {
    uint faceSize;
    uint mipLevels;
    uint reserved0;
    uint reserved1;
};

[[vk::push_constant]] ConstantBuffer<NativeEnvironmentConstants> environment;
[[vk::binding(0, 0)]] RWTexture2DArray<float4> sourceCube;
[[vk::binding(1, 0)]] RWTexture2DArray<float4> prefilteredCube;

[numthreads(8, 8, 1)]
void PrefilterCube(uint3 id : SV_DispatchThreadID) {
    if (id.x >= environment.faceSize || id.y >= environment.faceSize || id.z >= 6)
        return;
    const int2 coordinate = int2(id.xy);
    const int2 maximum = int2(environment.faceSize) - 1;
    float4 value = 0.0.xxxx;
    uint samples = 0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            const int2 sampleCoordinate = clamp(coordinate + int2(x, y), 0, maximum);
            value += sourceCube[uint3(sampleCoordinate, id.z)];
            ++samples;
        }
    }
    prefilteredCube[id] = value / float(samples);
}
