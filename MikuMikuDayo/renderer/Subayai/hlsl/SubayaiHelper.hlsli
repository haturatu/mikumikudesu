#ifdef __INTELLISENSE__
#include "../../../hlsl/yrz.hlsli"
#include "../../../hlsl/resources.hlsli"
#endif

/*****************************************************************
各パスで大体共通して使うモノ群
*****************************************************************/

/****************************
  定数・マクロ
****************************/
static float SkyboxPhi = C_SkyRotate;

static float3 LinearLightColor = ToLinear(LightColor) * PI * DirectLightGain * DirectionalLightGain;

//マテリアルのカテゴリ
#define CAT_DEFAULT 0
#define CAT_GLASS 1
#define CAT_METAL 2
#define CAT_SSS 3
#define CAT_BLACKBODY 4


//レイトレだとSampleが使えないので
#if defined(RAYTRACING)
	#define DO_SAMPLE(X,Y) Sample(X,Y)
#else
	#define DO_SAMPLE(X,Y) SampleLevel(X,Y,0)
#endif

//radeonではscalar変数以外をbindlessテクスチャの添え字にする場合に添え字をNURI()として囲う必要がある
#define NURI NonUniformResourceIndex


/****************************
  構造体
****************************/
struct Material
{
    int texture;        //MMDのテクスチャ番号
	int twosided;		//0:片面材質 1:両面材質
	float autoNormal;	//textureの明度勾配によるバンプマップの強さ
	int category;		
	float3 albedo;		//反射率
    float3 emission;	//発光要素を減衰無しで観察した時の輝度
	float alpha;		//不透過度
	float roughness;	
	float anisotropy;
	float2 IOR;			//IOR.xyでCauchyの分散係数ABを示す
	float4 cat;			//カテゴリ固有パラメータ(後述)
	float3 ao;			//AO項(AOマップの結果)
	float iridescence;	//iridescenceLUT参照用
	float3x3 ZH;
	float ramp;
	float3 lightVisibility;
	float3 skyVisibility;
	float3 AOVisibility;
	float3 shadowVisibility;
	float shadowCaster;
	uint4 lightBlockMask;
	//以下はSubayai構造体より読み込み後に算出する
	float2 ggxA;		//GGXのα。roughness, anisotropyより算出
};

struct Payload
{
	int ID;			//最後にぶっかった物体のinstanceID
	int iFace;		//↑の面番号
	float2 uv;		//↑のレイがぶつかった位置の重心座標
	float  tr;		//透過率
	float  t;		//レイの移動距離
	bool glass;		//ガラスの屈折を扱っている場合はtrue
	bool ccw;		//片面材質の場合、面の方向がレイの方向に対して左回りの時にヒットする
};

/****************************
  行列
****************************/

//iSampleに応じてジッター入りの射影行列を作る(Zn=1で決め打ち)
float4x4 JitteredProjection()
{
	if (iSample == 0)
		return ProjectionMatrix;

	//2025-0807 フレームでハッシュのシードを変えるのをやめた
	//iSampleが同じなら同じ行列が返ることでカメラ・モデルが静止している時はフレーム間でG-Bufferの値が変わらなくなる
	float2 jt = Hash3(uint3(114,iSample,514)).xy-0.5;
	float4x4 jm = ProjectionMatrix;

	if (Perspective) {
		jm._31_32 = jt/Resolution*2;
	} else {
		jm._41_42 = jt/Resolution*2;
	}
	return jm;
}

static float4x4 JtProjectionMatrix = JitteredProjection();
static float4x4 JtViewProjectionMatrix = mul(ViewMatrix, JtProjectionMatrix);
static float4x4 InvJtProjectionMatrix = Perspective ? InverseProjectionMatrix(JtProjectionMatrix) : InverseOrthoProjectionMatrix(JtProjectionMatrix);
static float4x4 InvViewMatrix = InverseRTMatrix(ViewMatrix);
static float4x4 InvViewProjectionMatrix = mul(InvJtProjectionMatrix,InvViewMatrix);

/****************************
  ヘルパー関数
****************************/

float3 SpotLightColor(int idx)
{
	//明るさ1の定義 … 反射率1のLambert反射面を10MMDの距離から面に垂直に照射して輝度1になる
	float v = L_Val[idx] * 100 * PI * DirectLightGain;
	v *= exp2(L_Bright[idx]*20);
	return HSVtoRGB(float3(L_Hue[idx],saturate(L_Sat[idx]),v));
}


//位置・法線・UVを得る。法線マップは考慮しない(stは重心座標:Attr.st)
Vertex GetVertex(uint id, uint triangleIndex, float2 st)
{
	float3 barycentrics = float3((1.0f - st.x - st.y), st.x, st.y);

	Vertex v = (Vertex)0;
    for (int i=0; i<3; i++) {
        Vertex vi = VB[id][IB[id][triangleIndex*3 + i]];
        v.position += vi.position * barycentrics[i];
        v.normal += vi.normal * barycentrics[i];
        v.uv += vi.uv * barycentrics[i];
		v.tangent += vi.tangent * barycentrics[i];
		for (int j=0; j<4; j++)
			v.exuv[j] += vi.exuv[j] * barycentrics[i];
    }
    v.normal = normalize(v.normal);
	v.tangent = normalize(v.tangent);
    return v;
}


//レイの向きvからSkyboxのUV座標を作る
float2 LongRat(float3 v)
{
	return float2(atan2(v.z, v.x), acos(v.y)) / float2(-2 * PI, PI) - float2(0.25 + SkyboxPhi, 0);
}

//uvからレイの向きだけ得る
float3 RaySaySay(float2 uv, uniform bool normalizeFlag = true)
{
	//パッチがビュー座標系でZ=1にあったと仮定した場合のワールド座標(ZNear=1で決め打ち)
	float4 s = float4((uv*2-1)*float2(1,-1),0,1);
	s = mul(s,InvViewProjectionMatrix);

	[branch]
	if (normalizeFlag)
		return normalize(s.xyz - CameraPosition);
	else
		return s.xyz - CameraPosition;
}

