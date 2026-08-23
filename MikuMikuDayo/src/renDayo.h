//レンダラーダヨー

#pragma once
#include "defsDayo.h"

/*****************************************************************************
レンダラ関連
*****************************************************************************/

inline Vector3 ComputeTangent(const Vector3& n)
{
	Vector3 Z = abs(n.y) < 0.999999 ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
	Vector3 X = n.Cross(Z);
	Vector3 Y = n.Cross(X);
	Y.Normalize();
	X = n.Cross(Y);
	X.Normalize();
	return X;
}

inline Vector3 NormalizeNormal(const Vector3& n)
{
	Vector3 N = n;
	float lensq = N.LengthSquared();
	if (lensq < 0.99) {
		if (lensq == 0) {
			N = { 0,0,-1 };	//適当に代入
		} else {
			N /= sqrtf(lensq);
		}
	}
	return N;
}

//oをviewProj行列によって変換し、画面上の位置に変換する
//W,Hはビューポートの幅・高さ・leftTopはビューポートのオフセット
auto ProjectPoint (ImVec2& projected, const vec3& o, const Matrix& viewProj, int W, int H, const ImVec2& leftTop)
{
	projected = {};
	vec4 p = vec4::Transform(xyzw(o, 1), viewProj);
	if (p.w <= 1e-3) {
		return true;
	} else {
		projected = ImVec2(p.x, p.y) / p.w;
		projected = (projected + ImVec2(1, 1)) / 2;
		projected.y = 1 - projected.y;
		projected *= ImVec2(W, H);
		projected += leftTop;
		return false;
	}
	};



//ボーンのマーカーを描く
//X,Y,W,Hはレンダーイメージの左上座標とサイズ
//floatingにマウスカーソルの下にあるボーン番号
void DrawMarkers(KeyFrameWindowContext* kc, const Matrix& viewproj,int X, int Y, int W, int H, std::vector<int>& floating)
{
	auto& io = ImGui::GetIO();

	struct marker {
		ImVec2 p = {};			//スクリーンに投影された中心点の位置
		ImVec2 e = {};			//骨の先端
		bool back = true;		//カメラより後ろに投影されて見えないならtrue
		bool b_back = true;		//骨の端点のうち片方でもカメラより後ろ
		bool visible = true;	//操作不能・不可視属性などで見えないならfalse
	};

	auto solver = dynamic_cast<PMX::PoseSolver*>(kc->solver);
	auto dl = ImGui::GetWindowDrawList();

	//スクリーンに投影された中心点の位置
	std::vector<marker>markers(solver->bones.size());
	std::unordered_map<int, bool>selected;

	//各ボーンが選択状態かどうか
	for (auto&& node : kc->nodes)
		for (auto&& leaves : node.leaves)
			if (leaves.source.index() == 0 && leaves.selected)
				selected[leaves.index] = true;

	//レンダラー表示領域の左上隅
	ImVec2 lt = ImGui::GetWindowPos() + ImVec2(X,Y);
	ImVec2 rb = lt + ImVec2(W,H);
	ImVec2 mp = io.MousePos;


	//マーカーの中心位置のスクリーン座標の計算
	for (int i = 0; const auto & pmxb : solver->pmx->bones) {
		auto& m = markers[i];
		if (!pmxb.IsVisible() || !pmxb.IsControllable() || (pmxb.IsPhysics && kc->pose.boneKeys[i].physics) || !kc->registeredBones.contains(i)) {
			m.visible = false;
			i++;
			continue;
		}

		Matrix mat = Matrix(solver->bones[i]->transform) * viewproj;


		m.back = ProjectPoint(m.p, solver->bones[i]->origin, mat, W,H, lt);
		m.b_back = m.back;

		//ボーンの表示先が相対の場合を解決, m.backがtrueの場合は表示されないので計算しない
		if (pmxb.toBone == -1 && !m.back)
			m.b_back = m.back || ProjectPoint(m.e, solver->bones[i]->origin + pmxb.toOffset, mat, W,H,lt);

		i++;
	}

	//マーカーと骨表示
	for (int i = 0;  auto && m : markers) {
		
		if (!m.visible || m.back) {
			i++;
			continue;
		}

		const auto& pmxb = solver->pmx->bones[i];
		constexpr float rad_out = 8;
		constexpr float rad_in = 4;
		constexpr float rad2_click = (rad_out * rad_out);	//クリックしたら命中する半径(の2乗)
		constexpr ImU32 col_normal = IM_COL32(100, 100, 230, 255);
		constexpr ImU32 col_selected = IM_COL32(237, 20, 91, 255);
		constexpr ImU32 col_updated = IM_COL32(18, 254, 18, 255);
		constexpr ImU32 col_ik = IM_COL32(247, 147, 39, 255);

		ImU32 col_out = col_normal;
		ImU32 col_in = col_normal;
		ImVec2 c0 = m.p;

		bool updated = kc->pose.boneKeys[i].selected;
		if (updated) {
			col_in = col_out = col_updated;
		}
		if (selected[i]) {
			col_in = col_out = col_selected;
		}

		if (pmxb.IsFixAxis()) {
			//軸固定。ねじりボーンなどの場合〇に×
			dl->AddCircle(c0, rad_out, col_out, 0, 2);
			dl->AddLine(ImVec2(c0.x - rad_in, c0.y - rad_in), ImVec2(c0.x + rad_in, c0.y + rad_in), col_out, 1);
			dl->AddLine(ImVec2(c0.x + rad_in, c0.y - rad_in), ImVec2(c0.x - rad_in, c0.y + rad_in), col_out, 1);
		} else if (!pmxb.IsTranslation()) {
			//平行移動不可ボーン◎
			dl->AddCircle(c0, rad_out, col_out, 0, 2);
			dl->AddCircleFilled(c0, rad_in, col_in);
		} else {
			ImVec2 Ro = ImVec2(rad_out, rad_out);
			ImVec2 Ri = ImVec2(rad_in, rad_in);
			dl->AddRect(m.p - Ro, m.p + Ro, col_out, 0, 0, 2);
			dl->AddRectFilled(m.p - Ri, m.p + Ri, col_in, 0);
		}

		//カーソルが合ってればリストに入れる
		if (LenSq(mp - m.p) <= rad2_click) {
			//カーソルがウィンドウからはみ出してないかチェック
			if (ImGui::IsMouseHoveringRect(lt,rb))
				floating.push_back(i);
		}

		//骨の表示 表示先がボーンの場合を解決
		if (pmxb.toBone >= 0) {
			m.b_back = markers[pmxb.toBone].back;
			m.e = markers[pmxb.toBone].p;
		}

		if (!m.b_back && (m.e!=m.p)) {
			ImVec2 ofs = Normalize(m.e - m.p) * rad_out;
			ofs = ImVec2(ofs.y, -ofs.x);
			dl->AddLine(m.p + ofs, m.e, col_in);
			dl->AddLine(m.p - ofs, m.e, col_in);
		}

		i++;
	}

}

//剛体表示
void DrawRigidBodies(KeyFrameWindowContext* kc, const Matrix& viewproj, int X, int Y, int W, int H)
{
	auto dl = ImGui::GetWindowDrawList();

	//レンダラー表示領域の左上隅
	ImVec2 lt = ImGui::GetWindowPos() + ImVec2(X,Y);

	for (auto&& solver : g_physics.Notifiees()) {
		for (int i = 0; i < solver->body.size(); i++) {
			auto bd = solver->body[i];
			auto pmxbd = solver->pmx->bodies[i];
			auto col = pmxbd.mode == 0 ? ImColor(0, 255, 0, 192) : ImColor(255, 0, 0, 192);
			if (bd->rigidBody) {
				Matrix mat = Matrix(bd->GetTransform()) * viewproj;

				switch (pmxbd.boxKind) {
				case 0: {
					ImVec2 c;
					vec3 o = { 0,0,0 };
					ProjectPoint(c, o, mat, W, H, lt);
					auto p = vec4::Transform(vec4(o.x, o.y, o.z, 1), mat);	//射影変換まで終わった時のo付近の拡大率(1mmdが何pxか)を出す
					float J = 2 * H / p.w;
					dl->AddCircle(c, J * pmxbd.boxSize.x, col, 64, 2);
					break;
				}
				case 1: {
					auto s = pmxbd.boxSize;
					vec3 v[8] = { {s.x,s.y,s.z}, {-s.x,s.y,s.z}, {s.x,-s.y,s.z}, {-s.x,-s.y,s.z}, {s.x,s.y,-s.z}, {-s.x,s.y,-s.z}, {s.x,-s.y,-s.z}, {-s.x,-s.y,-s.z} };
					ImVec2 c[8];
					for (int j = 0; j < 8; j++)
						ProjectPoint(c[j], v[j], mat, W, H, lt);
					dl->AddLine(c[0], c[1], col);
					dl->AddLine(c[0], c[2], col);
					dl->AddLine(c[3], c[1], col);
					dl->AddLine(c[3], c[2], col);
					dl->AddLine(c[4], c[5], col);
					dl->AddLine(c[4], c[6], col);
					dl->AddLine(c[7], c[5], col);
					dl->AddLine(c[7], c[6], col);
					dl->AddLine(c[0], c[4], col);
					dl->AddLine(c[1], c[5], col);
					dl->AddLine(c[2], c[6], col);
					dl->AddLine(c[3], c[7], col);
					break;
				}
				case 2: {
					ImVec2 c[2];
					vec3 o[2] = { {0,pmxbd.boxSize.y / (-2),0}, {0,pmxbd.boxSize.y / 2,0} };
					vec4 p[2];
					float r[2];

					//端の表示
					for (int j = 0; j < 2; j++) {
						ProjectPoint(c[j], o[j], mat, W, H, lt);
						p[j] = vec4::Transform(vec4(o[j].x, o[j].y, o[j].z, 1), mat);	//射影変換まで終わった時のo付近の拡大率(1mmdが何pxか)を出す
						float J = 2 * H / p[j].w;
						r[j] = J * pmxbd.boxSize.x;
						dl->AddCircle(c[j], r[j], col, 64, 1);
					}
					//線の表示
					ImVec2 e[2][2];
					ImVec2 dir = Normalize(c[0] - c[1]);
					ImVec2 perp = ImVec2(dir.y, -dir.x);	//両端点を結んだ線に垂直な方向
					e[0][0] = c[0] + perp * r[0];
					e[0][1] = c[0] - perp * r[0];
					e[1][0] = c[1] + perp * r[1];
					e[1][1] = c[1] - perp * r[1];
					dl->AddLine(e[0][0], e[1][0], col);
					dl->AddLine(e[0][1], e[1][1], col);
					dl->AddLine(c[0], c[1], col);

					break;
				}
				}
			}
		}
	}
}


//全体の事を考えない光源情報(現在未使用)
struct OneLight {
	int iFace;		//面番号
	vec3 Le;		//輝度
	float flux;		//輝度x面積x範囲(全光束)を明度に変換した物
};

//Walker's method用
struct WalkersAlias {
	UINT pair;		//ペアになる面の番号。-1でペア無し。pによらず「この面」を選択する
	float p;		//この面とペア面のどっちを選択するか？という確率。確率pで「この面」、1-pで「ペア面」を選択する
	float pdf;		//箱全体の中からこの面をサンプルする確率
};

//小さい箱と大きい箱に並べられる要素
struct WalkersWorker {
	UINT index;		//自分のインデックス
	float value;	//pの元になる値。平均値と同じとき1として正規化される
};

//材質→面についての情報
struct MaterialFace {
	UINT start;			//何番目の面から開始か？
	UINT count;			//材質に属する面数
	float totalArea;	//材質に属する面の面積総和
};

//pmxデータとpmxが読み込まれた時点で確定するデータ(VBとIBとテクスチャ、スキニング、頂点UVモーフテーブルなど)の組み
struct PMXRes {
	bool drawable = true;	//頂点・材質のあるモデルか？(ダミーボーンなど画面に何も表示しないモデルもあるので)
	fs::path path;		//読み込み元パス
	std::string u8name;	//モデル名称(utf-8変換済み)
	std::vector<std::string> u8materialNames;	//マテリアル名(utf-8変換済み)
	std::unique_ptr<PMX::PMXModel> pmx;
	std::vector<YRZ::Tex2D> textures;
	std::unordered_set<int> screenTextureIdx;	//screen.bmpが割り当てられているテクスチャ番号
	YRZ::Buf VB, IB, skinBuf, morphTableBuf, morphPointerBuf, face2MaterialBuf;
	//ImGuiの表情モーフウィンドウ用
	// モーフパネルは本家では4つだが内部的にシステム予約枠1つがあるので、一応それ込みの5つとしておく
	std::vector<std::string> u8morphPanel[5];	//morphPanel[panel[]でモーフ名一覧
	std::string u8morphPanelCombo[5];	//コンボボックス指定用 \0区切り
	std::vector<int>morphIndices[5];			//morphIndices[panel][]でモーフ番号一覧
	std::string u8IKCombo;				//IK選択用
	//モデル情報
	std::string u8description;

	//以下、光源2.0仕様
	//YRZ::Buf facePdfBuf;	//<float4>[面番号] = {面積,pdf,cdf,全面積} 材質がサンプリングされた後に面をサンプリングするためのモノ
	YRZ::Buf material2FaceBuf;	//<MaterialFace>[マテリアル番号] = {開始面番号,面数,総面積}

	YRZ::Buf faceWalkerBuf;	//Walker's method用情報

