// 共通リソース定義
// MikuMikuDayo用のエフェクトを作る際はこのファイルをincludeしてね
// レジスタ番号とかはMikuMikuDayoのバージョンごとに変わる可能性が高いけど名前と意味はそうそう変わらないと思うので

#ifndef RESOURCES_HLSLI
//#define RESOURCES_HLSLI

//SV_Barycentricsをサポートしない環境でなんとか開発を続けるためのオプション。テスト用です
//#define NO_BARYCENTRICS

#include "cb.hlsli"
#include "dayotypes.hlsli"
#include "yrztypes.hlsli"

namespace Dayo {
	
//リソースバインド
#ifndef RESOURCES_DEFORM_HLSLI
RWTexture2D<float4>RTOutput				: register(u0);	//レンダリング結果出力用
RWStructuredBuffer<OIDNInput>OIDNBuf	: register(u1);	//OIDNへの入力用
RWTexture2D<float4>NormalDepth 			: register(u2); //法線(rgb)奥行(a)バッファ
RWTexture2D<uint2>GBuffer1				: register(u3); //モデル番号(r)・面番号(g)
RWTexture2D<float2>GBuffer2				: register(u4); //重心座標
RaytracingAccelerationStructure TLAS	: register(t0);	//TLAS
StructuredBuffer<uint2>Model2Mat        : register(t1);	//モデル番号 → {総材質番号, 各モデルの材質数}
StructuredBuffer<uint>Mat2Model         : register(t2);	//総材質番号→モデル番号
StructuredBuffer<int>Peekaboo           : register(t3);	//モデル番号→show:1 hide:0
StructuredBuffer<int>MatSelected        : register(t4);	//総材質番号→選択中 yes:1, no:0

Texture2D<float4>Skybox	                        : register(t5); //skybox HDRI
StructuredBuffer<YRZ::WalkersAlias>Skywalker    : register(t6); //skyboxサンプリング用
StructuredBuffer<YRZ::WalkersAlias>SkywalkerRow : register(t7); //skyboxサンプリング用、行方向
StructuredBuffer<YRZ::SHCoeff>SkyboxSH          : register(t8); //skybox SH(1要素のみ)

Texture2D<float4>ScreenBMP : register(t9); //PMXモデルのテクスチャとしてscreen.bmpが割り当てられた物体に貼られるテクスチャ
#endif

#ifdef RESOURCES_PP_HLSLI
Texture2D<float4> ScreenTexture : register(t10);	//現フレーム内容の納められたテクスチャ
#endif

//全モデル用テクスチャ配列
StructuredBuffer<uint> TextureTable		        : register(t0, space1);	//モデルID → Texturess内の先頭インデックス
Texture2D<float4> Textures[]			        : register(t1, space1);	//albedo,emission,alphaマップ
//モデルの数だけある物
StructuredBuffer<MMDMaterial> MMDMaterials[]    : register(t0, space4);	//MMDのマテリアル材質モーフ適用後
StructuredBuffer<uint> Faces[]			        : register(t0, space5);	//面番号→マテリアル番号テーブル
StructuredBuffer<MaterialFace> Mat2face[]       : register(t0, space6);	//材質番号→{開始面番号,面数,総面積}
StructuredBuffer<YRZ::WalkersAlias> FaceWalker[]: register(t0, space7);	//WalkersAlias法による面サンプラ
StructuredBuffer<Vertex> PreVB[]		        : register(t0, space8);	//頂点バッファ(前フレームでの変換済み)
StructuredBuffer<Vertex> RawVB[]		        : register(t0, space9);	//頂点バッファ(変換前のモデルに格納されたままの状態)


//RootConstantに設定されている値
cbuffer CBuff1 : register(b0, space1) {
    uint ModelIndex;        //rasterizerパスのみ有効。描画中のモデル番号
    uint RasterizeOrder;    //rasterizerパスのみ有効。何番目に描画されているモデルか？
    uint DeformIndex;       //デフォーマのみ有効。変形中のモデル番号
    uint DeformOrder;       //デフォーマのみ有効。何番目に起動されたデフォーマか？
};	


#ifndef RESOURCES_DEFORM_HLSLI
//頂点バッファ ※rasterizerパスではIAに突っ込まれるので参照不可
StructuredBuffer<Vertex> VB[] : register(t0, space2);	
#endif

//インデクスバッファもrasterizerパスでは参照不可
//compute, raytacing, postprocessパスからは読めるのでこれから処理する三角形の情報を得ることが出来る
StructuredBuffer<uint> IB[] : register(t0, space3);

};	//namespace Dayo
#endif

