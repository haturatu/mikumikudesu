//プリミティブ番号からパッチ情報を得るなど、PMX用ユーティリティ

#ifndef PMXUTILS_HLSLI
#define PMXUTILS_HLSLI

#include "PMXDefs.hlsli"


//総材質番号→モデル番号、材質番号
void ToIDMat(int totalMat, out int id, out int iMat)
{
	id = Mat2Model[totalMat];
	iMat = totalMat - Model2Mat[id].x;
}

//総材質番号に変換
int ToTotalMat(int id, int iMat)
{
	return Model2Mat[id].x + iMat;
}


//UV座標だけ得る(stは重心座標:Attr.uv)
float2 GetUV(uint id, uint iFace, float2 st)
{
	float2 uv = 0;
	float3 bary = float3((1.0f - st.x - st.y), st.x, st.y);
    for (int i=0; i<3; i++) {
        uv += VB[id][IB[id][iFace*3+i]].uv * bary[i];
    }
	return uv;
}

//位置・法線・UVを得る。法線マップは考慮しない(stは重心座標:Attr.uv)
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

//ポリゴン自体の法線
float3 GeometryNormal(uint id, uint triangleIndex)
{
	float3 p[3];
	for (int i=0; i<3; i++) {
		p[i] = VB[id][IB[id][triangleIndex*3+i]].position;
	}
	return normalize(cross(p[0]-p[1],p[0]-p[2]));
}

//MMDマテリアルにPBRマテリアル構造体の情報を加えてレンダリング用マテリアルを作る
Material MMDtoPBR(MMDMaterial mmd, PBRValue pbr)
{
	Material m = (Material)0;

	m.category = pbr.Category;
	m.cat = pbr.Cat;
	m.IOR = pbr.IOR;
	m.roughness = pbr.Roughness;
	m.anisotropy = pbr.Anisotropy;
	m.ggxA = GGXAlpha(m.roughness, clamp(m.anisotropy,-0.99,0.99));	//anisotropic == ±の時に片側のαが0になってしまうのを防ぐ
	m.lightCosFalloff = saturate(cos(radians(pbr.LightFalloff)));
	m.lightCosHotspot = saturate(cos(radians(pbr.LightHotspot)));
	m.autoNormal = pbr.AutoNormal;

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

	m.alpha = saturate(mmd.diffuse.w) * saturate(pbr.Alpha);
	m.texture = mmd.tex;
	m.twosided = mmd.drawFlag & 1;

	m.emission = max(0,pbr.Light) * m.albedo;
	m.restrictedLight = pbr.RestrictedLight;

	//AL対応材質加算
	if (SqrLen(mmd.specular)<1e-4 && mmd.shininess>100) {
		m.emission += (mmd.shininess-100) * m.albedo * (C_AL ? Exerp(0.01,100,C_AL): 0);
	}


	return m;
}

//ColorConstを使って色変え
void ColorModulation(inout Material m, PBRValue pbr, float4x4 exuv)
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

//モデル番号と材質番号に割り当てられたマテリアルを得る
//場所ごとの差異は気にせず、テクスチャには全部白が割り当てられており、スフィアマップは無い状態として扱われる
Material GetMaterial(uint id, uint iMat)
{
	MMDMaterial mmd = MMDMaterials[id][iMat];
	PBRValue pbr = GetPBRValue(id,iMat);
	Material m = MMDtoPBR(mmd,pbr);
	ColorModulation(m,pbr,0);
	return m;
}


//マテリアルパラメータのUV座標を計算
float2 CalcUV(int idx, float2 uv, float4x4 exuv, float loops)
{
	if (idx == 0 || idx>=5)
		return uv * loops;
	
	return exuv[idx-1].xy * loops;
}

#define GET_TEXTURE(PARAM) ptex.PARAM##Map.SampleLevel(samp, CalcUV(pbr.PARAM##UV, texcoord, exuv, pbr.TextureLoops * pbr.PARAM##Loops), 0)


