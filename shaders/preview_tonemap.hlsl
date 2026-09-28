struct FullscreenVertex {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> hdrColor;
[[vk::binding(1, 0)]] SamplerState colorSampler;

FullscreenVertex VS(uint vertexId : SV_VertexID) {
    const float2 position = vertexId == 0 ? float2(-1.0, -1.0)
                            : vertexId == 1 ? float2(3.0, -1.0)
                                            : float2(-1.0, 3.0);
    FullscreenVertex output;
    output.position = float4(position, 0.0, 1.0);
    output.uv = position * 0.5 + 0.5;
    return output;
}

float3 acesFilm(float3 value) {
    return saturate((value * (2.51 * value + 0.03)) /
                    max(value * (2.43 * value + 0.59) + 0.14, 1e-5));
}

float4 PS(FullscreenVertex input) : SV_Target0 {
    const float4 color = hdrColor.Sample(colorSampler, input.uv);
    return float4(acesFilm(color.rgb), color.a);
}