	//言語の変更に伴うu8なんとかの更新
	void ResetLanguage()
	{

		u8name = (Language == 0 || pmx->name_e.empty()) ? YRZ::u8(pmx->name) : YRZ::u8(pmx->name_e);
		if (u8name.empty())
			u8name = (char*) ( u8"(" + path.filename().u8string() + u8")" ).c_str();
		u8description = (Language == 0 || pmx->description_e.empty()) ? YRZ::u8(pmx->description) : YRZ::u8(pmx->description_e);

		//モーフパネル情報
		for (int i = 0; i < 5; i++) {
			u8morphPanel[i].clear();
			u8morphPanelCombo[i].clear();
			morphIndices[i].clear();
		}

		for (int i = 0; auto && mo : pmx->morphs) {
			bool found = false;
			for (auto&& node : pmx->nodes) {
				for (auto&& item : node.items) {
					if (item.isMorph && item.index==i) {
						found = true;
						break;
					}
				}
			}
			if (found) {
				auto str = (Language == 0 || mo.name_e.empty()) ? YRZ::u8(mo.name) : YRZ::u8(mo.name_e);
				u8morphPanel[mo.panel].push_back(str);
				u8morphPanelCombo[mo.panel] += str;
				u8morphPanelCombo[mo.panel].push_back(0);
				morphIndices[mo.panel].push_back(i);
			}
			i++;
		}

		//IKボーン情報
		u8IKCombo.clear();
		for (int i = 0; auto && b : pmx->bones) {
			if (b.IsIK()) {
				auto str = (Language == 0 || b.name_e.empty()) ? YRZ::u8(b.name) : YRZ::u8(b.name_e);
				u8IKCombo += str;
				u8IKCombo.push_back(0);
			}
		}
		//UTF-8形式の材質名
		u8materialNames.resize(pmx->materials.size());
		for (int i = 0; auto && m : pmx->materials) {
			u8materialNames[i] = (Language == 0 || m.name_e.empty()) ? YRZ::u8(m.name) : YRZ::u8(m.name_e);
			i++;
		}
	}

	/// <summary>
	///モデルのバリデーション
	/// </summary>
	/// <param name="err">あればエラーの内容</param>
	/// <returns>正しいモデルならtrue</returns>
	bool ValidatePMX(std::wstring& err)
	{
		err = L"";

		std::unordered_set<std::wstring> mats;
		for (auto&& m : pmx->materials) {
			if (mats.contains(m.name)) {
				err += m.name + g_hon.L(L" is a duplicate material name\n");
			}
			mats.insert(m.name);
		}

		std::unordered_set<std::wstring> bones;
		for (auto&& b : pmx->bones) {
			if (bones.contains(b.name)) {
				err += b.name + g_hon.L(L" is a duplicate bone name\n");
			}
			bones.insert(b.name);
		}

		return err.empty();
	}

	//テクスチャを(0,0,0,255)で埋める(αも0になってしまうのはマズいので)
	//フォーマットはR8G8B8A8_UNORMでないとダメ
	void BlankTexture(YRZ::Tex2D& tex)
	{
		auto desc = tex.desc();
		if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
			YRZ::ThrowIfFailed(E_INVALIDARG, L"internal error dayo", __FILEW__, __LINE__);
			return;
		}
		
		std::vector<uint32_t> pix(desc.Width * desc.Height, 0xFF000000);
		g_dxr->Upload(tex, pix.data());
	}

	PMXRes() {}
	PMXRes(const std::wstring& filename)
	{
		path = filename;
		pmx = std::make_unique<PMX::PMXModel>(filename.c_str());

		auto aliased = pmx->ResolveDuplicatedBoneNames();
		if (!aliased.empty()) {
			std::wstring lines;
			for (int a : aliased) {
				lines += L"\n" + pmx->bones[a].name;
			}
			auto msg = g_hon.L(L"duplicate bone names were found in the model, so some of the bone names were changed and used.") + filename + lines;
			if (NowLoading)
				g_problems += msg + L"\n";
			else
				OKDlg(L"PMX warning", msg);
		}
		aliased = pmx->ResolveDuplicatedMaterialNames();
		if (!aliased.empty()) {
			std::wstring lines;
			for (int a : aliased) {
				lines += L"\n" + pmx->materials[a].name;
			}
			auto msg = g_hon.L(L"duplicate material names were found in the model, so some of the material names were changed and used.") + filename + lines;
			if (NowLoading)
				g_problems += msg + L"\n";
			else
				OKDlg(L"PMX warning", msg);
		}

		//モデルのバリデーション。現在のところは材質名とボーン名だけのチェックなのでおそらくもう引っかかる事は無い
		std::wstring err;
		if (!ValidatePMX(err)) {
			auto msg = filename + g_hon.L(L" is not valid PMX file\n") + err;
			YRZ::ThrowIfFailed(E_FAIL, msg, __FILEW__, __LINE__);
			return;
		}

		drawable = !pmx->indices.empty();

		//std::vector<VertexEx> vs;
		std::vector<Vertex> vs;
		//頂点フォーマットの変換
		vs.resize(pmx->vertices.size());
		int loops = pmx->vertices.size();
		float nearest2 = FLT_MAX;	//原点から最も近いところにある頂点までの距離の二乗
		#pragma omp parallel for
		for (int i = 0; i < loops; i++) {
			auto&& v = pmx->vertices[i];
			vs[i].position = v.position;
			//法線が正規化されていないモデルが時々あるので
			vs[i].normal = NormalizeNormal(v.normal);
			//tangentはスキニング時にも決めるけど元データを直接読み込む場合に問題になるので一応ここで入れとく
			vs[i].tangent = ComputeTangent(vs[i].normal);
			vs[i].uv = v.uv;
			vs[i].edge = v.edge;
			//追加uv
			for (int j = 0; j < 4; j++)
				vs[i].exuv[j] = v.extra_uv[j];
			nearest2 = min(nearest2, Dot(vs[i].position, vs[i].position));
		}

		//最も近いところにある頂点でも10万MMDあるようなら「コントローラなどで描画されないモデル」として扱う
		//確かにBLASとレイトレの事を考えるとパフォーマンスの向上につながる
		if (nearest2 >= 1e+10)
			drawable = false;

		//バッファの作成。要素数0だった場合は大きさ1の空のバッファを作る
		auto createBufLambda = [&]<class T>(const std::vector<T>&v, const std::wstring & name) {
			YRZ::Buf b = v.empty() ? g_dxr->CreateBuf(nullptr, sizeof(T), 1) : g_dxr->CreateBuf(v.data(), sizeof(T), v.size());
			b.SetName(name.c_str());
			return b;
		};

		VB = createBufLambda(vs, L"VertexBuffer:" + pmx->name);

		//インデクスはそのまま
		IB = createBufLambda(pmx->indices, L"IndexBuffer:" + pmx->name);

		//テクスチャ
		textures.resize(pmx->absTextures.size());
		for (int i = 0; auto && t : pmx->absTextures) {
			try {
				//予約されているファイル名ならそれなりに対処をする
				if (YRZ::LowerStr(pmx->textures[i]) == L"screen.bmp") {
					textures[i] = g_dxr->CreateTex2D(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM);	//いったんダミーをいれとく
					BlankTexture(textures[i]);
					screenTextureIdx.insert(i);
				} else {
					//テクスチャのメタデータに何と書いてあるのか分からないのでとりあえずそのまま読み込んでシェーダでリニアに変換する方針で
					textures[i] = g_dxr->CreateTex2D(t.c_str(), YRZ::ColorSpace::linear, D3D12_HEAP_FLAG_NONE, MipmapGen);
				}
			} catch(std::exception ex) {
				//テクスチャの読み込み時にエラーが有った場合、ダミーを入れとく。参照されていないが指定されているだけのテクスチャと思われるので
				textures[i] = g_dxr->CreateTex2D(8,8,DXGI_FORMAT_R8G8B8A8_UNORM);
				BlankTexture(textures[i]);
				//それを材質側から参照している場合は、texおよびsptexに-1を指定する
				for (auto&& mat : pmx->materials) {
					if (mat.tex == i) 
						mat.tex = -1;
					if (mat.spTex == i)
						mat.spTex = -1;
				}
			}
			//モノクロ画像だった場合はカラーに拡張する
			if (YRZ::IsGrayscale(textures[i].desc().Format))
				textures[i] = g_dxr->ExpandGrayImage(textures[i]);

			textures[i].SetName(std::format(L"Texture : {}", pmx->textures[i]).c_str());
			i++;
		}
		//テクスチャが空だとリソースバインドで失敗するのでダミーいれとく		
		if (textures.empty()) {
			textures.push_back(g_dxr->CreateTex2D(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM));
			textures.back().SetName(L"dummy texture");
		}

		//スキニング、頂点UVモーフ用
		skinBuf = createBufLambda(pmx->skinTable, L"skinBuf:" + pmx->name);
		morphTableBuf = createBufLambda(pmx->morphTable, L"morphTableBuf:" + pmx->name);
		morphPointerBuf = createBufLambda(pmx->morphPointers, L"morphPointerBuf:" + pmx->name);
		face2MaterialBuf = createBufLambda(pmx->face2material, L"face2MaterialBuf:" + pmx->name);
		{
			//マテリアル→面番号・面数バッファ
			std::vector<MaterialFace> m2f(pmx->materials.size());
			UINT idx = 0;
			for (int i = 0; i < m2f.size(); i++) {
				m2f[i].start = idx;
				m2f[i].count = pmx->materials[i].vertexCount / 3;
				m2f[i].totalArea = 0;	//後でcomputeShaderで計算する
				idx += m2f[i].count;
			}
			//material2FaceBuf = g_dxr->CreateBuf(m2f.data(), sizeof(XMUINT2), m2f.size());
			material2FaceBuf = g_dxr->CreateRWBuf(sizeof(MaterialFace), max(1,m2f.size()));
			material2FaceBuf.SetName(std::format(L"material2FaceBuf : {}", pmx->name).c_str());
			if (!m2f.empty()) {
				g_dxr->Upload(material2FaceBuf, m2f.data());
			}

			//コンピュートシェーダで材質毎の面pdfを計算
			UINT nFaces = pmx->indices.size() / 3;
			
			faceWalkerBuf = g_dxr->CreateRWBuf(sizeof(WalkersAlias), max(1, nFaces));
			faceWalkerBuf.SetName(std::format(L"faceWalkerBuf : {}", pmx->name).c_str());

			if (nFaces > 0) {
				auto sumBuf = g_dxr->CreateRWBuf(sizeof(float), max(1, pmx->materials.size()));
				std::vector<float>zeroVec(max(pmx->materials.size(), 1), 0);
				g_dxr->Upload(sumBuf, zeroVec.data());	//ゼロクリア

				auto facePdfBuf = g_dxr->CreateRWBuf(sizeof(XMFLOAT4), max(1, nFaces));
				auto workBuf = g_dxr->CreateRWBuf(sizeof(WalkersWorker), max(1, nFaces));
				auto areaBuf = g_dxr->CreateRWBuf(sizeof(float), max(1, nFaces));

				const std::vector<std::wstring>csNames = { L"ModelAreaCS", L"ModelLittleBigCS"};
				std::vector<UINT> threads = { YRZ::CeilDiv(nFaces,32), (UINT)pmx->materials.size() };
				for (int i = 0; i < 2; i++) {
					g_dxr->OpenCommandList();
					YRZ::Pass pass(g_dxr);
					pass.SRV[0] = { &VB, &IB };
					pass.UAV = { {&workBuf}, {&faceWalkerBuf }, {&areaBuf}, {&material2FaceBuf} };
					YRZ::Shader cs = LoadShader(Recompile, g_dxr, L"SystemModelPDF_" + csNames[i], L"hlsl\\system\\modelLight.hlsl", csNames[i].c_str(), L"cs_6_1", CompileOption);
					pass.ComputePass(cs);
					pass.Compute(threads[i], 1, 1);
					g_dxr->ExecuteCommandList();
				}
			}
		}

		//表示時の名前のセット
		ResetLanguage();
	}
};

//1体のモデルとそれ用のBLAS、モーション
struct Model {
	int id = 0;				//モデルID、0はカメラで予約。1からスタート。起動してから何番目に読みこまれたモデルか
	std::wstring pmxfile;	//自分が使っているpmxファイルの名前
	std::string ctrlname;	//コントローラとして参照される名前(ディレクトリ名を省いてUTF-8に変換した物)
	std::shared_ptr<PMXRes> res;
	std::unique_ptr<PMX::PoseSolver> solver;
	//スキニングとUVモーフ処理済VB。内容の更新はレンダリング時にやる
	YRZ::BLAS blas = {};
	YRZ::Buf skinnedBuf, boneBuf, morphValuesBuf, prevSkinnedBuf;
	//マテリアル
	YRZ::Buf materialBuf;
	//毎フレーム更新されるリソース用
	YRZ::Buf boneBufCPU, morphValuesBufCPU, materialBufCPU;
	char *pBoneBuf, *pMorphValuesBuf, *pMaterialBuf;	//CPU側のポインタ。Mapしてそのままキープしとく
	//面番号→光源番号
	//YRZ::Buf face2lightBuf;
	//キーフレーム編集用データ
	KeyFrameWindowContext kc = {};
	bool withPP = false;		//ポストプロセス付きモデル
	bool withDeform = false;	//デフォーマ付きモデル
	//表示用
	std::string u8name;	//モデル一覧などで表示される時の名前。Rendererから追加される時に決定される

	Model() {};

	//物理互換性設定
	void CompatibleJoints()
	{
		//6DoFspringジョイントのうち、名前にfilters内の要素を含む物について
		//CFM値(0～1で大きいほどどるーんとなる、デフォルトは0、0.1くらいが丁度いいらしい)と
		//useFrameOffset(旧Bulletではfalse, 現Bulletはtrue)を設定する
		if (!g_cfg->physicsCompat.only2_0 || res->pmx->version == 2.0f) {
			for (auto&& jt : solver->joint) {
				if (jt->src < 0)
					continue;
				const auto& pmxj = res->pmx->joints[jt->src];
				if (pmxj.kind != 0)
					continue;

				auto n = YRZ::u8(pmxj.name);
				bool found = g_cfg->physicsCompat.filters.empty();	//filtersが空の場合は全部のジョイントの設定を書き換える事にする
				for (auto&& f : g_cfg->physicsCompat.filters) {
					if (n.find(f) != std::string::npos) {
						found = true;
						break;
					}
				}
				if (found) {
					for (int a = 0; a < 6; a++)
						jt->constraint->setParam(BT_CONSTRAINT_STOP_CFM, g_cfg->physicsCompat.cfm, a);
					btGeneric6DofSpringConstraint* dof6 = dynamic_cast<btGeneric6DofSpringConstraint*>(jt->constraint.get());
					dof6->setUseFrameOffset(g_cfg->physicsCompat.useFrameOffset);
				}
			}
		}
	}

