struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 normal : NORMAL0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 color : COLOR0;
};

struct MaterialConstants
{
    float4 diffuse;
    float4 camera; // xyz Euler rotation, w distance
    float4 target; // xyz target, w signed field of view (negative means orthographic)
    float4 light;  // xyz direction, w framebuffer aspect
};
[[vk::push_constant]] ConstantBuffer<MaterialConstants> material;
[[vk::binding(0, 0)]] Texture2D<float4> baseTexture;
[[vk::binding(1, 0)]] SamplerState baseSampler;

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
    output.uv = input.uv;
    output.normal = float3(0.0, 0.0, 1.0);
    if (input.normal.x == 0.0 && input.normal.y == 0.0 && input.normal.z == 0.0)
    {
        output.position = float4(positions[vertexId], 0.0, 1.0);
        output.color = colors[vertexId];
    }
    else
    {
        float3 p = input.position - material.target.xyz;
        float3 n = normalize(input.normal);
        float3 s = sin(material.camera.xyz);
        float3 c = cos(material.camera.xyz);
        p.yz = float2(c.x * p.y - s.x * p.z, s.x * p.y + c.x * p.z);
        n.yz = float2(c.x * n.y - s.x * n.z, s.x * n.y + c.x * n.z);
        p.xz = float2(c.y * p.x + s.y * p.z, -s.y * p.x + c.y * p.z);
        n.xz = float2(c.y * n.x + s.y * n.z, -s.y * n.x + c.y * n.z);
        p.xy = float2(c.z * p.x - s.z * p.y, s.z * p.x + c.z * p.y);
        n.xy = float2(c.z * n.x - s.z * n.y, s.z * n.x + c.z * n.y);
        p.z += max(material.camera.w, 0.1);
        const float aspect = max(material.light.w, 0.01);
        if (material.target.w >= 0.0)
        {
            const float focal = 1.0 / tan(max(material.target.w, 0.05) * 0.5);
            const float nearPlane = 0.05;
            const float farPlane = 100.0;
            const float z = farPlane / (farPlane - nearPlane) * p.z
                          - farPlane * nearPlane / (farPlane - nearPlane);
            output.position = float4(p.x * focal / aspect, -p.y * focal, z, p.z);
        }
        else
        {
            output.position = float4(p.x / aspect, -p.y, p.z * 0.01, 1.0);
        }
        output.normal = normalize(n);
        output.color = float3(1.0, 1.0, 1.0);
    }
    return output;
}

float4 PS(VertexOutput input) : SV_Target0
{
    const float diffuseLight = 0.28 + 0.72 * saturate(dot(normalize(input.normal),
                                                        normalize(-material.light.xyz)));
    return float4(input.color * diffuseLight, 1.0) * material.diffuse
         * baseTexture.Sample(baseSampler, input.uv);
}
