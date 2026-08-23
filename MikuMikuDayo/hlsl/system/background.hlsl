#include "../cb.hlsli"
#include "../yrz.hlsli"
using namespace Dayo;
using namespace YRZ;

struct VSO {
	float4 position:SV_POSITION;
	float2 uv:TEXCOORD;
};

Texture2D<float4> ppOut : register(t0);
Texture2D<float4> ScreenBMP : register(t1);
Texture2D<uint2>GBuffer : register(t2); //モデル番号(r)・面番号(g)
StructuredBuffer<int>MatSelected : register(t3);	//総材質番号→選択中 yes:1, no:0
StructuredBuffer<uint2>Model2Mat : register(t4);	//モデル番号 → {総材質番号, 各モデルの材質数}
StructuredBuffer<uint> Faces[] : register(t0, space1);	//面番号→マテリアル番号テーブル


sampler samp:register(s0);

VSO VS(float4 pos : POSITION, float2 uv:TEXCOORD )
{
	VSO vso = (VSO)0;
	vso.position = pos;
	vso.uv = uv;
	return vso;
}

struct PSO {
	float4 dayo : SV_TARGET0;	//Renderer窓に出力される絵(_SRGB無し)
	float4 image : SV_TARGET1;	//出力ファイルに保存される絵(_SRGB有り)
};

PSO PS(VSO vso)
{
	PSO o;
	
	o.image = ppOut.Sample(samp, vso.uv);
	
	bool highlight = MaterialHighLight;

	//このピクセルに表示されている材質は選択中か？
	if (highlight) {
		uint2 GB = GBuffer[vso.position.xy];
		uint imo = GB.r;
		uint iFace = GB.g;
		if (imo != -1) {
			uint tmat = Model2Mat[imo] + Faces[imo][iFace];	//総材質番号
			highlight &= MatSelected[tmat];
		} else {
			highlight = false;
		}
	}

	if (highlight) {
		o.image.rgb = lerp(o.image.rgb, HSVtoRGB(float3(RealTime,0.8,0.75)) ,0.5);
	}

	//背景色と合成する
	if (BackgroundMode == 0) {
		//デフォルト
		o.dayo = o.image;
	} else if (BackgroundMode == 1) {
		//screen.bmp
		float4 s = ScreenBMP.Sample(samp, vso.uv);
		s.rgb = ToLinear(s.rgb);
		o.dayo = lerp(s, o.image, o.image.a);
	} else if (BackgroundMode == 2) {
		//白背景
		o.dayo = lerp(1, o.image, o.image.a);
	} else if (BackgroundMode == 3) { 
		//黒背景
		o.dayo = lerp(0, o.image, o.image.a);
	} else {
		//チェック模様
		float checker = ((int)(vso.position.x)/8+(int)(vso.position.y)/8) & 1 ? 1.0 : 0.5;
		o.dayo = lerp(checker, o.image, o.image.a);
	}

	//出力画像のrgb部分も合成結果に合わせる
	o.image.rgb = o.dayo.rgb;

	//dayoOutはSRGB無しなのでガンマ補正をする
	o.dayo.a = 1;
	o.dayo.rgb = ToGamma(saturate(o.dayo.rgb));

	//背景透過モードじゃなければimageのαも1にする
	if (!BackgroundTransparent)
		o.image.a = 1;

	//imageOutはSRGB付きなのでリニアのまま流し込む(補正はDirectX側でやってくれる)
	//SRGB付きフォーマットでないとpngに出力した時に「リニア色空間で書かれている」とみなされて
	//Webブラウザなどでリニア→ガンマ補正されてしまうためSRGB付きフォーマットで生成している

	return o;
}