	//MMDと互換性は無いが安定したBullet3のデフォルトのジョイント設定
	void StableJoints()
	{
		for (auto&& jt : solver->joint) {
			if (jt->src < 0)
				continue;
			const auto& pmxj = res->pmx->joints[jt->src];
			if (pmxj.kind != 0)
				continue;

			for (int a = 0; a < 6; a++)
				jt->constraint->setParam(BT_CONSTRAINT_STOP_CFM, 0, a);
			btGeneric6DofSpringConstraint* dof6 = dynamic_cast<btGeneric6DofSpringConstraint*>(jt->constraint.get());
			dof6->setUseFrameOffset(true);
		}
	}

	//既に読み込んであるresからバッファを作る
	void BuildBuffers()
	{
		//変換済み頂点バッファ
		skinnedBuf = g_dxr->CreateRWBuf(sizeof(Vertex), max(res->pmx->vertices.size(),1));
		skinnedBuf.SetName(std::format(L"skinnedBuf {}", pmxfile).c_str());
		//前フレームの頂点バッファ
		prevSkinnedBuf = g_dxr->CreateRWBuf(sizeof(Vertex), max(res->pmx->vertices.size(),1));
		prevSkinnedBuf.SetName(std::format(L"prevSkinnedBuf {}", pmxfile).c_str());

		//ポストプロセス用モデルはコピーされてそれきりなのでコピーはしとく
		if (!res->pmx->vertices.empty()) {
			g_dxr->OpenCommandList();
			g_dxr->CopyResource(skinnedBuf, res->VB);
			g_dxr->CopyResource(prevSkinnedBuf, res->VB);
			g_dxr->ExecuteCommandList();
			/*
			std::vector<Vertex>copybuf(res->pmx->vertices.size());
			for (int i = 0; auto&& v : copybuf) {
				auto& p = res->pmx->vertices[i];
				v.position = p.position;
				v.normal = p.normal;
				v.tangent = ComputeTangent(v.normal);
				v.uv = p.uv;
				i++;
			}
			g_dxr->Upload(skinnedBuf, copybuf.data());
			g_dxr->Upload(prevSkinnedBuf, copybuf.data());
			*/
		}

		if (g_dxr->RaytracingSupport()) {
			if (res->drawable) {
				blas = g_dxr->BuildBLAS(skinnedBuf, res->IB);
				blas.SetName(std::format(L"blas {}", pmxfile).c_str());
			}
		}

		//各フレーム毎のボーンのグローバル変形行列, 毎フレーム書き換わるのでとりあえず空データを入れておく
		boneBuf = g_dxr->CreateBuf(nullptr, sizeof(DirectX::XMMATRIX), max(res->pmx->bones.size(),1));
		boneBuf.SetName(std::format(L"boneBuf {}", pmxfile).c_str());
		boneBufCPU = g_dxr->CreateBufCPU(nullptr, sizeof(DirectX::XMMATRIX), max(res->pmx->bones.size(), 1), true, false);
		boneBufCPU.SetName(std::format(L"boneBufCPU {}", pmxfile).c_str());
		boneBufCPU.res->Map(0,nullptr,(void**)&pBoneBuf);

		//各フレームごとのモーフ値
		morphValuesBuf = g_dxr->CreateBuf(nullptr, sizeof(float), max((size_t)1, res->pmx->morphs.size()));
		morphValuesBuf.SetName(std::format(L"morphValuesBuf {}", pmxfile).c_str());
		morphValuesBufCPU = g_dxr->CreateBufCPU(nullptr, sizeof(float), max((size_t)1, res->pmx->morphs.size()), true, false);
		morphValuesBufCPU.SetName(std::format(L"morphValuesBufCPU {}", pmxfile).c_str());
		morphValuesBufCPU.res->Map(0, nullptr, (void**)&pMorphValuesBuf);

		//材質モーフ適用済みのMMDマテリアル、毎フレーム書き換わる
		materialBuf = g_dxr->CreateBuf(nullptr, sizeof(PMX::Material), max(res->pmx->materials.size(),1));
		materialBuf.SetName(std::format(L"materialBuf {}", pmxfile).c_str());
		materialBufCPU = g_dxr->CreateBufCPU(nullptr, sizeof(PMX::Material), max(res->pmx->materials.size(), 1), true, false);
		materialBufCPU.SetName(std::format(L"materialBufCPU {}", pmxfile).c_str());
		materialBufCPU.res->Map(0, nullptr, (void**)&pMaterialBuf);

		//面番号→光源番号バッファ(モデルの増減のタイミングで作り直される)
		//face2lightBuf = g_dxr->CreateBuf(nullptr, sizeof(int), max(res->pmx->indices.size() / 3,1));
		//face2lightBuf.SetName(std::format(L"face2lightBuf {}", pmxfile).c_str());

		//ソルバーの作成
		solver = std::make_unique<PMX::PoseSolver>(&g_physics, res->pmx.get(), nullptr);
		
		//config読んで一部のジョイントをMMD互換(風)のジョイント設定に書き換える
		CompatibleJoints();

		kc.Reset(res->pmx.get(), solver.get(), &g_copyBuffer, &g_undoBuffer);
	}
	//既に読み込んであるPMXResを割り当ててモデルを作る
	void SetPMX(std::shared_ptr<PMXRes> _res, std::wstring filename)
	{
		pmxfile = filename;
		ctrlname = YRZ::u8(YRZ::LowerStr(fs::path(pmxfile).filename().wstring()));
		res = _res;
		BuildBuffers();
	}
	//ファイルからPMXモデルを読む
	void LoadPMX(std::wstring filename)
	{
		pmxfile = filename;
		ctrlname = YRZ::u8(YRZ::LowerStr(fs::path(pmxfile).filename().wstring()));
		res = std::make_shared<PMXRes>(filename);
		BuildBuffers();
	}
	//現フレームにvmdを追加
	void LoadVMD(std::wstring filename)
	{
		auto vmd = PMX::VMD(filename.c_str(), g_cfg->vmdCodepage, res->pmx.get());
		auto subset = PMX::KeySubset(&vmd);
		kc.RegisterSubset(subset, std::format(L"Load {}", YRZ::ExtractFilename(filename)), true, true, Frame);
	}
	void Solve(float time, bool integerTime = true, bool contPhys = false)
	{
		solver->Solve(time, integerTime, contPhys);
	}
};

//OIDNに入出力するためのバッファの管理、OIDNデバイス・フィルタオブジェクトの管理
class OIDNIO {
private:
	OIDNBuffer oidnIn = nullptr, oidnOut = nullptr;
	HANDLE hOidn1 = 0, hOidn2 = 0;
public:
	OIDNDevice odev;
	OIDNFilter filter = nullptr;
	YRZ::Buf oidnBuf;		//oidnへの入力用(フィルタリング前の画像、albedo, normal)
	YRZ::Buf oidnOutBuf;	//oidnからの出力用(フィルタリング後の画像)

	OIDNIO() {
		odev = oidnNewDevice(OIDN_DEVICE_TYPE_CUDA);
		//OIDNデバイス
		oidnCommitDevice(odev);
	}
	~OIDNIO() {
		if (hOidn1 != 0)	CloseHandle(hOidn1);
		if (hOidn2 != 0)	CloseHandle(hOidn2);
		if (oidnIn != nullptr)	oidnReleaseBuffer(oidnIn);
		if (oidnOut != nullptr)	oidnReleaseBuffer(oidnOut);
		if (filter != nullptr)	oidnReleaseFilter(filter);
		oidnReleaseDevice(odev);
	}
	//OIDNバッファのサイズ設定。コンストラクタで生成されたら使う前に一度このメソッドが呼ばれる必要がある
	void Resize(int W, int H) {
		if (hOidn1 != 0) {
			CloseHandle(hOidn1);
		}
		if (hOidn2 != 0) {
			CloseHandle(hOidn2);
		}
		std::wstring shareNameIn = std::format(L"YRZ_OIDN_In{}", timeGetTime());	//なんかCloseHandleしても共有リソース名を毎回変えないと怒られるので
		std::wstring shareNameOut = std::format(L"YRZ_OIDN_Out{}", timeGetTime());
		oidnBuf = g_dxr->CreateRWBuf(sizeof(OIDNInput), W * H, D3D12_HEAP_FLAG_SHARED);
		oidnBuf.SetName(L"Input to OIDN Buffer (raw image,depth,normal)");
		oidnOutBuf = g_dxr->CreateRWBuf(12, W * H, D3D12_HEAP_FLAG_SHARED);
		oidnOutBuf.SetName(L"Output from OIDN Buffer (filtered image)");
		YRZ::ThrowIfFailed(g_dxr->Device()->CreateSharedHandle(oidnBuf.res.Get(), nullptr, GENERIC_ALL, shareNameIn.c_str(), &hOidn1), L"CreateSharedHandle failed", __FILEW__, __LINE__);
		YRZ::ThrowIfFailed(g_dxr->Device()->CreateSharedHandle(oidnOutBuf.res.Get(), nullptr, GENERIC_ALL, shareNameOut.c_str(), &hOidn2), L"CreateSharedHandle failed", __FILEW__, __LINE__);

		auto oidnFlags = OIDN_EXTERNAL_MEMORY_TYPE_FLAG_OPAQUE_WIN32;

		if (oidnIn != nullptr)
			oidnReleaseBuffer(oidnIn);
		if (oidnOut != nullptr)
			oidnReleaseBuffer(oidnOut);
		oidnIn = oidnNewSharedBufferFromWin32Handle(odev, oidnFlags, 0, shareNameIn.c_str(), W * H * 36);
		oidnOut = oidnNewSharedBufferFromWin32Handle(odev, oidnFlags, 0, shareNameOut.c_str(), W * H * 12);

		if (filter != nullptr)
			oidnReleaseFilter(filter);
		filter = oidnNewFilter(odev, "RT");
		oidnSetFilterImage(filter, "color", oidnIn, OIDN_FORMAT_FLOAT3, W, H, 0, 36, W * 36);
		oidnSetFilterImage(filter, "albedo", oidnIn, OIDN_FORMAT_FLOAT3, W, H, 12, 36, W * 36);	//albedoとnormalはDoFがきつい時はコメントアウトした方がいいかも
		oidnSetFilterImage(filter, "normal", oidnIn, OIDN_FORMAT_FLOAT3, W, H, 24, 36, W * 36);
		oidnSetFilterImage(filter, "output", oidnOut, OIDN_FORMAT_FLOAT3, W, H, 0, 12, W * 12);
		oidnSetFilterBool(filter, "hdr", true);
		oidnCommitFilter(filter);

		const char* errorMessage;
		if (oidnGetDeviceError(odev, &errorMessage) != OIDN_ERROR_NONE)
			YRZ::DEBA("{}", errorMessage);
	}
};