//カメラからの線形深度zとUVを入れてワールド座標を得る
float3 UVtoWorld(float2 uv, float z)
{
	float3 R = RaySaySay(uv,false);
	return CameraPosition + R * z;
}


//uvと深度zからパッチのワールド座標Pとレイの向きを得る
float3 RaySaySayZ(float2 uv, float z, out float3 P, uniform bool normalizeFlag = true)
{
	float3 R = RaySaySay(uv,false);
	float3 rd = R;
	
	[branch]
	if (normalizeFlag)
		rd = normalize(R);

	P = CameraPosition + R * z;
	return rd;
}


/*
//レイ生成
void RaySaySay(float2 Tex, out float3 ray, out float3 pos, uniform bool normalizeFlag = true)
{
	//ビュー座標系でZ=1の位置に置かれたパッチのスクリーン座標(Zn=1と仮定)
	float4 s = float4((Tex.xy*2-1)*float2(1,-1),0,1);

	s = mul(s,InvViewProjectionMatrix);
	
	pos = CameraPosition;
	ray = s.xyz - pos;

	[branch]
	if (normalizeFlag)
		ray = normalize(s.xyz - CameraPosition);
}

//カメラからの距離dとUVを入れてワールド座標を得る
float3 UVtoWorld(float2 uv, float d)
{
	float3 ro,rd;
	RaySaySay(uv, rd,ro);
	return ro + rd*d;
}
*/


//カメラ基準でSphereMapのUV座標を生成する。Nはワールド法線
float2 GetSPUV(float3 N)
{
	return mul(N,(float3x2)ViewMatrix) * float2(0.5,-0.5) + 0.5;
}


/****************************
  マテリアル関係のヘルパー
****************************/

//MMDマテリアルにSubayaiマテリアル構造体の情報を加えてレンダリング用マテリアルを作る
//この時点ではまだマテリアルの全パラメータは確定してない(テクスチャなどポリゴン上の位置毎に変わる情報が加味されないので)
Material MMDtoSubayai(MMDMaterial mmd, SubayaiValue pbr, uniform bool alphaOnly)
{
	Material m = (Material)0;

	//シャドウマップ作成のためにこれは要るのでα onlyでも入れとく
	m.shadowCaster = pbr.ShadowCaster;

	[branch]
	if (!alphaOnly) {
		m.category = pbr.Category;
		m.cat = pbr.Cat;
		m.IOR = pbr.IOR;
		m.roughness = pbr.Roughness;
		m.anisotropy = pbr.Anisotropy;
		m.ggxA = GGXAlpha(m.roughness, m.anisotropy);
		m.autoNormal = pbr.AutoNormal;
		m.texture = mmd.tex;
		m.iridescence = pbr.Iridescence;
		m.ramp = pbr.Ramp;
		m.lightVisibility = pbr.LightVisibility;
		m.skyVisibility = pbr.SkyVisibility;
		m.shadowVisibility = pbr.ShadowVisibility;
		m.AOVisibility = pbr.AOVisibility;
		m.lightBlockMask = pbr.LightBlockMask;

		mmd.diffuse = max(0,mmd.diffuse);
		
		if (pbr.AlbedoEstimation == 0) {
			//本家に近い色味を出す
			float3 mmdcol = saturate(mmd.diffuse.rgb*154.0/255 + mmd.ambient);
			m.albedo = ToLinear(mmdcol);
		} else if (pbr.AlbedoEstimation == 1) {
			//ray-mmdライクな拡散反射分だけからalbedoを取得
			m.albedo = ToLinear(mmd.diffuse.rgb);
		} else if (pbr.AlbedoEstimation == 2) {
			//環境色分だけからalbedoを取得
			m.albedo = ToLinear(mmd.ambient.rgb);
		}
		m.emission = pbr.Emission;

		//AL対応材質加算
		if (SqrLen(mmd.specular)<1e-4 && mmd.shininess>100) {
			m.emission += (mmd.shininess-100) * m.albedo * (C_AL ? Exerp(0.01,100,C_AL): 0);
		}
	}
	m.twosided = mmd.drawFlag & 1;
	m.alpha = saturate(mmd.diffuse.w) * saturate(pbr.Alpha);
	
	return m;
}

//ColorConstを使って色変え
void ColorModulation(inout Material m, SubayaiValue pbr, float4x4 exuv)
{
	float4 C = pbr.ColorConst;
	if (1 <= pbr.ColorConstUV && pbr.ColorConstUV <= 4)
		C = exuv[pbr.ColorConstUV-1];

	if (C.w) {
		float3 dest;
		if (pbr.ColorConstOp == 0) {
			dest = C.rgb;
		} else if (pbr.ColorConstOp == 1) {
			dest = saturate(C.rgb + m.albedo);
		} else if (pbr.ColorConstOp == 2) {
			dest = saturate(C.rgb * m.albedo);
		} else if (pbr.ColorConstOp == 3) {
			dest = saturate(pow(m.albedo, C.rgb));
		} else if (pbr.ColorConstOp == 4) {
			float3 hsv = RGBtoHSV(m.albedo);
			hsv.x += C.x;
			hsv.yz = saturate(C.yz * hsv.yz);
			dest = HSVtoRGB(hsv);
		}
		m.albedo = lerp(m.albedo, dest, C.w);
	}
}

