struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color : COLOR0;
};

VertexOutput VS(uint vertexId : SV_VertexID)
{
    const float2 positions[3] = {
        float2( 0.0, -0.65),
        float2( 0.65, 0.55),
        float2(-0.65, 0.55)
    };
    const float3 colors[3] = {
        float3(0.12, 0.84, 0.78),
        float3(0.95, 0.35, 0.64),
        float3(0.45, 0.55, 1.00)
    };

    VertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.color = colors[vertexId];
    return output;
}

float4 PS(VertexOutput input) : SV_Target0
{
    return float4(input.color, 1.0);
}
