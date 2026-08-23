//MikuMikuDayo固有型・定数情報

#ifndef DAYOTYPES_HLSLI
#define DAYOTYPES_HLSLI

namespace Dayo {

//この値以上だったらGBufferに載せる、という定数
static const float AlphaThreshold = 0.5;

//GBufferに何も書かれていないピクセルである、という定数。居ないので。
static const uint Inai = -1;

struct Vertex {
    float3 position;
    float3 normal;
	float3 tangent;
    float2 uv;
	float edge;
	float4 exuv[4];	//追加UV
};

//三角ポリゴンと交差した時に取り出せる情報 … 重心座標
struct Attribute { float2 st; };

struct MMDMaterial
{
	float4 diffuse;
	float3 specular;
	float shininess;
	float3 ambient;
	float4 edgeColor;
	float edgeSize;
	float4 textureAddValue;
	float4 sphereAddValue;
	float4 toonAddValue;
	float4 textureMulValue;
	float4 sphereMulValue;
	float4 toonMulValue;
	int drawFlag; //描画ﾌﾗｸﾞ 0x01:両面, 0x02:地面影, 0x04:セルフシャドウキャスターになる, 0x08:セルフシャドウ描画, 0x10:エッジ描画
	int tex;	//テクスチャ番号
	int spTex;
	int spmode;		//スフィアモード 0:無効 1:乗算, 2:加算 3:サブテクスチャ
	int toonFlag;	//共有Toonフラグ 0:継続値は個別Toon 1:継続値は共有Toon
	int toonTex;	//テクスチャインデックス、または共有toonテクスチャ番号(0～9がtoon01.bmp～toon10.bmpに対応)
	int vertexCount;	//頂点数、必ず3の倍数
};


//OIDNへの入力
struct OIDNInput {
	float3 color;
	float3 albedo;
	float3 normal;
};

//材質→面情報
struct MaterialFace {
	uint start;         //材質の開始面番号
	uint count;         //面数(pmxに納められている頂点数ではなく、その1/3の面数である点に注意)
	float totalArea;    //材質に属する面の面積合計(頂点モーフやデフォーマによる変形は考慮していない)
};

//頂点毎に1つのスキニング(ボーンによる変形)情報
struct Skinning {
	int4 iBone;		//影響を受けるボーン番号、最大4つ
	float4 weight;	//各ボーンのウェイト(SDEFの場合はweight[1-3]に(sdef_r0-sdef_r1)/2.xyzを入れる)
	int weightType;	//変形方式(0-2:BDEF1,2,4 / 3:SDEF)
	float3 sdef_c;	//SDEFパラメータ
};

//頂点モーフテーブルに入っている情報1つ
struct MorphItem {
	int iMorph;		//対応するモーフ番号(PoseSolver::morphValues[iMorph]の値を見て、頂点の位置またはUVをどれだけ動かすか決める)
	int target;		//-1:位置, 0:UV, 1～4:追加UV1～4
	float4 delta;	//モーフ値1につき座標orUVはどれだけ動くか
};

//各頂点に割り当てられる、頂点モーフテーブルの参照情報
struct MorphPointer {
	int where;	//テーブルのどこから自分に割り当てられたモーフ情報が始まるか？ -1で割り当て無し
	int count;	//いくつアイテムがあるか
};

};

#endif
