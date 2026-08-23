#pragma once
#include "defsDayo.h"

//材質注釈ファイル設定ウィンドウ

//モデル1体分のmatdescファイル
struct ModelMatdesc {
	std::string name;	//matdesc名(タブに表示される名前)
	std::string model;	//モデル名(pmxに設定された日本語名)
	std::unordered_map<std::string, std::string>dict;	//材質名→matdescファイル名テーブル

	template<class Archive>
	void serialize(Archive& A) {
		A(CEREAL_NVP(name));
		A(CEREAL_NVP(model));
		A(CEREAL_NVP(dict));
	}

	bool Load(const fs::path& filename)
	{
		std::ifstream ifs(filename, std::ios::binary);
		if (!ifs) {
			OKDlg(L"model matdesc error", L"open failed " + filename.wstring());
			return false;
		}

		try {
			cereal::JSONInputArchive archive(ifs);
			archive(cereal::make_nvp("modelmatdesc", *this));
		} catch (std::exception ex) {
			OKDlgA("model matdesc error", ex.what());
			return false;
		}
		return true;
	}

	bool Save(const fs::path& filename)
	{
		std::ofstream ofs(filename, std::ios::binary);
		if (!ofs) {
			OKDlg(L"model matdesc error", L"save failed " + filename.wstring());
			return false;
		}
		try {
			cereal::JSONOutputArchive archive(ofs);
			archive(cereal::make_nvp("modelmatdesc", *this));
		} catch (std::exception ex) {
			OKDlgA("model matdesc error", ex.what());
			return false;
		}
		return true;
	}
};


//モデル番号を指定してモデルに割り当てられた材質注釈ファイル名を得る
//モデルの材質毎に別のファイルが割り当てられている場合は空文字を返す
std::string MatDescModelFile(int imo, const YRZ::FXMatDescStorage& md)
{
	if (imo >= md.items.size())
		return "";

	std::string ret;
	for (auto && item : md.items[imo]) {
		if (ret.empty())
			ret = item.sourceFile;
		else if (ret != item.sourceFile)
			return "";
	}
	return ret;
}


//モデルの増減が無いかのチェック
bool MatDescSizeCheck(const std::vector<std::vector<bool>>& check, const YRZ::FXMatDescStorage& md)
{
	if (check.size() != md.items.size())
		return false;

	for (int i = 0; i < check.size(); i++) {
		if (check[i].size() != md.items[i].size())
			return false;
	}
	return true;
}

//MatDescの選択状態をバッファに反映する
void UploadMatDescSelectState(std::vector<std::vector<bool>>&check)
{
	//総材質数
	int N = 0;

	//カウント
	for (auto&& cs : check)
		N += cs.size();

	//リストに挙げられている材質数とくいちがいがあるなら帰る(ロード中など何かいじれない状態にあると仮定する)
	if (g_dayo->matSelected.size() != N || N==0)
		return;

	//コピー
	for (int i = 0; auto && cs : check) {
		for (bool c : cs) {
			g_dayo->matSelected[i] = c;
			i++;
		}
	}

	//うp
	g_dxr->Upload(g_dayo->matSelectedBuf, g_dayo->matSelected.data());

	//現在時刻の記録
	MatSelectT = StopWatch::Now();
}

