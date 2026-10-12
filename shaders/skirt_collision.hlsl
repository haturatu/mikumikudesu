#include "skirt_collision.hlsli"
struct SkirtVertex {
    uint vertex;
    uint firstTriangle;
    uint triangleCount;
    uint capsuleStart;
    float mobility;
    uint capsuleCount;
    uint2 outwardNormal;
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
[[vk::binding(4,0)]] RWStructuredBuffer<SkirtCapsule> capsules;
[[vk::binding(5,0)]] RWStructuredBuffer<float4> scratch;
[[vk::binding(6,0)]] RWStructuredBuffer<float4> history;
[[vk::binding(7,0)]] RWStructuredBuffer<CollisionStats> statistics;
[[vk::binding(8,0)]] StructuredBuffer<uint2> legRanges;
[[vk::binding(9,0)]] StructuredBuffer<uint> legVertices;
[[vk::binding(10,0)]] RWStructuredBuffer<float4> capsuleHistory;
groupshared float fitRadiusSquared[64];
groupshared float2 fitEndpoints[64];

void fitCapsule(uint body,uint lane) {
    SkirtCapsule capsule=capsules[body];
    uint2 range=legRanges[body];
    float3 origin=capsule.aRadius.xyz;
    float3 axis=skirtUnit(capsule.bMargin.xyz-origin,float3(0,1,0));
    float radiusSquared=capsule.aRadius.w*capsule.aRadius.w;
    [loop] for (uint vertex=lane;vertex<range.y;vertex+=64) {
        float3 p=asfloat(outputVertices.Load3(legVertices[range.x+vertex]*collision.vertexStride))-origin;
        if (all(isfinite(p))) {
            float3 radial=p-axis*dot(p,axis);
            radiusSquared=max(radiusSquared,dot(radial,radial));
        }
    }
    fitRadiusSquared[lane]=radiusSquared;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride=32;stride>0;stride/=2) {
        if (lane<stride) fitRadiusSquared[lane]=max(fitRadiusSquared[lane],fitRadiusSquared[lane+stride]);
        GroupMemoryBarrierWithGroupSync();
    }
    float clearance=range.y>0 ? capsule.bMargin.w*.25:0;
    float envelopeRadius=sqrt(fitRadiusSquared[0])+clearance;
    float radius=envelopeRadius+clearance;
    float2 ends=float2(0,length(capsule.bMargin.xyz-origin));
    [loop] for (uint vertex=lane;vertex<range.y;vertex+=64) {
        float3 p=asfloat(outputVertices.Load3(legVertices[range.x+vertex]*collision.vertexStride))-origin;
        if (all(isfinite(p))) {
            float z=dot(p,axis);
            float3 radial=p-axis*z;
            // A padded envelope avoids the square-root singularity at the
            // widest leg vertex. The second padding remains end-cap clearance.
            float reach=sqrt(max(0,envelopeRadius*envelopeRadius-dot(radial,radial)));
            ends.x=min(ends.x,z+reach);
            ends.y=max(ends.y,z-reach);
        }
    }
    fitEndpoints[lane]=ends;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride=32;stride>0;stride/=2) {
        if (lane<stride) {
            fitEndpoints[lane].x=min(fitEndpoints[lane].x,fitEndpoints[lane+stride].x);
            fitEndpoints[lane].y=max(fitEndpoints[lane].y,fitEndpoints[lane+stride].y);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane==0) {
        float2 bounds=fitEndpoints[0];
        capsule.aRadius=float4(origin+axis*bounds.x,radius);
        capsule.bMargin.xyz=origin+axis*bounds.y;
        capsule.previousA.xyz=collision.resetHistory==0 ? capsuleHistory[2*body].xyz:capsule.aRadius.xyz;
        capsule.previousB.xyz=collision.resetHistory==0 ? capsuleHistory[2*body+1].xyz:capsule.bMargin.xyz;
        capsules[body]=capsule;
        capsuleHistory[2*body]=capsule.aRadius;
        capsuleHistory[2*body+1]=capsule.bMargin;
    }
}

