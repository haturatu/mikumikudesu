#include "PMXUtils.hlsli"

//Skyboxの最大輝度。書き換える時はskyboxPDF.hlslの設定も書き換えるべし
static float MAX_SKYBOX_LUMINANCE = 1e+6;

//発光体光源が一つでもある？
bool IsLightFace()
{
	return (MatLightCounter[0] != 0);
}

//モデルidの面iFaceが光源ポリゴン群の中から選択される確率
float LightFacePdf(int id, int iFace)
{
	int localMat = Faces[id][iFace];
	int iMat = Model2Mat[id].x + localMat;	//総材質番号
	int iLight = Mat2Light[iMat];
	if (iLight < 0) {
		return 0;
	}

	//return MatLights[iLight].y * FacePDFs[id][iFace].pdf;
	return MatLights[iLight].y * FaceWalker[id][iFace].pdf;
}

//光源材質をサンプリングし、更に材質内のポリゴンを1つサンプリングする
void SampleLightIndex(float4 xi, out int iModel, out int iFace)
{
	iModel = 0;
	int iMat = 0;
	iFace = 0;

	if (!IsLightFace()) {
		return;
	}

	uint nMats,nModels,dmy;
	MatLights.GetDimensions(nMats, dmy);	//総材質数
	Model2Mat.GetDimensions(nModels,dmy);	//モデル数

    //CDFがXi以上になる最初のインデックスを探す
	//※pdf=0のエントリがあって同値が並ぶ場合は本当に最初のエントリになるのか分からないのでpdf=0になるものはMatLightsの中に入れてはならない
    int l = 0;
    int r = MatLightCounter[0]-1;
    while (l < r) {
        int mid = (l + r) / 2;
        if (MatLights[mid].z <= xi.x) {
            l = mid + 1;	
        } else {
            r = mid;
        }
    }
    int tMat = round(MatLights[l].w);	//総材質の中での材質番号
	//モデルとモデル内材質番号にする
	ToIDMat(tMat, iModel, iMat);

	//材質の中で面を探す
	/*
	l = Mat2face[iModel][iMat].x;
	r = l + Mat2face[iModel][iMat].y - 1;
    while (l < r) {
        int mid = (l + r) / 2;
        if (FacePDFs[iModel][mid].cdf <= xi.y) {
            l = mid + 1;
        } else {
            r = mid;
        }
    }
	//iFaceは材質内の面番号ではなく、モデル内の面番号
	iFace = l;
	*/
	MaterialFace mf = Mat2face[iModel][iMat];
	iFace = SampleWalkersAlias(FaceWalker[iModel], mf.start, mf.count, xi.zw) + mf.start;
}

//iLight番の光源ポリゴンの面積だけ計算する(スキニングによって変化している可能性があるので、リアルタイムの値を得たい時用)
float FaceArea(int id, int iFace)
{
	float3 P[3];

	for (int i=0; i<3; i++) {
		Vertex v = VB[id][IB[id][iFace*3+i]];
		P[i] = v.position;
	}
	return length(cross(P[0]-P[1], P[0]-P[2]))/2;
}

/*
int SampleLightIndex(float xi)
{
    //CDFがXi以上になる最初のインデックスを探す
    int l = 0;
    int r = nLights-1;
    while (l < r) {
        int mid = (l + r) / 2;
        if (lights[mid].cdf <= xi) {
            l = mid + 1;
        } else {
            r = mid;
        }
    }
    return l;
}

//iLight番の光源ポリゴンの面積だけ計算する
float LightArea(int iLight)
{
	int iFace = lights[iLight].iFace;
	int id = lights[iLight].ID;
	float3 P[3];

	for (int i=0; i<3; i++) {
		Vertex v = vertices[id][indices[id][iFace*3+i]];
		P[i] = v.position;
	}
	return length(cross(P[0]-P[1], P[0]-P[2]))/2;
}

//iLightのどこかからサンプリングする。帰り値はライトポリゴンの座標でpdfの単位は1/[MMD^2]
//lightNはライトポリゴンの法線
float3 SampleLightTriangle(int iLight, float2 uv, out float area, out float3 lightN, out Material m)
{
	int id = lights[iLight].ID;

	//重心座標uvの折り返し(u+vが1以上になってたら三角の外にはみ出るので)
	if (uv.x+uv.y >= 1)
		uv = 1-uv;

	int iFace = lights[iLight].iFace;
	float3 P[3];
	float3 N = 0;
	float3 L = 0;
	float2 texcoord = 0;	//テクスチャ座標(重心座標と混同しないように)

	float3 bary = {1-uv.x-uv.y, uv.x, uv.y};
	for (int i=0; i<3; i++) {
		Vertex v = vertices[id][indices[id][iFace*3+i]];
		P[i] = v.position;
		L += v.position * bary[i];
		N += v.normal * bary[i];
		texcoord += v.uv * bary[i];
	}
	N = normalize(N);
	m = GetMaterial(id, iFace, texcoord);

	area = length(cross(P[0]-P[1], P[0]-P[2]))/2;
	lightN = N;

	m.emission *= (m.category==CAT_GLASS ? 1 : m.alpha);
	//Le = m.emission * (m.category==CAT_GLASS ? 1 : m.alpha);	//ライト自体が半透明ポリゴンである場合、αを乗算する
	//twosided = m.twosided;
	return L;
}
*/