//MMDでの材質色情報を計算(特に使ってない)
float4 MMDColor(uint iFace, float2 uv, float2 spuv)
{
    float3 Le = 1;
    float4 o = 0;
    uint imo = ModelIndex;
	MMDMaterial m = MMDMaterials[imo][Faces[imo][iFace]];
	uint tidx = TextureTable[imo];	//モデルに割り当てられたテクスチャ番号の先頭

	//diffuse的な何か
	float4 tex = 1;
	if (m.tex >= 0) {
		tex =  Textures[NURI(tidx+m.tex)].Sample(samp, uv);
		tex.rgb = lerp(1, tex.rgb * m.textureMulValue.rgb + m.textureAddValue.rgb, m.textureMulValue.a + m.textureAddValue.a);
	}

	o = float4(saturate(m.diffuse.rgb * Le + m.ambient), m.diffuse.a) * tex;
	
	//toonは考慮しない

	//sphere map : spmodeが加算or乗算の時はスフィアマップを加算or乗算する
	if (m.spTex >= 0 && (m.spmode==1 || m.spmode==2)) {
		float4 sptex = Textures[NURI(tidx + m.spTex)].Sample(samp, spuv);
		sptex.rgb = lerp(m.spmode==1 ? 1 : 0, sptex.rgb * m.sphereMulValue.rgb + m.sphereAddValue.rgb, m.sphereAddValue.a + m.sphereMulValue.a);
		o.rgb = (m.spmode==1) ? o.rgb*sptex.rgb : o.rgb+sptex.rgb;
		o.a *= sptex.a;
	}
    return o;
}

//マテリアルパラメータのUV座標を計算
float2 CalcUV(int idx, float2 uv, float4x4 exuv, float loops)
{
	if (idx == 0 || idx>=5)
		return uv * loops;
	
	return exuv[idx-1].xy * loops;
}

#define GET_TEXTURE(PARAM) ptex.PARAM##Map.DO_SAMPLE(samp, CalcUV(pbr.PARAM##UV, texcoord, exuv, pbr.TextureLoops * pbr.PARAM##Loops))

//IDとマテリアル番号からマテリアルを得る
Material GetMaterialIDMat(uint id, uint iMat, float2 texcoord, float2 spuv, uniform bool alphaOnly, float4x4 exuv)
{
	MMDMaterial mmd = MMDMaterials[id][iMat];
	//Material m = materials[id][faces[id][triangleIndex].iMaterial];

	SubayaiValue pbr = GetSubayaiValue(id,iMat);
	SubayaiTexture ptex = GetSubayaiTexture(id,iMat);
	Material m = MMDtoSubayai(mmd,pbr,alphaOnly);

	//MMDテクスチャ
	if (mmd.tex >= 0) {
		int iTex = TextureTable[id];
		float4 tex = Textures[NURI(iTex + mmd.tex)].DO_SAMPLE(samp, texcoord);
		[branch]
		if (!alphaOnly) {
			tex.rgb = lerp(1, tex.rgb * mmd.textureMulValue.rgb + mmd.textureAddValue.rgb, mmd.textureMulValue.a + mmd.textureAddValue.a);
			tex.rgb = ToLinear(tex.rgb);
			m.albedo *= tex.rgb;
			m.emission *= tex.rgb;
		}
		m.alpha *= tex.a;
	}

	//スフィアマップ
	if (mmd.spTex >= 0 && pbr.EnableSphereMap && (mmd.spmode == 1 || mmd.spmode == 2)) {
		float2 uv = spuv;
		float4 tex = Textures[NURI(TextureTable[id]+mmd.spTex)].DO_SAMPLE(samp, uv);
		[branch]
		if (!alphaOnly) {
			tex.rgb = lerp(mmd.spmode==1 ? 1 : 0, tex.rgb * mmd.sphereMulValue.rgb + mmd.sphereAddValue.rgb, mmd.sphereAddValue.a + mmd.sphereMulValue.a);
			m.albedo = ToGamma(m.albedo);

			if (mmd.spmode == 1) {
				//乗算
				m.albedo *= tex.rgb;
			} else {
				//加算
				m.albedo += tex.rgb;
			}
			m.albedo = ToLinear(saturate(m.albedo));
		}
		m.alpha *= tex.a;
	}

	if (ptex.hasAlphaMap) {
		float tex = GET_TEXTURE(Alpha).r;
		m.alpha *= lerp(1,tex,pbr.AlphaScale);
	}

	if (pbr.Cutout > 0) {
		m.alpha = m.alpha < pbr.Cutout ? 0 : 1;
	}

	[branch]
	if (!alphaOnly) {
		if (ptex.hasRoughnessMap) {
			m.roughness *= lerp(1, GET_TEXTURE(Roughness).r, pbr.RoughnessScale);
			m.roughness = saturate(m.roughness);
		}
		if (ptex.hasAlbedoMap) {
			float3 tex = GET_TEXTURE(Albedo).rgb;
			tex = ToLinear(tex);
			m.albedo = lerp(m.albedo, tex, pbr.AlbedoScale);
		}
		m.ao = 1;
		if (ptex.hasAOMap) {
			m.ao = lerp(1, GET_TEXTURE(AO).r, pbr.AOScale);
		}
		if (ptex.hasRampZH) {
			float u = m.ramp;
			m.ZH[0] = ptex.RampZH.DO_SAMPLE(ClampSamp, float2(u,0)).rgb;
			m.ZH[1] = ptex.RampZH.DO_SAMPLE(ClampSamp, float2(u,0.5)).rgb;
			m.ZH[2] = ptex.RampZH.DO_SAMPLE(ClampSamp, float2(u,1.0)).rgb;
		} else {
			m.ZH = ZHCosine;
		}
		if (ptex.hasIridescenceMap) {
			m.iridescence *= lerp(1, GET_TEXTURE(Iridescence).r, pbr.IridescenceScale);
		}

		m.ggxA = GGXAlpha(m.roughness, m.anisotropy);
		ColorModulation(m,pbr, exuv);

	}
	
	m.emission += m.albedo * pbr.Light;
	m.emission *= DirectLightGain;

	return m;
}

//idとtriangleIndexからマテリアルを得る
Material GetMaterial(uint id, uint triangleIndex, float2 texcoord, float2 spuv, uniform bool alphaOnly, float4x4 exuv)
{
	if (id<0 || triangleIndex <0)
		return (Material)0;
	int iMat = Faces[id][triangleIndex];
	return GetMaterialIDMat(id,iMat,texcoord,spuv,alphaOnly, exuv);
}

