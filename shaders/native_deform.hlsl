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

// A thread-local one-vertex view adapts the existing native input ABI to
// the upstream resources used by DefaultSkinning. Morph positions have already
// been accumulated from the compact native morph stream; UV values arrive in
// the native base vertex stream. No skinning algorithm is duplicated here.
#include "yrz.hlsli"
#include "dayotypes.hlsli"
namespace Dayo {
static Vertex VB[1];
static Skinning Skin[1];
static float4x4 BoneMatrix[4];
static MorphPointer MorphTablePointer[1];
static MorphItem MorphTable[1];
static float MorphValues[1];
}
#include "skinning.hlsli"

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


Dayo::Vertex skinVertex(PreviewVertex source) {
    Dayo::Vertex vertex = (Dayo::Vertex)0;
    vertex.position = applyMorphs(source);
    vertex.normal = safeNormalize(source.normal, float3(0.0, 0.0, 1.0));
    vertex.uv = source.uv;
    vertex.edge = source.edgeScale;
    if (source.gpuSkinning == 0)
        return vertex;

    Dayo::Skinning skin = (Dayo::Skinning)0;
    skin.weightType = int(source.skinningType);
    skin.weight = source.weights;
    skin.sdef_c = source.sdefC;
    if (source.skinningType == 3)
        skin.weight.yzw = source.sdefHalfDelta;
    float validWeight = 0.0;
    [unroll]
    for (uint influence = 0; influence < 4; ++influence) {
        const int bone = source.bones[influence];
        const bool valid = bone >= 0 && uint(bone) < deform.boneCount;
        skin.iBone[influence] = valid ? int(influence) : -1;
        Dayo::BoneMatrix[influence] = YRZ::Identity44;
        if (valid) {
            const BoneTransform transform = bones[bone];
            Dayo::BoneMatrix[influence] = YRZ::MatrixFromQuat(transform.rotation);
            Dayo::BoneMatrix[influence][3].xyz = transform.translation.xyz;
            validWeight += abs(source.weights[influence]);
        }
    }
    // PMX with no valid influence has a stable passthrough result, including
    // QDEF whose dual-quaternion normalization otherwise divides by zero.
    if (validWeight == 0.0 && source.skinningType != 3)
        return vertex;
    Dayo::VB[0] = vertex;
    Dayo::Skin[0] = skin;
    Dayo::MorphTablePointer[0].where = -1;
    Dayo::MorphTablePointer[0].count = 0;
    return Dayo::DefaultSkinning(0);
}

[numthreads(64, 1, 1)]
void NativeDeform(uint3 id : SV_DispatchThreadID) {
    const uint index = id.x;
    if (index >= deform.vertexCount)
        return;
    const PreviewVertex source = baseVertices[index];
    const Dayo::Vertex skin = skinVertex(source);
    NativeDeformedVertex output;
    output.position = float4(skin.position, 1.0);
    output.normal = float4(safeNormalize(skin.normal, float3(0.0, 0.0, 1.0)), 0.0);
    output.uv = skin.uv;
    output.padding = 0.0.xx;
    deformedVertices[index] = output;
}