//skybox読み込みとpdfの計算など
class Skybox {
private:
public:
	YRZ::Tex2D skybox;//, skyboxAvgRow, skyboxAvgAll, skyboxPDFRow, skyboxPDF, skyboxCDFRow, skyboxCDF;
	YRZ::Buf skyboxSH;
	YRZ::Buf skywalker, skywalkerRow;
	Skybox() {
	}
	//読み込み、所望のテクスチャが読み込まれたらtrue, 読み込みに失敗してダミーが割り当てられたらfalse
	bool Load(std::wstring filename, const std::vector<std::string>& memos = {})
	{
		std::vector<std::string> memosLow;	//全部小文字に変換したメモ
		for (auto& s : memos)
			memosLow.push_back(YRZ::LowerStr(s));

		auto memoLambda = [&](const std::string & str) {
			for (auto& s : memosLow) {
				if (s == str)
					return true;
			}
			return false;
		};

		bool ret = true;
		//skybox
		try {
			//メタデータのないPNG,TGAなどは_SRGB付きで読み込む(Sample時にリニアに自動変換してもらえるので)
			skybox = g_dxr->CreateTex2D(filename.c_str(), YRZ::ColorSpace::srgb, D3D12_HEAP_FLAG_NONE, true);
			if (memoLambda("skyboxprefilter") && g_dxr->RaytracingSupport())
				skybox = g_dxr->PrefilterEnvmap(skybox, DXGI_FORMAT_UNKNOWN, 0, 8, 32, [&](int i, int t) { YRZ::DEB(L"prefiltering {}/{}", i + 1, t); Sleep(50); });
			skybox.SetName(std::format(L"skybox texture {}", filename).c_str());
		} catch (...) {
			skybox = g_dxr->CreateTex2D(256, 128, DXGI_FORMAT_R16G16B16A16_FLOAT);
			skybox.SetName(L"skybox texture (failed)");
			ret = false;
		}

		//ComputeShaderでskyboxのWalker's alias method用の箱を作る
		bool skywalkerRising = memoLambda("skyboxsampler");

		UINT SW, SH;	//skyboxの画像のサイズ
		SW = skybox.desc().Width;
		SH = skybox.desc().Height;
		
		auto CD = [&](auto a, auto b) {return YRZ::CeilDiv(a, b); };

		if (skywalkerRising) {
			//skybox用のWalker's method サンプラを作る
			skywalker = g_dxr->CreateRWBuf(sizeof(WalkersAlias), SW * SH);
			skywalker.SetName(L"skywalker");
			skywalkerRow = g_dxr->CreateRWBuf(sizeof(WalkersAlias), SH);
			skywalkerRow.SetName(L"skywalkerRow");
			auto skyworker = g_dxr->CreateRWBuf(sizeof(WalkersWorker), SW * SH);
			auto skyworkerRow = g_dxr->CreateRWBuf(sizeof(WalkersWorker), SH);
			auto skyLum = g_dxr->CreateRWBuf(sizeof(float), SW * SH);
			auto skyLumRow = g_dxr->CreateRWBuf(sizeof(float), SH);

			const std::vector<std::wstring> skyEntries = { L"SkyLuminance", L"SkyLuminanceRow", L"SkyWalkin", L"SkyWalkinRow" };
			const std::vector<DirectX::XMUINT2> skyThreads = { {CD(SW,16),CD(SH,16)}, {CD(SH,128),1}, {CD(SH,128),1}, {1,1} };
			for (int i = 0; i < skyEntries.size(); i++) {
				YRZ::Pass* skyPass = new YRZ::Pass(g_dxr);
				skyPass->SRV[0].push_back(&skybox);
				skyPass->UAV.push_back({ &skywalker });
				skyPass->UAV.push_back({ &skywalkerRow });
				skyPass->UAV.push_back({ &skyworker });
				skyPass->UAV.push_back({ &skyworkerRow });
				skyPass->UAV.push_back({ &skyLum });
				skyPass->UAV.push_back({ &skyLumRow });

				YRZ::Shader skyCS1 = LoadShader(Recompile, g_dxr, L"SystemSkyboxPDF_" + skyEntries[i], L"hlsl\\system\\skyboxPDF.hlsl", skyEntries[i].c_str(), L"cs_6_1", CompileOption);
				skyPass->ComputePass(skyCS1);
				g_dxr->OpenCommandList();
				skyPass->Compute(skyThreads[i].x, skyThreads[i].y, 1);
				g_dxr->ExecuteCommandList();
				delete skyPass;
				YRZ::DEB(L"Loading skybox {}/{}", i + 1, skyEntries.size());
			}
		} else {
			//ダミーリソースの割り当て
			skywalker = g_dxr->CreateRWBuf(sizeof(WalkersAlias), 1);
			skywalkerRow = g_dxr->CreateRWBuf(sizeof(WalkersAlias), 1);
		}

		//SH
		struct SHCoeff {
			vec4 c[9];	//skyboxを2次までのSHをした結果格納用coeff[0]がl=0, coeff[1-3]がl=1,m=-1,0,+1、coeff[4-8]がl=2,m=-2～+2
		};
		auto skyboxSHX = g_dxr->CreateRWBuf(sizeof(SHCoeff), SH);
		skyboxSH = g_dxr->CreateRWBuf(sizeof(SHCoeff), 1);
		skyboxSH.SetName(L"skyboxSH");
		const std::vector<std::wstring> skyEntries = { L"SHComboX", L"SHComboY" };
		const std::vector<DirectX::XMUINT2> skyThreads = { {CD(SH,32),1}, {1,1} };
		for (int i = 0; i < skyEntries.size(); i++) {
			YRZ::Pass* skyPass = new YRZ::Pass(g_dxr);
			skyPass->SRV[0].push_back(&skybox);
			skyPass->UAV.push_back({ &skyboxSHX });
			skyPass->UAV.push_back({ &skyboxSH });

			YRZ::Shader skyCS1 = LoadShader(Recompile, g_dxr, L"SystemSkyboxSH_" + skyEntries[i], L"hlsl\\system\\skyboxSH.hlsl", skyEntries[i].c_str(), L"cs_6_1", CompileOption);
			skyPass->ComputePass(skyCS1);
			g_dxr->OpenCommandList();
			skyPass->Compute(skyThreads[i].x, skyThreads[i].y, 1);
			g_dxr->ExecuteCommandList();
			delete skyPass;
			YRZ::DEB(L"Loading skybox {}/{}", i + 1, skyEntries.size());
		}

		return ret;
	}
};


//エフェクトからコントローラの値を得るためのコールバック① コントローラ名とモーフ名からモデル番号とアイテム番号を得る
//ない場合は-1が入る
void ControllerQuery(int selfIndex, int doppelIndex, const std::string& controllerName, const std::string& item, const std::string& type, int& modelIndex, int& itemIndex);

//コントローラの値を得るためのコールバック②↑で得たインデックスと型から値を得る
void ControllerCallback(void* value, int modelIndex, int itemIndex, const std::string& type);

//エフェクトから構造体のサイズを尋ねるコールバック関数
UINT ElemSizeQuery(const std::string& type);

//レンダラーに対して出せるメッセージ群。Render()時にCBへセットされる
enum class RenderMsg {
	OnStart,		//再生・停止または録画・停止後の最初のレンダリング時
	OnResize,		//リサイズ後の最初のレンダリング時
	OnLoadSkybox,	//skybox変更後の最初のレンダリング時
	OnLoad,			//エフェクトロード・リロード後の最初のレンダリング時
};
const std::vector<RenderMsg> AllMsg = { RenderMsg::OnStart, RenderMsg::OnResize, RenderMsg::OnLoadSkybox, RenderMsg::OnLoad };

//モデル管理とレンダリング
class Renderer {

private:
	int W, H;	//レンダラー窓のサイズ
	bool m_passCreated = false;			//一度でもcreatePass()が実行されたらtrue
	std::wstring m_skyboxFilename;	//最後に読み込まれたskyboxのファイル名

	std::vector<int>textureIndex = { 0 };	//各モデルのテクスチャ参照位置
	OIDNIO oidn;

	//DirectX12用リソース群
	YRZ::TLAS tlas = {};
	YRZ::Buf dmyTlas = {};			//レイトレサポート無しの時にTLAS代わりにSRVに入れるダミーリソース
	//サイズ依存リソース
	YRZ::Tex2D dxrout;				//メインレンダリング結果(アキュムレーション後の状態)出力用
	YRZ::Tex2D normalDepth;			//法線奥行マップ
	YRZ::Tex2D gbuffer1;			//モデル番号(R) 面番号(G) uint
	YRZ::Tex2D gbuffer2;			//重心座標 float2

	YRZ::Tex2D ppIn, ppOut;			//ポストプロセスへの入出力

	//モデルの増減で作り直す必要のあるリソース
	YRZ::Buf textureIndexBuf;	//モデル番号→総テクスチャ番号
	YRZ::Buf model2matBuf;		//モデル番号→{総材質番号先頭,材質数}
	YRZ::Buf mat2modelBuf;		//総材質番号→モデル番号
	std::vector<int>peekaboo;	//いないいない
	YRZ::Buf peekabooBuf;		//モデル番号→show(1)/hide(0)
	YRZ::Buf peekabooBufCPU;
	char* pPeekabooBuf;

	//modelsと出力サイズに依存するパス
	YRZ::Pass renderPass;		//レイトレーシングで現フレームの状態をレンダリングしてdxroutへ出力
	//出力サイズにのみ依存するパス
	YRZ::Pass backgroundPass;		//ppOutを最終的な画像であるdayoOutとimageOutに出力するパス
	YRZ::Pass oidnPass;			//historyをOIDNでデノイズした結果をppOutに出力するパス
	YRZ::Pass screenPass;		//screen.bmpの中身を作るパス(動画ソースにも依存する)
	//動画ソースに依存するパス
	YRZ::Pass moviePass;		//YUY2やNV12で書かれたフレームバッファをscreen.bmpの中身としてRGBAに変換するパス
	//modelsにのみ依存するパス
	std::vector<YRZ::Pass>skinPass;	//コンピュートシェーダでスキニング+頂点UVモーフ処理するパス
	//作りっぱなしのパス
	YRZ::Pass clearBBPass;	//バックバッファをクリアするパス

	//コンスタントバッファ
	YRZ::CB m_cb;
	//スタティックサンプラ
	D3D12_STATIC_SAMPLER_DESC samp;

	//メッセージキュー
	std::set<RenderMsg> m_msgQDeform;	//デフォーマ用
	std::set<RenderMsg> m_msgQRender;	//レンダラ用
	std::set<RenderMsg> m_msgQPP;		//ポストプロセス用
	//メッセージに対応したCB内の書き換えるべき値
	std::unordered_map<RenderMsg, int*> m_msg2CB;
public:
	//設定用定数
	const int nModelChunk = 32;	//モデル数がこの倍数を越えたらPSOを再作成すべしという値
	const int nTexChunk = 256;	//テクスチャ数がこの倍数を越えたらPSO再作成

	//制御用変数
	bool skinUpdateReq = false;	//次のレンダリング時にスキニングをやるべしというフラグ(Solveした場合はtrueがセットされる)

	//モデル関係
	int nModelLoaded = 0;	//起動してから何回モデルを読み込んだか？ID振り用
	std::vector<Model>models;
	DWORD modelsUpdatedT;	//最後にモデルに追加・削除が行われた時刻
	
	//システムリソース
	Skybox skybox;
	YRZ::Tex2D dayoOut;		//ポストプロセス・トーンマップ全部終わって画面にすぐ出せる状態の絵
	YRZ::Tex2D imageOut;	//デフォルト背景とコンポジットする前(ファイルに出力される絵)
	std::vector<int>matSelected;//材質がMatDescWindowで選択されているか？(1:yes 0:no)
	YRZ::Buf matSelectedBuf;

	//screen.bmp関係
	YRZ::Tex2D screenBMP;	//screen.bmpの中身(dayoOutを加工したり将来的にAVI貼ったり)
	std::unique_ptr<Wave::Movie>movie = nullptr;		//動画データ・デコーダ
	YRZ::Buf movieBuf;		//動画デコード結果(RGBA変換前)
	YRZ::Tex2D movieTex;	//動画デコード結果(RGBA変換後)

	//エフェクト関係
	Constantan* cb;			//コンスタントバッファの中身にアクセスし奴
	YRZ::FXLoadConfig defaultLoadConfig;	//ロードコンフィグのデフォルト値(モデルの数とかに依らない部分をセットした物)
	YRZ::FXLoadConfig commonLoadConfig;		//↑をモデル数の増減などに伴って変更した物
	std::unique_ptr<YRZ::FXWatcher> fxWatcher = nullptr;
	std::unique_ptr<YRZ::FXShaderCache> fxCache = nullptr;
	YRZ::Pass prepassPP;	//ポストプロセス用リソース定義パス
	std::vector<std::unordered_map<std::string, YRZ::FXController>> condicts;	//エフェクトから参照されるモーフの設定(最小・最大・デフォルト値) CreatePass()内を参照

	//プロパティ
	const int Width() { return W; }
	const int Height() { return H; }

	Renderer(int width, int height)
	{
		fxCache = std::make_unique<YRZ::FXShaderCache>(BasePath / L"ShaderCache");
		fxWatcher = std::make_unique<YRZ::FXWatcher>(*g_dxr, *fxCache);
		g_physics.fps = g_cfg->physicsFps;
		g_physics.maxSubsteps = g_cfg->physicsMaxSubsteps;

		m_cb = g_dxr->CreateCB(nullptr, sizeof(Constantan));
		m_cb.SetName(L"constant buffer");
		cb = (Constantan*)m_cb.pData;

		m_msg2CB[RenderMsg::OnStart] = &cb->onStart;
		m_msg2CB[RenderMsg::OnResize] = &cb->onResize;
		m_msg2CB[RenderMsg::OnLoadSkybox] = &cb->onLoadSkybox;
		m_msg2CB[RenderMsg::OnLoad] = &cb->onLoad;

		LoadSkybox(YRZ::L(g_cfg->skybox));
		//skybox.Load();

		samp = CD3DX12_STATIC_SAMPLER_DESC(0); //CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT);

		Resize(width, height);

		/*** サイズにのみ依存する(モデルには依存しない)パス群の作成 ***/

		//バックバッファクリアパス
		clearBBPass = YRZ::Pass(g_dxr);
		YRZ::Shader vs = LoadShader(Recompile, g_dxr, L"SystemClearScreenVS", L"hlsl/system/clearscreen.hlsl", L"VS", L"vs_6_1", CompileOption);
		YRZ::Shader ps = LoadShader(Recompile, g_dxr, L"SystemClearScreenPS", L"hlsl/system/clearscreen.hlsl", L"PS", L"ps_6_1", CompileOption);
		clearBBPass.PostProcessPass(vs, ps);

		//OIDNによってhitoryTexをデノイズした結果をppOutに出力するパス
		oidnPass = YRZ::Pass(g_dxr);
		oidnPass.SRV[0] = { &oidn.oidnOutBuf, &dxrout };
		oidnPass.CBV = { &m_cb };
		oidnPass.Samplers = { samp };
		oidnPass.RTV = { {&ppOut, YRZ::CV(0,0,0,0)} };
		auto vsBlobOidn = LoadShader(Recompile, g_dxr, L"SystemOIDNVS", L"hlsl\\system\\oidn.hlsl", L"VS", L"vs_6_1", CompileOption);
		auto psBlobOidn = LoadShader(Recompile, g_dxr, L"SystemOIDNPS", L"hlsl\\system\\oidn.hlsl", L"PS", L"ps_6_1", CompileOption);
		oidnPass.PostProcessPass(vsBlobOidn, psBlobOidn);

		//ppOutの中身をdayoOutに出力するパス
		//元々はトーンマッピングとガンマ変換をやっていたが、それらはポストプロセス用エフェクトに移行した
		//フォーマットの変換およびデフォルト背景とのコンポジット
		backgroundPass = YRZ::Pass(g_dxr);

		//screen.bmpの中身を作るパス
		screenPass = YRZ::Pass(g_dxr);
		screenPass.SRV[0] = { &dayoOut };
		screenPass.CBV = { &m_cb };
		screenPass.Samplers = { samp };
		screenPass.RTV = { {&screenBMP, YRZ::CV(0,0,0,1)}};
		auto vsScreenBMP = LoadShader(Recompile, g_dxr, L"SystemScreenVS", L"hlsl\\system\\Screen.hlsl", L"VS", L"vs_6_1", CompileOption);
		auto psScreenBMP = LoadShader(Recompile, g_dxr, L"SystemScreenPS", L"hlsl\\system\\Screen.hlsl", L"PS", L"ps_6_1", CompileOption);
		screenPass.PostProcessPass(vsScreenBMP, psScreenBMP);

		//動画のフレームをRGBAに変換するパス…は動画読み込み時に作る

		//エフェクトの準備
		defaultLoadConfig.SRVspace3DTex = SRVSpace3D;
		defaultLoadConfig.pCB = (char*)m_cb.pData;
		defaultLoadConfig.CBPointers = g_CBPointers;
		defaultLoadConfig.ControllerQuery = ControllerQuery;
		defaultLoadConfig.ControllerCallback = ControllerCallback;
		defaultLoadConfig.ElemSizeQuery = ElemSizeQuery;
		defaultLoadConfig.selfIndex = -1;
		defaultLoadConfig.defaultRT = { &ppOut };	//デフォルトのRTVItemはppOut,クリアしない設定
		const std::vector<D3D12_INPUT_ELEMENT_DESC> vertexLayout = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "EDGE", 0, DXGI_FORMAT_R32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};
		defaultLoadConfig.layout = YRZ::FXInputElementDesc::Convert(vertexLayout);
		defaultLoadConfig.reservedMDRes = { {"screen.bmp", &screenBMP} };
		for (auto& o : CompileOption)
			defaultLoadConfig.compileOption.push_back(o);

