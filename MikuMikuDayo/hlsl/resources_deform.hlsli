#ifndef RESOURCES_DEFORM_HLSLI
#define RESOURCES_DEFORM_HLSLI
#include "resources.hlsli"

namespace Dayo {

//デフォーム処理対象のモデル番号と、何番目に処理されているモデルか？という情報
//cbuffer CBuffDeform : register(b1) { uint DeformIndex; uint DeformOrder;};

RWStructuredBuffer<Vertex> OutBuf : register(u0);		//変換後の頂点バッファ
//StructuredBuffer<VertexEx> VB : register(t0);			//変換前の頂点バッファ
StructuredBuffer<Vertex> VB : register(t0);			//変換前の頂点バッファ
StructuredBuffer<Skinning> Skin : register(t1);			//スキニング情報
StructuredBuffer<float4x4> BoneMatrix : register(t2);	//各ボーンの変換行列
StructuredBuffer<float> MorphValues : register(t3);		//モーフ値
StructuredBuffer<MorphItem> MorphTable : register(t4);		//モーフ番号と位置・UVへの影響を書いたテーブル
StructuredBuffer<MorphPointer> MorphTablePointer : register(t5);	//各頂点が↑のテーブルのどこを読めばいいのか書いた物

StructuredBuffer<uint2>Model2Mat : register(t6);	//モデル番号 → {総材質番号, 各モデルの材質数}
StructuredBuffer<uint>Mat2Model : register(t7);	//総材質番号→モデル番号
StructuredBuffer<int>Peekaboo : register(t8);	//モデル番号→show:1 hide:0

};	//namespace

#endif