Material GetMaterialFromGB(float2 uv, out bool valid, bool alphaOnly, float4x4 exuv)
{
	float4 ND = MyNormalDepth.Sample(PointSamp, uv);
	if (ND.w == 0) {
		valid = false;
		return (Material)0;
	}
	valid = true;
	uint2 GB1 = GBuffer1[uv*Resolution];
	float2 GB2 = GBuffer2[uv*Resolution];
	return GetMaterial(GB1.r,GB1.g,GB2, GetSPUV(ND.xyz), alphaOnly, exuv);
}


//albedoマップの輝度勾配から法線マップを作る
float3 AutoNormal(Texture2D<float4>texture, float2 uv, float scale)
{
	float W,H;
	texture.GetDimensions(W,H);
    //本当はフットプリントにeを合わせたい
	//テクスチャの解像度からeを計算するのは良いアイディアのようだけど大体あんまりうまくいかない
    float3 e = {1.0/256,1.0/256,0};
    float xp = texture.SampleLevel(LinearSamp, uv + e.xz, 0).g;
    float xn = texture.SampleLevel(LinearSamp, uv - e.xz, 0).g;
    float yp = texture.SampleLevel(LinearSamp, uv + e.zy, 0).g;
    float yn = texture.SampleLevel(LinearSamp, uv - e.zy, 0).g;
	float2 d = float2(xn-xp,yn-yp)*scale;
    float3 N = normalize(float3(d,1));
    return N;
}

//法線マップを適用した接空間を得る
float3x3 NormalMap(Material m, uint iFace, float3 P, float2 uv, float3x3 TBN)
{
    uint id = ModelIndex;

	bool valid;
	float3x3 TBN_tex;	//テクスチャ空間→ワールド変換行列
	TBN_tex = ComputeTBNMap(TBN[2], P, uv, valid, true);
	float3x3 TBN_o = TBN;

	if (valid) {
    	int iMat = Faces[id][iFace];
        SubayaiValue pbr = GetSubayaiValue(id,iMat);
	    SubayaiTexture ptex = GetSubayaiTexture(id,iMat);
		float2 tuv = uv * pbr.TextureLoops;
		if ( (m.autoNormal && m.texture>=0) || (ptex.hasNormalMap)) {
			int iTex = TextureTable[id];
			float3 nmap;

			if (ptex.hasNormalMap) {
				nmap = ptex.NormalMap.Sample(samp,tuv*pbr.NormalLoops).rgb * 2 - 1;
				nmap = normalize(nmap * float3(pbr.NormalScale.xx,1));
			} else {
				nmap = AutoNormal(Textures[NURI(iTex + m.texture)], uv, m.autoNormal);
			}

			float3 n_after = normalize(mul(nmap,TBN_tex));
			float3 t_after = Rotate(TBN[0],FromToRotation(TBN[2],n_after));
			TBN = ComputeTBN(n_after, t_after, false);
		}
	}

    return TBN;
}



/****************************
  シェーディング関係のヘルパー
****************************/

//skyboxからの法線Nに対する照度を得る
float3 SkyIrradiance(float3 N, float3x3 ZH)
{
	N = RotY(N, -SkyboxPhi*2*PI);
	return SHIrradiance(float3(-N.z,-N.x,N.y), SkyboxSH[0], ZH) * SkyLightGain;	//SHの係数はZup右手系で求めているので軸を入れ替える必要アリ
}

float3 FetchSkybox(float3 rd, float roughness = 0)
{
	uint WW,HH,W,H,M;
	Skybox.GetDimensions(0,WW,HH,M);
	
	float lod = roughness * (M-1);
	int iLod = lod;
	Skybox.GetDimensions(iLod,W,H,M);

	float2 uv = LongRat(rd);
	//return Skybox.SampleLevel(SkySamp, uv, clamp(roughness*5,0,5)).xyz; //sdPBR用に作られたenvmapを使う場合
	float3 sky = Skybox.SampleLevel(SkySamp, uv, clamp(lod,0,M)).xyz * SkyLightGain;

	//avoid pseudo nipples
	if (W<=32) {
		float3 sh = SkyIrradiance(rd,ZHCosine);
		sky = lerp(sky,sh,frac(lod));
		if (W<=16)
			sky = sh;
	}

	return sky;
}


//ライトの消失係数μtを求める
float LightMuT(int idx)
{
	return L_Godray[idx] * 0.01;
}

//ライトのボーン行列を狙いボーンに向ける
float4x4 LightBone(int i)
{
	if (!L_Is[i])
		return Identity44;

	float4x4 m = L_Bone[i];
	float3 a = L_Target[i];
    float3 p = m._41_42_43;	//sdPBRではなんかbranchを明示的に付けないととうまくうごかないことがあった(狙いボーンが離れていない時だけ影が無くなる)
	[branch]
	if (length(a-p) > 0.01)
	{
        float3 Z = normalize(a-p);	//反対向き→逆行列で作る
        float3 up = (abs(Z.y) < 0.999) ? float3(0,1,0) : float3(-1,0,0);
		float3 X = normalize(cross(up,Z));
        float3 Y = cross(Z,X);
		return float4x4(X,0, Y,0, Z,0, p,1);	//(平行移動分以外は)逆行列
    } else {
        return m;
	}
}

