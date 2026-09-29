struct BackgroundInput {
    [[vk::location(0)]] float3 position : POSITION;
};
struct BackgroundOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};
[[vk::binding(0, 0)]] Texture2D<float4> backgroundTexture;
[[vk::binding(4, 0)]] SamplerState backgroundSampler;
BackgroundOutput BackgroundVS(BackgroundInput input) {
    BackgroundOutput output;
    output.position = float4(input.position.xy, 0.0, 1.0);
    output.uv = input.position.xy * 0.5 + 0.5;
    return output;
}
float4 BackgroundPS(BackgroundOutput input) : SV_Target0 {
    return backgroundTexture.Sample(backgroundSampler, input.uv);
}
