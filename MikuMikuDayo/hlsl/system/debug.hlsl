//FXDebug窓用、画像を見やすいように整理するシェーダ

#include "../cb.hlsli"
#include "../yrz.hlsli"
using namespace Dayo;
using namespace YRZ;

cbuffer ViewCB : register(b0)
{
    int Mode;
    bool Tex3d;
    float Scale;
    int Slice;
}

Texture2D<float4> Source : register(t0);
Texture3D<float4> Source3D : register(t1);

struct VSO { float4 pos:SV_POSITION; float2 uv:TEXCOORD; };
VSO VS(float4 pos : POSITION, float2 uv:TEXCOORD ) { VSO vso = (VSO)0; vso.pos = pos; vso.uv = uv; return vso; }

float3 Checker(uint2 p)
{
    uint2 g = p/8;
    float c = (g.x+g.y) & 1;
    c = lerp(0.5,1,c);
    return c;
}

float4 PS(VSO vso):SV_TARGET
{
    uint X,Y,Z;
    Source3D.GetDimensions(X,Y,Z);

    uint2 pix = vso.pos.xy;
    uint3 pix3d = uint3(vso.pos.xy,clamp(Slice,0,Z-1));
    float4 I = Tex3d ? Source3D[pix3d] : Source[pix];
    float4 O = I;

    O.rgb *= Scale;
    if (Mode == 0) {
        O.rgb = lerp(Checker(pix), O.rgb, I.a);
        O.a = 1;
    } else if (Mode == 1) {
        O.a = 1;
    } else if (2<=Mode && Mode<=5) {
        O.rgb = HSVtoRGB(float3(O[Mode-2],1,1));
        O.a = 1;
    }

    return O;
}