//位置PのLに垂直な面に対して届くライトの照度(NoL抜き),シャドウ抜き
//Lは正規化済みP→光源位置ベクトル
//cone : コーンによる減衰分
float3 LightIrradiance(int idx, float3 P, out float3 L, out float cone)
{
	cone = 1;
	float4x4 LBone = LightBone(idx);
	float3 PL = LBone._41_42_43 - P;
	float d = length(PL);
	L = normalize(PL);

	//最大到達距離
	if (L_MaxRadius[idx] && d > L_MaxRadius[idx]*100)
		return 0;

	//最小減衰距離
	float e = max(d,L_MinRadius[idx]*100);
	float3 Le = SpotLightColor(idx) / max(1e-3,e*e);

	float cosT = dot(-L,LBone._31_32_33);
	if (L_Cone[idx]) {
		float hotspot = lerp(L_Cone[idx],1,L_DullCone[idx]);
		float falloff = L_Cone[idx];
		cone = smoothstep(falloff,hotspot, cosT);
		Le *= cone;
	}

	Le *= exp(-LightMuT(idx) * d);

	if (all(Le<1e-10))
		return 0;

	SubayaiTexture ltex = GetSubayaiTexture(L_ID[idx], 0);
	SubayaiValue pbr = GetSubayaiValue(L_ID[idx],0);
	if (ltex.hasIESMap) {
		float3 tex;
		float3 LonA = mul((float3x3)LBone, L);
		if (pbr.Equirectangular) {
			float3 v = LonA;
			float2 uv = float2(atan2(v.z, v.x), acos(v.y)) / float2(-2 * PI, PI) - float2(0.25, 0);
			tex = pow(ltex.IESMap.SampleLevel(SkySamp, uv, 0).rgb, pbr.IESGamma);
		} else {
			float kuv = acos(L_Cone[idx]);
			//Zの向きが逆なのでsdSubayaiから符号をちょっと換えた
			float2 uv = (float2(atan2(-LonA.r, -LonA.z), atan2(LonA.y, -LonA.z)) / kuv + 1) / 2;
			tex = pow(ltex.IESMap.SampleLevel(samp, uv,0).rgb, pbr.IESGamma);
		}
		Le *= ToLinear(tex);
	}

	return Le;
}

//マテリアルパラメータからF項を返す
float3 Fresnel(SubayaiTexture ptex, Material m, float LoH, out float3 f0)
{
	float3 f90;

	if (ptex.hasIridescenceLUT) {
		f0 = ptex.IridescenceLUT.SampleLevel(ClampSamp, float2(LoH,m.iridescence), 0).rgb;
		return f0;
	}
	if (m.category == CAT_METAL) {
		f0 = m.albedo;
		f90 = 1;
	} else {
		f0 = IORtoF0(m.IOR.x);
		if (m.category != CAT_GLASS) {
			//ガラス・金属以外の場合はroughnessが高い(0.9以上)場合はグレージング角で全反射にならぬようf0,f90を制限する
			//てきとうな多孔質近似
			f90 = saturate((1-m.roughness)*10);
			f0 = min(f0,f90);
		}
	}
	return lerp(f0,f90,FresnelSchlick(LoH));
}

//Lと垂直な面での照度Leの光から、V方向への輝度を返す
void Shading(float3 Le, float3 L, SubayaiTexture ptex, Material m, float3x3 TBN, float3 P, float3 V, out float3 diffuse, out float3 specular)
{
	diffuse = 0;
	specular = 0;

	float3 N = TBN[2];
		
	float3 H = normalize(L+V);
	float NoL = dot(N,L);
	float LoH = abs(dot(L,H));

	if (m.category != CAT_METAL) {
		if (ptex.hasRampLUT) {
			diffuse = m.albedo * Le * ptex.RampLUT.SampleLevel(ClampSamp, float2(NoL*0.5+0.5,m.ramp), 0).rgb / PI;
		} else {
			diffuse = m.albedo * Le * saturate(NoL) / PI;
		}
	}

	float f0;
	float3 F = Fresnel(ptex, m,LoH,f0);

	//点光源でハイライトが全く無くなってしまうのでroughnessの最低保証を入れる
	float DG = GGXAnisoBRDF(max(m.ggxA,1e-4), L, V, TBN);	//DG / 4NoLNoV
	DG = clamp(DG,0,100);	//邪道だがチカチカをなんとかするためDGを制限する
	float NoV = abs(dot(N,V));

	specular = Le * F * DG * saturate(NoL) * m.lightVisibility;
	//2025-1129 スムージングの影響で法線が急激に変わる部分で不自然になることがあるため最初の項は省略
	diffuse *= /*saturate(1-F) */ m.lightVisibility;
}


float VisibilityVSM(int idx, float3 P);	//プロトタイプ宣言

//影付きライトによる直接光の計算
void Lighting(int idx, SubayaiTexture ptex, Material m, float3x3 TBN, float3 P, float3 V, out float3 diffuse, out float3 specular)
{
	diffuse = 0;
	specular = 0;

	if (!L_Is[idx])
		return;
	
	if (m.lightBlockMask[0] & (1U<<idx)) {
		return;
	}

	float cone;
	float3 L;
	float3 Le = LightIrradiance(idx, P, L, cone);
	if (all(Le < 1e-10))
		return;

	float3 w = VisibilityVSM(idx, P);
	w = lerp(1, w, m.shadowVisibility);
	if (all(w==0))
		return;

	Shading(Le, L, ptex, m, TBN, P,V, diffuse, specular);
	diffuse *= w;
	specular *= w;
}

//Giovanni Cocco氏
//"Anisotropic Image-Based Lighting"
//https://www.shadertoy.com/view/4fV3W3 より

float3 slopeToNormal(float2 p) {
    return normalize(float3(-p, 1));
}