//iLightのどこかからサンプリングする。帰り値はライトポリゴンの座標でpdfの単位は1/[MMD^2]
//lightNはライトポリゴンの法線
float3 SampleLightTriangle(int id, int iFace, float2 uv, out float area, out float3 lightN, out Material m)
{
	//重心座標uvの折り返し(u+vが1以上になってたら三角の外にはみ出るので)
	if (uv.x+uv.y >= 1)
		uv = 1-uv;

	float3 P[3];
	float3 N = 0;
	float3 L = 0;
	float2 texcoord = 0;	//テクスチャ座標(重心座標と混同しないように)
	float4x4 exuv = 0;

	float3 bary = {1-uv.x-uv.y, uv.x, uv.y};
	for (int i=0; i<3; i++) {
		Vertex v = VB[id][IB[id][iFace*3+i]];
		P[i] = v.position;
		L += v.position * bary[i];
		N += v.normal * bary[i];
		texcoord += v.uv * bary[i];
		for (int j=0; j<4; j++)
			exuv[j] += v.exuv[j] * bary[i];
	}
	N = normalize(N);
	m = GetMaterial(id, iFace, texcoord, N, exuv);

	area = length(cross(P[0]-P[1], P[0]-P[2]))/2;
	lightN = N;

	m.emission *= (m.category==CAT_GLASS ? 1 : m.alpha);
	//Le = m.emission * (m.category==CAT_GLASS ? 1 : m.alpha);	//ライト自体が半透明ポリゴンである場合、αを乗算する
	//twosided = m.twosided;
	return L;
}

//光源/skyboxサンプリング後にLからV方向に反射される輝度を計算する
//etaV : 視点側の媒質の屈折率。光源側の媒質の屈折率は常に1とする(ガラスや水中に光源を置くと本来の値からズレる)
//SSS : 表面下散乱分の計算をさせるフラグ
float2x3 Lighting(int waveCh, Material m, float3 L, float3 V, float3x3 TBN, float3 Le, float etaV = 1, uniform bool SSS = false)
{
	float2x3 Lt = 0;

	if (all(Le == 0))
		return Lt;
	
	float2 a = m.ggxA;
	float NoL = dot(TBN[2],L);
	float mior = m.IOR.y ? Cauchy(m.IOR, WaveChannelToLambda(waveCh)) : m.IOR.x;	//分散を考慮したIOR

	[branch]
	if (SSS) {
		//表面下散乱分の計算
		float3 N = TBN[2];
		//フレネル透過率。マイクロ法線については平均化されていると見なして計算する
		float f0 = IORtoF0(mior/etaV);
		float Ft = 1 - lerp(f0, 1, FresnelSchlick(abs(dot(L,N))));	//光源側
		Ft *= 1 - lerp(f0, 1, FresnelSchlick(abs(dot(N,V))));		//視線側
		Lt[0] = Ft * m.albedo / PI * Le * saturate(NoL);
		Lt[1] = 0;
		return Lt;
	}

	if (m.category == CAT_GLASS) {
		//ガラスの場合はNoLがマイナスになった場合、BTDFで評価
		float3 specular;
		float3 H = normalize(L+V);
		float u = FresnelSchlick(saturate(dot(L,H)));
		float F = lerp(IORtoF0(mior/etaV), 1, u);
		if (NoL >= 0) {
			specular = F * GGXAnisoBRDF(a, L, V, TBN) * GGXMultiScatteringTerm(a, 1, V, H, TBN) * NoL;
		} else {
			specular = (1-F) * GGXAnisoBTDF(a, V,L, TBN, etaV, 1) * abs(NoL);
		}

		Lt[0] = 0;
		Lt[1] = Le * specular;
	} else {
		NoL = saturate(NoL);
		if (NoL == 0)
			return 0;

		float3 H = normalize(L+V);
		float u = FresnelSchlick(saturate(dot(L,H)));
		float3 F0_ms = m.category==CAT_METAL ? m.albedo : 1;
		float3 F = m.category==CAT_METAL ?  lerp(m.albedo, 1, u) : lerp(IORtoF0(mior/etaV), 1, u);
		float3 kD = m.category==CAT_METAL ? 0 : 1-F;

		float kFire = GGXAnisoBRDF(a, L, V, TBN);	//スペキュラのギラつき度
		float3 specular = F * kFire * GGXMultiScatteringTerm(a, F0_ms, V, H, TBN) * NoL;
		float3 diffuse = kD * m.albedo / PI * NoL;

		Lt[0] = Le * diffuse;
		Lt[1] = Le * specular;
	}

	return Lt;
}

