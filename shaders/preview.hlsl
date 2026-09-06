struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] int4 bones : BLENDINDICES;
    [[vk::location(4)]] float4 weights : BLENDWEIGHT;
    [[vk::location(5)]] uint skinningType : TEXCOORD2;
    [[vk::location(6)]] uint gpuSkinning : TEXCOORD3;
    [[vk::location(7)]] float3 sdefC : TEXCOORD4;
    [[vk::location(8)]] float3 sdefHalfDelta : TEXCOORD5;
    [[vk::location(9)]] float edgeScale : TEXCOORD6;
    [[vk::location(10)]] uint morphStart : TEXCOORD7;
    [[vk::location(11)]] uint morphCount : TEXCOORD8;
};

struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 normal : NORMAL0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float3 color : COLOR0;
    [[vk::location(3)]] float3 viewPosition : TEXCOORD1;
    [[vk::location(4)]] float2 sphereUv : TEXCOORD2;
    [[vk::location(5)]] nointerpolation uint materialIndex : TEXCOORD3;
    [[vk::location(6)]] nointerpolation uint edgePass : TEXCOORD4;
};

struct PreviewSceneConstants {
    float4 camera; // xyz Euler rotation, w distance
    float4 target; // xyz target, w signed field of view (negative means orthographic)
    float4 light;  // xyz direction, w framebuffer aspect
    uint materialIndex;
    uint instanceCount;
    float4 lightColor;
    float4 viewport; // xy framebuffer size
    float4 debug; // x isolated material, y debug flags
};
[[vk::push_constant]] ConstantBuffer<PreviewSceneConstants> scene;

struct BoneTransform {
    float4 rotation;
    float4 translation;
};
[[vk::binding(0, 1)]] StructuredBuffer<BoneTransform> boneTransforms;

struct PreviewMaterialData {
    float4 diffuse;
    float4 ambientShininess;
    float4 specular;
    float4 textureMultiply;
    float4 textureAdd;
    float4 sphereMultiply;
    float4 sphereAdd;
    float4 toonMultiply;
    float4 toonAdd;
    float4 edgeColor;
    float edgeSize;
    uint flags;
    uint2 reserved;
    uint4 textureSlots;
};
[[vk::binding(0, 2)]] StructuredBuffer<PreviewMaterialData> previewMaterials;

struct PreviewMorphDelta
{
    float3 delta;
    uint morphIndex;
};
[[vk::binding(2, 3)]] Texture2D<float4> previewTextureTable[];
[[vk::binding(0, 3)]] SamplerState previewRepeatSampler;
[[vk::binding(1, 3)]] SamplerState previewClampSampler;
[[vk::binding(0, 4)]] StructuredBuffer<PreviewMorphDelta> previewMorphDeltas;
[[vk::binding(1, 4)]] StructuredBuffer<float> previewMorphWeights;

[[vk::binding(0, 0)]] Texture2D<float4> baseTexture;
[[vk::binding(1, 0)]] Texture2D<float4> toonTexture;
[[vk::binding(2, 0)]] Texture2D<float4> sphereTexture;
[[vk::binding(3, 0)]] SamplerState repeatSampler;
[[vk::binding(4, 0)]] SamplerState clampSampler;

struct SkinResult {
    float3 position;
    float3 normal;
};

struct DualQuaternion {
    float4 real;
    float4 dual;
};

float3 rotateQuaternion(float4 quaternion, float3 value) {
    return value + 2.0 * cross(quaternion.xyz, cross(quaternion.xyz, value) + quaternion.w * value);
}

float4 multiplyQuaternion(float4 left, float4 right) {
    return float4(left.w * right.xyz + right.w * left.xyz + cross(left.xyz, right.xyz),
                  left.w * right.w - dot(left.xyz, right.xyz));
}

float4 conjugateQuaternion(float4 quaternion) {
    return float4(-quaternion.xyz, quaternion.w);
}

float4 slerpQuaternion(float4 left, float4 right, float amount) {
    float cosine = dot(left, right);
    if (cosine < 0.0) {
        right = -right;
        cosine = -cosine;
    }
    if (cosine > 0.9995)
        return normalize(lerp(left, right, amount));
    const float angle = acos(clamp(cosine, -1.0, 1.0));
    const float sine = max(sin(angle), 0.000001);
    return (sin((1.0 - amount) * angle) * left + sin(amount * angle) * right) / sine;
}