//==================================================================================//
//              Compute the extrema using our closed form solution                  //
//==================================================================================//
//                                                                                  //
//  The view vector, wo, must be expressed in normal space                          //
//                                                                                  //
//  Return 2 extrema in slope space, to obtain all 4:                               //
//   - the further extrema are 'farExtremum' and '-farExtremum'                     //
//   - the closest extrema are 'closeExtremum' and '-closeExtremum                  //
//                                                                                  //
//==================================================================================//
void extremumPoints(float radius_x, float radius_y, float3 wo, float anisotropyAngle, out float2 closeExtremum, out float2 farExtremum, out float2 roots)
{
    //==============================================================================//
    //================== Closed-form solution - Appendix A =========================//
    //==============================================================================//
    float2x2 R = float2x2(cos(anisotropyAngle), -sin(anisotropyAngle), sin(anisotropyAngle), cos(anisotropyAngle));
    float2 o = mul(wo.xy,R);
    
    float2 o2 = o.xy*o.xy;
    float r2x = radius_x*radius_x;
    float r2y = radius_y*radius_y;
    float p0 = 2.*o.x*o.y*radius_x*radius_y;
    float p1 = o2.x*(r2x*r2y + r2y) - o2.y*(r2x*r2y + r2x) + r2x -r2y;
    float p2 = -p0*(r2y + 1.);
    float p3 = p0*p2*(r2x + 1.);
    float t0 = p2 == 0. ? step(0., p1)*PI/2. : atan((p1+sqrt(p1*p1-p3))/p2);
    float t1 = p2 == 0. ? t0+PI/2. : atan((p1-sqrt(p1*p1-p3))/p2);
    
    //==============================================================================//
    //==================== Return the slope space extrema ==========================//
    //==============================================================================//
    
    closeExtremum = mul(R ,float2(radius_x*cos(t0), radius_y*sin(t0)));
    farExtremum = mul(R,float2(radius_x*cos(t1), radius_y*sin(t1)));
    roots = float2(t0, t1);
}

//==================================================================================//
//                            Clamp the futher extrema                              //
//==================================================================================//
//                                                                                  //
//  Update the 2 extrema in normal space and return tau_0 and tau_1                 //
//                                                                                  //
//==================================================================================//
void clampExtrema(inout float3 farNormal0, inout float3 farNormal1, float3 wo, out float2 tau)
{
    //==============================================================================//
    //================== Closed-form solution - Appendix B =========================//
    //==============================================================================//
    float p0 = farNormal1.x*wo.x + farNormal1.y*wo.y;
    float p1 = wo.z*(1.-farNormal1.z*farNormal1.z);
    float p3 = sqrt(p0*p0 + wo.z*p1);
    float t0 = max(0.5+farNormal1.z*(p0-p3)/(2.*p1), 0.);
    float t1 = min(0.5+farNormal1.z*(p0+p3)/(2.*p1), 1.);
    
    float3 tmp = farNormal0;
    farNormal0 = normalize(lerp(farNormal0, farNormal1, t0));
    farNormal1 = normalize(lerp(farNormal0, farNormal1, t1));
    tau = float2(t0, t1);
}


//==================================================================================//
//        Shift the further extrema and return the LOD to sample the cubemap        //
//==================================================================================//
//                                                                                  //
//  Update the 2 extrema in normal space and return the LOD                         //
//                                                                                  //
//  NOTE: Shadertoy does not support GGX kernel pre-integration of the cubemap,     //
//        the returned LOD is different from the one in the paper.                  //
//                                                                                  //
//==================================================================================//
void shiftExtrema(float radius_x, float radius_y, float3 wo, float anisotropyAngle, float2 roots, float2 tau, inout float3 farNormal0, inout float3 farNormal1, out float level)
{
    //==============================================================================//
    //============== Update the closest extrema - Appendix C =======================//
    //==============================================================================//
    float2x2 R = float2x2(cos(anisotropyAngle), -sin(anisotropyAngle), sin(anisotropyAngle), cos(anisotropyAngle));
    float angleDelta = (tau.x > 0. ? tau.x : tau.y-1.)*(roots.x-roots.y);
    float min0t = roots.x+angleDelta;
    float min1t = roots.x-angleDelta+PI;
    float2 close0Slope = mul( R, float2(radius_x*cos(min0t), radius_y*sin(min0t)));
    float2 close1Slope = mul( R, float2(radius_x*cos(min1t), radius_y*sin(min1t)));
    float3 close0Normal = slopeToNormal(close0Slope);
    float3 close1Normal = slopeToNormal(close1Slope);
    
    //==============================================================================//
    //======================== Shift the furthest extrema ==========================//
    //==============================================================================//
    float minLength = length(reflect(-wo, close0Normal)-reflect(-wo, close1Normal));
    float maxLength = length(reflect(-wo, farNormal0)-reflect(-wo, farNormal1));
    float rateo = (maxLength-minLength)/maxLength;
    float3 axisCenter = normalize(farNormal0+farNormal1);
    farNormal0 = normalize(axisCenter + rateo*(farNormal0-axisCenter));
    farNormal1 = normalize(axisCenter + rateo*(farNormal1-axisCenter));
    
    //==============================================================================//
    //============================== Compute the LOD ===============================//
    //==============================================================================//
    
    // LOD formula for GGX
    // level = log2(float(textureSize(cubeMap, 0).x))*sqrt(max(minLength, 2.3*maxLength/SAMPLE_COUNT)/2.);
    
    // THIS LOD CALCULATION IS ONLY FOR THE BOX FILTERED CUBEMAP OF SHADERTOY!
    //level = log2(float(textureSize(iChannel1, 0).x)*max(minLength/2., 2.5*maxLength/SAMPLE_COUNT));
	uint W,H;
	Skybox.GetDimensions(W,H);
	const int SAMPLE_COUNT = 16;
	float maxlod = 1;//log2(H);
	level = maxlod * max(minLength/2., 2.5*maxLength/SAMPLE_COUNT);
}


