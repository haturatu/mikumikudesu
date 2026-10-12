#ifndef DAYO_SKIRT_COLLISION
#define DAYO_SKIRT_COLLISION
struct SkirtCapsule {
    float4 aRadius;
    float4 bMargin;
    float4 previousA;
    float4 previousB;
    float4 rotationDelta;
};
float3 skirtNearest(float3 p, float3 a, float3 b) {
    float3 ab = b-a;
    return a+ab*saturate(dot(p-a,ab)/max(dot(ab,ab),1e-8));
}
float3 skirtUnit(float3 v, float3 fallback) {
    return dot(v,v)>1e-12 ? normalize(v):fallback;
}
float3 skirtRotate(float4 q, float3 p) {
    return p+2*cross(q.xyz,cross(q.xyz,p)+q.w*p);
}
float3 skirtEscapeNormal(SkirtCapsule capsule) {
    float2 oct=float2(capsule.previousA.w,capsule.previousB.w);
    float3 n=float3(oct,1-abs(oct.x)-abs(oct.y));
    if (n.z<0) n.xy=(1-abs(n.yx))*float2(n.x<0 ? -1:1,n.y<0 ? -1:1);
    return skirtUnit(n,float3(0,0,1));
}
float skirtPenetration(float3 p, SkirtCapsule capsule) {
    return max(0,capsule.aRadius.w+capsule.bMargin.w-length(p-skirtNearest(p,capsule.aRadius.xyz,capsule.bMargin.xyz)));
}
float3 skirtProject(float3 p, SkirtCapsule capsule, float3 fallback, float3 previous, bool history, float mobility) {
    float3 nearest=skirtNearest(p,capsule.aRadius.xyz,capsule.bMargin.xyz);
    float3 delta=p-nearest;
    float radius=capsule.aRadius.w+capsule.bMargin.w;
    float3 normal=skirtUnit(delta,skirtUnit(fallback,float3(0,0,1)));
    bool crossed=false;
    if (history) {
        float3 oldDelta=previous-skirtNearest(previous,capsule.previousA.xyz,capsule.previousB.xyz);
        float3 transported=previous-skirtNearest(previous,capsule.aRadius.xyz,capsule.bMargin.xyz);
        float3 oldNormal=skirtUnit(transported,skirtUnit(skirtRotate(capsule.rotationDelta,oldDelta),normal));
        crossed=dot(oldNormal,delta)<0 && length(oldDelta)<radius*1.5 && length(delta)<radius*1.5;
        if (crossed || length(delta)<1e-6)
            normal=oldNormal;
    }
    if (!crossed && length(delta)>=radius)
        return p;
    float3 correction=nearest+normal*radius-p;
    // Waist vertices move more slowly. Repeated projection resolves contact
    // without a single-frame displacement larger than one capsule radius.
    float limit=max(capsule.aRadius.w*.5,1e-5)*mobility;
    return p+correction*min(1,limit/max(length(correction),1e-8));
}
float3 skirtTrianglePoint(float3 p,float3 a,float3 b,float3 c) {
    float3 n=cross(b-a,c-a);
    float area=dot(n,n);
    if (area>1e-12) {
        float3 q=p-n*(dot(p-a,n)/area);
        if (dot(cross(b-a,q-a),n)>=0 && dot(cross(c-b,q-b),n)>=0 && dot(cross(a-c,q-c),n)>=0)
            return q;
    }
    float3 x=skirtNearest(p,a,b),y=skirtNearest(p,b,c),z=skirtNearest(p,c,a);
    if (dot(p-y,p-y)<dot(p-x,p-x)) x=y;
    if (dot(p-z,p-z)<dot(p-x,p-x)) x=z;
    return x;
}
void skirtSegmentPair(float3 a,float3 b,float3 c,float3 d,out float3 p,out float3 q) {
    float3 u=b-a,v=d-c,w=a-c;
    float uu=dot(u,u),vv=dot(v,v),uv=dot(u,v),uw=dot(u,w),vw=dot(v,w);
    float denominator=uu*vv-uv*uv;
    float s=denominator>1e-12 ? saturate((uv*vw-vv*uw)/denominator):0;
    float t=vv>1e-12 ? (uv*s+vw)/vv:0;
    if (t<0) { t=0;s=uu>1e-12 ? saturate(-uw/uu):0; }
    else if (t>1) { t=1;s=uu>1e-12 ? saturate((uv-uw)/uu):0; }
    p=a+u*s;q=c+v*t;
}
void skirtCloser(float3 p,float3 q,inout float3 delta,inout float distanceSquared) {
    float3 d=p-q;
    if (dot(d,d)<distanceSquared) { distanceSquared=dot(d,d);delta=d; }
}
float4 skirtTriangleContact(float3 a,float3 b,float3 c,SkirtCapsule capsule,float3 fallback) {
    float3 start=capsule.aRadius.xyz,end=capsule.bMargin.xyz;
    float radius=capsule.aRadius.w+capsule.bMargin.w;
    if (any(min(a,min(b,c))>max(start,end)+radius) || any(max(a,max(b,c))<min(start,end)-radius))
        return 0;
    float3 delta=skirtTrianglePoint(start,a,b,c)-start;
    float distanceSquared=dot(delta,delta);
    skirtCloser(skirtTrianglePoint(end,a,b,c),end,delta,distanceSquared);
    float3 p,q;
    skirtSegmentPair(a,b,start,end,p,q);skirtCloser(p,q,delta,distanceSquared);
    skirtSegmentPair(b,c,start,end,p,q);skirtCloser(p,q,delta,distanceSquared);
    skirtSegmentPair(c,a,start,end,p,q);skirtCloser(p,q,delta,distanceSquared);
    // A centerline crossing the triangle interior has zero distance even
    // when all endpoints, edge midpoints and the centroid are outside.
    float3 n=cross(b-a,c-a);
    float denominator=dot(end-start,n);
    if (abs(denominator)>1e-10) {
        float t=dot(a-start,n)/denominator;
        if (t>=0 && t<=1) {
            q=lerp(start,end,t);p=skirtTrianglePoint(q,a,b,c);
            skirtCloser(p,q,delta,distanceSquared);
        }
    }
    float depth=max(0,capsule.aRadius.w+capsule.bMargin.w-sqrt(distanceSquared));
    return float4(skirtUnit(delta,skirtUnit(fallback,float3(0,0,1))),depth);
}
float4 skirtSupportPlane(float3 a,float3 b,float3 c,SkirtCapsule capsule,float3 normal) {
    float support=max(dot(capsule.aRadius.xyz,normal),dot(capsule.bMargin.xyz,normal))+
        capsule.aRadius.w+capsule.bMargin.w;
    float minimum=min(dot(a,normal),min(dot(b,normal),dot(c,normal)));
    return float4(normal,max(0,support-minimum));
}
float4 skirtTriangleConstraint(float3 a,float3 b,float3 c,SkirtCapsule capsule,float3 fallback,bool temporal) {
    float4 contact=skirtTriangleContact(a,b,c,capsule,fallback);
    if (contact.w<=1e-6) return contact;
    float3 center=(a+b+c)/3;
    float3 radial=center-skirtNearest(center,capsule.aRadius.xyz,capsule.bMargin.xyz);
    // A supporting plane clears the complete triangle, including a capsule
    // axis crossing. A distance-only correction can leave a chord through
    // the capsule even after every vertex reaches its surface.
    float3 normal=temporal ? skirtUnit(fallback,contact.xyz):contact.xyz;
    if (!temporal && length(radial)>1e-5 && dot(normal,radial)<0) normal=-normal;
    float3 axis=capsule.bMargin.xyz-capsule.aRadius.xyz;
    if (contact.w>capsule.aRadius.w*.25 && dot(axis,axis)>1e-10) {
        axis=normalize(axis);
        float3 perpendicular=normal-axis*dot(normal,axis);
        float3 basis=abs(axis.y)<.9 ? float3(0,1,0):float3(1,0,0);
        normal=skirtUnit(perpendicular,normalize(cross(axis,basis)));
    }
    return skirtSupportPlane(a,b,c,capsule,normal);
}
#endif