BoneTransform identityBone() {
    BoneTransform result;
    result.rotation = float4(0.0, 0.0, 0.0, 1.0);
    result.translation = float4(0.0, 0.0, 0.0, 0.0);
    return result;
}

BoneTransform getBone(int index) {
    return index >= 0 ? boneTransforms[index] : identityBone();
}

float3 transformPoint(BoneTransform bone, float3 value) {
    return rotateQuaternion(bone.rotation, value) + bone.translation.xyz;
}

SkinResult skinLbs(VertexInput input, uint influenceCount) {
    SkinResult result;
    result.position = float3(0.0, 0.0, 0.0);
    result.normal = float3(0.0, 0.0, 0.0);
    float totalWeight = 0.0;
    [unroll] for (uint influence = 0; influence < 4; ++influence) {
        if (influence >= influenceCount || input.bones[influence] < 0 || input.weights[influence] == 0.0)
            continue;
        const BoneTransform bone = getBone(input.bones[influence]);
        result.position += transformPoint(bone, input.position) * input.weights[influence];
        result.normal += rotateQuaternion(bone.rotation, input.normal) * input.weights[influence];
        totalWeight += input.weights[influence];
    }
    if (totalWeight > 0.000001) {
        result.position /= totalWeight;
        result.normal = normalize(result.normal);
    } else {
        result.position = input.position;
        result.normal = input.normal;
    }
    return result;
}

SkinResult skinSdef(VertexInput input) {
    const float weight = clamp(input.weights.x, 0.0, 1.0);
    const float3 cr1 = input.sdefC - weight * input.sdefHalfDelta;
    const float3 cr0 = cr1 + input.sdefHalfDelta;
    const BoneTransform bone0 = getBone(input.bones[0]);
    const BoneTransform bone1 = getBone(input.bones[1]);
    const float4 rotation = slerpQuaternion(bone1.rotation, bone0.rotation, weight);

    SkinResult result;
    result.position = rotateQuaternion(rotation, input.position - input.sdefC) +
                      lerp(transformPoint(bone1, cr1), transformPoint(bone0, cr0), weight);
    result.normal = normalize(rotateQuaternion(rotation, input.normal));
    return result;
}

DualQuaternion makeDualQuaternion(BoneTransform bone) {
    DualQuaternion result;
    result.real = bone.rotation;
    result.dual = multiplyQuaternion(float4(bone.translation.xyz, 0.0), bone.rotation) * 0.5;
    return result;
}

DualQuaternion normalizeDualQuaternion(DualQuaternion value) {
    const float magnitude = max(length(value.real), 0.000001);
    value.real /= magnitude;
    value.dual /= magnitude;
    value.dual -= value.real * dot(value.real, value.dual);
    return value;
}

SkinResult skinQdef(VertexInput input) {
    DualQuaternion blended;
    blended.real = float4(0.0, 0.0, 0.0, 0.0);
    blended.dual = float4(0.0, 0.0, 0.0, 0.0);
    float4 pivot = float4(0.0, 0.0, 0.0, 1.0);
    bool pivotInitialized = false;
    [unroll] for (uint influence = 0; influence < 4; ++influence) {
        if (input.bones[influence] < 0 || input.weights[influence] == 0.0)
            continue;
        DualQuaternion bone = makeDualQuaternion(getBone(input.bones[influence]));
        float weight = input.weights[influence];
        if (!pivotInitialized) {
            pivot = bone.real;
            pivotInitialized = true;
        } else if (dot(pivot, bone.real) < 0.0) {
            weight = -weight;
        }
        blended.real += bone.real * weight;
        blended.dual += bone.dual * weight;
    }

    SkinResult result;
    if (!pivotInitialized || length(blended.real) <= 0.000001) {
        result.position = input.position;
        result.normal = input.normal;
        return result;
    }
    blended = normalizeDualQuaternion(blended);
    const float4 translationQuaternion = multiplyQuaternion(blended.dual, conjugateQuaternion(blended.real));
    result.position = rotateQuaternion(blended.real, input.position) + 2.0 * translationQuaternion.xyz;
    result.normal = normalize(rotateQuaternion(blended.real, input.normal));
    return result;
}

