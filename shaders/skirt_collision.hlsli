#ifndef DAYO_SKIRT_COLLISION
#define DAYO_SKIRT_COLLISION
struct SkirtCapsule {
    float4 aRadius;
    float4 bMargin;
    float4 previousA;
    float4 previousB;
    float4 rotationDelta;
    float4 surfaceRotation;
};
float3 skirtNearest(float3 p, float3 a, float3 b) {
    float3 ab = b-a;
    return a+ab*saturate(dot(p-a,ab)/max(dot(ab,ab),1e-20));
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
    float3 local=p-capsule.aRadius.xyz;
    return max(0,capsule.aRadius.w+capsule.bMargin.w-length(local-skirtNearest(local,0,capsule.bMargin.xyz-capsule.aRadius.xyz)));
}
float3 skirtOrientedNormal(float3 radial,float3 reference) {
    reference=skirtUnit(reference,float3(0,0,1));
    radial=skirtUnit(radial,reference);
    // Reflect only the inward component. Flipping the whole radial vector
    // also flips lateral motion and lets the two legs pull cloth apart.
    return skirtUnit(radial-2*min(0,dot(radial,reference))*reference,reference);
}
float3 skirtCapsuleAxis(SkirtCapsule capsule) {
    float3 axis=capsule.bMargin.xyz-capsule.aRadius.xyz;
    return dot(axis,axis)>1e-10 ? normalize(axis):float3(0,0,0);
}
float3 skirtRadialReference(float3 reference,SkirtCapsule capsule) {
    reference=skirtUnit(reference,float3(0,0,1));
    float3 axis=skirtCapsuleAxis(capsule);
    float3 radial=reference-axis*dot(reference,axis);
    float3 escape=skirtEscapeNormal(capsule);
    escape=skirtUnit(escape-axis*dot(escape,axis),float3(1,0,0));
    float3 direction=skirtUnit(radial,escape);
    // A perpendicular turn avoids antipodal interpolation when the skirt
    // reference is nearly parallel to a horizontally raised thigh.
    float3 turn=skirtUnit(cross(axis,direction),escape);
    if (turn.y<0) turn=-turn;
    return skirtUnit(lerp(turn,direction,saturate(length(radial)/.25)),direction);
}
float3 skirtCapsuleNormal(float3 normal,float3 reference,SkirtCapsule capsule) {
    float3 axis=skirtCapsuleAxis(capsule);
    float axial=dot(normal,axis);
    float3 radial=normal-axis*axial;
    // Keep the end-cap component: flipping it would select the far end of
    // the thigh and pull a small hip contact up over the entire raised leg.
    if (dot(radial,radial)<1e-12 && dot(axis,axis)>0) return normal;
    float3 outward=skirtOrientedNormal(radial,skirtRadialReference(reference,capsule));
    return skirtUnit(axis*axial+outward*length(radial),outward);
}
float3 skirtProject(float3 p, SkirtCapsule capsule, float3 fallback, float3 previous, bool history, float mobility, bool oriented) {
    float3 local=p-capsule.aRadius.xyz;
    float3 delta=local-skirtNearest(local,0,capsule.bMargin.xyz-capsule.aRadius.xyz);
    float radius=capsule.aRadius.w+capsule.bMargin.w;
    float3 normal=skirtUnit(delta,skirtUnit(fallback,float3(0,0,1)));
    if (length(delta)<radius*.05) {
        float3 axis=skirtUnit(capsule.bMargin.xyz-capsule.aRadius.xyz,float3(0,1,0));
        float3 authored=skirtUnit(fallback-axis*dot(fallback,axis),skirtEscapeNormal(capsule));
        if (dot(normal,authored)<0) normal=-normal;
        normal=skirtUnit(lerp(authored,normal,saturate(length(delta)/max(radius*.05,1e-6))),authored);
    }
    float3 reference=skirtRadialReference(fallback,capsule);
    float axial=abs(dot(delta,skirtCapsuleAxis(capsule)));
    if (oriented) normal=skirtCapsuleNormal(normal,fallback,capsule);
    bool crossed=oriented && dot(reference,delta)<0 && length(delta)<radius*1.5 && axial<radius*.5;
    if (history) {
        float3 oldDelta=previous-skirtNearest(previous,capsule.previousA.xyz,capsule.previousB.xyz);
        float3 oldNormal=skirtUnit(skirtRotate(capsule.rotationDelta,oldDelta),normal);
        if (!oriented) {
            crossed=dot(oldNormal,delta)<0 && length(oldDelta)<radius*1.5 && length(delta)<radius*1.5;
            if (crossed || length(delta)<1e-6) normal=oldNormal;
        }
    }
    if (!crossed && length(delta)>=radius)
        return p;
    float3 correction=normal*radius-delta;
    if (oriented) {
        float projected=dot(delta,normal);
        float discriminant=projected*projected+radius*radius-dot(delta,delta);
        if (discriminant>=0) {
            float root=sqrt(discriminant);
            float travel=projected>0 ? (radius*radius-dot(delta,delta))/max(root+projected,1e-8):root-projected;
            correction=normal*max(0,travel);
        }
        // Only the side-recovery band needs gradual activation. An ordinary
        // surface contact follows the shortest ray to the capsule boundary.
        if (length(delta)>=radius) {
            float strength=saturate(-dot(reference,delta)/max(radius*.1,1e-6))*
                saturate((radius*1.5-length(delta))/max(radius*.1,1e-6))*
                saturate((radius*.5-axial)/max(radius*.1,1e-6));
            correction*=strength;
        }
    }
    // Waist vertices move more slowly. Repeated projection resolves contact
    // without a single-frame displacement larger than one capsule radius.
    float limit=max(capsule.aRadius.w*.5,1e-5)*mobility;
    return p+correction*min(1,limit/max(length(correction),1e-8));
}
float3 skirtTrianglePoint(float3 p,float3 a,float3 b,float3 c) {
    float3 n=cross(b-a,c-a);
    float area=dot(n,n);
    float scale=max(dot(b-a,b-a),max(dot(c-b,c-b),dot(a-c,a-c)));
    if (area>max(1e-30,scale*scale*1e-12)) {
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
    float s=denominator>max(1e-30,uu*vv*1e-6) ? saturate((uv*vw-vv*uw)/denominator):0;
    float t=vv>1e-20 ? (uv*s+vw)/vv:0;
    if (t<0) { t=0;s=uu>1e-20 ? saturate(-uw/uu):0; }
    else if (t>1) { t=1;s=uu>1e-20 ? saturate((uv-uw)/uu):0; }
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
    // Work relative to the capsule to keep tiny contact distances stable
    // under model normalization and far-from-origin animation.
    a-=start;b-=start;c-=start;end-=start;start=0;
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
    float crossingScale=sqrt(dot(end-start,end-start)*dot(n,n));
    if (abs(denominator)>max(1e-30,crossingScale*1e-6)) {
        float t=dot(a-start,n)/denominator;
        if (t>=0 && t<=1) {
            q=lerp(start,end,t);p=skirtTrianglePoint(q,a,b,c);
            skirtCloser(p,q,delta,distanceSquared);
        }
    }
    float depth=max(0,capsule.aRadius.w+capsule.bMargin.w-sqrt(distanceSquared));
    // A centerline crossing has no unique radial normal. Do not normalize
    // floating-point roundoff into an arbitrary escape direction.
    float3 authored=skirtUnit(fallback,float3(0,0,1));
    float3 normal=skirtUnit(delta,authored);
    float distance=sqrt(distanceSquared);
    if (distance<radius*.02) {
        if (dot(normal,authored)<0) normal=-normal;
        normal=skirtUnit(lerp(authored,normal,saturate(distance/max(radius*.02,1e-6))),authored);
    }
    return float4(normal,depth);
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
    if (!temporal && dot(normal,radial)<-max(1e-6,capsule.aRadius.w*.001)) normal=-normal;
    float3 axis=capsule.bMargin.xyz-capsule.aRadius.xyz;
    if (dot(axis,axis)>1e-10) {
        axis=normalize(axis);
        float3 perpendicular=normal-axis*dot(normal,axis);
        float3 basis=abs(axis.y)<.9 ? float3(0,1,0):float3(1,0,0);
        float3 outward=skirtUnit(perpendicular,normalize(cross(axis,basis)));
        float deep=saturate((contact.w/max(capsule.aRadius.w,1e-6)-.15)/.2);
        normal=skirtUnit(lerp(normal,outward,deep),outward);
    }
    return skirtSupportPlane(a,b,c,capsule,normal);
}
#endif
