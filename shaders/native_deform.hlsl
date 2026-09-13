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
    return index >= 0 ? bones[index] : identityBone();
}

float3 transformPoint(BoneTransform bone, float3 value) {
    return rotateQuaternion(bone.rotation, value) + bone.translation.xyz;
}

float3 applyMorphs(PreviewVertex vertex) {
    float3 position = vertex.position;
    [loop]
    for (uint offset = 0; offset < vertex.morphCount; ++offset) {
        const PreviewMorphDelta delta = morphDeltas[vertex.morphStart + offset];
        position += delta.delta * morphWeights[delta.morphIndex];
    }
    return position;
}

void skinVertex(PreviewVertex vertex, out float3 position, out float3 normal) {
    position = applyMorphs(vertex);
    normal = vertex.normal;
    if (vertex.gpuSkinning == 0)
        return;

    const uint influenceCount = vertex.skinningType == 0 ? 1 : (vertex.skinningType == 1 ? 2 : 4);
    float3 skinnedPosition = 0.0.xxx;
    float3 skinnedNormal = 0.0.xxx;
    float totalWeight = 0.0;
    [unroll]
    for (uint influence = 0; influence < 4; ++influence) {
        if (influence >= influenceCount || vertex.bones[influence] < 0 || vertex.weights[influence] == 0.0)
            continue;
        const BoneTransform bone = getBone(vertex.bones[influence]);
        skinnedPosition += transformPoint(bone, position) * vertex.weights[influence];
        skinnedNormal += rotateQuaternion(bone.rotation, normal) * vertex.weights[influence];
        totalWeight += vertex.weights[influence];
    }
    if (totalWeight > 0.000001) {
        position = skinnedPosition / totalWeight;
        normal = normalize(skinnedNormal);
    }
}

[[numthreads(64, 1, 1)]]
void NativeDeform(uint3 id : SV_DispatchThreadID) {
    const uint index = id.x;
    const PreviewVertex source = baseVertices[index];
    float3 position;
    float3 normal;
    skinVertex(source, position, normal);
    NativeDeformedVertex output;
    output.position = float4(position, 1.0);
    output.normal = float4(normalize(normal), 0.0);
    output.uv = source.uv;
    output.padding = 0.0.xx;
    deformedVertices[index] = output;
}