//Xavier Chermain et al. "Anisotropic Specular Image-Based Lighting Based on BRDF Major Axis Sampling"
//https://xavierchermain.github.io/publications/aniso-ibl
float3 SampleEnvmap(float3 V, float3x3 TBN, float2 a)
{
	float3 N = TBN[2];
    // We use Kulla and Conty 2017 parametrization for the anisotropic roughness
    //float radius_x = roughness*roughness/2. * (1. + anisotropy);
    //float radius_y = roughness*roughness/2. * (1. - anisotropy);
    float radius_x = a.x;
    float radius_y = a.y;
	float anisotropyAngle = 0;
    //==============================================================================//
    //============== Get the extrema with the closed-form solution =================//
    //==============================================================================//
    
    float3 wo = mul(TBN,V);
    float2 closeExtremum;
    float2 farExtremum;
    float2 roots;
    extremumPoints(radius_x, radius_y, wo, anisotropyAngle, closeExtremum, farExtremum, roots);
    
    float3 farNormal0 = slopeToNormal(farExtremum);
    float3 farNormal1 = slopeToNormal(-farExtremum);
   
    //==============================================================================//
    //======================== Clamp and shift the extrema =========================//
    //==============================================================================//
    float2 tau;
    clampExtrema(farNormal0, farNormal1, wo, tau);
    
    float level;
    shiftExtrema(radius_x, radius_y, wo, anisotropyAngle, roots, tau, farNormal0, farNormal1, level);
     
    //==============================================================================//
    //========================= Sample the enviroment map  =========================//
    //==============================================================================//
	const int SAMPLE_COUNT= 16;
    float3 farNormal0WS = mul(farNormal0,TBN);
    float3 farNormal1WS = mul(farNormal1,TBN);
    float3 environment = 0;
    for (float i = 0.; i < SAMPLE_COUNT; i++) {
        float3 wm = normalize(lerp(farNormal0WS, farNormal1WS, i/(SAMPLE_COUNT-1.)));
		float2 uv = LongRat(reflect(-V, wm));
        environment += Skybox.SampleLevel(SkySamp, uv, level).rgb;
    }
    environment /= SAMPLE_COUNT;
	return environment;
}

//==============================================================================//
//============= Bend the normal [Lagarde and Golubev 2018]  ====================//
//==============================================================================//
float3 sampleEnviroment_bendNormal(float3 view, float3x3 TBN, float2 a)
{
	float3 tangent = TBN[0];
	float3 bitangent = TBN[1];
   
	float r2 = (a.x+a.y)/2;
	float r = sqrt(r2);
    float anisotropy = r2 ? (a.x-a.y)/(2*r2) : 0;
    float3 grainDirWs = anisotropy >= 0.0 ? bitangent : tangent;
    float stretch = abs(anisotropy) * clamp(1.5*sqrt(r), 0., 1.);
    float3 B = cross(grainDirWs, view);
    float3 grainNormal = cross(B, grainDirWs);
    float3 iblN = normalize(lerp(TBN[2], grainNormal, stretch));

    //==============================================================================//
    //======================= Sample the enviroment map ============================//
    //==============================================================================//
	/*
    float roughIBL = roughness*clamp(1.2-abs(anisotropy), 0., 1.);
    float level = log2(float(textureSize(iChannel1, 0).x)*roughIBL*roughIBL);
    float3 environment = pow(textureLod(iChannel1, reflect(-view, iblN), level).rgb, float3(2.2));
    
    return environment;
	*/
	return FetchSkybox(reflect(-view,iblN), sqrt(min(a.x,a.y)));
}

//環境マップ分の計算
void EnvLighting(uint2 pix, SubayaiTexture ptex, Material m, float3x3 TBN, float3 V, out float3 diffuse, out float3 specular)
{
	diffuse = 0;
	specular = 0;
	float3 ao = lerp(1,m.ao,m.AOVisibility);
	if (all(ao) <= 0)
		return;

	float3 N = TBN[2];

	float NoV = abs(dot(N,V));
	float3 f0;
	float3 F = Fresnel(ptex, m, NoV, f0);
	float isoR = m.roughness;

	if (m.category == CAT_DEFAULT || m.category == CAT_SSS)
		diffuse = m.albedo * SkyIrradiance(N,m.ZH) * ao * m.skyVisibility;
	
	//サンプリングによるground truth
	/*
	float3 H = VNDF(V,TBN,Hash3(uint3(pix,RealTime*1000)).xy, sqr(m.roughness));
	float3 LEnv = FetchSkybox(reflect(-V,H),0);
	specular = LEnv * F * GGXAnisoBRDFdivPDF(sqr(m.roughness), V,H,TBN);
	*/
	
	//float3 LEnv = FetchSkybox(reflect(-V,N), isoR) * m.ao;	//単純にisoでサンプリング
	//float3 LEnv = SampleEnvmap(V,TBN,m.roughness);			//複数サンプルする版
	float3 LEnv = sampleEnviroment_bendNormal(V,TBN,m.ggxA);	//bendNormal
	specular = LEnv * EnvBRDFApprox(f0, NoV, isoR) * ao * m.skyVisibility;
	
	//エネルギー保存… 2025-1129 スムージングで法線が急激に変わる場合に不快な見た目になりがちなのでやめとく
	//diffuse *= 1-saturate(F);

}

void SpotLightMatrix(int idx, out float4x4 lightView, out float4x4 lightViewProj)
{
	//ライトのビュー行列
	float4x4 ILW = InverseRTMatrix(LightBone(idx));

	float LightTheta = acos(L_Cone[idx]);
	float LightCot = 1/tan(LightTheta);
	float nearZ = 1;
	float farZ = 1000;
	float4x4 LPM = {
		LightCot,0,0,0,
		0,LightCot,0,0,
		0,0,farZ/(farZ-nearZ),1,
		0,0,-farZ*nearZ/(farZ-nearZ),0
	};

	lightView = ILW;
	lightViewProj = mul(ILW, LPM);
}


float2 ShadowUV(int idx,  float3 P)
{
	float4x4 view,vp;
	SpotLightMatrix(idx,view,vp);
	return Project(P, vp);
}

float ShadowMapBias(float ds)
{
	/*
	#ifdef RAYTRACING
		return 0.5;
	#else
		return 0.1 + min(0.1, max(abs(ddx(ds)),abs(ddy(ds))));
	#endif
	*/
	return 0.1;
}