SkinResult skinVertex(VertexInput input) {
    if (input.gpuSkinning == 0) {
        SkinResult result;
        result.position = input.position;
        result.normal = input.normal;
        return result;
    }
    switch (input.skinningType) {
    case 1:
        return skinLbs(input, 2); // BDEF2
    case 2:
        return skinLbs(input, 4); // BDEF4
    case 3:
        return skinSdef(input);
    case 4:
        return skinQdef(input);
    default:
        return skinLbs(input, 1); // BDEF1
    }
}

float3 applyVertexMorphs(VertexInput input)
{
    float3 position = input.position;
    [loop]
    for (uint offset = 0; offset < input.morphCount; ++offset)
    {
        const PreviewMorphDelta delta = previewMorphDeltas[input.morphStart + offset];
        position += delta.delta * previewMorphWeights[delta.morphIndex];
    }
    return position;
}

VertexOutput makeVertex(VertexInput input, uint materialIndex, uint edgePass, uint instanceIndex)
{
    VertexOutput output;
    output.uv = input.uv;
    output.materialIndex = materialIndex;
    output.edgePass = edgePass;
    output.sphereUv = float2(0.5, 0.5);
    if (input.normal.x == 0.0 && input.normal.y == 0.0 && input.normal.z == 0.0) {
        output.position = float4(input.position.xy, 0.0, 1.0);
        output.normal = float3(0.0, 0.0, 1.0);
        output.viewPosition = float3(0.0, 0.0, 1.0);
        output.color = float3(0.0, 0.0, 0.0);
        return output;
    }

    input.position = applyVertexMorphs(input);
    SkinResult skin = skinVertex(input);
    const float cloneCenter = (float(scene.instanceCount) - 1.0) * 0.5;
    skin.position.x += (float(instanceIndex) - cloneCenter) * 2.2;
    float3 p = skin.position - scene.target.xyz;
    float3 n = normalize(skin.normal);
    const float3 s = sin(scene.camera.xyz);
    const float3 c = cos(scene.camera.xyz);
    p.yz = float2(c.x * p.y - s.x * p.z, s.x * p.y + c.x * p.z);
    n.yz = float2(c.x * n.y - s.x * n.z, s.x * n.y + c.x * n.z);
    p.xz = float2(c.y * p.x + s.y * p.z, -s.y * p.x + c.y * p.z);
    n.xz = float2(c.y * n.x + s.y * n.z, -s.y * n.x + c.y * n.z);
    p.xy = float2(c.z * p.x - s.z * p.y, s.z * p.x + c.z * p.y);
    n.xy = float2(c.z * n.x - s.z * n.y, s.z * n.x + c.z * n.y);
    p.z += max(scene.camera.w, 0.1);
    const float aspect = max(scene.light.w, 0.01);
    if (scene.target.w >= 0.0) {
        const float focal = 1.0 / tan(max(scene.target.w, 0.05) * 0.5);
        const float nearPlane = 0.05;
        const float farPlane = 100.0;
        const float z = farPlane / (farPlane - nearPlane) * p.z - farPlane * nearPlane / (farPlane - nearPlane);
        output.position = float4(p.x * focal / aspect, -p.y * focal, z, p.z);
    } else {
        const float extent = max(scene.camera.w, 0.1) * tan(max(-scene.target.w, 0.05) * 0.5);
        const float nearPlane = 0.05;
        const float farPlane = 100.0;
        const float z = (p.z - nearPlane) / (farPlane - nearPlane);
        output.position = float4(p.x / (aspect * extent), -p.y / extent, z, 1.0);
    }
    if (edgePass != 0)
    {
        const float4 baseClip = output.position;
        const float edgeSize = max(previewMaterials[materialIndex].edgeSize * input.edgeScale, 0.0);
        const float focal = scene.target.w >= 0.0
                                ? 1.0 / tan(max(scene.target.w, 0.05) * 0.5)
                                : 1.0;
        const float pixelWidth = scene.target.w >= 0.0
                                     ? edgeSize * focal * scene.viewport.y / max(2.0 * p.z, 0.05)
                                     : edgeSize * scene.viewport.y * 0.5;
        float2 screenNormal = float2(n.x, -n.y);
        if (length(screenNormal) < 0.000001)
            screenNormal = float2(0.0, 1.0);
        else
            screenNormal = normalize(screenNormal);
        const float2 viewport = max(scene.viewport.xy, 1.0.xx);
        output.position.xy += screenNormal * (2.0 * pixelWidth / viewport) * baseClip.w;
    }
    output.normal = normalize(n);
    output.viewPosition = p;
    output.sphereUv = n.xy * float2(0.5, -0.5) + 0.5;
    output.color = float3(1.0, 1.0, 1.0);
    return output;
}