bool MatDescTable(const YRZ::FXMatDescStorage& md, YRZ::FX* fx)
{
	bool ret = false;
	int nmodels = g_dayo->models.size();

	//タブが変更されていたりモデルの増減があったらアイテム列の再構築
	static std::string prevMD = "";
	static std::vector<std::vector<bool>>check;	//各マテリアルのチェック状態

	if (prevMD != md.name || !MatDescSizeCheck(check, md)) {
		check.resize(nmodels);
		for (int i = 0; i < nmodels; i++) {
			check[i].resize(md.items[i].size(), false);
			UploadMatDescSelectState(check);
		}
	}

	//どれかチェックされてる？フラグ。ボタンの有効無効の判定に使う
	bool anyChecked = false;
	for (int i = 0; i < nmodels; i++) {
		if (std::find(check[i].begin(), check[i].end(), true) != check[i].end())
			anyChecked = true;
	}

	//モデル単位でチェック
	auto mocheckLamba = [&](int imo, bool c) { std::fill(check[imo].begin(), check[imo].end(), c); };
	//モデルに属するマテリアルが全部選択されてればtrue
	auto isMoChekedLambda = [&](int imo) { if (check[imo].empty()) return false; for (auto&& c : check[imo]) if (!c) return false; return true; };
	//モデルに属するマテリアルが1つでも選択されてればtrue
	auto anyMoChekedLambda = [&](int imo) { for (auto&& c : check[imo]) if (c) return true; return false; };

	//マテリアル変更ボタン
	if (!anyChecked)
		ImGui::BeginDisabled();
	if (ImGui::Button("load MatDesc")) {
		std::wstring filename;
		if (FileDialog(filename, { L"Text file", L"*.txt", L"All(*.*)", L"*.*" }, L"")) {
			fs::path path = fs::relative(filename, fx->BasePath());
			if (path.empty())
				path = filename;
			std::string u8file = YRZ::u8(path.wstring());
			bool updated = false;
			for (int i = 0; auto && cs : check) {
				for (int j = 0; auto && c : cs) {
					if (check[i][j]) {
						try {
							fx->SetMatDesc(u8file, i, j, true, true, false);
							updated = true;
						} catch (std::exception ex) {
							OKDlgA("MatDesc error", ex.what());
						}
					}
					j++;
				}
				i++;
			}
			if (updated) {
				ret = true;
				fx->Update();
			}
		}
	}
	if (!anyChecked)
		ImGui::EndDisabled();

	//オススメ設定のセット
	ImGui::SameLine();
	ImGui::BeginDisabled(!anyChecked);
	if (ImGui::Button("set recommends")) {
		if (OKCancelDlg(L"set recommends", g_hon.L(L"Find the material to which the default material annotation is assigned and set it as the renderer's recommended material annotation, are you sure?"))) {
			bool updated = false;
			fs::path defmat = fx->BasePath() / (char8_t*)md.defaultFile.c_str();
			for (int i = 0; auto && mm : md.items) {
				for (int j = 0; auto && m : mm) {
					if (!check[i][j])
						continue;
					fs::path current = fx->BasePath() / (char8_t*)m.sourceFile.c_str();
					//デフォルト材質だったらオススメを検索する
					bool isDefault = true;
					try {
						isDefault = fs::equivalent(current, defmat);
					} catch (...) {
						//パスが見つからない場合はdefaultと同じ扱いにする
						;
					}
					if (isDefault) {
						std::string matname = g_dayo->models[i].res->u8materialNames[j];
						std::transform(matname.begin(), matname.end(), matname.begin(), tolower);
						//オススメを検索
						for (auto&& [key, value] : fx->parser.recommends) {
							std::string low = key;
							std::transform(low.begin(), low.end(), low.begin(), tolower);
							if (matname.find(low) != std::string::npos) {
								//キーワードを材質名に含む
								try {
									fx->SetMatDesc(value, i, j, true, true, false);
									updated = true;
									break;
								} catch (std::exception ex) {
									OKDlgA("MatDesc error", ex.what());
								}
							}
						}
					}
					j++;
				}
				i++;
			}
			if (updated) {
				ret = true;
				fx->Update();
			}
		}
	}
	ImGui::EndDisabled();

	//モデル単位で読み込み
	ImGui::BeginDisabled(!anyChecked);
	ImGui::SameLine();
	if (ImGui::Button("load model's")) {
		for (int i = 0; i < nmodels; i++) {
			if (anyMoChekedLambda(i)) {
				std::wstring filename;
				if (FileDialog(filename, { L"JSON files(*.json)", L"*.json" , L"all files(*.*)", L"*.*" }, BasePath)) {
					ModelMatdesc mmd;
					auto res = g_dayo->models[i].res.get();
					if (mmd.Load(filename)) {
						bool goon = true;
						if ( (mmd.name != fx->matDescs.name) || (mmd.model != res->u8name)) {
							goon = OKCancelDlg(L"matdesc loading", g_hon.L(L"It looks like a json file that is not for the model or effect you are trying to load, do you want to continue loading it?"));
						}
						//読み込み先のマテリアル名→番号辞書を作る
						std::unordered_map<std::string, int> destdict;
						for (int j = 0; j<res->u8materialNames.size(); j++)
							destdict[res->u8materialNames[j]] = j;
						//辞書を見てmatdescを設定
						bool upd = false;
						if (goon) {
							for (auto&& [key,value] : mmd.dict) {
								if (destdict.contains(key)) {
									int idx = destdict[key];
									fx->SetMatDesc(value, i, idx, true, true, false);
									upd = true;
								}
							}
						}
						if (upd) {
							ret = true;
							fx->Update();
						}
					}
					break;
				}
			}
		}
	}

	//モデル単位で保存
	ImGui::SameLine();
	if (ImGui::Button("save model's")) {
		//null以外で最初に見つかったモデルについて格納する
		for (int i = 0; i < nmodels; i++) {
			if (anyMoChekedLambda(i)) {
				auto res = g_dayo->models[i].res.get();
				std::wstring filename = (res->pmx->name.empty() ? L"nanashi" : res->pmx->name) + L".json";
				if (SaveFileDialog(filename, { L"JSON files(*.json)", L"*.json" , L"all files(*.*)", L"*.*" }, BasePath)) {
					ModelMatdesc md;
					md.name = fx->matDescs.name;
					md.model= res->u8name;
					for (int j = 0; auto && item : fx->matDescs.items[i]) {
						md.dict[res->u8materialNames[j]] = item.sourceFile;
						j++;
					}
					if (YRZ::ExtractExt(filename).empty())
						filename += L".json";
					md.Save(filename);
					break;
				}
			}
		}
	}
	ImGui::EndDisabled();

	//リロードボタン
	ImGui::SameLine();
	if (ImGui::Button("reload")) {
		for (int i = 0; auto && mm : md.items) {
			for (int j = 0; auto && m : mm) {
				try {
					fx->SetMatDesc(m.sourceFile, i, j, true, true, false);
				} catch (std::exception ex) {
					OKDlgA("MatDescError", ex.what());
				}
				j++;
			}
			i++;
		}
		ret = true;
		fx->Update();
	}


	//材質注釈一覧。テーブルを一番上にするとボタンがはみ出るので、ボタン→テーブルの順に配置することになった
	if (ImGui::BeginTable("matdesc", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
		ImGui::TableSetupColumn("object", ImGuiTableColumnFlags_WidthStretch, 100);
		ImGui::TableSetupColumn("file", ImGuiTableColumnFlags_WidthStretch, 200);
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();


		//クリックで選択できるリスト
		int id = 0;
		static int prevClick = -1;	//shift+クリックの範囲選択用
		static int prevClickMo = -1;
		bool updated = false;	//選択状態はアップデートされたか？
		for (int imo = 0; imo < nmodels; imo++) {
			auto& mo = g_dayo->models[imo];
			ImGui::TableNextColumn();
			ImGui::PushID(id++);
			bool open = ImGui::TreeNodeEx(mo.res->u8name.c_str());
			ImGui::PopID();
			ImGui::TableNextColumn();
			ImGui::PushID(id++);
			//モデル選択
			bool mochecked = isMoChekedLambda(imo);
			if (ImGui::Selectable(MatDescModelFile(imo, md).c_str(), mochecked)) {
				auto io = ImGui::GetIO();
				if (io.KeyShift && prevClickMo >= 0) {
					if (prevClickMo >= imo) {
						for (int j = imo; j < prevClickMo; j++)
							mocheckLamba(j, !mochecked);
					} else {
						for (int j = imo; j > prevClickMo; j--)
							mocheckLamba(j, !mochecked);
					}
					mochecked = !mochecked;
				} else {
					bool c = mochecked;
					if (!io.KeyCtrl)
						for (int j = 0; j < nmodels; j++)
							mocheckLamba(j, false);
					mocheckLamba(imo, !c);
					mochecked = !c;
				}
				updated = true;
				prevClickMo = imo;
			}
			ImGui::PopID();
			if (open) {
				//マテリアル選択
				for (int i = 0; auto && mat : md.items[imo]) {
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(mo.res->u8materialNames[i].c_str());
					ImGui::TableNextColumn();
					ImGui::PushID(id++);
					if (ImGui::Selectable(mat.sourceFile.c_str(), check[imo][i])) {
						auto io = ImGui::GetIO();
						if (io.KeyShift && prevClick >= 0 && prevClickMo == imo) {
							//shift+クリックで前回選択したアイテムとの間の選択状態を反転
							if (prevClick >= i) {
								for (int j = i; j < prevClick; j++)
									check[imo][j] = !check[imo][j];
							} else
								for (int j = i; j > prevClick; j--) {
									check[imo][j] = !check[imo][j];
							}
						} else {
							//クリックで全選択解除 + クリックしたモノは反転
							//ctrlが押されているなら全選択解除はしない
							bool c = check[imo][i];
							if (!io.KeyCtrl)
								for (int j = 0; j < nmodels; j++)
									std::fill(check[j].begin(), check[j].end(), false);
							check[imo][i] = !c;
						}
						updated = true;
						prevClick = i;
						prevClickMo = imo;
					}
					ImGui::PopID();
					i++;
				}
				ImGui::TreePop();
			} else {
				//ノードが閉じている場合、属しているマテリアルの選択状態は親の選択状態に合わせる
				mocheckLamba(imo, mochecked);
			}
		}

		if (updated) {
			UploadMatDescSelectState(check);
		}

		ImGui::EndTable();
		prevMD = md.name;
	}
	return ret;
}

bool MatDescWindow()
{
	bool updated = false;
	ImGui::Begin("Materials");
	{
		int id = 0;
		if (ImGui::BeginTabBar("TabBar")) {
			for (auto&& w : g_dayo->fxWatcher->watchList) {
				//エフェクトで材質注釈を持っている物があれば、そのタブを作る
				if (w.fx && !w.fx->matDescs.items.empty()) {
					auto& md = w.fx->matDescs;
					auto& parser = w.fx->parser;
					ImGui::PushID(id++);
					if (ImGui::BeginTabItem(md.name.c_str())) {
						//材質注釈一覧表示
						updated |= MatDescTable(md, w.fx.get());
						ImGui::EndTabItem();
					}
					ImGui::PopID();
				}
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::End();
	return updated;
}