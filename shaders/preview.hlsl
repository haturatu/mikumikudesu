struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color : COLOR0;
};

VertexOutput VS(VertexInput input, uint vertexId : SV_VertexID)
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
    if (input.normal.x == 0.0 && input.normal.y == 0.0 && input.normal.z == 0.0)
    {
        output.position = float4(positions[vertexId], 0.0, 1.0);
        output.color = colors[vertexId];
    }
    else
    {
        float3 normal = normalize(input.normal);
        output.position = float4(input.position.x, -input.position.y, input.position.z * 0.1, 1.0);
        output.color = 0.28 + 0.62 * abs(normal.zyx);
    }
    return output;
}

float4 PS(VertexOutput input) : SV_Target0
{
    return float4(input.color, 1.0);
}