//ほぼゴッドレイ用
//idx番のシャドウマップを使って位置Pの可視率の計算をする
//jtはシャドウマップサンプル時のジッター
float Visibility(int idx, float3 P, float2 jt = 0)
{
	if (!L_Is[idx])
		return 1;
	if (L_Cone[idx]==0)
		return 1;

	float2 uv = ShadowUV(idx,P);
	if (any(uv!=saturate(uv)))
		return 1;

	uint W,H;
	ShadowMap0.GetDimensions(W,H);

	float smap = ShadowMap(idx,PointSamp, uv + jt/float2(W,H)).x;	//ライト→最寄りの物体間距離
	if (smap==0)
		return 1;
	float ds = abs(dot(P - LightBone(idx)._41_42_43, LightBone(idx)._31_32_33));	//注目点→ライト間距離
	float eps = ShadowMapBias(ds);
	if (smap+eps < ds)
		return 0;

	return 1;
}

float2 SampleShadow(int idx, float2 uv)
{
	uint W,H;
	ShadowMap0.GetDimensions(W,H);

	#define SHADOWMAP_VSM_FILTERSIZE 5
	float2 e = 1.0 / float2(W,H);
	const float N = SHADOWMAP_VSM_FILTERSIZE;
	const float bias = N/2-0.5;
	int count = 0;
	float2 moments = 0;

	for (int i=0; i<N*N; i++) {
		float u = i+0.5;
		float r = sqrt(u/N);
		float t = 2 * PI * u / ((1 + sqrt(5)) / 2);
		float2 sp = uv + e * r * CosSin(t);
		//シャドウマップの範囲チェック
		if (all(saturate(sp.xy) == sp.xy)) {
			float smap = ShadowMap(idx,ClampSamp, sp).r; //ライト→着目点の間で最も近い障害物までの距離
			if (smap > 0) {
				moments += float2(smap, smap * smap);
				count++;
			}
		}
	}

	if (count == 0)
		return 0;

	moments /= count;
	return moments;
}

float VisibilityVSM(int idx, float3 P)
{
	if (!L_Is[idx])
		return 1;

	float3 spoPos = LightBone(idx)._41_42_43;
	float ds = abs(dot(P-spoPos, LightBone(idx)._31_32_33)); //ライト→着目点のZ軸上の長さ
	float ofs = ShadowMapBias(ds); //シャドウアクネ対策オフセット

	float2 moments = SampleShadow(idx, ShadowUV(idx,P));
	//手前に何も割り込んでない or サンプルが取れてない場合は1を返す
	if (moments.x >= ds - ofs || moments.x == 0)
		return 1;
	
	float v = moments.y - moments.x * moments.x;
	float delta = (ds - ofs) - moments.x;
	float pmax = v / (v+ delta*delta);

	return saturate(pmax);
}

//ライトのコーンで切り取られる線分t0-t1を返す
//tmaxは探索範囲の上限
bool RayLightCone(int idx, float3 ro, float3 rd, float tmax, out float t0, out float t1)
{
	t0=0;
	t1=0;
	float4x4 LBone = LightBone(idx);
	float3 c = LBone._41_42_43;
	float3 d = LBone._31_32_33;
	float cosT = L_Cone[idx];
	return SegCone(ro,ro+rd*tmax, c,d,cosT, t0,t1);
}

#if defined(YRZ_PASS_MMD) || defined(YRZ_PASS_RTGI)
//環境光やらライトやら全部で照明
//vis : 平行光源の可視性(RTはレイトレで得る、ラスタライザではバッファから読むので)
void Illumination(uint2 pix, SubayaiTexture ptex, Material m, float3x3 TBN, float3 P, float3 V, out float3 diffuse, out float3 specular, out float3 envSpecular, float vis)
{
	specular = 0;
	diffuse = 0;
	envSpecular = 0;
	float3 dif=0, spec=0;

	//完全黒体の場合はemission分を返すだけ
	if (m.category == CAT_BLACKBODY) {
		specular = m.emission;
		return;
	}

	EnvLighting(pix, ptex, m, TBN, V, dif, spec);
	specular += spec;
	envSpecular = spec;	//環境光のspecular分はspecularGIの時に引かなければならないのでストアしとく
	diffuse += dif;
	
	//平行光源
	float3 Le = LinearLightColor;	//光源色
	float3 L = -LightDirection;

	Le *= lerp(1,vis,m.shadowVisibility);
	Shading(Le, L, ptex, m, TBN, P, V, dif, spec);
	specular += spec;
	diffuse += dif;
	
	//スポットライト
	for (int i=0; i<NUM_LIGHTS; i++) {
		float3 d,s;
		Lighting(i, ptex, m, TBN, P, V, d,s);
		diffuse += d;
		specular += s;
	}
}
#endif

#ifdef YRZ_PASS_MMD
//平行光源シャドウマップから可視性を得る
float ShadowRVisibility(float2 uv, float3 n)
{
	int N = 16;
	float2 d = float2(AspectRatio,1)*0.005/sqrt(N);
	float4 C = MyNormalDepth.SampleLevel(ClampSamp, uv, 0);

	//何も書いて無いピクセルなら帰る
	if (C.w == 0)
		return 1;

	float tw = 0, tv = 0;
	for (int i=0; i<N; i++) {
		float u = i+0.5;
		float r = sqrt(u);
		float t = 2 * PI * u / ((1 + sqrt(5)) / 2);
		float2 sp = uv + CosSin(t)*r*d;
		float4 nd = MyNormalDepth.SampleLevel(ClampSamp,sp,0);
		if (nd.w) {
			float v = ShadowMapR.SampleLevel(ClampSamp, sp, 0);
			float w = smoothstep(0.1,0.05,abs(nd.w-C.w)/C.w);
			tv += v*w;
			tw += w;
		}
	}
	
	//TODO 変な値が却ってミラーボールがめっちゃまぶしくなる事ある
	//この関数の返り値を0.5とかに固定するとなおる事を確認した
	return tw ? tv/tw : 1;
}
#endif