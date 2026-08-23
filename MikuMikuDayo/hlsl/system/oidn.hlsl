//OIDNでデノイズされた結果をテクスチャに展開するシェーダ

#include "../cb.hlsli"
using namespace Dayo;

struct VSO {
	float4 position:SV_POSITION;
	float2 uv:TEXCOORD;
};

StructuredBuffer<float3> OIDNBuf : register(t0);
Texture2D<float4>RTOutput : register(t1);
sampler samp:register(s0);


VSO VS( float4 pos : POSITION, float2 uv:TEXCOORD )
{
	VSO vso = (VSO)0;
	vso.position = pos;
	vso.uv = uv;
	return vso;
}

float4 PS(VSO vso) : SV_TARGET
{
	int2 pix = floor(vso.uv * Resolution);
	uint idx = pix.y * Resolution.x + pix.x;
	float4 o = float4(OIDNBuf[idx],1);
	float4 a = RTOutput.Sample(samp, vso.uv);
	return float4(o.rgb,a.a);
}