		//defaultLoadConfig.definedPass = &prepassPP;

	}

	//サイズ依存リソース作成
	void Resize(int width, int height)
	{
		W = width;
		H = height;
		cb->resolution = { (UINT)W, (UINT)H };
		oidn.Resize(W, H);

		dxrout = g_dxr->CreateRWTex2D(W, H, DXGI_FORMAT_R32G32B32A32_FLOAT);
		dxrout.SetName(L"dxrout");
		normalDepth = g_dxr->CreateRWTex2D(W, H, DXGI_FORMAT_R32G32B32A32_FLOAT);
		normalDepth.SetName(L"normaldepth");
		gbuffer1 = g_dxr->CreateRWTex2D(W, H, DXGI_FORMAT_R32G32_UINT);
		gbuffer1.SetName(L"gbuffer1");
		gbuffer2 = g_dxr->CreateRWTex2D(W, H, DXGI_FORMAT_R32G32_FLOAT);
		gbuffer2.SetName(L"gbuffer2");
		dayoOut = g_dxr->CreateRT2D(W, H, g_dxr->BackBufferFormat());
		dayoOut.SetName(L"dayoOut");
		imageOut = g_dxr->CreateRT2D(W, H, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);	//SRGB付きフォーマットでないとpng出力した時に「リニア色空間で書かれている」として出力されるので
		imageOut.SetName(L"imageOut");
		ppIn = g_dxr->CreateRT2D(W, H, DXGI_FORMAT_R32G32B32A32_FLOAT);
		ppIn.SetName(L"ppIn");
		ppOut = g_dxr->CreateRT2D(W, H, DXGI_FORMAT_R32G32B32A32_FLOAT);
		ppOut.SetName(L"ppOut");
		screenBMP = g_dxr->CreateRT2D(W,H, g_dxr->BackBufferFormat());
		screenBMP.SetName(L"screenBMP");

		renderPass.Update();
		backgroundPass.Update();
		oidnPass.Update();
		screenPass.Update();

		//matDescのリロード(screenBMPをmatDescから使う場合があるが、matDescは使うリソースをコピーするため、再コピーする必要がある)
		auto reloadMDLambda = [&](YRZ::FX* fx) {
			if (fx->hasMatDesc()) {
				auto& md = fx->matDescs;
				for (int i = 0; auto&& mm : md.items) {
					for (int j = 0; auto&& m : mm) {
						try {
							fx->SetMatDesc(m.sourceFile, i, j, true, true, false);
						} catch (std::exception ex) {
							;
						}
						j++;
					}
					i++;
				}
				fx->Update();
			}
		};

		for (auto&& mo : models) {
			//ポストプロセス・デフォーマのリサイズ
			auto pp = fxWatcher->Get(mo.id);
			if (pp) {
				pp->Resize(W, H);
				reloadMDLambda(pp);
			}
		}
		
		auto ren = fxWatcher->Get(WID_RENDERER);
		if (ren) {
			ren->Resize(W, H);
			reloadMDLambda(ren);
		}

		auto bar = fxWatcher->Get(WID_BAR);
		if (bar) {
			bar->Resize(W, H);
			reloadMDLambda(bar);
		}

		//次回Render時にCBにセットされるメッセージ
		Message(RenderMsg::OnResize);
	}


	//skyboxの変更とskyboxオブジェクトに依存するレンダリングパスの更新
	void LoadSkybox(std::wstring filename)
	{
		bool success = true;
		try {
			//レンダラのmemoを渡す
			std::vector<std::string>memos = {};
			auto ren = fxWatcher->Get(WID_RENDERER);
			if (ren)
				memos = ren->memos;

			if (filename.empty())
				success = skybox.Load(m_skyboxFilename, memos);
			else
				success = skybox.Load(filename, memos);
			renderPass.Update();
			//renderOidnPass.Update();

			for (auto && w : fxWatcher->watchList) {
				if (w.fx) {
					w.fx->Update();
				}
			}
		} catch (std::exception ex) {
			OKDlgA("fail to load skybox",ex.what());
		}
		if (!success) {
			OKDlgA("fail to load skybox", "fail to load skybox");
		} else {
			//読み込みに成功した
			Message(RenderMsg::OnLoadSkybox);
			if (!filename.empty())
				m_skyboxFilename = filename;
		}
	}

	//動画の読み込み
	void LoadMovie(const fs::path& filename)
	{
		//空文字が指定されていたら動画データの開放をする
		if (filename.empty()) {
			g_edi.movieFile = "";
			movie = nullptr;
			return;
		}

		try {
			movie = std::make_unique<Wave::Movie>();
			movie->Load(filename);
			movieTex = g_dxr->CreateRWTex2D(movie->Width(), movie->Height(), DXGI_FORMAT_R8G8B8A8_UNORM);
			movieTex.SetName(L"movieTex");
			//ハマった話…バッファの要素サイズは4の倍数でなければならない
			//そうでなくても普通に生成されてオブジェクト自体の参照も出来るしエラーも出力されないが正しく読み書きできない
			movieBuf = g_dxr->CreateBuf(nullptr, 4, max(1, (movie->Width() * movie->Height() * movie->BitsPerPixel() / 8 + 3)/4 ));
			movieBuf.SetName(L"movieBuf");
			screenPass.SRV[0][0] = &movieTex;
			screenPass.Update();

			//フレーム変換パスの作成
			moviePass = YRZ::Pass(g_dxr);
			std::wstring csname = L"SystemVideoCS" + YRZ::L(movie->FourCC());
			auto cs = LoadShader(Recompile, g_dxr, csname, L"hlsl\\system\\VideoConv.hlsl", csname, L"cs_6_1", CompileOption);
			moviePass.SRV[0].push_back({&movieBuf});
			moviePass.UAV.push_back({ &movieTex });
			moviePass.ComputePass(cs);

			//現在のフレームゲット
			GetMovieFrame(AnimFrame);

			//全部うまく行ってたら「読み込めました」という事にする
			g_edi.movieFile = (char*)(filename.u8string().c_str());
		} catch(std::exception ex) {
			OKDlg(L"Movie error", YRZ::L(ex.what()));
			UnloadMovie();
		}
	}

	//ムービーのアンロード、screen.bmpのソースを戻す
	void UnloadMovie()
	{
		screenPass.SRV[0][0] = { &dayoOut };
		screenPass.Update();
		movie = nullptr;
		g_edi.movieFile = "";
	}

	//動画のフレームをmovieTexに反映する
	void GetMovieFrame(float frame)
	{
		if (!movie)
			return;

		//パフォーマンス計測用
		StopWatch watch;
		watch.Lap();
		double decodeTime=0, uploadTime=0, convertTime=0;

		try {
			bool fetched = false;
			movie->GetFrame((double)frame / 30.0, fetched);
			decodeTime = watch.Lap();
			if (fetched) {
				g_dxr->Upload(movieBuf, movie->frameBuffer.data());
				uploadTime = watch.Lap();
				g_dxr->OpenCommandList();
				
				//現在の所bpp=16はYUY2しかない。YUY2の場合は2ピクセル1度に変換する
				//bpp=12もNV12だけ。NV12の場合は4ピクセル1度に変換する
				UINT w = movie->Width();
				if (movie->BitsPerPixel() == 16)
					w /= 2;
				else if (movie->BitsPerPixel() == 12)
					w /= 4;
				moviePass.Compute(YRZ::CeilDiv(w,8), YRZ::CeilDiv(movie->Height(),8), 1);
				g_dxr->ExecuteCommandList();
				convertTime = watch.Lap();
				/*テスト用
				D3D12_BOX box = {};
				box.right = movie->Width();
				box.bottom = movie->Height() / 2;
				g_dxr->Upload(movieTex, dec.data(), 0,0,0,0, &box);
				*/
				//YRZ::LOG(L"movie : dec{:.3}ms, up{:.3}ms, conv{:.3}ms", decodeTime*1e+3, uploadTime*1e+3, convertTime*1e+3);
			}
		} catch (...) {
			//例外が起きたら動画データは開放する
			movie = nullptr;
			g_edi.movieFile = "";
		}
	}

	//全部のエフェクトからimodel番のモデルについての材質注釈を除去する
	void PurgeMatDesc(int imodel) {
		int nModel = models.size();
		for (auto&& w : fxWatcher->watchList) {
			if (w.fx && w.fx->hasMatDesc()) {
				auto& items = w.fx->matDescs.items;
				items.erase(items.begin() + imodel);
			}
		}
	}

	//全部のエフェクトの材質注釈に末尾を追加する
	void AddMatDesc() {
		for (auto&& w : fxWatcher->watchList) {
			if (w.fx && w.fx->hasMatDesc()) {
				auto& desc = w.fx->matDescs;
				auto& items = desc.items;
				//新たに追加されたモデルの情報を追加
				YRZ::FXMatDesc m;
				m.sourceFile = desc.defaultFile;	//テンプレートに指定されたデフォルトファイルの内容で埋める
				std::vector<YRZ::FXMatDesc> mom(models.back().res->pmx->materials.size(), m);
				items.push_back(mom);
			}
		}
	}

	//モデルの名前一覧とマニュアルの更新
	void UpdateModelNames()
	{
		std::unordered_set<std::string> usedNames;
		for (auto&& mo : models) {
			auto r = mo.res->u8name;
			for (int i = 1;; i++) {
				if (usedNames.contains(r)) {
					r = mo.res->u8name + "(" +  std::to_string(i) + ")";
				} else {
					usedNames.insert(r);
					break;
				}
			}
			mo.u8name = r;

		}
	}

	//エフェクトリロード時の処理
	void FXReloaded(CPFlag cpflag = CPFlag::del)
	{
		//コントローラ辞書、モデルの数だけある
		//condicts[iModel][モーフ名] = モーフの最小値・最大値・デフォルト値の組み
		condicts.clear();
		condicts.resize(models.size());
		for (auto&& w : fxWatcher->watchList) {
			if (!w.fx)
				continue;
			for (const auto& c : w.fx->controllers) {
				//モーフ以外を対象としていたりスライダー設定がデフォルトの場合は飛ばす
				if (c.type != "float" || (c.slider == YRZ::FXSlider({}) && c.desc.empty()))
					continue;
				//コントローラ名が一致する物を探す
				if (c.controllerName == "(self)") {
					int imo = Find(w.id);
					if (imo >= 0)
						condicts[imo][c.item] = c;
				} else {
					for (int i = 0; auto&& mo : models) {
						if (c.controllerName == mo.ctrlname)
							condicts[i][c.item] = c;
						i++;
					}
				}
			}
		}

		//モーフデフォルト値の設定
		for (int i = 0; auto&& mo : models) {
			bool set0 = true;	//キーフレームが初期状態になっていてデフォルト値で埋める必要があるならtrue

			if (cpflag == CPFlag::add && i != models.size() - 1)
				set0 = false;
			if (cpflag == CPFlag::del)
				set0 = false;

			for (auto&& di : condicts[i]) {
				if (di.second.slider.defValue != 0.0f) {
					std::wstring wmorph = YRZ::L(di.first);
					mo.solver->SetDefaultMorphValue(wmorph, di.second.slider.defValue, set0);
					//YRZ::DEB8("default morph value set (#{} {}) : {}", i, di.first, di.second.defValue);
				}
			}
			i++;
		}

		//LoadConfigのデフォーマの更新
		std::vector<YRZ::FX*> deformers(models.size(), nullptr);
		for (int i = 0; auto&& mo : models) {
			auto fx = fxWatcher->Get(mo.id);
			if (fx && fx->category == YRZ::FXCategory::deform)
				deformers[i] = fx;
			i++;
		}

		//matdescに共有リソースやデフォーマ内のリソースの参照を持っている可能性があるのでアップデートしとく
		for (auto&& w : fxWatcher->watchList) {
			if (w.fx) {
				w.fx->usedConfig.deformers = deformers;
				w.fx->UpdateMatDescSharedTexture();
			}
		}

		//次回のレンダリング時にCBにフラグを立てる
		Message(RenderMsg::OnLoad);
	}

	//modelsの状態からレンダリング用パスを作る
	//appended : 新たに追加されたモデル番号
	void CreatePass(CPFlag cpflag)
	{
		m_passCreated = true;

		//バッファの作成。要素数0だった場合は大きさ1の空のバッファを作る
		auto createBufLambda = [&]<class T>(const std::vector<T>&v, const std::wstring& name, bool gpu = true) {
			YRZ::Buf b;
			if (v.empty()) {
				b = gpu ? g_dxr->CreateBuf(nullptr, sizeof(T), 1) : g_dxr->CreateBufCPU(nullptr, sizeof(T), 1);
			} else {
				b = gpu ? g_dxr->CreateBuf(v.data(), sizeof(T), v.size()) : g_dxr->CreateBufCPU(v.data(), sizeof(T), v.size());
			}
			b.SetName(name.c_str());
			return b;
		};

		skinUpdateReq = true;

		//テクスチャ読み込み位置を勘定する
		textureIndex.resize(models.size());
		for (int i = 0, t = 0; auto && mo : models) {
			textureIndex[i] = t;
			t += mo.res->textures.size();
			i++;
		}
		textureIndexBuf = createBufLambda(textureIndex, L"textureIndexBuf");

		if (g_dxr->RaytracingSupport()) {
			std::vector<YRZ::BLAS> blass;
			for (int i = 0; auto && mo : models) {
				//ポストプロセス付きモデルはTLASに加えない
				if (mo.res->drawable && (!mo.withPP)) {
					mo.blas.ID = i;	//ここでIDを振り直す
					blass.push_back(mo.blas);
				}
				i++;
			}
			tlas = g_dxr->BuildTLAS(blass.size(), blass.data());
			tlas.SetName(L"TLAS");
		}
		dmyTlas = g_dxr->CreateBuf(nullptr, 16, 1);

		//モデル番号⇔総材質番号バッファ
		UINT nModels = (UINT)models.size();
		UINT nTotalMats = 0;
		std::vector<XMUINT2>mo2mat(nModels);
		for (UINT i = 0; i < nModels; i++) {
			UINT nmat = (UINT)models[i].res->pmx->materials.size();
			mo2mat[i] = { nTotalMats, nmat };
			nTotalMats += nmat;
		}
		model2matBuf = createBufLambda(mo2mat, L"model2mat");

		//コンスタントバッファに↑の情報を入れる
		cb->modelCount = nModels;
		cb->totalMaterialCount = nTotalMats;
		
		std::vector<UINT>mat2mo(nTotalMats);
		size_t idx = 0;
		for (UINT i = 0; i < nModels; i++) {
			for (UINT j = 0; j < mo2mat[i].y; j++) {
				mat2mo[idx] = i;
				idx++;
			}
		}
		mat2modelBuf = createBufLambda(mat2mo, L"mat2model");

		//いないいないバッファ
		peekaboo.resize(models.size());
		peekabooBuf = createBufLambda(peekaboo, L"peekaboo");
		peekabooBufCPU = createBufLambda(peekaboo, L"peekabooCPU", false);
		peekabooBufCPU.res->Map(0, nullptr, (void**)&pPeekabooBuf);

		//材質選択中？バッファ
		matSelected.resize(nTotalMats, 0);
		matSelectedBuf = createBufLambda(matSelected, L"matSelected");

		//レンダリングパス作成(レンダラーのFXファイルに指定されるprepass、リソース参照の格納だけ)
		renderPass = YRZ::Pass(g_dxr);
		renderPass.SRV.resize(10);
		//renderPass.Samplers.push_back(samp);	//テスト用
		renderPass.CBV.push_back(&m_cb);
		renderPass.UAV.push_back({ &dxrout });				//u0
		renderPass.UAV.push_back({ &oidn.oidnBuf });		//u1
		renderPass.UAV.push_back({ &normalDepth });			//u2
		renderPass.UAV.push_back({ &gbuffer1});				//u3
		renderPass.UAV.push_back({ &gbuffer2});				//u4

		if (g_dxr->RaytracingSupport())
			renderPass.SRV[0].push_back(&tlas);				//t0
		else 
			renderPass.SRV[0].push_back(&dmyTlas);			//t0(レイトレ非対応環境用のSRV梅用ダミー)
		renderPass.SRV[0].push_back(&model2matBuf);			//t1
		renderPass.SRV[0].push_back(&mat2modelBuf);			//t2
		renderPass.SRV[0].push_back(&peekabooBuf);			//t3
		renderPass.SRV[0].push_back(&matSelectedBuf);		//t4
		renderPass.SRV[0].push_back(&skybox.skybox);		//t5

		renderPass.SRV[0].push_back(&skybox.skywalker);		//t6
		renderPass.SRV[0].push_back(&skybox.skywalkerRow);	//t7

		renderPass.SRV[0].push_back(&skybox.skyboxSH);		//t8
		renderPass.SRV[0].push_back(&screenBMP);			//t9
		

		static int currentModelChunk = nModelChunk;
		static int currentTexChunk = nTexChunk;

		//SRV[1]-[9]
		renderPass.SRV[1].push_back(&textureIndexBuf);
		size_t nTex = 0;	//モデルから使用されているテクスチャの総数
		for (auto&& m : models) {
			//モデルの数だけあるモノ
			for (int i = 0; auto&& t : m.res->textures) {
				if (m.res->screenTextureIdx.contains(i))
					renderPass.SRV[1].push_back(&screenBMP);
				else
					renderPass.SRV[1].push_back(&t);
				i++;
			}
			nTex += m.res->textures.size();
			renderPass.SRV[2].push_back(&m.skinnedBuf);
			renderPass.SRV[3].push_back(&m.res->IB);
			renderPass.SRV[4].push_back(&m.materialBuf);
			renderPass.SRV[5].push_back(&m.res->face2MaterialBuf);
			renderPass.SRV[6].push_back(&m.res->material2FaceBuf);
			renderPass.SRV[7].push_back(&m.res->faceWalkerBuf);
			renderPass.SRV[8].push_back(&m.prevSkinnedBuf);
			renderPass.SRV[9].push_back(&m.res->VB);
		}

		//チャンクに納まらなくなったら拡張
		while (nTex > currentTexChunk)
			currentTexChunk += nTexChunk;
		//チャンクの空きを埋める
		for (int i = nTex; i < currentTexChunk; i++) {
			renderPass.SRV[1].push_back(&screenBMP);
		}
		//チャンクに納まらなくなったら拡張する
		while (models.size() > currentModelChunk)
			currentModelChunk += nModelChunk;
		//チャンクの空きを埋める
		for (int i = models.size(); i < currentModelChunk; i++) {
			renderPass.SRV[2].push_back(&dmyTlas);
			renderPass.SRV[3].push_back(&dmyTlas);
			renderPass.SRV[4].push_back(&dmyTlas);
			renderPass.SRV[5].push_back(&dmyTlas);
			renderPass.SRV[6].push_back(&dmyTlas);
			renderPass.SRV[7].push_back(&dmyTlas);
			renderPass.SRV[8].push_back(&dmyTlas);
			renderPass.SRV[9].push_back(&dmyTlas);
		}
		
		//SetWindowTextW(g_app->hWnd(), std::format(L"scene stat. models:{}, mats:{}, texs:{}", models.size(), cb->totalMaterialCount, nTex).c_str());
	
		//背景合成と材質ハイライトパス
		backgroundPass = YRZ::Pass(g_dxr);
		backgroundPass.SRV[0].push_back(&ppOut);
		backgroundPass.SRV[0].push_back(&screenBMP);
		backgroundPass.SRV[0].push_back(&gbuffer1);
		backgroundPass.SRV[0].push_back(&matSelectedBuf);
		backgroundPass.SRV[0].push_back(&model2matBuf);
		backgroundPass.SRV[1] = renderPass.SRV[5];
		backgroundPass.CBV = { &m_cb };
		backgroundPass.Samplers = { samp };
		backgroundPass.RTV = { { &dayoOut, YRZ::CV(0,0,0,1) }, {&imageOut, YRZ::CV(0,0,0,0)} };
		auto vsBG = LoadShader(Recompile, g_dxr, L"SystemBackgroundVS", L"hlsl\\system\\background.hlsl", L"VS", L"vs_6_1", CompileOption);
		auto psBG = LoadShader(Recompile, g_dxr, L"SystemBackgroundPS", L"hlsl\\system\\background.hlsl", L"PS", L"ps_6_1", CompileOption);
		backgroundPass.PostProcessPass(vsBG, psBG);
		backgroundPass.Update();

		//スキニング用パス作成
		auto csBlob = LoadShader(Recompile, g_dxr, L"SystemDefaultSkinningCS", L"hlsl\\system\\DefaultSkinning.hlsl", L"CS", L"cs_6_1", CompileOption);
		skinPass.resize(models.size());
		for (int i = 0; auto && m : models) {
			skinPass[i] = YRZ::Pass(g_dxr);
			skinPass[i].CBV = { &m_cb };
			skinPass[i].SRV.resize(10);
			skinPass[i].UAV.push_back({ &m.skinnedBuf });			//u0 変換後の頂点バッファ
			skinPass[i].SRV[0].push_back(&m.res->VB);				//t0 変換前頂点バッファ
			skinPass[i].SRV[0].push_back(&m.res->skinBuf);			//t1 スキニング情報
			skinPass[i].SRV[0].push_back(&m.boneBuf);				//t2 各ボーンの変換行列
			skinPass[i].SRV[0].push_back(&m.morphValuesBuf);		//t3 モーフ値
			skinPass[i].SRV[0].push_back(&m.res->morphTableBuf);	//t4 モーフ番号と位置・UVへの影響を書いたテーブル
			skinPass[i].SRV[0].push_back(&m.res->morphPointerBuf);	//t5 各頂点が↑のテーブルのどこを読めばいいのか書いた物
			skinPass[i].SRV[0].push_back(&model2matBuf);			//t6 
			skinPass[i].SRV[0].push_back(&mat2modelBuf);			//t7
			skinPass[i].SRV[0].push_back(&peekabooBuf);				//t8

			//space1,3-9をコピー(2はスキニングから参照できないので)
			skinPass[i].SRV[1] = renderPass.SRV[1];
			for (int j = 3; j <= 9; j++) {
				skinPass[i].SRV[j] = renderPass.SRV[j];
			}
			skinPass[i].ComputePass(csBlob);
			i++;
		}


		//材質注釈用
		std::vector<UINT>materialCount(models.size());
		for (int i = 0;  auto && c : materialCount) {
			c = models[i].res->pmx->materials.size();
			i++;
		}
		
		//ロードコンフィグの設定
		commonLoadConfig = defaultLoadConfig;
		commonLoadConfig.materialCount = materialCount;
		commonLoadConfig.rasterVB.resize(models.size());
		for (int i = 0; auto && v : commonLoadConfig.rasterVB) { v = &models[i].skinnedBuf; i++; }
		commonLoadConfig.rasterIB.resize(models.size());
		for (int i = 0; auto && v : commonLoadConfig.rasterIB) { v = &models[i].res->IB; i++; }
		commonLoadConfig.rasterOrder = g_edi.rasterOrder;


		//ポストプロセス用プレパスとselfIndexの更新
		prepassPP = renderPass;
		prepassPP.SRV[0].push_back(&ppIn);	//t12にppInを入れる
		for (int i = 0; auto && mo:models) {
			auto w = fxWatcher->Find(mo.id);
			if (w && w->category == YRZ::FXCategory::postprocess && w->fx) {
				w->fx->usedConfig.selfIndex = i;
			}
			i++;
		}


		//デフォーマ用プレパスの差し替え。プレパスに指定されるリソースがモデル毎に違うので
		std::vector<int> delayedDeformerID;	//遅延ロードの対象になるデフォーマリスト
		for (int i = 0; auto && mo : models) {
			auto w = fxWatcher->Find(mo.id);
			if (w && w->category == YRZ::FXCategory::deform) {
				YRZ::FXLoadConfig cfg = commonLoadConfig;
				cfg.definedPass = &skinPass[i];
				cfg.deformOutputCount = mo.res->pmx->vertices.size();
				cfg.selfIndex = i;
				cfg.deformIndex = i;
				cfg.deformOrder = i;	//仮にセット。Renderの時に書き換える
				if (w->fx)
					w->fx->usedConfig = cfg;
				else
					delayedDeformerID.push_back(mo.id);	//読み込まれていないデフォーマが有った場合、後で読み込むリストに入れる
				w->config = cfg;	//遅延ロード時やコンパイルエラーからの復帰などw->fxが無い場合、w.configが使われるのでw.confgにも入れとく
			}
			i++;
		}

		//ポストプロセス・デフォーマの材質注釈・ラスタライザの再設定用設定
		for (auto&& w : fxWatcher->watchList) {
			if (w.fx && (w.fx->category == YRZ::FXCategory::postprocess || w.fx->category == YRZ::FXCategory::deform)) {
				w.fx->usedConfig.materialCount = materialCount;
				w.fx->usedConfig.rasterVB = commonLoadConfig.rasterVB;
				w.fx->usedConfig.rasterIB = commonLoadConfig.rasterIB;
				w.fx->usedConfig.rasterOrder = commonLoadConfig.rasterOrder;
			}
		}

		//レンダラ用エフェクトの設定
		{
			YRZ::FXLoadConfig cfg = commonLoadConfig;
			cfg.definedPass = &renderPass;

			//まだ生成されてない場合はレンダラ用エフェクト生成
			if (fxWatcher->Find(WID_RENDERER) == nullptr)
				fxWatcher->StartWatch(WID_RENDERER, YRZ::FXCategory::render, Renderers[RendererIndex], cfg);
			if (fxWatcher->Find(WID_BAR) == nullptr)
				fxWatcher->StartWatch(WID_BAR, YRZ::FXCategory::render, fs::path(BasePath / (L"renderer\\colorbar" + DayoFXExtension)), cfg);
			
			auto renderer = fxWatcher->Get(WID_RENDERER);
			if (renderer) {
				renderer->usedConfig.materialCount = materialCount;
				renderer->usedConfig = cfg;
			}
		}

		//エフェクトのリロード
		fxWatcher->UpdateDefinedPass();	

		for (auto id : delayedDeformerID) {
			fxWatcher->DelayedLoad(id);
		}


		FXReloaded(cpflag);

		
		//モデルの表示名の設定
		UpdateModelNames();
	
	}

	//timeにおける状態をモデル群の姿勢に反映
	void Solve(float time, bool integerTime = true, bool contPhys = false)
	{
		skinUpdateReq = true;

		//マルチスレッドで並列にすると外部親の解決が順不同になってしまう(というか解決できない)ので逐次計算するようにした
		//#pragma omp parallel for	
		for (int i : g_edi.motionOrder) {
			auto& mo = models[i];
			mo.Solve(time, integerTime, contPhys);
			mo.kc.pose = mo.solver->solvedPose;	//編集中の未登録ボーンは捨てられる
		}

		GetMovieFrame(time);

		//↑計算順によっては外部親の子の動きは1F遅れになる、というのが本家通りっぽい
	}

	//全モデルのポーズの表示状態を更新。
	//phys : true->PhysicsUpdate(physTime)を行って物理の状態を反映する, false->物理抜きの表示状態にする
	//iModel : 更新の原因になった編集中モデル番号
	//physTime : 物理の時間をどれだけ進めるか(0だと物理が反映されない)
	void PoseUpdate(bool phys = true, int iModel = -1, float physTime = 1/30.0f)
	{
		skinUpdateReq = true;
		//#pragma omp parallel for
		for (int i : g_edi.motionOrder) {
			auto& mo = models[i];
			mo.solver->PoseMining(mo.kc.pose);
		}
		if (phys) {
			//物理付けはsolver.solvedPoseを元に付けられるので、編集中モデルがある場合は一時的にsolvedPoseに編集のポーズを設定する
			if (iModel >= 0) {
				auto backup = models[iModel].solver->solvedPose;
				models[iModel].solver->solvedPose = models[iModel].kc.pose;
				UpdatePhysics(physTime);
				models[iModel].solver->solvedPose = backup;
			} else {
				UpdatePhysics(physTime);
			}
		}
	}

	//外部親解決のための情報を更新する(AddModel, DeleteModelから呼ばれる)
	void UpdateExternalSolver() {
		for (auto&& mo : models) {
			mo.solver->externalSolver.clear();
			for (auto&& ex : models) {
				mo.solver->externalSolver[ex.id] = ex.solver.get();
			}
		}
	}

	//モデル追加 
	//createPassがtrueの時、バッファやパスの構築をする。連続してモデルを読み込む場合は最後だけcreatePassすると良い
	//fxidはLoadDayo時に指定されるID、デフォルトでは自動割り当てID
	//成功した場合はtrue
	bool AddModel(std::wstring filename, bool createPass = true, int fxid = -1)
	{
		Model* src = nullptr;

		//既に同じPMXファイルがロードされてるか調べる ※MMDayo起動後にモデルが更新されている可能性を考慮していない
		for (auto && mo : models) {
			if (mo.pmxfile == filename) {
				src = &mo;
				break;
			}
		}

		//既に読み込まれているpmxファイルなら、それを借りる
		//読み込まれていないpmxなら新しくロードする
		//IDはfxidで指定されている場合(Load時)はfxid。そうでない場合は自動割り当てID
		Model m;
		m.kc.id = m.id = (fxid>=0) ? fxid : nModelLoaded;
		try {
			if (src == nullptr)
				m.LoadPMX(filename);
			else
				m.SetPMX(src->res, filename);
		} catch(std::exception ex) {
			OKDlgA("Add Model Failed", ex.what());
			return false;
		}

		//同名のfxファイルがあるならポストプロセスorデフォーマとして読み込む
		fs::path fxp = filename;
		fxp = fxp.replace_extension(DayoFXExtension);
		if (std::filesystem::exists(fxp)) {
			int id = m.id;
			//fxpをソースとしてコンパイルして出来たFXオブジェクトを監視対象に入れる
			//ソースファイルが更新されたらm.postprocessも自動的に更新される
			//m.postprocessはfxWatcher->Poll()をやる都度 nullptrになる可能性もあるので、それは留意すべし
			try {
				auto cat = YRZ::FX::CategoryFromFile(fxp);
				if (cat == YRZ::FXCategory::postprocess) {
					auto cfg = defaultLoadConfig;
					cfg.definedPass = &prepassPP;
					cfg.selfIndex = models.size();
					for (auto&& mo: models)
						cfg.materialCount.push_back((UINT)mo.res->pmx->materials.size());
					m.withPP = true;
					fxWatcher->StartWatch(id, YRZ::FXCategory::postprocess, fxp, cfg);
				} else if (cat == YRZ::FXCategory::deform) {
					m.withDeform = true;
					//ここではLoadConfigを設定しない。後で決定する
					fxWatcher->StartWatch(id, YRZ::FXCategory::deform, fxp, {}, true);	//遅延ロードフラグを立てて今すぐコンパイルしない
				}
			} catch (cereal::RapidJSONException ex) {
				auto err = YRZ::FX::ReportJSONError(fxp);
				OKDlgA("json error", err.c_str());
			} catch (std::exception ex) {
				OKDlgA("fx error", ex.what());
			}
		}
		//LoadConfigのうち材質注釈はCreatePassで設定する


		models.push_back(std::move(m));

		//順序テーブル更新
		g_edi.motionOrder.push_back(models.size() - 1);
		g_edi.postprocessOrder.push_back(models.size() - 1);
		g_edi.deformOrder.push_back(models.size() - 1);
		g_edi.rasterOrder.push_back(models.size() - 1);

		//監視リスト内の各エフェクトに、このモデルの分の材質注釈を追加
		AddMatDesc();
		if (createPass) {
			CreatePass(CPFlag::add);
			UpdateExternalSolver();
		}

		nModelLoaded++;
		modelsUpdatedT = timeGetTime();
		return true;
	}
	//モデル削除
	void DeleteModel(int idx, bool createPass = true)
	{
		//エフェクトが割り当てられているなら削除
		fxWatcher->EndWatch(models[idx].id);	//watchIDが無効な場合何もしないのでこれでいい

		int id = models[idx].id;
		g_undoBuffer.Purge(id);
		models.erase(models.begin() + idx);
		UpdateExternalSolver();
		//既に登録されているキーで外部親として指定されているモデルだった場合、当該外部親キーを削除する
		for (auto&& mo : models) {
			mo.solver->PurgeExternalKeys(id);
		}
		//監視リスト内の各エフェクトの、このモデルについての材質注釈を削除
		PurgeMatDesc(idx);

		//順序テーブルから削除
		auto eraseOrderLambda = [](auto& v, auto idx) {
			int u = v[idx];
			for (auto&& item : v)
				if (item > u)
					item--;
			v.erase(v.begin() + idx); 
		};
		eraseOrderLambda(g_edi.motionOrder, idx);
		eraseOrderLambda(g_edi.postprocessOrder, idx);
		eraseOrderLambda(g_edi.deformOrder, idx);
		eraseOrderLambda(g_edi.rasterOrder, idx);

		if (createPass)
			CreatePass(CPFlag::del);

		modelsUpdatedT = timeGetTime();
	}

	//IDからモデル番号に変換。無効なIDだったら-1を返す
	int Find(int id)
	{
		for (int i = 0; auto && mo : models) {
			if (id == mo.id) {
				return i;
			}
			i++;
		}
		return -1;
	}

	//物理を落ち着けてアニメーションの準備をする
	//まだ良く分かってないが、solve->Update(1F)->solve->Resetが大事みたい... interpolation**Velocityをクリアしたら解決できた
	void CalmPhysics()
	{
		/* //初期の案
		Solve(AnimFrame);
		UpdatePhysics(1 / 30.0);
		Solve(AnimFrame);
		g_physics.Reset();	//運動量などを0にする
		//↓の2回目のUpdateでようやく落ち着くらしい
		//モーションブラー有り録画の場合はこれ無しで録画に入る可能性があるのでこの関数内でここまで実行する
		Solve(AnimFrame);
		UpdatePhysics(1 / 30.0);
		*/
		
		/* //Bullet3のデフォルトジョイント設定で正しく動くが、MMD互換ジョイント設定では崩れる版
		g_physics.Reset();
		//最低一回はやった方がいいみたい
		for (int i = 0; i < g_cfg->physicsPrewarmSteps; i++) {
			UpdatePhysics(1 / 30.0);
		}
		*/

		
		/*
		//物理互換性アプデ後、0Fに真後ろをセットしてあるようなモーション再生時にジョイントが壊れる事象が出るようになったので対策した版
		//…しかし、この後にUpdateループ入れるとまた崩れてしまう
		g_physics.Reset();
		//最低一回はやった方がいいみたい
		for (int i = 0; i < g_cfg->physicsPrewarmSteps; i++) {
			UpdatePhysics(1 / 30.0);
		}
		Solve(AnimFrame);
		g_physics.Reset();
		*/
		
		//物理互換性アプデ後、0Fでいきなり真後を向くようセットしてあるようなモーション再生時にジョイントが壊れる事象が出るようになった
		//色々試したが結局再生直前に安定する最新ジョイント設定にして、物理安定後に互換ジョイント設定に戻すことにした
		for (auto&& mo : models)
			mo.StableJoints();

		g_physics.Reset();
		//最低一回はやった方がいいみたい
		for (int i = 0; i < g_cfg->physicsPrewarmSteps; i++) {
			UpdatePhysics(1 / 30.0);
		}
		
		for (auto&& mo : models)
			mo.CompatibleJoints();

	}


	//エフェクトのrasterOtderを現状に合わせる
	void SetRasterOrder()
	{
		auto renderer = fxWatcher->Get(WID_RENDERER);
		if (renderer) {
			renderer->usedConfig.rasterOrder.clear();
			//表示モデルかつPPなしならラスタライズする
			for (auto&& o : g_edi.rasterOrder)
				if (peekaboo[o] && (!models[o].withPP))
					renderer->usedConfig.rasterOrder.push_back(o);
		}
		
		for (int i = 0;  auto && mo : models) {
			//デフォーマ
			if (mo.withDeform) {
				auto* fx = fxWatcher->Get(mo.id);
				if (fx) {
					auto& order = fx->usedConfig.rasterOrder;
					order.clear();
					for (auto&& o : g_edi.rasterOrder)
						if (peekaboo[o] && (!models[o].withPP))
							order.push_back(o);
				}
			}
			//PP
			if (mo.withPP) {
				auto* fx = fxWatcher->Get(mo.id);
				if (fx) {
					auto& order = fx->usedConfig.rasterOrder;
					order.clear();
					//自分モデルはラスタライズする
					for (auto&& o : g_edi.rasterOrder)
						if (peekaboo[o] && (!models[o].withPP || (i==o)))
							order.push_back(o);
				}
			}
			i++;
		}

	}

	//言語の変更に伴うラベルの更新
	void ResetLanguage()
	{
		for (auto&& mo : models) {
			mo.res->ResetLanguage();
			mo.kc.Reset();
		}
		UpdateModelNames();
	}

	//メッセージをポストする。次回Render時にCBにセットされる
	void Message(RenderMsg msg)
	{
		m_msgQRender.insert(msg);
		m_msgQDeform.insert(msg);
		m_msgQPP.insert(msg);
	}

	//レンダリング
	//denoise : UIでデノイズのチェックが入ってるか？ ... DENOISEで始まるパスをスキップする
	//skip : レンダリング全体をスキップする？
	//PP : ポストプロセスを実行する
	//copySkin : 現在のスキニング結果をprevSkinnedBufにコピーする(skipがtrueの場合は実行されない)
	void Render(bool denoise, bool skip = false, bool PP = true, bool copySkin = true)
	{
		auto& io = ImGui::GetIO();
		cb->mouseDown = 0;
		cb->mouseDown += io.MouseDown[0] ? 1 : 0;
		cb->mouseDown += io.MouseDown[1] ? 2 : 0;
		cb->mouseDown += io.MouseDown[2] ? 4 : 0;
		cb->mouseClicked = 0;
		cb->mouseClicked += ImGui::IsMouseClicked(ImGuiMouseButton_Left) ? 1 : 0;
		cb->mouseClicked += ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? 2 : 0;
		cb->mouseClicked += ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ? 4 : 0;

		//deltaTime計算用
		//現在の仕様ではplay/recordボタンが押された直後のフレームでは必ずデフォーマが実行される
		//ポストプロセスについてはモーションブラーがonになっている場合は最終サンプルが描画された時に初めて実行される
		static float prevRenderTime = 0;	//前回Renderが呼ばれた時刻(RealTime)
		static float prevDeformTime = 0;	//
		static float prevPPTime = 0;
		static float prevRenderFrame = 0;	//前回Renderが呼ばれた時のフレーム番号
		static float prevDeformFrame = 0;	//
		static float prevPPFrame = 0;

		//play/recordが押された直後である
		if (m_msgQRender.contains(RenderMsg::OnStart)) {
			prevRenderTime = 0;
			prevDeformTime = 0;
			prevPPTime = 0;
			prevRenderFrame = cb->frameTime;
			prevDeformFrame = cb->frameTime;
			prevPPFrame = cb->frameTime;
		}
		//TimeにRealTimeの方を使う？
		bool realtime = (AlwaysSolve && !(Animation || Recording));
		cb->time = realtime ? cb->realTime : cb->frameTime;

		//レンダリングスキップモードの場合でもバックバッファのクリアだけはやる(ImGuiのため)
		if (skip) {
			g_dxr->OpenCommandList();
			clearBBPass.Render();
			g_dxr->ExecuteCommandList();
			return;
		}

		//コントローラから値を得る
		for (auto&& w : fxWatcher->watchList) {
			if (w.fx) {
				w.fx->UpdateController();
			}
		}

		//エフェクトのラスタライザ起動順を現状に合わせる
		SetRasterOrder();

		//メッセージ処理, ms内の全メッセージを取り出してcbに1をセットする。該当メッセージが無い場合は0をセット
		//どれかのイベントが発生して場合はtrueを返す
		auto messageLambda = [&](auto& queue, const std::vector<RenderMsg>& ms) {
			bool ret = false;
			for (auto&& msg : ms) {
				if (queue.contains(msg)) {
					*m_msg2CB[msg] = 1;
					queue.erase(msg);
					ret = true;
				} else {
					*m_msg2CB[msg] = 0;
				}
			}
			return ret;
		};

		//時刻処理
		//cbのd*Timeのセット
		auto timeLambda = [&](float& prevT, float& prevF) {
			cb->dFrameTime = cb->frameTime - prevF;
			cb->dRealTime = cb->realTime - prevT;
			cb->dTime = realtime ? cb->dRealTime : cb->dFrameTime;
			prevF = cb->frameTime;
			prevT = cb->realTime;
		};


		g_dxr->OpenCommandList();

		//エフェクトのMatDescにコントローラの値を反映させる
		for (auto&& w : fxWatcher->watchList) {
			if (w.fx) {
				w.fx->ResolveMatDescController();
			}
		}

		//スキニング
		if (skinUpdateReq) {
			skinUpdateReq = false;
			timeLambda(prevDeformTime, prevDeformFrame);
			bool anyEvent = messageLambda(m_msgQDeform, AllMsg );	//何かイベントが起こった

			//solveごとに更新されるリソースの転送
			for (auto&& m : models) {
				if (m.res->drawable) {
					//ボーン行列
					size_t bytes = sizeof(XMMATRIX) * m.solver->boneMatrices.size();
					memcpy(m.pBoneBuf, m.solver->boneMatrices.data(), bytes);
					g_dxr->CopyBufferRegion(m.boneBuf, 0, m.boneBufCPU, 0, bytes);
					//モーフ値
					if (m.solver->morphValues.size() > 0) {
						bytes = sizeof(float) * m.solver->morphValues.size();
						memcpy(m.pMorphValuesBuf, m.solver->morphValues.data(), bytes);
						g_dxr->CopyBufferRegion(m.morphValuesBuf, 0, m.morphValuesBufCPU, 0, bytes);
					}
					//マテリアル
					bytes = sizeof(PMX::Material) * m.solver->materials.size();
					memcpy(m.pMaterialBuf, m.solver->materials.data(), bytes);
					g_dxr->CopyBufferRegion(m.materialBuf, 0, m.materialBufCPU, 0, bytes);
				}
			}

			//コンピュートシェーダでスキニングと頂点UVモーフ
			std::vector<YRZ::BLAS>blass;
			for (int n = 0; int i : g_edi.deformOrder) {
				auto& mo = models[i];
				auto& p = skinPass[i];
				//ポストプロセス付きモデルと頂点無しモデルは対象外
				if (models[i].res->drawable && !(models[i].withPP)) {
					//不可視状態でもイベントが発生していた場合はデフォーマを起動する
					if (models[i].solver->visible || anyEvent) {
						auto deform = fxWatcher->Get(models[i].id);
						if (deform && deform->category == YRZ::FXCategory::deform) {
							try {
								deform->usedConfig.deformOrder = n;
								deform->Render();
							} catch (std::exception ex) {
								OKDlgA("deform error", ex.what());
								auto w = fxWatcher->Find(models[i].id);
								w->fx = nullptr;
							}
						} else {
							p.SetRootConst(2, i);
							p.SetRootConst(3, n);
							auto nThread = YRZ::CeilDiv(models[i].res->pmx->vertices.size(), 1024ull);
							p.Compute(nThread, 1, 1);
						}
						if (g_dxr->RaytracingSupport())
							g_dxr->UpdateBLAS(mo.blas, mo.skinnedBuf, mo.res->IB);
					}
					if (mo.solver->visible) {
						mo.blas.mask = 0xFF;
					} else {
						mo.blas.mask = 0;
					}
					blass.push_back(mo.blas);
				}
				n++;
			}
			if (g_dxr->RaytracingSupport())
				g_dxr->UpdateTLAS(tlas, blass.size(), blass.data());


			//いないいない
			for (int i = 0; auto && mo : models) {
				peekaboo[i] = mo.solver->visible ? 1 : 0;
				i++;
			}
			if (!peekaboo.empty()) {
				size_t bytes = peekabooBuf.desc().Width;
				memcpy(pPeekabooBuf, peekaboo.data(), bytes);
				g_dxr->CopyBufferRegion(peekabooBuf, 0, peekabooBufCPU, 0, bytes);
			}
		}

		//レンダリング開始！

		//バックバッファのクリア
		clearBBPass.Render();

		//dxroutに現在のフレームを作る(UAVで書き込み)
		auto renderer = fxWatcher->Get(WID_RENDERER);
		timeLambda(prevRenderTime, prevRenderFrame);
		messageLambda(m_msgQRender, AllMsg);
		if (renderer) {
			try {
				renderer->Render();
			} catch (std::exception ex) {
				OKDlgA("render error", ex.what());
				auto w = fxWatcher->Find(WID_RENDERER);
				w->fx = nullptr;
			}
		} else {
			//レンダラがコンパイルエラーなどで読み込めない時はカラーバーを出す
			auto bar = fxWatcher->Get(WID_BAR);
			if (bar)
				bar->Render();
		}


		bool denoiseFailed = false;
		if (denoise) {
			//OIDNでデノイズ
			g_dxr->ExecuteCommandList();

			oidnExecuteFilterAsync(oidn.filter);
			const char* errorMessage;
			if (oidnGetDeviceError(oidn.odev, &errorMessage) != OIDN_ERROR_NONE) {
				YRZ::DEBA("{}", errorMessage);
				denoiseFailed = true;
			}

			oidnSyncDevice(oidn.odev);

			//OIDNからの出力をppOutへ
			g_dxr->OpenCommandList();
			if (!denoiseFailed)
				oidnPass.Render(W, H);
		} else {
			//デノイズしない場合、ppOutにdxroutをそのままコピー
			g_dxr->CopyResource(ppOut, dxrout);
		}

		//デノイザが失敗した場合もコピー処理を走らせる
		if (denoise && denoiseFailed)
			g_dxr->CopyResource(ppOut, dxrout);

		//ポストプロセス。ppInを処理してppOutへ出力
		if (PP) {
			//CBの書き換えを行うので一旦コマンドリスト閉じて再開する
			g_dxr->ExecuteCommandList();
			g_dxr->OpenCommandList();
			timeLambda(prevPPTime, prevPPFrame);
			bool anyEvent = messageLambda(m_msgQPP, AllMsg);
			for (int i : g_edi.postprocessOrder) {
				//非表示状態ならポストプロセスを実行しないがイベントが発生していたら実行する(ppOutには後述の処理により影響しない)
				if (peekaboo[i] || anyEvent) {
					auto& mo = models[i];
					g_dxr->CopyResource(ppIn, ppOut);	//今までのppOutをppInへコピーして入力とす
					auto pp = fxWatcher->Get(mo.id);
					if (pp && pp->category == YRZ::FXCategory::postprocess) {
						try {
							pp->Render();
						} catch (std::exception ex) {
							OKDlgA("postprocess error", ex.what());
							auto w = fxWatcher->Find(mo.id);
							w->fx = nullptr;
						}
					}
					//非表示だがイベントが原因で起動された場合、ポストプロセス実行前の状態に巻き戻す
					if (!peekaboo[i])
						g_dxr->CopyResource(ppOut, ppIn);
				}
			}
		}

		//ppOutをトーンマッピングしてdayoOutへ
		backgroundPass.Render(W, H);

		//次のフレームのためにscreenBMPを作る
		screenPass.Render(W, H);

		//レンダリング終了！
		g_dxr->ExecuteCommandList();

		//現在の頂点バッファを前の頂点バッファとしてコピー
		if (copySkin) {
			g_dxr->OpenCommandList();
			for (int i = 0; auto&& mo : models) {
				g_dxr->CopyResource(mo.prevSkinnedBuf, mo.skinnedBuf);
			}
			g_dxr->ExecuteCommandList();
		}

	}
};


