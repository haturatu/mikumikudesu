#pragma once

#include "defsDayo.h"

//補間曲線ウィンドウ
void InterpolationEditor(KeyFrameWindowContext* kc)
{
	//ドラッグできるハンドルのためのデータ構造
	struct Draggable {
		bool dragging = false;
		ImVec2 pos = {};	//エディタの左下を0,0 右上を1,1とする
		float radius = 0;	//画素数で指定する
		ImU32 color;
	};

	ImGui::Begin("Interpolation");

	auto io = ImGui::GetIO();
	auto dl = ImGui::GetWindowDrawList();
	static Draggable handle[2] = {
		{false, {0,0},6, IM_COL32(255,0,0,192)},
		{false, {1,1},6, IM_COL32(0,255,0,192)}
	};
	static int dragging = -1;
	static KeyFrameWindowContext* prevkc = nullptr;
	//キーフレームウィンドウが更新されるたびに毎回補間曲線も更新すると特にドラッグや移動がモタつきまくる
	//これを回避するため、最後に更新された時刻を記録しておいて、最大で500msに1度しか更新できないようにする
	static bool updateReq = true;
	static DWORD prevUpdate = 0;
	static int findF0,findF1 = -1;	//キーフレームが1つだけ選択状態だった場合の前後のキーフレーム

	if (ImGui::BeginTable("Interpolation", 2)) {

		constexpr int maxCurves = 8;	//探索する既存の補間曲線の最大数
		static int nCurves[6] = { 0,0,0,0,0,0 };	//各軸毎の見つかった補間曲線の数
		static char cp[maxCurves][6][4];	//見つかったコントロールポイント[順番][軸6種類][0:ax,1:ay,2:bx,3:by]
		//const char* poseCombo = (char*)u8"All\0X\0Y\0Z\0Rt";
		//const char* camCombo = (char*)u8"All\0X\0Y\0Z\0Rt\0dist\0fov";
		static int iCombo = 0;	//コンボボックスの選択軸

		ImGui::TableNextColumn();

		PMX::PoseSolver* psolver = nullptr;
		PMX::CameraSolver* csolver = nullptr;
		int nAxis = 0;	//軸の数
		if (kc->pmx != nullptr) {
			psolver = dynamic_cast<PMX::PoseSolver*>(kc->solver);
			nAxis = 4;
		} else {
			csolver = dynamic_cast<PMX::CameraSolver*>(kc->solver);
			nAxis = 6;
		}


		//補間曲線エディタセルの左上座標とサイズ
		ImVec2 TL = ImGui::GetCursorScreenPos();
		ImVec2 SZ = ImGui::GetContentRegionAvail();

		//正方形の領域になるように一辺のサイズと左上座標を補正
		float L = min(SZ.x, SZ.y);
		if (L == SZ.x) {
			TL.y += (SZ.y - L) / 2;
		} else {
			TL.x += (SZ.x - L) / 2;
		}
		SZ = ImVec2(L, L);

		//0～1の範囲になっているax,ayまたはbx,byからベジェエディタ上の点を返す
		auto pLambda = [&](const ImVec2& p) { return TL + ImVec2(p.x * L, (1 - p.y) * L); };
		//マウスカーソルがdraggableの上に乗っているか？
		auto moLambda = [&](int i) {
			ImVec2 c = io.MousePos;
			ImVec2 p = pLambda(handle[i].pos);
			float r = handle[i].radius;
			return YRZ::ImMath::LenSq(p - c) <= r * r;
			};
		//マウスカーソルが乗っているdraggableのインデクスを返す、どこにも乗ってなければ-1
		auto moanyLambda = [&] { for (int i = 0; i < 2; i++) if (moLambda(i)) return i; return -1; };

		dl->AddRectFilled(TL, TL + SZ, IM_COL32(64, 64, 64, 128));


		//ドラッグでハンドルをうごかす
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
			//ドラッグ状態に入ってないけどマウスがドラッグされている場合ハンドルを掴む
			if (dragging == -1) {
				int mon = moanyLambda();
				if (mon != -1) {
					dragging = mon;
				}
			} else {
				handle[dragging].pos = (io.MousePos - TL) / L;
				handle[dragging].pos.y = 1 - handle[dragging].pos.y;
				handle[dragging].pos = YRZ::ImMath::Clamp(handle[dragging].pos, { 0, 0 }, { 1, 1 });
			}
		} else {
			dragging = -1;
		}

		constexpr ImU32 curveColor[6] = {
			IM_COL32(255,128,128,255), IM_COL32(128,255,128,255), IM_COL32(128,128,255,255),
			IM_COL32(255,128,255,255), IM_COL32(255,255,128,255), IM_COL32(128,255,255,255) };

		//補間曲線表示
		//キーフレームの更新に追いついてない場合は描画しない
		if (!updateReq) {
			for (int j = 0; j < nAxis; j++) {
				for (int i = 0; i < nCurves[j]; i++) {
					if (iCombo == 0 || iCombo == (j + 1)) {
						ImVec2 p0 = pLambda(ImVec2(cp[i][j][0], cp[i][j][1]) / 127.0f);
						ImVec2 p1 = pLambda(ImVec2(cp[i][j][2], cp[i][j][3]) / 127.0f);
						dl->AddBezierCubic(TL + ImVec2(0, L), p0, p1, TL + ImVec2(L, 0), curveColor[j], 1);
					}
				}
			}

			//編集中用の補間曲線を太く表示、ハンドルも付ける
			if (nCurves[0] >= 1) {
				//1つは補間曲線を持ったキーが選択されていないと編集対象にならない
				ImU32 maincol = IM_COL32(255, 255, 255, 255);
				if (iCombo != 0)
					maincol = curveColor[iCombo - 1];

				dl->AddBezierCubic(TL + ImVec2(0, L), pLambda(handle[0].pos), pLambda(handle[1].pos), TL + ImVec2(L, 0), maincol, 3);

				for (int i = 0; i < 2; i++) {
					dl->AddCircleFilled(pLambda(handle[i].pos), handle[i].radius, handle[i].color);
					ImVec2 p0;
					if (i == 0)
						p0 = TL + ImVec2(0, L);
					else
						p0 = TL + ImVec2(L, 0);
					dl->AddLine(p0, pLambda(handle[i].pos), handle[i].color);
				}

				//補間前後のフレーム番号の表記
				if (findF0 >= 0) {
					dl->AddText(TL + ImVec2(0, L), IM_COL32(255, 255, 255, 255), std::format("{}F", findF0).c_str());
					auto text = std::format("{}F", findF1);
					auto sz = ImGui::CalcTextSize(text.c_str());
					dl->AddText(TL + ImVec2(L-sz.x, L), IM_COL32(255, 255, 255, 255), text.c_str());
				}
			}
		}


		ImGui::TableNextColumn();
		constexpr ImU32 buttonColor[7] = {
			IM_COL32(255,255,255,255),
			IM_COL32(255,128,128,255), IM_COL32(128,255,128,255), IM_COL32(128,128,255,255),
			IM_COL32(255,128,255,255), IM_COL32(255,255,128,255), IM_COL32(128,255,255,255) };
		bool comboed = false;
		if (kc->pmx != nullptr) {
			const char *str[] = {"All","X","Y","Z","Rt"};
			for (int i = 0; i < 5; i++) {
				ImGui::PushStyleColor(ImGuiCol_Text, buttonColor[i]);
				if (ImGui::RadioButton(str[i], iCombo == i)) {
					iCombo = i;
					comboed = true;
				}
				ImGui::PopStyleColor();
				if (1 <= i && i <= 2)
					ImGui::SameLine();
			}
		} else {
			const char* str[] = { "All","X","Y","Z","Rt","dist","fov"};
			for (int i = 0; i < 7; i++) {
				ImGui::PushStyleColor(ImGuiCol_Text, buttonColor[i]);
				if (ImGui::RadioButton(str[i], iCombo == i)) {
					iCombo = i;
					comboed = true;
				}
				ImGui::PopStyleColor();
				if ( i==1 || i==2 || i==4 || i==5)
					ImGui::SameLine();
			}
		}

		
		//登録・ペースト用ラムダ式
		char bp[6][4];	//登録したいベジェ曲線、呼び出しもとで書き換えられる
		//condはコンボボックスのインデックスに対応。0ならAll, 1-6なら各軸
		auto regpasLambda = [&](auto action, int cond) {
			PMX::KeySubset sub;
			if (psolver) {
				for (int i = 0; auto && item : psolver->motionKeys) {
					for (auto&& [iFrame, value] : item) {
						PMX::VMDMotion key = { value, psolver->pmx->bones[i].name };
						if (value.selected) {
							for (int b = 0; b < nAxis; b++) {
								if (cond == 0 || cond == b + 1) {
									memcpy_s(key.bezierParams + b * 4, 4, bp[b], 4);
								}
							}
							sub.motions.push_back(key);
						}
					}
					i++;
				}
			} else {
				for (auto&& [iFrame, value] : csolver->cameraKeys) {
					PMX::VMDCamera key = value;
					if (value.selected) {
						for (int b = 0; b < nAxis; b++) {
							if (cond == 0 || cond == b + 1) {
								memcpy_s(key.bezierParams + b * 4, 4, bp[b], 4);
							}
						}
						sub.cameras.push_back(key);
					}
				}
			}
			if (!sub.Empty()) {
				auto desc = std::format(L"{} {}F-{}F", action, sub.FirstFrame(), sub.LastFrame());
				kc->RegisterSubset(sub, desc, false, true, 0);
				return true;
			} else {
				return false;
			}
		};

		//登録とかボタン。編集対象のキーフレームが無い場合は表示されない
		if (nCurves[0] >= 1) {
			//登録
			if (ImGui::Button("Register")) {
				for (int i = 0; i < nAxis; i++) {
					bp[i][0] = roundf(handle[0].pos.x * 127.0f);
					bp[i][1] = roundf(handle[0].pos.y * 127.0f);
					bp[i][2] = roundf(handle[1].pos.x * 127.0f);
					bp[i][3] = roundf(handle[1].pos.y * 127.0f);
				}
				regpasLambda(L"curve", iCombo);
			}

			static bool copied = false;
			static char copybuf[6][4] = {};	//補間曲線コピーバッファ

			//コピー…コピー元が1つの曲線群である場合でないとコピー元が分からないのでコピーしない
			bool all1 = true;
			for (int i = 0; i < nAxis; i++) {
				if (nCurves[i] != 1) {
					all1 = false;
					break;
				}
			}
			if (!all1) ImGui::BeginDisabled();
			if (ImGui::Button("Copy")) {
				for (int i = 0; i < nAxis; i++) {
					//Allの場合は各曲線を各々コピー、1軸を選んでいる場合は1軸の曲線を全体にコピー
					int s = (iCombo == 0) ? i : iCombo - 1;
					copybuf[i][0] = cp[0][s][0];
					copybuf[i][1] = cp[0][s][1];
					copybuf[i][2] = cp[0][s][2];
					copybuf[i][3] = cp[0][s][3];
					copied = true;
				}
			}
			if (!all1) ImGui::EndDisabled();


			//ペースト
			if (!copied) ImGui::BeginDisabled();
			if (ImGui::Button("Paste")) {
				memcpy_s(bp, 24, copybuf, 24);
				if (regpasLambda(L"paste curve", iCombo))
					MessageBeep(MB_ICONINFORMATION);
			}
			if (!copied) ImGui::EndDisabled();

			if (ImGui::Button("Init")) {
				handle[0].pos = ImVec2(20, 20) / 127.0f;
				handle[1].pos = ImVec2(107, 107) / 127.0f;
			}
		}

		/*** 以下、補間曲線表示用のデータ取得 ***/

		//同じカーブか確認す
		auto isSameCurveLambda = [](char* c1, char* c2) {
			for (int i = 0; i < 4; i++) {
				if (*(c1 + i) != *(c2 + i)) {
					return false;
				}
			}
			return true;
			};
		//未知のカーブを集める
		auto collectCurveLambda = [&](char* c, char axis, int frame) {
			if (nCurves[axis] >= maxCurves)
				return;
			bool known = false;
			for (int i = 0; i < nCurves[axis]; i++) {
				if (isSameCurveLambda(c, &cp[i][axis][0])) {
					known = true;
					return;
				}
			}
			if (!known) {
				cp[nCurves[axis]][axis][0] = c[0];
				cp[nCurves[axis]][axis][1] = c[1];
				cp[nCurves[axis]][axis][2] = c[2];
				cp[nCurves[axis]][axis][3] = c[3];
				nCurves[axis]++;
				findF0 = frame;
			}
			};

		//補間曲線がmaxまで集まった？
		auto isFullCurveLambda = [&]() {
			for (int i = 0; i < nAxis; i++) {
				if (nCurves[i] < maxCurves)
					return false;
			}
			return true;
			};

		//「既知の曲線」のアップデート
		auto updateCurveLambda = [&]() {
			findF0 = findF1 = -1;
			int nChecked = 0;	 //発見されたチェックされているキーの数
			int checkedIdx = -1; //チェックされているボーン番号(ポースソルバーの場合のみ有効)
			for (int i = 0; i < 6; i++)
				nCurves[i] = 0;
			if (kc->pmx != nullptr) {
				for (int i = 0; auto && item : psolver->motionKeys) {
					for (auto& [iFrame, value] : item) {
						if (value.selected) {
							for (int i = 0; i < 4; i++) {
								collectCurveLambda(value.bezierParams + i * 4, i, iFrame);
							}
							nChecked++;
							checkedIdx = i;
							findF1 = iFrame;
							if (isFullCurveLambda())
								break;
						}
					}
					if (isFullCurveLambda())
						break;
					i++;
				}
				if (findF1 >= 0 && nChecked == 1) {
					if (findF1 == 0)
						findF0 = 0;
					else {
						auto it = psolver->motionKeys[checkedIdx].equal_range(findF1);
						it.first--;
						findF0 = it.first->first;
					}
				};
			} else {
				//All\0X\0Y\0Z\0Rt\0dist\0fov";
				for (auto& [iFrame, value] : csolver->cameraKeys) {
					if (value.selected) {
						for (int i = 0; i < 6; i++) {
							collectCurveLambda(value.bezierParams + i * 4, i, iFrame);
						}
						nChecked++;
						findF1 = iFrame;
						if (isFullCurveLambda())
							break;
					}
				}
				if (findF1 >= 0 && nChecked == 1) {
					if (findF1 == 0)
						findF0 = 0;
					else {
						auto it = csolver->cameraKeys.equal_range(findF1);
						it.first--;
						findF0 = it.first->first;
					}
				}
			}
			};


		//選択状態が更新されているかチェック
		auto updateCheckLambda = [&]() {
			//そもそも前回のコンテクストと違う
			if (prevkc != kc)
				return true;
			//キーフレームが更新されている？ ドラッグ中に更新扱いすると重いので省く
			updateReq |= (kc->status >= KeyFrameStatus::check && !kc->dragging);
			if (updateReq) {
				//キーフレームの更新毎に即曲線も更新すると重いので前回更新後から500msの間は更新しない
				auto now = timeGetTime();
				if (now - prevUpdate > 500)
					return true;
			}
			return false;
			};


		if (comboed || updateCheckLambda()) {
			updateCurveLambda();
			updateReq = false;
			int a = iCombo == 0 ? 0 : iCombo - 1;
			handle[0].pos = ImVec2(cp[0][a][0], cp[0][a][1]) / 127.0f;
			handle[1].pos = ImVec2(cp[0][a][2], cp[0][a][3]) / 127.0f;
		}

		ImGui::EndTable();
	}


	prevkc = kc;

	ImGui::End();
}

