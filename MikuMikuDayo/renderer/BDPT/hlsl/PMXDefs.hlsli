//パストレーサ共通

#ifndef PMXDEFS_HLSLI
#define PMXDEFS_HLSLI

#include "../../../hlsl/resources.hlsli"
#include "../../../hlsl/yrz.hlsli"
using namespace YRZ;
using namespace Dayo;

//PMX用レンダラーで想定しているマテリアル
struct Material
{
	int texture;		//albedo,emission,alpha用テクスチャ
	int twosided;		//0:片面材質 1:両面材質
	float autoNormal;	//textureの明度勾配によるバンプマップの強さ
	int category;		//0:普通, 1:ガラス, 2:金属, 3:表面下散乱, 4:薄い誘電体
	float roughness;
	float anisotropy;
	float3 albedo;		//反射率
    float3 emission;	//発光要素を減衰無しで観察した時の輝度
	float alpha;		//不透過度
	float2 IOR;			//IOR.xyでCauchyの分散係数ABを示す
	float4 cat;			//カテゴリ固有パラメータ(後述)
	float lightCosFalloff;	//スポットライトの範囲全体
	float lightCosHotspot;	//スポットライトの中心部、減衰なしの所
	int restrictedLight;	//サンプルされないライト(白目とか)
	float2 ggxA;	//GGXのα, roughness, anisotropyより算出する
};

struct Payload
{
	uint xi;		//ハッシュ関数のシード、RNGマクロが呼ばれるたびに+1される
	bool miss;		//レイがミスった
	int ID;			//最後にぶっかった物体のinstanceID
	int iFace;		//↑の面番号
	float2 uv;		//↑のレイがぶつかった位置の重心座標
	float  t;		//レイの移動距離
};

struct PayloadShadow {
	float3 Tr;		//Transmittance
	uint xi;
};

//ランダム関数
//frameはフレーム番号、xiは適当なオフセット値
//呼ぶたびにオフセット値が+1されるのでRandom4を呼ぶたびに違う値が返る
float4 Random4(uint frame, inout uint xi)
{
	float4 ret = Hash4(uint4(DispatchRaysIndex().xy, frame, xi));
	xi++;
	return ret;
}

//乱数を得る
#define RNG (Random4(iSample,payload.xi))

//予約されたインスタンスID
//#define ID_FOG 9999


/*マテリアルのカテゴリ*/

//全マテリアルカテゴリ共通事項
//輻射・蛍光は考慮しない
//現状では全てのマテリアルはフレネルインターフェイスを持ち、鏡面反射を起こす
//フレネルインターフェイスはマイクロファセットで構成され、GGXモデルで近似される
//フレネルインターフェイスと拡散反射面との境界での相互反射については考慮しない


//金属でもガラスでもない「普通の」マテリアル
//鏡面反射を起こさなかった成分は拡散反射を起こす
//拡散反射も起こさなかった成分(1-albedo分)は吸収される
//光は面の裏側に全く透過しない
#define CAT_DEFAULT 0

//ガラス
//界面で屈折を起こし、内部で吸収を起こす。散乱は起こさない
//1.ポリゴンで閉じられた空間になっている事
//2.ガラス同士で貫通していない事
//以上の2つを満たしていなければならない
//光源からは不透明体と見なされる(単純なNEEは失敗する)
//alphaは無視される (確率的に「閉じている」「閉じていない」が選択されると困るから)
//片面材質であっても両面張りとして扱われる
#define CAT_GLASS 1

//金属
//鏡面反射を起こさなかった成分は吸収される
//光は面の裏側に全く透過しない
#define CAT_METAL 2	

#define CAT_SUBSURFACE 3	//表面下散乱

//薄い誘電体(葉や障子紙、薄いガラスなど厚さの無いトランスルーセントな物体)
//CAT_GLASSはモデリングに制約があるので、GLASSで上手くいかない時なんかにどうぞ
#define CAT_THIN 4




/* 光源のカテゴリ */
#define LIGHT_EMISSIVE	0
#define LIGHT_SKYBOX	1



//BRDFサンプリングの結果格納用
struct BRDFOut {
	float3 beta;	//反射率/pdf
	float pdf;		//サンプリングした際のpdf
	float3 L;		//レイの出射方向(光の入射方向)
	float IOR;		//レイの出た先の媒質のIOR(屈折しなかった場合は元のIORと同じ)

	bool isMirror;	//完全鏡面反射パスを通った
	bool dispersion;	//分光を考慮しなければならないパスを通った
	bool diffuse;	//拡散反射パスを通った(SSSでもLambertでも)

	bool BSSRDF;	//BSSRDFの評価をした(表面下散乱マテリアルである)
	bool SSS;		//表面下散乱パスが選択された
	float3 POut;	//表面下散乱を起こして出た場所
	float3 NOut;	//表面下散乱を起こして出た場所の法線
	float3 NgOut;	//表面下散乱を起こして出た場所のポリゴン自体の法線
	float3 betaSSS;	//表面下散乱の結果、1ステップでbetaがどれだけ変わったか
};


struct Reservoir {
	float3 yPhat;	//主光源のBRDF・NoL
	float3 yLe;		//主光源の輝度
	float3 yPos;	//主光源上の位置
	float yPDF;
	bool yTwosided;	//主光源は片面か？
	float3 yN;		//主光源の向き
	float yw;		//主光源のウェイト
	float wsum;		//ウェイト合計
	int M;			//今までカウントしたライトの数
};



#endif