//エフェクトから構造体のサイズを尋ねるコールバック関数
//一般的な型はYRZFXでカバーがそうでない物はホストアプリケーションに問い合わせがくる
//エフェクト内で定義されている構造体の場合はelemSizeの明示的な指定が不可避
UINT ElemSizeQuery(const std::string& type)
{
	//名前空間を剥いで判定する
	auto t = type;
	auto idx = t.find("::");
	if (idx != std::string::npos) {
		t = t.substr(idx + 2);
	}

	const std::unordered_map<std::string, UINT> dict = {
		{ "Vertex", sizeof(Vertex) },
		//{ "VertexEx", sizeof(VertexEx) },
		{ "MMDMaterial", sizeof(PMX::Material) },
		{ "WalkersWorker", sizeof(WalkersWorker) },
		{ "WalkersAlias", sizeof(WalkersAlias) },
		{ "DualQ", sizeof(XMFLOAT4)*2 },
		{ "SHCoeff", sizeof(XMFLOAT4)*9 },
		{ "OIDNInput", sizeof(OIDNInput)},
		{ "MaterialFace", sizeof(MaterialFace)},
		{ "Skinning", sizeof(PMX::GPUSkinning)},
		{ "MorphItem", sizeof(PMX::GPUMorphItem)},
		{ "MorphPointer", sizeof(PMX::GPUMorphPointer)},
		{ "Attribute", sizeof(XMFLOAT2)}
	};

	if (dict.contains(t)) {
		return dict.at(t);
	}

	return 0;
}