VertexOutput VS(VertexInput input, uint instanceIndex : SV_InstanceID) {
    const bool indirect = scene.materialIndex == 0xffffffffU;
    return makeVertex(input, indirect ? instanceIndex : scene.materialIndex, 0, indirect ? 0 : instanceIndex);
}

VertexOutput EdgeVS(VertexInput input, uint instanceIndex : SV_InstanceID) {
    const bool indirect = scene.materialIndex == 0xffffffffU;
    return makeVertex(input, indirect ? instanceIndex : scene.materialIndex, 1, indirect ? 0 : instanceIndex);
}

float4 applyTextureMorphRgb(float4 sample, float4 multiply, float4 add, float3 neutral) {
    sample.rgb = lerp(neutral, sample.rgb * multiply.rgb + add.rgb, multiply.a + add.a);
    return sample;
}

float4 samplePreviewTextureRepeat(uint textureSlot, float2 uv) {
    return previewTextureTable[textureSlot].Sample(previewRepeatSampler, uv);
}

float4 samplePreviewTextureClamp(uint textureSlot, float2 uv) {
    return previewTextureTable[textureSlot].Sample(previewClampSampler, uv);
}

float4 PS(VertexOutput input, bool frontFace : SV_IsFrontFace) : SV_Target0 {
    const PreviewMaterialData material = previewMaterials[input.materialIndex];
    const uint debugFlags = uint(scene.debug.y);
    const uint4 textureSlots = material.textureSlots;
    const float4 sampled = applyTextureMorphRgb(
        samplePreviewTextureRepeat(textureSlots.x, input.uv),
        material.textureMultiply, material.textureAdd, 1.0.xxx);
    if (scene.debug.x >= 0.0 && input.materialIndex != uint(scene.debug.x))
        discard;
    const float4 baseSampled = (debugFlags & 0x01U) != 0U ? 1.0.xxxx : sampled;

    if (input.edgePass != 0)
    {
        if ((material.flags & 0x20U) == 0U || material.edgeColor.a <= 0.0) discard;
        return material.edgeColor;
    }
    if ((debugFlags & 0x20U) == 0U && !frontFace && (material.flags & 0x01U) == 0U)
        discard;

    if ((debugFlags & 0x08U) != 0U)
        return float4(normalize(input.normal) * 0.5 + 0.5, 1.0);
    if ((debugFlags & 0x10U) != 0U)
        return float4(frac(input.uv), 0.0, 1.0);

    const float3 normal = normalize(input.normal);
    const float3 lightDirection = normalize(-scene.light.xyz);
    const float noLight = dot(normal, lightDirection);
    const float3 halfVector = normalize(lightDirection + normalize(-input.viewPosition));
    const float specularLight = pow(max(1e-6, dot(normal, halfVector)), material.ambientShininess.w);
    const float3 Le = scene.lightColor.rgb;
    float4 color = float4(saturate(material.ambientShininess.xyz
                                  + material.diffuse.rgb * Le), material.diffuse.a) * baseSampled;

    const uint toonMode = (material.flags >> 1U) & 0x03U;
    if ((debugFlags & 0x04U) == 0U && toonMode == 0U)
    {
        color *= samplePreviewTextureClamp(textureSlots.y, float2(0.0, 0.5 - noLight * 0.5));
    }
    else if ((debugFlags & 0x04U) == 0U && toonMode == 1U)
        color *= samplePreviewTextureClamp(textureSlots.y, float2(0.0, 0.5 - noLight * 0.5));

    const uint sphereMode = (material.flags >> 3U) & 0x03U;
    if ((debugFlags & 0x02U) == 0U && (sphereMode == 1U || sphereMode == 2U))
    {
        const float4 sphere = applyTextureMorphRgb(
            samplePreviewTextureRepeat(textureSlots.z, input.sphereUv),
            material.sphereMultiply, material.sphereAdd,
            sphereMode == 1U ? 1.0.xxx : 0.0.xxx);
        color.rgb = sphereMode == 1U ? color.rgb * sphere.rgb : color.rgb + sphere.rgb;
    }

    color.rgb += material.specular.rgb * Le * specularLight;
    if (color.a == 0.0) discard;
    if (color.a >= 0.98) color.a = 1.0;
    return color;
}
