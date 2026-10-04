// Source ABI matches PreviewVertex; output is the compact vertex stream shared by every Preview pass.
struct VertexInput {
    float3 position;
    float3 normal;
    float2 uv;
    int4 bones;
    float4 weights;
    float3 sdefC;
    float3 sdefHalfDelta;
    uint skinningType;
    uint gpuSkinning;
    float edgeScale;
    uint morphStart;
    uint morphCount;
};
struct BoneTransform { float4 rotation; float4 translation; };
struct PreviewMorphDelta { float3 delta; uint morphIndex; };
struct DeformedVertex { float3 position; float3 normal; float2 uv; float edgeScale; };
struct DeformConstants { uint vertexCount; uint boneCount; uint morphDeltaCount; uint morphCount; };
[[vk::push_constant]] ConstantBuffer<DeformConstants> deform;
[[vk::binding(0, 0)]] ByteAddressBuffer baseVertices;
[[vk::binding(1, 0)]] StructuredBuffer<BoneTransform> boneTransforms;
[[vk::binding(2, 0)]] StructuredBuffer<PreviewMorphDelta> previewMorphDeltas;
[[vk::binding(3, 0)]] StructuredBuffer<float> previewMorphWeights;
[[vk::binding(4, 0)]] RWByteAddressBuffer deformedVertices;
struct SkinResult {
    float3 position;
    float3 normal;
};

float3 safeNormalize(float3 value, float3 fallback) {
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1e-6 ? value * rsqrt(lengthSquared) : fallback;
}
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
    if (index >= 0 && uint(index) < deform.boneCount)
        return boneTransforms[index];
    return identityBone();
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
        result.normal = safeNormalize(result.normal, float3(0.0, 0.0, 1.0));
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
    result.normal = safeNormalize(rotateQuaternion(rotation, input.normal), float3(0.0, 0.0, 1.0));
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
    result.normal = safeNormalize(rotateQuaternion(blended.real, input.normal), float3(0.0, 0.0, 1.0));
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
    for (uint offset = 0; offset < min(input.morphCount, deform.morphDeltaCount - min(input.morphStart, deform.morphDeltaCount)); ++offset)
    {
        const PreviewMorphDelta delta = previewMorphDeltas[input.morphStart + offset];
        if (delta.morphIndex < deform.morphCount)
            position += delta.delta * previewMorphWeights[delta.morphIndex];
    }
    return position;
}


[numthreads(64, 1, 1)]
void PreviewDeform(uint3 id : SV_DispatchThreadID) {
    if (id.x >= deform.vertexCount)
        return;
    // Explicit byte offsets avoid glslc std430 float3 padding and match DXC's scalar ABI.
    const uint address = id.x * 108U;
    VertexInput source;
    source.position = asfloat(baseVertices.Load3(address));
    source.normal = asfloat(baseVertices.Load3(address + 12U));
    source.uv = asfloat(baseVertices.Load2(address + 24U));
    source.bones = asint(baseVertices.Load4(address + 32U));
    source.weights = asfloat(baseVertices.Load4(address + 48U));
    source.sdefC = asfloat(baseVertices.Load3(address + 64U));
    source.sdefHalfDelta = asfloat(baseVertices.Load3(address + 76U));
    source.skinningType = baseVertices.Load(address + 88U);
    source.gpuSkinning = baseVertices.Load(address + 92U);
    source.edgeScale = asfloat(baseVertices.Load(address + 96U));
    source.morphStart = baseVertices.Load(address + 100U);
    source.morphCount = baseVertices.Load(address + 104U);
    source.position = applyVertexMorphs(source);
    const SkinResult skin = skinVertex(source);
    DeformedVertex output;
    output.position = skin.position;
    output.normal = skin.normal;
    output.uv = source.uv;
    output.edgeScale = source.edgeScale;
    const uint outputAddress = id.x * 36U;
    deformedVertices.Store3(outputAddress, asuint(output.position));
    deformedVertices.Store3(outputAddress + 12U, asuint(output.normal));
    deformedVertices.Store2(outputAddress + 24U, asuint(output.uv));
    deformedVertices.Store(outputAddress + 32U, asuint(output.edgeScale));
}
