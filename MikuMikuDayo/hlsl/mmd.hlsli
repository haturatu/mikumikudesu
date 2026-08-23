#include "resources.hlsli"

#ifndef MMD_HLSLI
#define MMD_HLSLI

//MMDモデルを扱うためのヘルパー
namespace Dayo {

#define NURI NonUniformResourceIndex

//frontface=falseかつ 片面描画の時はα=0が返る
float4 MMDColor(uniform sampler samp, uint imo, uint iFace, float2 uv, float3 N, bool frontface)
{
	MMDMaterial m = MMDMaterials[imo][Faces[imo][iFace]];

	//片面描画材質の裏面をスキップ
	if (!frontface && ((m.drawFlag&1) == 0))
		return 0;

	uint tidx = TextureTable[imo];	//モデルに割り当てられたテクスチャ番号の先頭

	//diffuse的な何か
	float4 tex = 1;
	if (m.tex >= 0) {
		tex =  Textures[NURI(tidx+m.tex)].SampleLevel(samp, uv, 0);
		tex.rgb = lerp(1, tex.rgb * m.textureMulValue.rgb + m.textureAddValue.rgb, m.textureMulValue.a + m.textureAddValue.a);
	}

    float4 o = m.diffuse * tex;

	//toon (共有toonには非対応)
    /*
	if (m.toonFlag==0 && m.toonTex>=0) {
		o *= Textures[NURI(tidx + m.toonTex)].Sample(ClampSamp, float2(0, 0.5 - NoL*0.5) );
	} else if (m.toonFlag == 1 ){
		//共有Toon代わりのてきとう
		o.rgb *= lerp(0.5,1,smoothstep(-0.1,0.1,NoL));
	}
    */

	//sphere map : spmodeが加算or乗算の時はスフィアマップを加算or乗算する
	if (m.spTex >= 0 && (m.spmode==1 || m.spmode==2)) {
        float2 spuv = mul(N,(float3x2)ViewMatrix) * float2(0.5,-0.5) + 0.5;
		float4 sptex = Textures[NURI(tidx + m.spTex)].SampleLevel(samp, spuv, 0);
		sptex.rgb = lerp(m.spmode==1 ? 1 : 0, sptex.rgb * m.sphereMulValue.rgb + m.sphereAddValue.rgb, m.sphereAddValue.a + m.sphereMulValue.a);
		o.rgb = (m.spmode==1) ? o.rgb*sptex.rgb : o.rgb+sptex.rgb;
		o.a *= sptex.a;
	}

	o.rgb = YRZ::ToLinear(o.rgb);

	return o;
}

#endif
}//namespace