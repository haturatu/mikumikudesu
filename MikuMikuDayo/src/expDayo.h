#ifdef __INTELLISENSE__
#include "defsDayo.h"
#endif

//外部親情報構造体
struct ExpInfo {
	int childB = 0;		//外部子になるボーン番号
	int parent = -1;	//親モデル番号
	int parentB = -1;	//親モデルのボーン番号
	//以下、UI用
	bool checked = false;
	std::vector<std::string> PBLabel;	//親モデルのボーン名コンボボックス用ラベル

	//有効？
	bool Valid() const { return (childB >= 0 && parent >= 0 && parentB >= 0); }

	//PBLabelを作る
	void UpdateLabel() {
		PBLabel = { "(none)" };
		if (parent >= 0) {
			const auto& pp = g_dayo->models[parent].res->pmx;
			for (const auto& b : pp->bones) {
				auto name = (Language == 0 || b.name_e.empty()) ? b.name : b.name_e;
				PBLabel.push_back(YRZ::u8(name));
			}
		}
	}

	//expベクタのアイテムを作る
	PMX::VMDExParent ExpItem(const KeyFrameWindowContext& kc) const {
		const auto& pp = g_dayo->models[parent].res->pmx;
		
		int pb = (pp->bones.size() <= parentB) ? 0 : parentB;

		PMX::VMDExParent e = {};
		e.parentID = g_dayo->models[parent].id;
		e.parentBone = pb;
		e.bone = childB;
		e.boneName = kc.pmx->bones[childB].name;
		e.parentBoneName = pp->bones[pb].name;
		return e;
	}

	//コンストラクタ
	ExpInfo() { UpdateLabel(); }

	//expアイテムから生成
	ExpInfo(PMX::VMDExParent e) {
		childB = e.bone;
		parent = g_dayo->Find(e.parentID);
		parentB = e.parentBone;
		UpdateLabel();
	}
};

//externalsボタン押すと外部親設定ダイアログが出る
//返り値 : resampleすべきか？
bool ExpWindow(int iModel, KeyFrameWindowContext& kc)
{
	bool resample = false;
	auto& exp = kc.pose.extraKey.exp;
	static std::vector<std::string> childBoneLabel;		//ボーン名リスト
	static std::vector<int> childBoneIndex;				//有効なボーン番号リスト
	static std::vector<std::string> parentModelLabel;	//親モデル名リスト
	static std::vector<std::string> parentBoneLabel;	//親ボーン名リスト
	static std::vector<ExpInfo>eis;	//外部親情報リスト

	if (ImGui::Button("externals...")) {
		ImGui::OpenPopup("External");

		//ボーンラベルを作る
		childBoneLabel.clear();
		childBoneIndex.clear();
		for (int i = 0; auto&& b : g_dayo->models[iModel].res->pmx->bones) {
			//移動可能かつ物理ボーンでない、MMDから見えるボーン
			if (b.IsTranslation() && !b.IsPhysics && b.IsVisible() && kc.registeredBones.contains(i)) {
				childBoneIndex.emplace_back(i);
				childBoneLabel.push_back(Language == 0 ? YRZ::u8(b.name) : YRZ::u8(b.name_e));
			}
			i++;
		}
		//親モデルラベルを作る
		parentModelLabel.clear();
		parentModelLabel.push_back("(none)");
		for (int i = 0; i < g_dayo->models.size(); i++) {
			parentModelLabel.push_back(g_dayo->models[i].u8name);
		}

		//現状の外部親情報取得
		eis.clear();
		for (auto& e : exp) {
			eis.push_back(ExpInfo(e));
			eis.back().UpdateLabel();
		}
	}
	if (!exp.empty()) {
		TooltipDayo("%zu bone(s) attached", exp.size());
	}

	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	if (ImGui::BeginPopupModal("External", NULL)) {
		
		//登録ボタン
		if (ImGui::Button("register")) {
			auto backup = exp;

			int dup = -1;
			exp.clear();
			for (auto&& e : eis) {
				//有効なアイテムを探す
				if (e.Valid()) {
					//同じボーンから違う親に接続してる場合、重複あり
					for (auto&& ex : exp) { if (e.childB == ex.bone) { dup = e.childB; break; } }
					exp.push_back(e.ExpItem(kc));
				}
			}
			if (dup >= 0) {
				//重複が見つかった場合、メッセージ出す
				OKDlg(L"external", std::format(L"{} ({})",g_hon.L(L"The same bone cannot be connected to multiple external parents.") ,kc.pmx->bones[exp[dup].bone].name));
				exp = backup;
			} else {
				//うまくいった
				PMX::KeySubset sub;
				sub.extras = { kc.pose.extraKey };
				std::wstring desc;
				if (exp.empty()) {
					desc = std::format(L"detach");
				} else if (exp.size() == 1) {
					int iparent = g_dayo->Find(exp[0].parentID);
					desc = std::format(L"attach {}->{}@{}", kc.pmx->bones[exp[0].bone].name,
						g_dayo->models[iparent].res->pmx->bones[exp[0].parentBone].name, g_dayo->models[iparent].res->pmx->name);
				} else {
					desc = std::format(L"attach {}bones", exp.size());
				}

				kc.RegisterSubset(sub, desc, true, true, 0);
				g_dayo->PoseUpdate(true, iModel);
				resample = true;
				ImGui::CloseCurrentPopup();
			}
		}

		ImGui::SameLine();
		if (ImGui::Button("close") || ImGui::IsKeyPressed(ImGuiKey_Escape))
			ImGui::CloseCurrentPopup();

		if (ImGui::BeginTable("externalTable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {

			ImGui::TableSetupColumn("bone", ImGuiTableColumnFlags_WidthStretch, 0.3);
			ImGui::TableSetupColumn("parent model", ImGuiTableColumnFlags_WidthStretch, 0.3);
			ImGui::TableSetupColumn("parent bone", ImGuiTableColumnFlags_WidthStretch, 0.3);
			ImGui::TableSetupColumn("del", ImGuiTableColumnFlags_WidthStretch, 0.1);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();
			int sakujo = -1;
			for (int i = 0; auto& e : eis) {
				ImGui::TableNextRow();
				auto istr = std::to_string(i);

				//自モデルの子ボーン名
				ImGui::TableNextColumn();
				ImGui::PushItemWidth(ImGui::CalcItemWidth());
				ComboBoxWithFilter(("##>" + istr).c_str(), childBoneLabel, e.childB, 0);
				ImGui::PopItemWidth();

				//親モデル名
				ImGui::TableNextColumn();
				if (ComboBoxWithFilter(("##|" + istr).c_str(), parentModelLabel, e.parent, -1)) {
					e.parentB = 0;
					e.UpdateLabel();
				}

				//親ボーン名
				ImGui::TableNextColumn();
				ComboBoxWithFilter(("##<" + istr).c_str(), e.PBLabel, e.parentB, -1);

				//削除ボタン
				ImGui::TableNextColumn();
				if (ImGui::Button(("-##" + istr).c_str())) {
					sakujo = i;
				}

				i++;
			}

			//削除ボタンが押されていたら削除
			if (sakujo >= 0) {
				eis.erase(eis.begin() + sakujo);
			}

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			//最後の行に追加ボタン
			if (ImGui::Button("+")) {
				eis.push_back({});
			}


			ImGui::EndTable();
		}

		ImGui::EndPopup();
	}

	return resample;

}