//vをSHの係数が書かれている方向(Z-up右手系)に合うように回転する
float3 SkyRotate(float3 v)
{
	v = RotY(v,2*PI*(0.25-SkyboxPhi));
	v.x = -v.x;
	return v.xzy;
}

//Y-up左手系で書かれた方向から右手系用skyboxテクスチャのuvを返す
float2 LongRat(float3 v)
{
	return float2(atan2(v.z, v.x), acos(v.y)) / float2(-2 * PI, PI) - float2(0.25 + SkyboxPhi, 0);
}

//Cの明度をtまでに制限する
float3 ClampLum(float3 C, float t)
{
	float L = RGBtoY(C);
	return (L<=t) ? C : C/L*t;
}

float3 FetchSkybox(float3 rd)
{
	float3 t = Skybox.SampleLevel(samp, LongRat(rd), 0, 0).xyz;
	//t = min(t,1e+5);	//あんまり高い値が来るとオーバーフローするっぽいので
	//return InverseACESTonemap(pow(t, GAMMA));
	return ClampLum(t,MAX_SKYBOX_LUMINANCE);
}


#if 1
//skyboxをサンプリングする
//入力  P:パッチの位置, visTest:可視判定の是非
//返り値 光源ベクトル
//出力 pdfと遮蔽済み輝度Le
float3 SampleSky(inout Payload payload, float3 P, out float pdf, out float3 Le, bool visTest = true)
{
	uint W,H;
	Skybox.GetDimensions(W,H);

	float4 xi = RNG;
	uint row = SampleWalkersAlias(SkywalkerRow, 0, H, xi.xy);
	uint col = SampleWalkersAlias(Skywalker, row*W, W, xi.zw);
	//skywalkerに入っているpdfはあくまで「そのセルが選択される確率」なので
	// 単位を [1/セル] → [1/立体角] に変換する
	pdf = SkywalkerRow[row].pdf * Skywalker[row*W + col].pdf / (4*PI) * W * H;

	//Le = Skybox[uint2(col,row)];
	//Le = ClampLum(Le, MAX_SKYBOX_LUMINANCE);
	float2 uv = (float2(col,row)+RNG.xy) / float2(W,H);
	Le = ClampLum(Skybox.SampleLevel(samp, uv, 0).rgb, MAX_SKYBOX_LUMINANCE);

	//col,rowに対応するLを求める
	float t = PI * uv.y;
	float p = -2 * PI * (uv.x + 0.25 + SkyboxPhi);
	float st,ct,sp,cp;
	sincos(t,st,ct);
	sincos(p,sp,cp);
	float3 L = float3(st*cp, ct, st*sp);

	if (pdf == 0)
		Le = 0;
	else {
		[branch]
		if (visTest)
			Le *= Visibility(P, P+L*1e+5, -L, true);
	}
	return L;
}