//マテリアルをテクスチャ座標UVから得る
Material GetMaterial(uint id, uint triangleIndex, float2 texcoord, float3 N, float4x4 exuv)
{
	int iMat = Faces[id][triangleIndex];
	MMDMaterial mmd = MMDMaterials[id][iMat];
	//Material m = materials[id][faces[id][triangleIndex].iMaterial];

	PBRValue pbr = GetPBRValue(id,iMat);
	PBRTexture ptex = GetPBRTexture(id,iMat);
	Material m = MMDtoPBR(mmd,pbr);

	//MMDテクスチャ
	if (mmd.tex >= 0) {
		int iTex = TextureTable[id];
		float4 tex = Textures[iTex + mmd.tex].SampleLevel(samp, texcoord,0,0);
		tex.rgb = lerp(1, tex.rgb * mmd.textureMulValue.rgb + mmd.textureAddValue.rgb, mmd.textureMulValue.a + mmd.textureAddValue.a);
		tex.rgb = ToLinear(tex.rgb);
		m.albedo *= tex.rgb;
		m.emission *= tex.rgb;
		m.alpha *= tex.a;
	}

	//スフィアマップ
	if (mmd.spTex >= 0 && pbr.EnableSphereMap && (mmd.spmode == 1 || mmd.spmode == 2)) {
		//ほんとはViewMatrixではなく接wi空間かもしれないが、プライマリヒットの事だけ考えてViewMatrixで代用してしまう
		//描画上の破綻は生じるかもしれないが少なくとも多方向から見た時の結果に矛盾は生じない
		float2 uv = mul(N,(float3x2)ViewMatrix) * float2(0.5,-0.5) + 0.5;
		float4 tex = Textures[TextureTable[id]+mmd.spTex].SampleLevel(samp, uv,0);
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
		m.alpha *= tex.a;
	}

	if (ptex.hasRoughnessMap) {
		m.roughness *= lerp(1, GET_TEXTURE(Roughness).r, pbr.RoughnessScale.x);
		m.roughness = saturate(m.roughness);
	}
	if (ptex.hasAlbedoMap) {
		float3 tex = GET_TEXTURE(Albedo).rgb;
		tex = ToLinear(tex);
		m.albedo = lerp(m.albedo, tex, pbr.AlbedoScale);
	}
	if (ptex.hasAlphaMap) {
		float tex = GET_TEXTURE(Alpha).r;
		m.alpha *= lerp(1,tex,pbr.AlphaScale);
	}

	m.ggxA = GGXAlpha(m.roughness, clamp(m.anisotropy,-0.99,0.99));
	ColorModulation(m,pbr,exuv);
	return m;
}


//マテリアルを重心座標から得る
Material GetMaterialBary(uint id, uint triangleIndex, Attribute attr)
{
	//float2 uv = GetUV(id,triangleIndex,attr.uv);
	Vertex v = GetVertex(id, triangleIndex, attr.st);
	float4x4 exuv = {v.exuv[0], v.exuv[1], v.exuv[2], v.exuv[3]};

	return GetMaterial(id, triangleIndex, v.uv, v.normal, exuv);
}

//albedoマップの輝度勾配から法線マップを作る
float3 AutoNormal(Texture2D<float4>texture, float2 uv, float scale = 0.25)
{
	float W,H;
	texture.GetDimensions(W,H);
    //本当はフットプリントにeを合わせたい
	//テクスチャの解像度からeを計算するのは良いアイディアのようだけど大体あんまりうまくいかない
    float3 e = {1.0/256,1.0/256,0};
    float xp = texture.SampleLevel(samp, uv + e.xz, 0).g;
    float xn = texture.SampleLevel(samp, uv - e.xz, 0).g;
    float yp = texture.SampleLevel(samp, uv + e.zy, 0).g;
    float yn = texture.SampleLevel(samp, uv - e.zy, 0).g;
	float2 d = float2(xn-xp,yn-yp)*scale;
    float3 N = normalize(float3(d,1));
    return N;
}

//三角形についての情報全部ゲット
//法線マップが要らない場合はsimple = trueにしよう
void GetPatch(uint id, uint iFace, float3 wi, float2 st, out Material m, out float3x3 TBN, uniform bool simple = false)
{
    //紛らわしいので重心座標の方はst, テクスチャ座標の方はuvとす
	float3 bary = float3((1.0f - st.x - st.y), st.x, st.y);

    Vertex v[3];
    float3 n = 0, t=0, b=0;
    float2 uv = 0;
	float4x4 exuv = 0;
    for (int i=0; i<3; i++) {
        v[i] = VB[id][IB[id][iFace*3+i]];
        n += v[i].normal * bary[i];
		t += v[i].tangent * bary[i];
        uv += v[i].uv * bary[i];
		for (int j=0; j<4; j++)
			exuv[j] += v[i].exuv[j] * bary[i];
    }
    n = normalize(n);
	t = normalize(t);
	m = GetMaterial(id, iFace, uv, n, exuv);

	TBN = ComputeTBN(n,t,true);

	[branch]
	if (simple) {
		//ノーマルマップなし版ならこれで終わり
		return;
	}

	bool valid;
	float3x3 TBN_tex;	//テクスチャ空間→ワールド変換行列
	valid = ComputeTBN_UV(n, v[0].position, v[1].position, v[2].position, v[0].uv, v[1].uv, v[2].uv, TBN_tex);

    //法線マップが必要な場合はここで実行してnを変化させるべし
	int iMat = Faces[id][iFace];
	PBRValue pbr = GetPBRValue(id,iMat);
	PBRTexture ptex = GetPBRTexture(id,iMat);
	if (valid) {
		float2 tuv = uv * pbr.TextureLoops;
		if ( (m.autoNormal && m.texture>=0) || (ptex.hasNormalMap)) {
			int iTex = TextureTable[id];
			float3 nmap;

			if (ptex.hasNormalMap) {
				float2 texcoord = uv;
				nmap = GET_TEXTURE(Normal).rgb*2-1; //ptex.NormalMap.SampleLevel(samp,tuv*pbr.NormalLoops,0).rgb * 2 - 1;
				nmap = normalize(nmap * float3(pbr.NormalScale.xx,1));
			} else {
				nmap = AutoNormal(Textures[iTex + m.texture], uv, m.autoNormal);
			}

			float3 n_after = normalize(mul(nmap,TBN_tex));
			float3 t_after = Rotate(TBN[0],FromToRotation(TBN[2],n_after));
			TBN = ComputeTBN(n_after, t_after, false);
		}
	}
}



