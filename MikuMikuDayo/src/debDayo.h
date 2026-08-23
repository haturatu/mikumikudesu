#pragma once

//シェーダデバッグウィンドウ

#ifdef __INTELLISENSE__
#include "defsdayo.h"
#endif

//画像表示用定数
struct DebugImageConstants {
	int hsvmode = 0;
	int tex3d = 0;
	float scale = 1.0f;
	int slice = 0;
};

//画像表示オブジェクト
// 2Dか3Dのイメージを表示する他、マニュアル作成ユーティリティ付
struct DebugImageProcessor {
private:
	int manualLang = -2;	//最後にマニュアルを作った時の情報
	YRZ::FX* manualFX = nullptr;
public:
	YRZ::Pass pass;	//プロセス用パス
	YRZ::CB constantBuffer;
	DebugImageConstants* cb;
	YRZ::Tex2D output;
	YRZ::Res* source = nullptr;
	YRZ::Tex2D dmy;
	YRZ::Tex3D dmy3d;
	std::string manual;
	bool initialized = false;
	DebugImageProcessor() {
		DebugImageConstants data;
		constantBuffer = g_dxr->CreateCB(&data, sizeof(data));
		cb = (DebugImageConstants*)constantBuffer.pData;
		dmy = g_dxr->CreateTex2D(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM);
		dmy3d = g_dxr->CreateTex3D(8, 8, 8, DXGI_FORMAT_R8G8B8A8_UNORM);
	}
	//表示テクスチャの変更
	//forceReset : テクスチャが今まで指定されていた物でなくてもパスを作り直す?
	void Reset(YRZ::Res* tex, bool forceReset = false) {
		if (source && tex->res == source->res && !forceReset) {
			return;
		}
		initialized = true;
		source = tex;
		auto desc = tex->desc();
		output = g_dxr->CreateRT2D(desc.Width, desc.Height, DXGI_FORMAT_R8G8B8A8_UNORM);

		pass = YRZ::Pass(g_dxr);
		pass.CBV = { &constantBuffer };
		if (tex->type == YRZ::ResType::tex2D) {
			cb->tex3d = false;
			pass.SRV[0] = { {tex},{ &dmy3d} };
		} else {
			cb->tex3d = true;
			pass.SRV[0] = { {&dmy},{ tex} };
		}
		pass.RTV = { {&output} };
		auto vs = LoadShader(Recompile, g_dxr, L"SystemDebugVS", L"hlsl\\system\\debug.hlsl", L"VS", L"vs_6_1", CompileOption);
		auto ps = LoadShader(Recompile, g_dxr, L"SystemDebugPS", L"hlsl\\system\\debug.hlsl", L"PS", L"ps_6_1", CompileOption);
		pass.PostProcessPass(vs, ps);
	}
	//リサイズされたことを知らせる
	void OnResize() {
		if (initialized) {
			Reset(source, true);
		}
	}
	//レンダリングする
	void Render() {
		auto desc = output.desc();
		g_dxr->OpenCommandList();
		pass.Render(desc.Width, desc.Height);
		g_dxr->ExecuteCommandList();
	}
	//マニュアル作る
	void ReadyManual(YRZ::FX* fx, int selfIndex) {
		if (!fx) {
			manual = "";
			return;
		}

		//マニュアル再作成
		if (Language != manualLang || manualFX != fx) {
			manual = "";
			manualLang = Language;
			manualFX = fx;
			std::ostringstream ss;

			auto descLambda = [&](const auto& descs)->std::string {
				int lang = Language + 1;
				if (descs.empty())
					return "";
				if (descs.size() > lang) {
					return descs[lang];
				} else {
					return descs[0];
				}
			};

			//アイテム名取得
			auto itemLambda = [&](const std::string& item, const std::string& item_e)->std::string {
				//日本語の場合はそのままでOK
				if (Language == 0 || item_e.empty())
					return item;
				//英語
				return item_e;
			};

			//概要
			if (selfIndex >= 0) {
				auto& mo = g_dayo->models[selfIndex];
				auto desc = mo.res->u8description;
				desc = std::regex_replace(desc, std::regex("\r\n|\n|\r"), "<br>\n");
				ss << "# " << mo.res->u8name << "\n" << desc << "\n";
			}

			//コントローラをcontrollerNameで仕分ける
			std::vector<YRZ::FXController> sorted = fx->controllers;
			std::sort(sorted.begin(), sorted.end(), [&](const YRZ::FXController& a, const YRZ::FXController& b) { return a.controllerName < b.controllerName; });
			std::erase_if(sorted, [&](const auto& c) { return c.doppelIndex != 0; });

			if (sorted.empty()) {
				manual = ss.str();
				return;
			}

			ss << (char*)u8"## コントローラについて(about controllers)\n";

			std::string prevcont = "";
			//コントローラ別にモーフ・ボーンをそれぞれ表組にする
			for (auto& cont : sorted) {
				if (prevcont == cont.controllerName)
					continue;

				ss << "### " << cont.controllerName << "\n";
				//コントローラ
				bool found = false;
				for (auto& c : sorted) {
					if (c.type == "float" && c.controllerName == cont.controllerName) {
						if (!found) {
							ss << (char*)u8"#### 表情モーフ(expression morphs)\n";
							ss << "|name|description|\n";
							ss << "|--|--|\n";
							found = true;
						}
						int moidx, iidx;
						//英語名はコントローラのリソースから読み取るのでコントローラが読み込まれていないと日本語しか表示されません
						ControllerQuery(selfIndex, 0, c.controllerName, c.item, c.type, moidx, iidx);
						if (moidx >= 0 && iidx >= 0) {
							auto name_e = g_dayo->models[moidx].res->pmx->morphs[iidx].name_e;
							std::string item_e = YRZ::u8(name_e);
							ss << "|" << itemLambda(c.item, item_e) << "|" << descLambda(c.desc) << "|\n";
						} else {
							ss << "|" << c.item << "|" << descLambda(c.desc) << "|\n";
						}
					}
				}

				found = false;
				for (auto& c : sorted) {
					if ((c.type == "float3" || c.type == "float4x4") && c.controllerName == cont.controllerName) {
						if (!found) {
							ss << (char*)u8"#### ボーン(bones)\n";
							ss << "|name|description|\n";
							ss << "|--|--|\n";
							found = true;
						}
						int moidx, iidx;
						ControllerQuery(selfIndex, 0, c.controllerName, c.item, c.type, moidx, iidx);
						if (moidx >= 0 && iidx >= 0) {
							std::string item_e = YRZ::u8(g_dayo->models[moidx].res->pmx->bones[iidx].name_e);
							ss << "|" << itemLambda(c.item, item_e) << "|" << descLambda(c.desc) << "|\n";
						} else {
							ss << "|" << c.item << "|" << descLambda(c.desc) << "|\n";
						}
					}
				}
				prevcont = cont.controllerName;
			}

			manual = ss.str();

		}
	}

};