//L方向をサンプリングするpdf
float SkyPDF(float3 L)
{
    float2 uv = LongRat(L);
	uint W,H;
	Skybox.GetDimensions(W,H);
    uint col = uv.x*W;
	uint row = uv.y*H;
	return SkywalkerRow[row].pdf * Skywalker[row*W + col].pdf * W * H / (4*PI);
}


#else //skywalker前のモノ
float3 SampleSky(inout Payload payload, float3 P, out float pdf, out float3 Le, bool visTest = true)
{
	float2 Xi = RNG.xy;
	uint W,H;
	SkyboxPDF.GetDimensions(W,H);

    //CDFがXi以上になる最初のインデックスを探す
    int l = 0;
    int r = H-1;
    while (l < r) {
        int mid = (l + r) / 2;
        if (SkyboxCDFRow[uint2(0,mid)] <= Xi.y) {
            l = mid + 1;
        } else {
            r = mid;
        }
    }
    int row = l;
	float fy = InvLerp(l==0 ? 0 : SkyboxCDFRow[uint2(0,l-1)],SkyboxCDFRow[uint2(0,l)],Xi.y);

    l = 0;
    r = W-1;
    while (l < r) {
        int mid = (l + r) / 2;
        if (SkyboxCDF[uint2(mid,row)] <= Xi.x) {
            l = mid + 1;
        } else {
            r = mid;
        }
    }
	int col = l;
	float fx = InvLerp(l==0 ? 0: SkyboxCDF[uint2(l-1,row)],SkyboxCDF[uint2(l,row)],Xi.x);

	float2 uv = float2(col+fx,row+fy) / float2(W,H);
	//float2 uv = float2(col,row) / float2(W,H);

	pdf = SkyboxPDF[uint2(col,row)] * SkyboxPDFRow[uint2(0,row)] / (4*PI);	//TODOこれでいい?
	/*
	pdf = SkyboxPDF.SampleLevel(samp, float2(uv.x,(float)row/H) *
		SkyboxPDFRow.SampleLevel(samp, float2((float)col/W,uv.y), 0), 0) / (4*PI);	//TODOこれでいい?
	*/

	//Le = skybox[uint2(col,row)];
	Le = ClampLum(Skybox.SampleLevel(samp, uv, 0).rgb, MAX_SKYBOX_LUMINANCE);

	//col,rowに対応するLを求める
	float t = PI * uv.y;
	float p = -2 * PI * (uv.x + 0.25 + SkyboxPhi);
	float st,ct,sp,cp;
	sincos(t,st,ct);
	sincos(p,sp,cp);
	float3 L = float3(st*cp, ct, st*sp);

	if (pdf == 0)
		Le = 0;
	else {
		[branch]
		if (visTest)
			Le *= Visibility(P, P+L*1e+5, -L, true);
			//Le *= VisibilitySky(P, L);
	}

	return L;
}

//L方向をサンプリングするpdf
float SkyPDF(float3 L)
{
    float2 uv = LongRat(L);
	uint W,H;
	SkyboxPDF.GetDimensions(W,H);
    uint2 pix = {uv.x*W, uv.y*H};
    return SkyboxPDF[pix] * SkyboxPDFRow[uint2(0,pix.y)] / (4*PI);
}
#endif


bool UpdateReservoir(float xi, inout Reservoir r, float3 Le, float3 LPos, float3 yN, bool twosided, float3 phat, float pdf, float w, int c = 1)
{
	if (w == 0)
		return false;

    r.wsum += w;
    r.M += c;
 
    if ( xi < w / r.wsum  ) {
		r.yPhat = phat;
        r.yLe = Le;
		r.yPos = LPos;
		r.yN = yN;
		r.yTwosided = twosided;
		r.yPDF = pdf;
        return true;
    }
 
    return false;
}

//光源ポリゴンの配光特性
//m : 光源ポリゴンのマテリアル
//L : 光源→受光面ベクトル(シェーディング時とは逆なので注意)
//N : 光源ポリゴンの法線
float DirectLe1(Material m, float3 L, float3 N)
{
	float NoL = dot(N,L);

	//片面ライトを裏から見ている場合
	if (NoL<0 && !m.twosided)
		return 0;

	float cosT = abs(NoL);
	float ctF = m.lightCosFalloff;

	//コーンの外
	if (cosT<ctF)
		return 0;

	float ctH = max(m.lightCosHotspot,ctF);	//θH < θF → cosH > cosFを必ず満たすようにする
	float delta = (ctH == ctF) ? 1 : saturate((cosT-ctF)/(ctH-ctF));
	float Le1 = sqr(delta) * sqr(delta);

	return Le1;
}

