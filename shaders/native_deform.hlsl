struct PreviewVertex {
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

struct BoneTransform {
    float4 rotation;
    float4 translation;
};

struct PreviewMorphDelta {
    float3 delta;
    uint morphIndex;
};

struct NativeDeformedVertex {
    float4 position;
    float4 normal;
    float2 uv;
    float2 padding;
};

struct NativeDeformConstants {
    uint vertexCount;
    uint boneCount;
    uint morphDeltaCount;
    uint morphCount;
};

[[vk::push_constant]] ConstantBuffer<NativeDeformConstants> deform;

[[vk::binding(0, 0)]] StructuredBuffer<PreviewVertex> baseVertices;
[[vk::binding(1, 0)]] StructuredBuffer<BoneTransform> bones;
[[vk::binding(2, 0)]] StructuredBuffer<PreviewMorphDelta> morphDeltas;
[[vk::binding(3, 0)]] StructuredBuffer<float> morphWeights;
[[vk::binding(4, 0)]] RWStructuredBuffer<NativeDeformedVertex> deformedVertices;

float3 rotateQuaternion(float4 quaternion, float3 value) {
    return value + 2.0 * cross(quaternion.xyz, cross(quaternion.xyz, value) + quaternion.w * value);
}

BoneTransform identityBone() {
    BoneTransform result;
    result.rotation = float4(0.0, 0.0, 0.0, 1.0);
    result.translation = float4(0.0, 0.0, 0.0, 0.0);
    return result;
}

BoneTransform getBone(int index) {
    return index >= 0 && uint(index) < deform.boneCount ? bones[index] : identityBone();
}

float3 transformPoint(BoneTransform bone, float3 value) {
    return rotateQuaternion(bone.rotation, value) + bone.translation.xyz;
}

float3 applyMorphs(PreviewVertex vertex) {
    float3 position = vertex.position;
    if (vertex.morphStart >= deform.morphDeltaCount)
        return position;
    const uint available = min(vertex.morphCount, deform.morphDeltaCount - vertex.morphStart);
    [loop]
    for (uint offset = 0; offset < available; ++offset) {
        const PreviewMorphDelta delta = morphDeltas[vertex.morphStart + offset];
        if (delta.morphIndex < deform.morphCount)
            position += delta.delta * morphWeights[delta.morphIndex];
    }
    return position;
}

float3 safeNormalize(float3 value, float3 fallback) {
    const float lengthSquared = dot(value, value);
    return lengthSquared > 0.000001 ? value * rsqrt(lengthSquared) : fallback;
}

struct SkinResult {
    float3 position;
    float3 normal;
};

SkinResult skinLbs(PreviewVertex vertex, uint influenceCount) {
    SkinResult result;
    result.position = 0.0.xxx;
    result.normal = 0.0.xxx;
    float totalWeight = 0.0;
    [unroll]
    for (uint influence = 0; influence < 4; ++influence) {
        if (influence >= influenceCount || vertex.bones[influence] < 0 || vertex.weights[influence] == 0.0)
            continue;
        const BoneTransform bone = getBone(vertex.bones[influence]);
        result.position += transformPoint(bone, vertex.position) * vertex.weights[influence];
        result.normal += rotateQuaternion(bone.rotation, vertex.normal) * vertex.weights[influence];
        totalWeight += vertex.weights[influence];
    }
    if (totalWeight > 0.000001) {
        result.position /= totalWeight;
        result.normal = safeNormalize(result.normal, vertex.normal);
    } else {
        result.position = vertex.position;
        result.normal = vertex.normal;
    }
    return result;
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

SkinResult skinSdef(PreviewVertex vertex) {
    const float weight = clamp(vertex.weights.x, 0.0, 1.0);
    const float3 cr1 = vertex.sdefC - weight * vertex.sdefHalfDelta;
    const float3 cr0 = cr1 + vertex.sdefHalfDelta;
    const BoneTransform bone0 = getBone(vertex.bones[0]);
    const BoneTransform bone1 = getBone(vertex.bones[1]);
    const float4 rotation = slerpQuaternion(bone1.rotation, bone0.rotation, weight);

    SkinResult result;
    result.position = rotateQuaternion(rotation, vertex.position - vertex.sdefC) +
                      lerp(transformPoint(bone1, cr1), transformPoint(bone0, cr0), weight);
    result.normal = safeNormalize(rotateQuaternion(rotation, vertex.normal), vertex.normal);
    return result;
}

struct DualQuaternion {
    float4 real;
    float4 dual;
};

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

SkinResult skinQdef(PreviewVertex vertex) {
    DualQuaternion blended;
    blended.real = 0.0.xxxx;
    blended.dual = 0.0.xxxx;
    float4 pivot = float4(0.0, 0.0, 0.0, 1.0);
    bool pivotInitialized = false;
    [unroll]
    for (uint influence = 0; influence < 4; ++influence) {
        if (vertex.bones[influence] < 0 || vertex.weights[influence] == 0.0)
            continue;
        DualQuaternion bone = makeDualQuaternion(getBone(vertex.bones[influence]));
        float weight = vertex.weights[influence];
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
        result.position = vertex.position;
        result.normal = vertex.normal;
        return result;
    }
    blended = normalizeDualQuaternion(blended);
    const float4 translationQuaternion = multiplyQuaternion(blended.dual, conjugateQuaternion(blended.real));
    result.position = rotateQuaternion(blended.real, vertex.position) + 2.0 * translationQuaternion.xyz;
    result.normal = safeNormalize(rotateQuaternion(blended.real, vertex.normal), vertex.normal);
    return result;
}

SkinResult skinVertex(PreviewVertex vertex) {
    vertex.position = applyMorphs(vertex);
    if (vertex.gpuSkinning == 0) {
        SkinResult result;
        result.position = vertex.position;
        result.normal = vertex.normal;
        return result;
    }
    switch (vertex.skinningType) {
    case 1:
        return skinLbs(vertex, 2); // BDEF2
    case 2:
        return skinLbs(vertex, 4); // BDEF4
    case 3:
        return skinSdef(vertex);
    case 4:
        return skinQdef(vertex);
    default:
        return skinLbs(vertex, 1); // BDEF1
    }
}

[[numthreads(64, 1, 1)]]
void NativeDeform(uint3 id : SV_DispatchThreadID) {
    const uint index = id.x;
    if (index >= deform.vertexCount)
        return;
    const PreviewVertex source = baseVertices[index];
    const SkinResult skin = skinVertex(source);
    NativeDeformedVertex output;
    output.position = float4(skin.position, 1.0);
    output.normal = float4(safeNormalize(skin.normal, float3(0.0, 0.0, 1.0)), 0.0);
    output.uv = source.uv;
    output.padding = 0.0.xx;
    deformedVertices[index] = output;
}
