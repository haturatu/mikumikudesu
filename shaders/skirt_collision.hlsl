#include "skirt_collision.hlsli"
struct SkirtVertex {
    uint vertex;
    uint firstTriangle;
    uint triangleCount;
    uint capsuleStart;
    float mobility;
    uint capsuleCount;
    uint2 padding;
};
struct CollisionConstants {
    uint vertexCount;
    uint vertexStride;
    uint normalOffset;
    uint phase;
    uint sourceSlice;
    uint destinationSlice;
    uint resetHistory;
    uint padding;
};
struct CollisionStats {
    float maxPenetration;
    float correctionDistance;
    float2 padding;
    uint penetratingVertexCount;
    uint penetratingTriangleCount;
    uint2 padding2;
};
[[vk::push_constant]] ConstantBuffer<CollisionConstants> collision;
[[vk::binding(0,0)]] RWByteAddressBuffer outputVertices;
[[vk::binding(1,0)]] StructuredBuffer<SkirtVertex> vertices;
[[vk::binding(2,0)]] StructuredBuffer<uint4> triangles;
[[vk::binding(3,0)]] StructuredBuffer<uint> adjacency;
[[vk::binding(4,0)]] StructuredBuffer<SkirtCapsule> capsules;
[[vk::binding(5,0)]] RWStructuredBuffer<float4> scratch;
[[vk::binding(6,0)]] RWStructuredBuffer<float4> history;
[[vk::binding(7,0)]] RWStructuredBuffer<CollisionStats> statistics;

float3 position(uint i) { return scratch[collision.sourceSlice*collision.vertexCount+i].xyz; }
float3 project(uint i,float3 p,bool temporal) {
    SkirtVertex v=vertices[i];
    float3 normal=asfloat(outputVertices.Load3(v.vertex*collision.vertexStride+collision.normalOffset));
    float3 previous=0;
    if (temporal && collision.resetHistory==0)
        previous=history[i].xyz;
    [loop] for (uint c=0;c<v.capsuleCount;++c)
        p=skirtProject(p,capsules[v.capsuleStart+c],normal,previous,
            temporal && collision.resetHistory==0,v.mobility);
    return p;
}
float penetration(SkirtVertex v,float3 p) {
    float depth=0;
    [loop] for (uint c=0;c<v.capsuleCount;++c)
        depth=max(depth,skirtPenetration(p,capsules[v.capsuleStart+c]));
    return depth;
}
[numthreads(64,1,1)]
void SkirtCollision(uint3 dispatch : SV_DispatchThreadID) {
    uint i=dispatch.x;
    if (i>=collision.vertexCount)
        return;
    SkirtVertex v=vertices[i];
    uint address=v.vertex*collision.vertexStride;
    if (collision.phase==0) {
        float3 p=asfloat(outputVertices.Load3(address));
        scratch[2*collision.vertexCount+i]=float4(p,0);
        scratch[i]=float4(collision.padding==0 ? project(i,p,true):p,0);
        return;
    }
    float3 p=position(i);
    if (collision.phase==1) {
        float3 correction=0;
        float3 smooth=0;
        float contributions=0;
        [loop] for (uint t=0;t<v.triangleCount;++t) {
            uint3 tri=triangles[adjacency[v.firstTriangle+t]].xyz;
            float3 a=position(tri.x),b=position(tri.y),c=position(tri.z);
            float totalMobility=vertices[tri.x].mobility+vertices[tri.y].mobility+vertices[tri.z].mobility;
            float3 fallback=cross(b-a,c-a);
            float3 authored=asfloat(outputVertices.Load3(address+collision.normalOffset));
            if (dot(fallback,authored)<0) fallback=-fallback;
            [loop] for (uint cap=0;cap<v.capsuleCount;++cap) {
                SkirtCapsule capsule=capsules[v.capsuleStart+cap];
                float4 contact=skirtTriangleContact(a,b,c,capsule,fallback);
                if (contact.w>1e-6) {
                    correction+=contact.xyz*contact.w*(3*v.mobility/max(totalMobility,1e-6));
                    contributions+=1;
                }
            }
            smooth+=(a-scratch[2*collision.vertexCount+tri.x].xyz)+
                    (b-scratch[2*collision.vertexCount+tri.y].xyz)+
                    (c-scratch[2*collision.vertexCount+tri.z].xyz);
        }
        if (contributions>0)
            p+=correction/contributions;
        if (v.triangleCount>0) {
            float3 own=p-scratch[2*collision.vertexCount+i].xyz;
            p+=.1*v.mobility*(smooth/(3*v.triangleCount)-own);
        }
        p=project(i,p,true);
        // Limit the total displacement too, and expose residual penetration.
        float limit=0;
        [loop] for (uint cap=0;cap<v.capsuleCount;++cap)
            limit=max(limit,capsules[v.capsuleStart+cap].aRadius.w*2);
        float3 initial=scratch[2*collision.vertexCount+i].xyz;
        float3 delta=p-initial;
        p=initial+delta*min(1,limit/max(length(delta),1e-8));
        scratch[collision.destinationSlice*collision.vertexCount+i]=float4(p,0);
        return;
    }
    float3 normal=0;
    CollisionStats stats=(CollisionStats)0;
    stats.maxPenetration=penetration(v,p);
    stats.penetratingVertexCount=stats.maxPenetration>1e-4 ? 1:0;
    stats.correctionDistance=length(p-scratch[2*collision.vertexCount+i].xyz);
    [loop] for (uint t=0;t<v.triangleCount;++t) {
        uint3 tri=triangles[adjacency[v.firstTriangle+t]].xyz;
        float3 a=position(tri.x),b=position(tri.y),c=position(tri.z);
        normal+=cross(b-a,c-a);
        if (tri.x==i) {
            float depth=0;
            [loop] for (uint cap=0;cap<v.capsuleCount;++cap)
                depth=max(depth,skirtTriangleContact(a,b,c,capsules[v.capsuleStart+cap],normal).w);
            stats.maxPenetration=max(stats.maxPenetration,depth);
            stats.penetratingTriangleCount+=depth>1e-4 ? 1:0;
        }
    }
    float3 oldNormal=asfloat(outputVertices.Load3(address+collision.normalOffset));
    normal=skirtUnit(normal,oldNormal);
    // Preserve authored orientation even for reversed material winding.
    if (dot(normal,oldNormal)<0)
        normal=-normal;
    outputVertices.Store3(address,asuint(p));
    outputVertices.Store3(address+collision.normalOffset,asuint(normal));
    history[i]=float4(p,1);
    statistics[i]=stats;
}