//位置Pにおける光源サンプリング
//payload.xi のみ更新される
//入力  P:パッチの位置
//		visCheck : 遮蔽の計算もする
//返り値 光源ベクトル
//出力 pdfと遮蔽済み輝度Le
float3 SampleLight(inout Payload payload, float3 P, out float pdf, out float3 Le, out float3 lightP, uniform bool visCheck = true)
{
	pdf = 0;

	if (!IsLightFace())
		return 0;

	//ライトのサンプリング
	float3 lightN;


	int idL, ifL;
	SampleLightIndex(RNG,idL,ifL);

	//ライトポリゴンのサンプリング
	float area;	//ポリゴンの面積[MMD^2]
	Material m;
	lightP = SampleLightTriangle(idL,ifL, RNG.xy, area, lightN, m);	//ライトの位置
	Le = m.emission;

	float3 dl = lightP - P;
	float3 L = normalize(dl);

	//配光特性
	Le *= DirectLe1(m, L, lightN);

	pdf = dot(dl,dl) / max(area * abs(dot(lightN, L)),1e-10) * LightFacePdf(idL,ifL);	//単位を[MMD^2]から[1/sr]にする

	//可視性のチェック
	[branch]
	if (visCheck)
		Le *= Visibility(P, lightP, lightN, m.twosided);
	
	return normalize(lightP - P);
}


//発光ポリゴンかskyboxをサンプリングする
float3 SampleDirectLight(uniform int lightKind, inout Payload payload, int waveCh, Material m, float3 P, float3x3 TBN, float3 V, float IOR, uniform bool SSS, out float pdf, out float3 Le, out float3 Lt, out float3 lightPos)
{
	float3 L;

	[branch]
	if (lightKind == LIGHT_EMISSIVE) {
		L = SampleLight(payload, P, pdf,Le, lightPos);
		float2x3 lt_sep = Lighting(waveCh, m, L, V, TBN, Le, IOR, SSS);
		Lt = lt_sep[0] + lt_sep[1];
	} else {
		L = SampleSky(payload, P, pdf,Le);
		lightPos = P + L * 1e+5;	//てきとう
		float2x3 lt_sep = Lighting(waveCh, m, L, V, TBN, Le, IOR, SSS);
		Lt = lt_sep[0] + lt_sep[1];
	}

	return L;
}

float3 DirectLight(
	uniform int lightKind, inout Payload payload, int waveCh,
	Material m, BRDFOut B, float3 P, float3x3 TBN, float3 V, float IOR,
	out float pdf, out float3 lightPos, float3 Pssin, float3x3 TBNssin)
{
	float3 Lt;
	float3 Le = 0;
	float3 L;

	if (B.SSS) {
		float2x3 lt_sep = 0;
		
		//SSSの場合、鏡面反射はPssin, 拡散反射はPで評価する。どっちを評価するのかは確率的に決める
		if (RNG.x < 0.5) {
			//SSS、鏡面反射で評価
			L = SampleDirectLight(lightKind, payload, waveCh, m, Pssin, TBNssin, V, IOR, false, pdf, Le, Lt, lightPos);
			lt_sep = Lighting(waveCh, m, L, V, TBNssin, Le, IOR, false);
			Lt = lt_sep[1]*2;
		} else {
			//SSS、拡散反射で評価
			Material mss = m;
			mss.albedo = B.betaSSS;	//元々のalbedoから、P→POutへの透過分を考えた値に置き換える
			L = SampleDirectLight(lightKind, payload, waveCh, m, B.POut, TBN, V, IOR, true, pdf, Le, Lt, lightPos);
			lt_sep = Lighting(waveCh, mss, L, V, TBN, Le, IOR, true);
			Lt = lt_sep[0]*2;
		}

		pdf /= 2;
	} else {
		L = SampleDirectLight(lightKind, payload, waveCh, m, P, TBN, V, IOR, false, pdf, Le, Lt, lightPos);
	}
	return Lt;
}