/**********************************************************************************
    光源サンプリング用のシャドウレイ
***********************************************************************************/
[shader("closesthit")]
void ClosestHitShadow(inout PayloadShadow payload, Attribute attr)
{
	//特に何もしない
}

[shader("anyhit")]
void AnyHitShadow(inout PayloadShadow payload, Attribute attr)
{
	/*
	if (InstanceID() == ID_FOG) {
		IgnoreHit();
		return;
	}
	*/

	//8/19 修正。GetMaterialに重心座標のまま渡されててm.alphaが正しく評価できてなかったのを修正
	Material m = GetMaterialBary(InstanceID(), PrimitiveIndex(), attr);

	if (m.category != CAT_GLASS) {
		//半透明は確率的に透過。ただしα=0.98,0.99はフラグとして使われている事があるらしいので不透過とす
		if (m.alpha < RNG.x && m.alpha < 0.98) {
			IgnoreHit();
			return;
        }

		//ライトから見て裏向き面かつ片面描画材質ならミス
		//光源側から見えたら「見える」のか、パッチ側から見えたら「見える」のか？
		//BRDFサンプリングと揃えるため、「パッチ側からライトが見えるなら照明される」という扱いにする
		//WorldRayDirectionはパッチ→光源の方向なので、faceが正の時はパッチからライトは隠され、負の時はパッチからライトが見える
		float face = -dot(normalize(WorldRayDirection()), GeometryNormal(InstanceID(), PrimitiveIndex()));
		if (face < 0 && !m.twosided) {
			IgnoreHit();
			return;
		}
		//ぶつかった
		payload.Tr = 0;
	} else {
		//ガラスはライトからは常に不透明扱い
		payload.Tr = 0;
	}
}

[shader("miss")]
void MissShadow(inout PayloadShadow payload)
{
	//特に何もしない
}

//ro(パッチの位置)からp(ライトの位置)の方向にレイを飛ばして見えるか返す
float3 Visibility(float3 ro, float3 p, float3 lightN, bool twosided, float eps = 1e-3)
{
	//ライトポリゴンが片面材質の場合はライト法線の方から見えるのかチェック
	if (dot(normalize(ro-p), lightN) < 0 && !twosided)
		return 0;

	float d = length(p-ro);
	float e = max(eps, d*1e-3);

	//ライトポリゴンが遮られていないかテスト
	RayDesc shadowDesc;
	shadowDesc.Origin = ro;
	shadowDesc.Direction = normalize(p-ro);
	shadowDesc.TMin = e;
	shadowDesc.TMax = d-eps;
	PayloadShadow payshadow;
	payshadow.xi = iSample;
	payshadow.Tr = 1; 
	TraceRay(TLAS, RAY_FLAG_NONE, 0xFF, 2,0,2, shadowDesc, payshadow);

	if (any(payshadow.Tr!=1))
		return payshadow.Tr;

	//ライトリーク防止のため近場からもう一回撃つ
	if (e > eps) {
		shadowDesc.TMin = eps;
		shadowDesc.TMax = min(d-eps,e+eps);
		payshadow.xi = iSample+1024;
		TraceRay(TLAS, RAY_FLAG_NONE, 0xFF, 2,0,2, shadowDesc, payshadow);
	}

	return payshadow.Tr;
}

//テスト用
//パッチの位置からskydirの方向に遮蔽があれば0、無ければ1を返す
float VisibilitySky(float3 ro, float3 skydir)
{
	RayDesc shadowDesc;
	shadowDesc.Origin = ro;
	shadowDesc.Direction = skydir;
	shadowDesc.TMin = 1e-3;
	shadowDesc.TMax = 1e+3;
	PayloadShadow payshadow;
	payshadow.xi = iSample;
	payshadow.Tr = 1; 
	TraceRay(TLAS, RAY_FLAG_NONE, 0xFF, 2,0,2, shadowDesc, payshadow);
	return payshadow.Tr;
}

#endif