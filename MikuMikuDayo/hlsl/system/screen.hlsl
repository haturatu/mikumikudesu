//screen.bmp処理シェーダ

//SourceFrameに割り当てられた現フレームの表示内容をscreen.bmpの内容としてトリミング・リサイズなどを行って出力する
//このシェーダを書き換えてモザイクを掛けたりスポット状に切り抜いたりすると
//screen.bmpの割り当てられたモデルの表示にエフェクトを掛けられます

#include "../cb.hlsli"
using namespace Dayo;


//現フレームの表示内容
Texture2D<float4> SourceFrame : register(t0);

sampler samp:register(s0);

struct VSO {
	float4 position:SV_POSITION;
	float2 uv:TEXCOORD;
};

VSO VS( float4 pos : POSITION, float2 uv:TEXCOORD )
{
	VSO vso = (VSO)0;
	vso.position = pos;
	vso.position.w = 1;
	vso.uv = uv;
	return vso;
}

float4 PS(VSO vso) : SV_TARGET
{
    //offモードの場合は無効にする(1で埋められたテクスチャを生成する)
    if (ScreenBMPMode == 0) {
        return 1;
    }

    float2 uv = vso.uv;
    //4:3モードの場合はトリミングする
    if (ScreenBMPMode == 2) {
        uint dmy;
        uint2 srcsize;
        SourceFrame.GetDimensions(dmy, srcsize.x, srcsize.y, dmy);
        float aspsrc = (float)srcsize.x / srcsize.y;    //映像ソースのアス比
        float t = 4.0/3;    //投影先のアス比
        if (aspsrc > t) {
            float k = (1 - t/aspsrc)*0.5;
            uv.x = lerp(k,1-k,uv.x);
        } else {
            float k = (1 - aspsrc/t)*0.5;
            uv.y = lerp(k,1-k,uv.y);
        }
    }
	float4 o = SourceFrame.Sample(samp, uv);
	return o;
}
