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
    [[vk::location(3)]] float3 viewPosition : TEXCOORD1;
};

struct MaterialConstants
{
    float4 diffuse;
    float4 camera; // xyz Euler rotation, w distance
    float4 target; // xyz target, w signed field of view (negative means orthographic)
    float4 light;  // xyz direction, w framebuffer aspect
    float4 ambientShininess;
    float4 specular;
    float4 textureMultiply;
    float4 textureAdd;
};
[[vk::push_constant]] ConstantBuffer<MaterialConstants> material;
[[vk::binding(0, 0)]] Texture2D<float4> baseTexture;
[[vk::binding(1, 0)]] SamplerState baseSampler;

VertexOutput VS(VertexInput input, uint vertexId : SV_VertexID)
{
    VertexOutput output;
    output.uv = input.uv;
    output.normal = float3(0.0, 0.0, 1.0);
    output.viewPosition = float3(0.0, 0.0, 1.0);
    if (input.normal.x == 0.0 && input.normal.y == 0.0 && input.normal.z == 0.0)
    {
        output.position = float4(input.position.xy, 0.0, 1.0);
        output.color = float3(1.0, 1.0, 1.0);
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
        output.viewPosition = p;
        output.color = float3(1.0, 1.0, 1.0);
    }
    return output;
}

float4 PS(VertexOutput input) : SV_Target0
{
    const float3 normal = normalize(input.normal);
    const float3 lightDirection = normalize(-material.light.xyz);
    const float diffuseLight = saturate(dot(normal, lightDirection));
    const float3 halfVector = normalize(lightDirection + normalize(-input.viewPosition));
    const float specularLight = material.ambientShininess.w > 0.0
        ? pow(saturate(dot(normal, halfVector)), material.ambientShininess.w) : 0.0;
    const float4 sampled = baseTexture.Sample(baseSampler, input.uv)
                         * material.textureMultiply + material.textureAdd;
    const float3 lighting = material.ambientShininess.xyz
                          + material.diffuse.rgb * diffuseLight
                          + material.specular.rgb * specularLight;
    return float4(input.color * lighting, material.diffuse.a) * sampled;
}