//エフェクトからコントローラについての所在を問い合わせるコールバック関数
//入力
//selfIndex : (self)が表すモデル番号(IDではなく、あくまで現状のmodels[]の添え字)
//doppelIndex : 同名のpmxのうち、何台目のpmxか
//controllerName : コントローラのpmxファイル名(UTF-8)
//item : コントローラの項目(モーフ名やボーン名)
//type : 変数の型
//出力
//modelIndex : コントローラのモデル番号(ない場合は-1)
//itemIndex : モーフやボーンの番号(ない場合は-1)
void ControllerQuery(int selfIndex, int doppelIndex, const std::string& controllerName, const std::string& item, const std::string& type, int& modelIndex, int& itemIndex)
{
	modelIndex = -1;
	itemIndex = -1;

	//小文字に変換したコントローラ名
	//dayo.modelsのctrlnameは小文字になっているので小文字に揃える
	auto contname = YRZ::LowerStr(controllerName);

	int iDoppel = 0;
	for (int i = 0; auto && m : g_dayo->models) {
		if ((selfIndex == i && controllerName == "(self)")) {
			modelIndex = i;
			break;
		}
		if (m.ctrlname == contname) {
			if (iDoppel == doppelIndex) {
				modelIndex = i;
				break;
			}
			iDoppel++;
		}
		i++;
	}

	if (modelIndex < 0)
		return;

	auto& m = g_dayo->models[modelIndex];
	auto wi = YRZ::L(item);
	
	if (type == "float") {
		//floatならモーフ番号を返す
		if (m.solver->morphDict.contains(wi)) {
			itemIndex = m.solver->morphDict[wi];
		}
	} else if (type == "bool") {
		//コントローラまたはボーン・モーフ名の有無を返す
		// ほぼモーフなどの値に依存しない情報なので、現時点で値は確定出来る物が多い
		// itemIndex = 1ならTrue, 0以下ならFalse
		// itemIndex = 2ならモデルのshow状態
		if (item.empty()) {
			//itemが空文字ならshow/hide状態、Callbackで具体的な値を決める
			itemIndex = 2;	//show/hide
		} else if (item == "(exists)") {
			//コントローラがあるか？自動的にtrue
			itemIndex = 1;
		} else if (m.solver->morphDict.contains(wi)) {
			//モーフが有ればtrue
			itemIndex = 1;
		} else if (m.solver->boneDict.contains(wi)) {
			//ボーンが有ればtrue
			itemIndex = 1;
		} else {
			//問い合わせられたアイテムは存在しない、false
			itemIndex = 0;
		}
	} else if (type == "float3" || type == "float4x4") {
		//ボーンの位置・姿勢
		if (m.solver->boneDict.contains(wi)) {
			itemIndex = m.solver->boneDict[wi];
		}
	} else if (type == "int") {
		//intの場合、モーフの値など毎フレーム変わる情報は無いのでこの時点で返り値は確定する
		//itemIndexに返り値を格納する
		if (item.empty()) {
			//モデルの番号
			itemIndex = modelIndex;
		} else if (item == "(exists)") {
			//同名のモデルが何個あるか
			itemIndex = std::count_if(g_dayo->models.begin(), g_dayo->models.end(), [&](const auto& mo) {return mo.ctrlname == contname; });
		} else {
			//ボーン・モーフの番号。モーフとボーンで同名の物が有ればモーフ優先
			if (m.solver->boneDict.contains(wi)) {
				itemIndex = m.solver->boneDict[wi];
			}
			if (m.solver->morphDict.contains(wi)) {
				itemIndex = m.solver->morphDict[wi];
			}
		}
	}
}