std::unique_ptr<DebugImageProcessor> DebIP;


void DebugWindow()
{
	ImGui::Begin("FXdebug");
	{
		//エフェクト選択コンボ
		static int fxcombo = 0;
		std::string fxnames = "(none)";
		fxnames.push_back(0);
		fxnames += "(common)";
		fxnames.push_back(0);

		std::unordered_map<int, YRZ::FX*>combo2fx = { {0,nullptr},{1,nullptr} };
		for (int i = 2; auto& w : g_dayo->fxWatcher->watchList) {
			if (w.fx) {
				fxnames += (char*)w.fx->filename().stem().u8string().c_str();
				fxnames.push_back(0);
				combo2fx[i] = w.fx.get();
				i++;
			}
		}
		ImGui::Combo("effect", &fxcombo, fxnames.c_str());

		auto fx = combo2fx[fxcombo];
		YRZ::Buf* pbuf = nullptr;
		YRZ::Tex2D* ptex = nullptr;
		YRZ::Tex3D* ptex3D = nullptr;
		YRZ::CB* pCB = nullptr;
		bool dispPass = false;	//パス情報表示
		bool dispManual = false; //マニュアル表示
		static int mode = 0;	//表示モード 0:float 1:int 2:hex
		static int rescombo = 0;
		std::unordered_map<int, YRZ::Res*>combo2res;
		std::string resnames;
		auto addResLambda = [&](int& i, const std::string& s, YRZ::Res* r) {
			resnames += s;
			resnames.push_back(0);
			combo2res[i] = r;
			i++;
			};
		//リソース選択コンボ
		if (fx) {
			int i = 0;
			addResLambda(i, "Passes", nullptr);
			addResLambda(i, "Manual", nullptr);
			if (!fx->controllers.empty())
				addResLambda(i, "Controllers", &fx->controllerCB);
			for (auto& b : fx->buffers)
				addResLambda(i, YRZ::u8(b.Name()), &b);
			if (fx->hasMatDesc())
				addResLambda(i, fx->matDescs.name, &fx->matDescValues);
			for (auto& t : fx->textures)
				addResLambda(i, YRZ::u8(t.Name()), &t);
			for (auto& t : fx->textures3D)
				addResLambda(i, YRZ::u8(t.Name()), &t);
		} else if (fxcombo == 1) {
			//コモンリソース
			int i = 0;
			for (auto&& mo : g_dayo->models) {
				addResLambda(i, std::format("VB@{}", mo.res->u8name), &mo.skinnedBuf);
				addResLambda(i, std::format("preVB@{}", mo.res->u8name), &mo.prevSkinnedBuf);
				addResLambda(i, std::format("rawVB@{}", mo.res->u8name), &mo.res->VB);
				addResLambda(i, std::format("IB@{}", mo.res->u8name), &mo.res->IB);
				addResLambda(i, std::format("BoneMatrix@{}", mo.res->u8name), &mo.boneBuf);
				addResLambda(i, std::format("MorphValues@{}", mo.res->u8name), &mo.morphValuesBuf);
				addResLambda(i, std::format("material@{}", mo.res->u8name), &mo.materialBuf);
				for (int j = 0; auto& t : mo.res->textures) {
					addResLambda(i, std::format("texture {}@{}", j, mo.res->u8name), &t);
					j++;
				}
			}
		}
		if (!combo2res.empty()) {
			rescombo = min(rescombo, combo2res.size() - 1);
			ImGui::Combo("resource", &rescombo, resnames.c_str());
			auto r = combo2res[rescombo];
			if (r != nullptr) {
				if (r->type == YRZ::ResType::buf) {
					if ((r->caps & YRZ::ResCaps::cbv) != YRZ::ResCaps::none)
						pCB = dynamic_cast<YRZ::CB*>(r);
					else
						pbuf = dynamic_cast<YRZ::Buf*>(r);
				} else if (r->type == YRZ::ResType::tex2D) {
					ptex = dynamic_cast<YRZ::Tex2D*>(r);
				} else if (r->type == YRZ::ResType::tex3D) {
					ptex3D = dynamic_cast<YRZ::Tex3D*>(r);
				}
			} else if (rescombo == 0){
				dispPass = true;
			} else if (rescombo == 1) {
				dispManual = true;
			}
		}

		//ファイルダンプ
		auto dumpLambda = [&]<class T>(T * r) {
			std::wstring filename;
			std::vector<std::wstring> filter;
			if constexpr (std::is_same<T, YRZ::Buf>::value) {
				filter = { L"all files(*.*)", L" *.*" };
			} else {
				filter = { L"image files(*.dds,*.jpg,*.png)", L" *.dds;*.jpg;*.png" };
			}
			if (SaveFileDialog(filename, filter, BasePath)) {
				if constexpr (std::is_same<T, YRZ::Buf>::value) {
					std::ofstream ofs(filename, std::ios::binary);
					if (!ofs) {
						OKDlg(L"dump error", L"failed to open " + filename);
					} else {
						std::vector<char> buf(r->desc().Width);
						g_dxr->Download(buf.data(), *r);
						ofs.write(buf.data(), buf.size());
					}
				} else {
					//拡張子が無い場合はddsにする
					if (fs::path(filename).extension().empty()) {
						filename += L".dds";
					}
					try {
						if constexpr (std::is_same<T, YRZ::Tex2D>::value) {
							g_dxr->SaveTex2DToFile(*r, filename.c_str());
						} else if constexpr (std::is_same<T, YRZ::Tex3D>::value) {
							g_dxr->SaveTex3DToFile(*r, filename.c_str());
						}
					} catch (std::exception ex) {
						OKDlgA("dump error", ex.what());
					}
				}
				MessageBeep(MB_ICONINFORMATION);
			}
		};

		//バッファのダンプ表示
		if (pbuf) {
			int X = 8, Y = 16;	//横・縦に何個数値を出すか

			//データ型の選択
			if (ImGui::RadioButton("float", mode == 0))
				mode = 0;
			ImGui::SameLine();
			if (ImGui::RadioButton("int", mode == 1))
				mode = 1;
			ImGui::SameLine();
			if (ImGui::RadioButton("hex", mode == 2))
				mode = 2;
			ImGui::SameLine();
			ImGui::Text("elemSize:%d, elemCount:%d, width:%d", pbuf->elemSize, pbuf->desc().Width / pbuf->elemSize, pbuf->desc().Width);
			ImGui::SameLine();
			if (ImGui::Button("dump")) {
				dumpLambda(pbuf);
			}
			int bpl = X * 4;	//1行あたりのバイト数
			int bpp = Y * bpl;	//1ページ当たりのバイト数
			static int start = 0;	//表示開始位置(バイト単位)
			ImGui::SliderInt("start address", &start, 0, max(0, (int)(pbuf->desc().Width - bpp)), "%08X");
			start = ((start + bpl - 1) / bpl) * bpl;

			if (ImGui::BeginTable("FXdebugTableBuffer", X + 1, ImGuiTableFlags_RowBg)) {
				ImGui::TableSetupColumn("address");
				ImGui::TableSetupScrollFreeze(1, 1);
				ImGui::TableHeadersRow();
				std::vector<uint32_t>dwords(X * Y);
				g_dxr->Download(dwords.data(), *pbuf, 0, start, bpp);

				for (int y = 0; y < Y; y++) {
					uint32_t addr = start + y * bpl;
					ImGui::TableNextColumn();
					ImGui::Text("%08X", addr);
					for (int x = 0; x < X; x++) {
						int o = y * X + x;
						ImGui::TableNextColumn();
						if (o * 4 + start >= pbuf->desc().Width)
							continue;
						//バッファの各要素の先頭
						bool c = ((addr + x * 4) % pbuf->elemSize == 0);
						if (c)
							ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));

						if (mode == 0) {
							float f;
							memcpy(&f, &dwords[o], sizeof(float));
							ImGui::Text("%f", f);
						} else if (mode == 1) {
							ImGui::Text("%d", dwords[o]);
						} else {
							ImGui::Text("%08X", dwords[o]);
						}

						if (c)
							ImGui::PopStyleColor();
					}
				}
				ImGui::EndTable();
			}
		}

		//テクスチャ
		if (ptex || ptex3D) {
			auto* pt = ptex ? (YRZ::Res*)ptex : (YRZ::Res*)ptex3D;
			DebIP->Reset(pt);
			auto debugImageID = YRZ::ImGuiTextureID(*g_dxr, DebIP->output, g_imguiDH, DebugImageDHIndex);
			auto desc = pt->desc();
			static int zoom = 100;
			if (ImGui::RadioButton("25%", zoom == 25))
				zoom = 25;
			ImGui::SameLine();
			if (ImGui::RadioButton("50%", zoom == 50))
				zoom = 50;
			ImGui::SameLine();
			if (ImGui::RadioButton("100%", zoom == 100))
				zoom = 100;
			ImGui::SameLine();
			if (ImGui::RadioButton("200%", zoom == 200))
				zoom = 200;
			ImGui::SameLine();
			if (ImGui::RadioButton("400%", zoom == 400))
				zoom = 400;
			ImGui::SameLine();
			if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
				ImGui::Text("%d x %d %s", desc.Width, desc.Height, YRZ::DXGIFormatToU8(desc.Format).c_str());
			else
				ImGui::Text("%d x %d x %d %s", desc.Width, desc.Height, desc.DepthOrArraySize, YRZ::DXGIFormatToU8(desc.Format).c_str());

			ImGui::SameLine();
			if (ImGui::Button("dump")) {
				dumpLambda(pt);
			}
			const std::vector<std::string> combos = { "default","opaque","hsvR","hsvG","hsvB","hsvA" };
			ImGui::PushItemWidth(ImGui::CalcTextSize("default").x + 48);
			ComboBoxDayo("mode", combos, DebIP->cb->hsvmode);
			ImGui::PopItemWidth();
			ImGui::SameLine();
			ImGui::PushItemWidth(128);
			ImGui::SliderFloat("scale", &DebIP->cb->scale, 1e-3, 1e+3, "%.3f", ImGuiSliderFlags_Logarithmic);
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right))	//右クリックでスケール1.0に戻す
				DebIP->cb->scale = 1.0f;
			if (pt->type == YRZ::ResType::tex3D) {
				ImGui::SameLine();
				ImGui::SliderInt("slice", &DebIP->cb->slice, 0, desc.DepthOrArraySize);
			}
			ImGui::PopItemWidth();

			DebIP->Render();
			ImGui::Image(debugImageID, ImVec2(desc.Width * zoom / 100, desc.Height * zoom / 100));
		}


		//コントローラー
		if (pCB) {
			auto p = (uint8_t*)pCB->pData;
			if (ImGui::BeginTable("FXDebugTableController", 5, ImGuiTableFlags_RowBg)) {
				ImGui::TableSetupColumn("offset", ImGuiTableColumnFlags_WidthStretch, 0.15f);
				ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.4f);
				ImGui::TableSetupColumn("item", ImGuiTableColumnFlags_WidthStretch, 0.4f);
				ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthStretch, 0.15f);
				ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
				ImGui::TableSetupScrollFreeze(1, 1);
				ImGui::TableHeadersRow();
				for (auto&& item : fx->controllers) {
					//オフセット
					ImGui::TableNextColumn();
					ImGui::Text("%d", item.offset);
					//変数名
					ImGui::TableNextColumn();
					if (item.doppelCount == 0)
						ImGui::Text(item.name.c_str());
					else
						ImGui::Text("%s[%d]", item.name.c_str(), item.doppelIndex);
					//アイテム名
					ImGui::TableNextColumn();
					ImGui::Text(item.item.c_str());
					//型
					ImGui::TableNextColumn();
					ImGui::Text(item.type.c_str());
					//データ
					ImGui::TableNextColumn();
					std::string str;
					if (item.type == "float") {
						float f = *(float*)(p + item.offset);
						str = std::to_string(f);
					} else if (item.type == "int") {
						int i = *(int*)(p + item.offset);
						str = std::to_string(i);
					} else if (item.type == "bool") {
						bool i = *(int*)(p + item.offset);
						str = i ? "true" : "false";
					} else if (item.type == "float3") {
						vec3 v = *(vec3*)(p + item.offset);
						str = std::format("{}", v);
					} else if (item.type == "float4") {
						vec4 v = *(vec4*)(p + item.offset);
						str = std::format("{}", v);
					} else if (item.type == "float4x4") {
						Matrix m = *(Matrix*)(p + item.offset);
						m = m.Transpose();	//GPUに渡す値とは転置した状態で表示する
						str = std::format("{}", m);
					}
					ImGui::Text(str.c_str());
					TooltipDayo(str);
				}
				ImGui::EndTable();
			}
		}

		//パス情報表示
		if (dispPass) {
			if (ImGui::BeginTable("FXDebugPassTable", 4, ImGuiTableFlags_RowBg)) {
				ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.15f);
				ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthStretch, 0.15f);
				ImGui::TableSetupColumn("outputSize", ImGuiTableColumnFlags_WidthStretch, 0.25f);
				ImGui::TableSetupColumn("misc", ImGuiTableColumnFlags_WidthStretch, 0.45f);
				ImGui::TableSetupScrollFreeze(1, 1);
				ImGui::TableHeadersRow();
				for (int i = 0; i < fx->passes.size(); i++) {
					auto order = fx->passOrder[i];
					ImGui::TableNextRow();

					ImGui::TableNextColumn();
					ImGui::Text(fx->passNames[i].c_str());

					ImGui::TableNextColumn();
					std::string misc = "";
					const auto& tpass = fx->tech().passes[i];
					//シェーダのエントリポイントを表示するラムダ
					auto shaderLambda = [&]()->std::string {
						std::string s;
						if (!tpass.computeShader.empty()) 
							s += "computeShader:" + tpass.computeShader + " ";
						if (!tpass.pixelShader.empty())
							s += "pixelShader:" + tpass.pixelShader + " ";
						if (!tpass.vertexShader.empty())
							s += "vertexShader:" + tpass.vertexShader+ " ";
						if (!tpass.raygenShader.empty()) {
							s += "raygenShader:" + tpass.raygenShader + " ";
							for (int i = 0; auto& m : tpass.missShader) {
								s += std::format("\nmissShader[{}]:{} ", i, m);
								i++;
							}
							for (int i = 0; auto& h : tpass.hitGroup) {
								s += std::format("\nhitGroup[{}]:(type:{}", i, h.type);
								if (!h.closestHit.empty())
									s += " closestHit:" + h.closestHit;
								if (!h.anyHit.empty())
									s += " anyHit:" + h.anyHit;
								if (!h.intersection.empty())
									s += " intersection:" + h.intersection;
								s += ") ";
								i++;
							}
						}
						return s;
					};
					switch (order) {
					case YRZ::FXPassType::raytracing: ImGui::Text("raytracing"); break;
					case YRZ::FXPassType::postprocess: ImGui::Text("postprocess"); break;
					case YRZ::FXPassType::compute: ImGui::Text("compute");
						const auto ns = fx->CalcNumthreads(tpass.numthreads, fx->outputSize[i].dimension);
						misc = std::format("numthreads[{},{},{}] ", ns.x, ns.y, ns.z);
						break;
					case YRZ::FXPassType::rasterizer: ImGui::Text("rasterizer"); break;
					case YRZ::FXPassType::copy: ImGui::Text("copy");
						misc = std::format("src:{} dest:{}", tpass.src, tpass.dest);
						break;
					case YRZ::FXPassType::clearUAV: ImGui::Text("clearUAV");
						misc = std::format("target:{} value:{},{},{},{}", tpass.target, tpass.value.x, tpass.value.y, tpass.value.z, tpass.value.w);
						break;
					case YRZ::FXPassType::clearRTV: ImGui::Text("clearRTV");
						misc = std::format("target:{} value:{},{},{},{}", tpass.target, tpass.value.x, tpass.value.y, tpass.value.z, tpass.value.w);
						break;
					case YRZ::FXPassType::mipmapgen: ImGui::Text("mipmapgen");
						misc = std::format("target:{}", tpass.target);
						break;
					default: ImGui::Text("none"); break;
					}
					misc += shaderLambda();

					ImGui::TableNextColumn();
					ImGui::Text(fx->outputSize[i].String().c_str());

					ImGui::TableNextColumn();
					ImGui::Text(misc.c_str());
					TooltipDayo(misc);
				}

				ImGui::EndTable();
			}
		}

		//マニュアル
		if (dispManual) {
			if (ImGui::Button("copy")) {
				YRZ::CopyTextToClipboard(YRZ::L(DebIP->manual));
			}

			//エフェクトを割り当てているモデルを逆探知
			int selfIndex = -1;
			for (auto&& mo : g_dayo->models) {
				if (fx == g_dayo->fxWatcher->Get(mo.id)) {
					selfIndex = g_dayo->Find(mo.id);
				}
			}

			DebIP->ReadyManual(fx,selfIndex);
			ImGui::Text(DebIP->manual.c_str());
		}

	}
	ImGui::End();
}