float3 outwardNormal(uint i,SkirtCapsule capsule) {
    uint2 encoded=vertices[i].outwardNormal;
    if (encoded.y==0) return skirtEscapeNormal(capsule);
    float2 oct=float2(int(encoded.x << 16) >> 16,int(encoded.x) >> 16)/32767.0;
    float3 n=float3(oct,1-abs(oct.x)-abs(oct.y));
    if (n.z<0) n.xy=(1-abs(n.yx))*float2(n.x<0 ? -1:1,n.y<0 ? -1:1);
    n=skirtRotate(capsule.surfaceRotation,n);
    return skirtUnit(n,skirtEscapeNormal(capsule));
}
float3 triangleOutward(uint3 tri,SkirtCapsule capsule) {
    return skirtUnit(outwardNormal(tri.x,capsule)+outwardNormal(tri.y,capsule)+outwardNormal(tri.z,capsule),
        skirtEscapeNormal(capsule));
}
float3 position(uint i) { return scratch[collision.sourceSlice*collision.vertexCount+i].xyz; }
float3 project(uint i,float3 p,bool temporal) {
    SkirtVertex v=vertices[i];
    float3 normal=asfloat(outputVertices.Load3(v.vertex*collision.vertexStride+collision.normalOffset));
    float3 previous=0;
    if (temporal && collision.resetHistory==0)
        previous=history[i].xyz;
    [loop] for (uint c=0;c<v.capsuleCount;++c) {
        SkirtCapsule capsule=capsules[v.capsuleStart+c];
        float3 outward=v.outwardNormal.y!=0 ? outwardNormal(i,capsule):normal;
        p=skirtProject(p,capsule,outward,previous,
            temporal && collision.resetHistory==0 && collision.phase!=3,
            collision.phase==3 ? 1:v.mobility,v.outwardNormal.y!=0 && collision.phase!=3);
    }
    return p;
}
float penetration(SkirtVertex v,float3 p) {
    float depth=0;
    [loop] for (uint c=0;c<v.capsuleCount;++c)
        depth=max(depth,skirtPenetration(p,capsules[v.capsuleStart+c]));
    return depth;
}
[numthreads(64,1,1)]
void SkirtCollision(uint3 dispatch : SV_DispatchThreadID,uint3 group : SV_GroupID,uint3 lane : SV_GroupThreadID) {
    if (collision.phase==4) {
        fitCapsule(group.x,lane.x);
        return;
    }
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
    if (collision.phase==1 || collision.phase==3) {
        float3 correction=0;
        float3 smooth=0;
        // Local constraints use a fixed neighbor average. Recovery applies
        // each incident supporting plane without diluting isolated contacts.
        if (collision.phase==3) {
            [loop] for (uint cap=0;cap<v.capsuleCount;++cap) {
                SkirtCapsule capsule=capsules[v.capsuleStart+cap];
                float deep=0;
                float3 coherent=0;
                [loop] for (uint t=0;t<v.triangleCount;++t) {
                    uint3 tri=triangles[adjacency[v.firstTriangle+t]].xyz;
                    float3 reference=skirtRadialReference(triangleOutward(tri,capsule),capsule);
                    float4 contact=skirtTriangleContact(position(tri.x),position(tri.y),position(tri.z),capsule,reference);
                    float strength=saturate((contact.w-capsule.bMargin.w)/max(capsule.aRadius.w*.1,capsule.bMargin.w));
                    float3 normal=v.outwardNormal.y!=0 ? skirtCapsuleNormal(contact.xyz,reference,capsule):reference;
                    float4 plane=skirtSupportPlane(p,p,p,capsule,normal);
                    p+=plane.xyz*plane.w*strength;
                    float weight=max(0,contact.w-capsule.aRadius.w*.2);
                    coherent+=reference*weight;
                    deep=max(deep,contact.w);
                }
                float strength=saturate((deep/max(capsule.aRadius.w,1e-6)-.2)/.2);
                float3 normal=skirtUnit(coherent,skirtRadialReference(outwardNormal(i,capsule),capsule));
                float4 plane=skirtSupportPlane(p,p,p,capsule,normal);
                p+=plane.xyz*plane.w*strength;
            }
        }
        [loop] for (uint t=0;collision.phase==1 && t<v.triangleCount;++t) {
            uint3 tri=triangles[adjacency[v.firstTriangle+t]].xyz;
            float3 a=position(tri.x),b=position(tri.y),c=position(tri.z);
            float totalMobility=vertices[tri.x].mobility+vertices[tri.y].mobility+vertices[tri.z].mobility;
            float3 fallback=float3(0,0,1);
            if (collision.phase==1) {
                fallback=cross(b-a,c-a);
                float3 authored=asfloat(outputVertices.Load3(address+collision.normalOffset));
                if (dot(fallback,authored)<0) fallback=-fallback;
            }
            [loop] for (uint cap=0;cap<v.capsuleCount;++cap) {
                SkirtCapsule capsule=capsules[v.capsuleStart+cap];
                if (v.outwardNormal.y!=0) fallback=skirtRadialReference(triangleOutward(tri,capsule),capsule);
                if (collision.resetHistory==0 && v.outwardNormal.y==0) {
                    float3 oldCenter=(history[tri.x].xyz+history[tri.y].xyz+history[tri.z].xyz)/3;
                    float3 oldRadial=oldCenter-skirtNearest(oldCenter,capsule.previousA.xyz,capsule.previousB.xyz);
                    float3 oldNormal=skirtRotate(capsule.rotationDelta,oldRadial);
                    fallback=skirtUnit(oldNormal,fallback);
                }
                float4 contact=skirtTriangleContact(a,b,c,capsule,fallback);
                float strength=saturate(contact.w/max(capsule.aRadius.w*.1,capsule.bMargin.w));
                float3 direction=v.outwardNormal.y!=0 ? skirtCapsuleNormal(contact.xyz,fallback,capsule):
                    skirtTriangleConstraint(a,b,c,capsule,fallback,collision.resetHistory==0).xyz;
                float4 plane=skirtSupportPlane(p,p,p,capsule,direction);
                correction+=plane.xyz*plane.w*strength*(3*v.mobility/max(totalMobility,1e-6));
            }
            smooth+=(a-scratch[2*collision.vertexCount+tri.x].xyz)+
                    (b-scratch[2*collision.vertexCount+tri.y].xyz)+
                    (c-scratch[2*collision.vertexCount+tri.z].xyz);
        }
        p+=correction/(collision.phase==1 ? max(1,v.triangleCount):1);
        if (collision.phase==1 && v.triangleCount>0) {
            float3 own=p-scratch[2*collision.vertexCount+i].xyz;
            p+=.1*v.mobility*(smooth/(3*v.triangleCount)-own);
        }
        p=project(i,p,true);
        // Limit the total displacement too, and expose residual penetration.
        float limit=0;
        [loop] for (uint cap=0;cap<v.capsuleCount;++cap)
            limit=max(limit,capsules[v.capsuleStart+cap].aRadius.w*6);
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