//MatDescからコントローラの値を得るためのコールバック。↑で得たインデックスから値を得る
void ControllerCallback(void* value, int modelIndex, int itemIndex, const std::string& type)
{
	//無効なモデル・アイテム番号が指定されていたらデフォルト値をセットして帰る
	if (modelIndex < 0 || itemIndex < 0 || modelIndex >= g_dayo->models.size()) {
		if (type == "float") {
			*(float*)value = 0;
		} else if (type == "bool") {
			*(int*)value = 0;
		} else if (type == "float3") {
			*(vec3*)value = { 0,0,0 };
		} else if (type == "float4x4") {
			*(Matrix*)value = XMMatrixIdentity();
		} else if (type == "int") {
			*(int*)value = -1;
		}
		return;
	}

	auto& mo = g_dayo->models[modelIndex];
	if (type == "float") {
		float* f = (float*)value;
		*f = 0;

		//モデル読み込み直後でposeがない場合にエラーが出る可能性があるので
		auto& keys = mo.kc.pose.morphKeys;
		if (itemIndex >= keys.size())
			return;

		*f = g_dayo->models[modelIndex].kc.pose.morphKeys[itemIndex].value;
	} else if (type == "bool") {
		int* b = (int*)value;
		if (itemIndex <= 1) {
			*b = (itemIndex >= 1);
		} else {
			*b = mo.kc.pose.extraKey.show;
		}
	} else if (type == "float3") {
		vec3* v = (vec3*)value;
		auto b = mo.solver->bones[itemIndex];
		*v = Vector3::Transform(b->origin, b->transform);
	} else if (type == "float4x4") {
		Matrix* m = (Matrix*)value;
		*m = mo.solver->boneMatrices[itemIndex];
		//*m = mo.solver->bones[itemIndex]->transform;
	} else if (type == "int") {
		int* i = (int*)value;
		*i = itemIndex;
	}
}
