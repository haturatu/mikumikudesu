// MikuMikuDayo
//
// MMXXIV,MMXXV (c) SANDMAN presents!!
//
// 注意：単にモデルというとキャラクターの事なのかPMXデータの事なのか分からないので
// 「モデル」と「PMX」として区別する
// UE用語と対比すると「モデル」はアクター、「PMX」はスケルタルメッシュに相当する

#include "defsDayo.h"
#include "configDayo.h"
#include "keyframeDayo.h"

//キーフレーム編集窓用コピペ、アンドゥバッファ
struct KeyFrameCopyBuffer;
KeyFrameCopyBuffer g_copyBuffer;
struct KeyFrameUndoBuffer;
KeyFrameUndoBuffer g_undoBuffer;

#include "renDayo.h"
#include "gizmoDayo.h"
#include "hokanDayo.h"
#include "matDayo.h"
#include "expDayo.h"
#include "debDayo.h"

#include "resource.h"

//カメラを示すモデルID
#define IDX_CAMERA	-1

/*****************************************************************************
メインループ
*****************************************************************************/

//カメラのパラメータ
//angle, distanceはUIでの値を入れる(キーフレームとはangle, distanceは反転している)
struct Camera {
	int id = 0;
	Vector3 rotation = { 0,0,0 };		//姿勢
	Vector3 target = { 0,10,0 };	//注目点の位置
	float distance = 45;	//注目点までの距離[MMD]
	float fov = 30;			//垂直画角[deg]
	int parentID = -1;		//追従モデルID,-1で追従なし
	int parentBone = -1;	//追従ボーン
	bool perspective = true;

	PMX::CameraSolver solver;
	KeyFrameWindowContext kc;
	//VMD読み込み
	void LoadVMD(std::wstring filename) {
		auto vmd = PMX::VMD(filename.c_str(), g_cfg->vmdCodepage);
		auto subset = PMX::KeySubset(&vmd);
		kc.RegisterSubset(subset, std::format(L"Load {}", filename), true, true, Frame);
	}
	//timeにおけるカメラのパラメータを取得
	void Solve(float time) {
		solver.Solve(time);
		//キーフレームに描き込まれている値とUIで表示される値は反転している項目があるので補正する
		rotation = Vector3(solver.camera.rotation);
		target = solver.camera.target;
		distance = -solver.camera.distance;
		fov = solver.camera.viewAngle;
		parentID = solver.camera.parentID;
		parentBone = solver.camera.parentBone;
		perspective = solver.camera.parth;
	}
	//コンストラクタ
	Camera() {
		solver = PMX::CameraSolver(nullptr);
		Solve(0);
		kc = {};	//念のため
		kc.id = -1;
		kc.Reset(nullptr, &solver, &g_copyBuffer, &g_undoBuffer);
	};

	//キーフレームデータを作る
	PMX::VMDCamera keyframe(int frame) const {
		PMX::VMDCamera ret;
		ret.iFrame = frame;
		ret.distance = -distance;
		ret.rotation = rotation;
		ret.target = target;
		ret.viewAngle = fov;
		ret.parentID = parentID;
		ret.parentBone = parentBone;
		ret.parth = perspective;
		return ret;
	}

	//コンスタントバッファにパラメータをセットする
	void ToCB(Constantan* cb)
	{
		PMX::VMDCamera cam = keyframe(Frame);
		float aspect = (float)g_dayo->cb->resolution.x / g_dayo->cb->resolution.y;
		XMFLOAT3 campos;
		if (parentID < 0)
			PMX::CameraSolver::Matrices(cam, *reinterpret_cast<XMMATRIX*>(&cb->viewMatrix), *reinterpret_cast<XMMATRIX*>(&cb->projectionMatrix), campos, aspect);
		else {
			//ボーン追従
			int idx = g_dayo->Find(parentID);
			auto bone = g_dayo->models[idx].solver->bones[parentBone];
			PMX::CameraSolver::TrackingMatrices(bone->transform, bone->origin, cam, *reinterpret_cast<XMMATRIX*>(&cb->viewMatrix), *reinterpret_cast<XMMATRIX*>(&cb->projectionMatrix), campos, aspect);
		}

		cb->lightColor = solver.light.color;
		cb->lightDirection = solver.light.direction;
		cb->lightDirection.Normalize();
		cb->selfShadowMode = solver.selfshadow.kind;
		cb->selfShadowDistance = solver.selfshadow.distance;
		cb->perspective = solver.camera.parth;
		cb->cameraInterpolated = solver.interpolated;

		g_physics.World()->setGravity(solver.btGravity);
	}
};


#include "saveDayo.h"


//ユーティリティ
bool LoadWave()
{
	try {
		g_player.Reset();
		if (!g_edi.wavFile.empty()) {
			g_wave.Load(YRZ::L(g_edi.wavFile).c_str());
			g_player.Set(g_wave);
		} else {
			g_wave.Clear();
		}
		PlotWave(KFCellWidth + 1);
	} catch (...) {
		OKDlg(YRZ::L(g_edi.wavFile), g_hon.L(L"audio file error"));
		return false;
	}
	return true;
}

//ファイルのロードなどでプロジェクト設定g_ediが更新された
void UpdateProjectSettings()
{
	LoadWave();
	g_dayo->LoadSkybox(YRZ::L(g_edi.skyboxFile));
	g_dayo->LoadMovie(YRZ::L(g_edi.movieFile));	//2026-0309修正(YRZ::Lで囲うの忘れてた)
	g_dayo->cb->iSample = 0;
	g_physics.GroundEnable(g_edi.floorCollision);
	Wave::Volume(g_edi.wavVolume);
}

//新規プロジェクト設定(モデルの削除などはしない、あくまでg_ediの初期化のみ)
void NewProjectSettings()
{
	g_edi = {};
	g_edi.skyboxFile = g_cfg->skybox;
	g_edi.outputFile = YRZ::u8(BasePath / L"movie\\dayo.png");
	AnimFrame = Frame = 0;
	DayoPath = L"";
}

HICON CreateIconFromFile(const wchar_t* filename)
{
	TexMetadata metadata;
	ScratchImage image;
	if (FAILED(LoadFromWICFile(filename, WIC_FLAGS_NONE, &metadata, image)))
		return NULL;

	const Image* img = image.GetImage(0, 0, 0);
	if (!img) return NULL;

	// DIB作成
	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = (LONG)img->width;
	bmi.bmiHeader.biHeight = -(LONG)img->height; // top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	void* bits = nullptr;
	HBITMAP hBmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!hBmp) return NULL;

	memcpy(bits, img->pixels, img->rowPitch * img->height);

	BITMAP bmpInfo;
	if (GetObject(hBmp, sizeof(bmpInfo), &bmpInfo) == 0) {
		MessageBox(NULL, L"GetObject failed", L"Debug", MB_OK);
	} else {
		wchar_t buf[256];
		swprintf_s(buf, L"Width=%ld Height=%ld Bits=%d Planes=%d",
			bmpInfo.bmWidth, bmpInfo.bmHeight, bmpInfo.bmBitsPixel,
			bmpInfo.bmPlanes);
		MessageBox(NULL, buf, L"HBITMAP Info", MB_OK);
	}

	ICONINFO ii = {};
	ii.fIcon = TRUE;
	ii.hbmColor = hBmp;
	ii.hbmMask = NULL;

	HICON hIcon = CreateIconIndirect(&ii);
	DeleteObject(hBmp);
	return hIcon;
}

void Dayo2(HINSTANCE hInstance)
{
	//コンフィグの読み込み
	Config cfg;
	g_cfg = &cfg;
	cfg.Load();
	MipmapGen = cfg.mipmapGen;
	Language = cfg.language;

	//初期化
	HICON hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
	YRZ::App app(L"MikuMikuDayo", cfg.width, cfg.height, WS_OVERLAPPEDWINDOW, nullptr, hIcon, hIcon);

#if 1
	//通常仕様
	YRZ::DXR dxr(app.hWnd(), true);
#else
	//出先仕様
	YRZ::DXR dxr(app.hWnd(), false);
	CompileOption.push_back(L"-D");
	CompileOption.push_back(L"NO_BARYCENTRICS");
#endif

	DragAcceptFiles(app.hWnd(), true);
	g_app = &app;
	g_dxr = &dxr;
	Wave::Init();

	//GPUビデオてすと
	/*
	Wave::GPUMovie gpu;
	bool fallback;
	gpu.Load(g_dxr, "dreiik.mp4", fallback);
	*/

	//ImGuiのiniファイルが見つからない場合はimgui_default.iniをコピーする
	if (!fs::exists(BasePath / L"imgui.ini")) {
		try {
			fs::copy_file(BasePath / L"imgui_default.ini", BasePath / L"imgui.ini");
		} catch(...) {
			OKDlg(L"startup", L"imgui_default.ini not found");
		}
	}

	//出力用
	int imgW, imgH;					//レンダラー窓のサイズ
	int imgX, imgY;					//レンダー窓の左上座標
	NewProjectSettings();

	//レンダラー一覧
	EnumRenderer();

	//ホットキーの設定(なんかPrintScreenキーが押されたことを知るためのメッセージが無いので)
	if (g_cfg->printScreen)
		RegisterHotKey(g_app->hWnd(), 0, MOD_NOREPEAT, VK_SNAPSHOT);

	//メインのレンダラー作成
	Renderer dayo(640, 480);
	g_dayo = &dayo;
	//dayo.AddModel(BasePath / L"sample\\null.pmx");	//0番に「表示されないモデル」を入れる
	dayo.CreatePass(CPFlag::init);	//AddModelしない場合は明示的にCreatePass

	//カメラ制御用(UIで入力される値であり、キーフレームとは距離は反転する)
	Camera camera;

	//CBのデフォルト値
	dayo.cb->iSample = 0;
	dayo.cb->time = 0;
	dayo.cb->sceneRadius = 10000;
	dayo.cb->screenBMPMode = 1;
	dayo.cb->backgroundMode = 0;
	camera.ToCB(dayo.cb);

	//IMGUI用ディスクリプタヒープを作る
	g_imguiDH = YRZ::CreateImGuiDescriptorHeap(*g_dxr);

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigWindowsMoveFromTitleBarOnly = true;

	if (!g_cfg->font.empty()) {
		auto ttf = YRZ::A(YRZ::FindFontFile(YRZ::L(g_cfg->font)));
		
		ImFontConfig cfg = {};
		//static ImWchar exclude_ranges[] = { 0x1F600, 0x1FFFF, 0 };
		//cfg.GlyphExcludeRanges = exclude_ranges;

		io.Fonts->AddFontFromFileTTF(ttf.c_str(), 12.0f, &cfg);
		
		/*
		static const ImWchar emoji_ranges[] = {
			0x1F300, 0x1F5FF,   // Miscellaneous Symbols and Pictographs
			0x1F600, 0x1F64F,   // Emoticons
			0
		};
		ImFontConfig cfg2;
		cfg.MergeMode    = true;
		cfg.OversampleH  = 1;
		cfg.OversampleV  = 1;
		cfg.PixelSnapH   = true;
		ttf = YRZ::wstrToANSI(YRZ::FindFontFile(L"seguiemj.ttf"));
		io.Fonts->AddFontFromFileTTF(ttf.c_str(), 12.0f, &cfg2);
		io.Fonts->Build();
		*/
	}


	/*
	static ImWchar const emoji_ranges[] = { 0x10000, 0x1ffff,0, };
	auto fontEmoji = YRZ::FindFontFile(L"Segoe UI Emoji");
	if (!fontEmoji.empty())
		io.Fonts->AddFontFromFileTTF(YRZ::wstrToANSI(fontEmoji).c_str(), 12.0f, nullptr, emoji_ranges);
	*/
	//ImGui::StyleColorsDark();
	StyleColorsDayo();

	g_physics.Reset();
	dayo.Solve(0);

	// Setup Platform/Renderer backends
	ImGui_ImplWin32_Init(g_app->hWnd());
	auto iminfo = YRZ::ImGuiInfo(dxr, g_imguiDH);
	ImGui_ImplDX12_Init(&iminfo);

	//ppOutをImageとして使えるようにする
	auto renderImageDHIndex = YRZ::ImguiDHAlloc.FreeIndices.back();	//imguiDH内でのimageの置かれる場所
	auto renderImageID = YRZ::ImGuiTextureID(*g_dxr, dayo.dayoOut, g_imguiDH, renderImageDHIndex);
	YRZ::ImguiDHAlloc.FreeIndices.pop_back();

	//debug窓用
	DebugImageDHIndex = YRZ::ImguiDHAlloc.FreeIndices.back();
	YRZ::ImguiDHAlloc.FreeIndices.pop_back();
	DebIP = std::make_unique<DebugImageProcessor>();

	//dayoicon
	auto dayoIcon = dxr.CreateTex2D((BasePath / L"res\\dayoicon.png").wstring().c_str());
	auto dayoIconDHIndex = YRZ::ImguiDHAlloc.FreeIndices.back();
	auto dayoIconID = YRZ::ImGuiTextureID(*g_dxr, dayoIcon, g_imguiDH, dayoIconDHIndex);
	YRZ::ImguiDHAlloc.FreeIndices.pop_back();

	//fps計測用
	StopWatch watch;
	float dtime = (float)watch.Lap();
	uint64_t ftime = watch.splitTime;	//フレーム開始時の時刻[tick]
	uint64_t lastPtime = ftime;	//最後にplayボタンが押された時間または起動時の時刻のうち遅い方

	//制御用変数
	uint32_t nFramesDrawn = 0;	//起動してから今までレンダリングした回数(fps計測用)
	bool nowLoadingPops = false;	//dayoファイル読み込みちゅうダイアログの表示
	LoadDayoContext ldc;		//ローディング進捗管理

	//GUI用変数
	static int iModel = IDX_CAMERA;
	static auto* kc = &camera.kc;
	static KeyFrameWindowResult kr = {};		//前フレームでのキーフレーム表示ウィンドウの編集結果

	//iModel == ID_CAMERAの時はカメラを表す

	/* ラムダ式の定義 */

	//現在のキーフレームコンテクスト切り替え
	auto switchKCLambda = [&](auto* nextkc) { kc = nextkc; kc->Reset(); };
	//モデルとキーフレームコンテクストまとめて切り替え
	auto switchModelLambda = [&](int idx) { iModel = idx; if (iModel == IDX_CAMERA) switchKCLambda(&camera.kc); else switchKCLambda(&dayo.models[iModel].kc); };
	//ID→models内の添え字、無効なIDだった場合、falseを返す
	auto IDtoIndexLambda = [&](int id, int& idx) {
		if (id == -1) {
			idx = IDX_CAMERA;
			return true;
		} else {
			for (int i = 0; auto && m : dayo.models) {
				if (m.id == id) {
					idx = i;
					return true;
				}
				i++;
			}
		}
		idx = -2;
		return false;
		};

	//dayoファイルの読み込み後処理(以降、時々使いまわされるので)
	auto DayoLoadedLambda = [&]() {
		dayo.Solve(AnimFrame);
		g_physics.Reset();
		camera.Solve(AnimFrame);
		camera.ToCB(dayo.cb);
		dayo.cb->iSample = 0;
		switchModelLambda(dayo.models.size() - 1);
		UpdateProjectSettings();
	};

	//最後にキーが打たれているフレーム番号を得る
	auto lastFrameLambda = [&]() {
		int lastFrame = camera.solver.LastFrame();
		for (auto&& mo : dayo.models)
			lastFrame = max(lastFrame, mo.solver->LastFrame());
		return lastFrame;
		};

	//PMXのローカル軸フレーム取得(ローカル軸座標系→自分座標系)
	auto localFrameLambda = [](const auto& pmxb) {
		Matrix r = Matrix::Identity;
		if (pmxb.IsFixAxis()) {
			//固定軸回転の場合は固定軸をX軸として表示させる
			_11_21_31(r, pmxb.fixAxis);
			vec3 Z = vec3(pmxb.fixAxis) != vec3(0, 0, 1) ? vec3(0, 0, 1) : vec3(-1, 0, 0);
			vec3 Y = _12_22_32(r, Normalize(Cross(Z, pmxb.fixAxis)));
			_13_23_33(r, Cross(pmxb.fixAxis, Y));	//この時点では自分座標系→ローカル軸座標系
			r = r.Transpose();	//転置して逆行列を作り、ローカル軸→自分変換にする
		} else if (pmxb.IsLocalFrame()) {
			//ローカル軸はX,Zの指定がある
			_11_21_31(r, pmxb.localAxisX);
			vec3 Y = _12_22_32(r, Normalize(Cross(pmxb.localAxisZ, pmxb.localAxisX)));
			_13_23_33(r, Cross(pmxb.localAxisX, Y));
			r = r.Transpose();
		}
		return r;
	};



	//コマンドラインオプション、第一引数がある場合はdayoファイルとして開く
	if (g_argv.size() >= 2) {
		auto dayofile = g_argv[1];
		switchModelLambda(IDX_CAMERA);
		NowLoading = true;
		nowLoadingPops = true;
		LoadDayo(ldc, dayofile, &camera.solver);
	}

	MSG msg;
	while (g_app->MainLoop(msg)) {
		static bool resample = false;	//カメラの視点移動など操作が原因でサンプルの蓄積し直しが必要？
		static bool eventHandled = false;	//イベントをハンドリングしてcontinueで戻ってきた？
		if (!eventHandled) {
			resample = g_shaderUpdated;
			if (kc != nullptr)
				kc->status = KeyFrameStatus::none;
		}
		//新しいフレームの始めに、コンテキストの更新状態をクリアする
		//↑をチェックしてるのはイベントハンドラでコンテキストを更新される可能性があるため

		//イベントハンドラ
		ImGui_ImplWin32_WndProcHandler(g_app->hWnd(), msg.message, msg.wParam, msg.lParam);

		if (msg.message == WM_DROPFILES && !NowLoading) {
			auto drops = YRZ::DroppedFiles(reinterpret_cast<HDROP>(msg.wParam), 1, false);
			if (!drops.empty()) {
				auto& dropped = drops[0];
				auto ext = wtolower(std::filesystem::path(dropped).extension().wstring());
				if (ext == L".vmd") {
					bool isCam = PMX::VMD::IsCameraVMDFile(dropped.c_str());
					if (isCam && iModel == IDX_CAMERA) {
						camera.LoadVMD(dropped);
						camera.Solve(AnimFrame);
						camera.ToCB(g_dayo->cb);
					} else if (!isCam && iModel != IDX_CAMERA) {
						dayo.models[iModel].LoadVMD(dropped);
						dayo.Solve(AnimFrame);
						g_physics.Reset();
					} else if (iModel == IDX_CAMERA) {
						OKDlg(L"VMD", g_hon.L(L"Not a VMD file for camera/light"));
					} else {
						OKDlg(L"VMD", g_hon.L(L"Not a VMD file for models"));
					}
					resample = true;
				} else if( ext == L".vpd") {
					if (iModel == IDX_CAMERA) {
						OKDlg(L"VPD", g_hon.L(L"VPD files cannot be loaded into the camera."));
					} else {
						try {
							PMX::KeySubset sub;
							sub.LoadVPD(dropped.c_str(), cfg.vmdCodepage);
							kc->RegisterSubset(sub, std::format(L"Load {}", YRZ::ExtractFilename(dropped)), true, true, Frame);
							dayo.Solve(AnimFrame);
							g_physics.Reset();
							resample = true;
						} catch (std::exception ex) {
							OKDlgA("VPD error", ex.what());
						}
					}
				} else if (ext == L".pmx") {
					PMX::PMXModel pmx(dropped.c_str(), true);
					if (OKCancelDlg(pmx.name, (Language==0 || pmx.description_e.empty()) ? pmx.description : pmx.description_e)) {
						dayo.AddModel(dropped);
						dayo.Solve(AnimFrame);
						g_physics.Reset();
						resample = true;
						switchModelLambda(dayo.models.size() - 1);
					}
				} else if (ext == L".bmp" || ext == L".jpg" || ext == L".png" || ext == L".hdr" || ext == L".dds") {
					dayo.LoadSkybox(dropped);
					g_edi.skyboxFile = YRZ::u8(dropped);
					resample = true;
				} else if (ext == L".wav" || ext == L".m4a" || ext == L".mp3") {
					auto tmp = g_edi.wavFile;
					g_edi.wavFile = YRZ::u8(dropped);
					Wave::Volume(g_edi.wavVolume);
					if (!LoadWave())
						g_edi.wavFile = tmp;
				} else if (ext == L".dayo") {
					if (OKCancelDlg(L"Load Dayo", dropped + g_hon.L(L" is going to load. Are you sure?"))) {
						nowLoadingPops = true;
						NowLoading = true;
						switchModelLambda(IDX_CAMERA);
						LoadDayo(ldc, dropped, &camera.solver);
						resample = true;
					}
				} else if (ext == L".mp4" || ext == L".avi") {
					dayo.LoadMovie(dropped);
					resample = true;
				}
			}
		}
		if (msg.message == WM_HOTKEY) {
			//スクショ撮る
			time_t t = time(NULL);
			struct tm local;
			localtime_s(&local, &t);
			wchar_t buf[MAX_PATH];	//24昼修正,invalid string positionが出る人がいるとのことで128→MAX_PATHへ増やした
			wcsftime(buf, MAX_PATH, (BasePath / L"screenshots\\SS_%Y_%m%d_%H%M%S.png").wstring().c_str(), &local);
			g_dxr->SaveTex2DToFile(dayo.imageOut, buf);
		}
		if (msg.message == WM_SIZE) {
			int w = LOWORD(msg.lParam);
			int h = HIWORD(msg.lParam);
			if (w && h) {
				g_dxr->Resize(w, h);
			}
		}

		//メッセージを処理し終わるまではレンダリングしない
		//そうしないと1つメッセージ処理するのに1/60秒以上掛かるため、メッセージが少し多く到着する期間においては簡単にメッセージが溢れて動けなくなってしまう
		if (msg.message != WM_NULL) {
			eventHandled = true;
			continue;
		}
		eventHandled = false;
		
		//dtimeはここで更新しないとイベントハンドラが起動された場合、描画が行われないのでdtimeが過小に評価されてしまう
		dtime = (float)watch.Lap();
		ftime = watch.splitTime;
		g_edi.totalEditTime += (size_t)roundf(dtime * 1000);

		/***** ImGui *****/
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		ImGuizmo::BeginFrame();

		ImGui::ShowDemoWindow();

		//ポップアップ制御変数
		static bool aboutPops = false;
		static bool keybindPops = false;
		static bool prefPops = false;
		static bool jointPops = false;
		static bool creditPops = false;
		static bool creditPopsOpening = false;	//借り物リストが開かれるフレームであるフラグ

		if (ImGui::BeginMainMenuBar()) {
			//ダヨメニュー
			if (ImGui::BeginMenu("Dayo")) {
				//新規ダヨ
				if (ImGui::MenuItem(g_hon.C("New Dayo..."))) {
					if (NoYesDlg(L"new dayo", g_hon.L(L"Clears all data being edited, cannot be undone. Are you sure?"))) {
						int n = dayo.models.size();
						//モデル削除
						for (int i = n - 1; i > IDX_CAMERA; i--) {
							dayo.DeleteModel(i, false);
						}
						dayo.CreatePass(CPFlag::del);
						dayo.nModelLoaded = 0;
						switchModelLambda(IDX_CAMERA);
						g_undoBuffer.buffer.clear();
						g_undoBuffer.index = 0;
						//カメラを初期化
						camera.solver.SelectAllKeys(true);
						camera.solver.DeleteKeys();
						camera.Solve(0);
						camera.ToCB(dayo.cb);
						resample = true;
						//その他初期化
						NewProjectSettings();
						UpdateProjectSettings();
						SetWindowText(dxr.hWnd(), L"MikuMikuDayo");
					}
				}

				//開くダヨ
				if (ImGui::MenuItem(g_hon.C("Open Dayo..."), nullptr)) {
					if (AskLoadDayo(&camera.solver,ldc)) {
						switchModelLambda(IDX_CAMERA);
						nowLoadingPops = true;
						NowLoading = true;
					}
				}
				//上書保存ダヨ
				if (ImGui::MenuItem(g_hon.C("Save Dayo"), nullptr)) {
					bool saved = false;
					if (!DayoPath.empty()) {
						bool canceled = false;
						//上書対象ファイルが存在するなら必要に応じて上書して良いかきく
						if (fs::exists(DayoPath) && cfg.overwritePrompt) {
							if (!OKCancelDlg(L"save dayo", g_hon.L(L"Overwrite and save, OK?"))) {
								canceled = true;
							}
						}
						if (!canceled) {
							if (!SaveDayo(DayoPath, &camera.solver))
								OKDlg(L"Save Dayo Failed", g_problems);
							else
								saved = true;
						}
					} else {
						//ファイル名が設定されてない場合は名前を付けて保存
						if (AskSaveDayo(&camera.solver)) {
							saved = true;
							if (!g_problems.empty())
								OKDlg(L"Save As Dayo", g_problems);
						}
					}
					kc->UpdateKeyDisp(KeyFrameStatus::check);
					if (saved)
						MessageBeep(MB_ICONINFORMATION);
				}
				//名前を付けて保存ダヨ
				if (ImGui::MenuItem(g_hon.C("Save As Dayo..."), nullptr)) {
					if (AskSaveDayo(&camera.solver)) {
						if (!g_problems.empty())
							OKDlg(L"Save As Dayo", g_problems);
						kc->UpdateKeyDisp(KeyFrameStatus::check);
						MessageBeep(MB_ICONINFORMATION);
					}
				}

				ImGui::Separator();

				//VMD保存ダヨ
				if (ImGui::MenuItem(g_hon.C("Export VMD..."), nullptr)) {
					std::wstring filename;
					if (!kc->solver->AnySelected()) {
						OKDlg(L"Export VMD", g_hon.L(L"no keys selected for export"));
					} else if (SaveFileDialog(filename, { L"vocaloid motion data(*.vmd)" ,L"*.vmd" }, BasePath)) {
						if (fs::path(filename).extension().empty()) {
							filename += L".vmd";
						}
						PMX::KeySubset sub;
						kc->solver->Subset(sub, true);
						PMX::VMD vmd;
						vmd = sub.MakeVMD(kc->namew);
						vmd.Save(iModel==IDX_CAMERA, filename, g_cfg->vmdCodepage);
					}
				}
				//クレジット作成補助ダヨ
				if (ImGui::MenuItem(g_hon.C("Items list..."), nullptr)) {
					creditPops = true;
					creditPopsOpening = true;
				}
				//アプリ自体の設定ダヨ
				if (ImGui::MenuItem((char*)u8"設定(Preferences)...", nullptr)) {
					prefPops = true;
				}
				ImGui::EndMenu();

			}
			//表示メニュー
			if (ImGui::BeginMenu("View")) {
				ImGui::MenuItem(g_hon.C("Display frame infomation"), nullptr, &Infomative);
				ImGui::MenuItem(g_hon.C("Display rigid bodies"), nullptr, &DisplayRB);
				ImGui::Separator();
				ImGui::MenuItem(g_hon.C("Update camera/light in model mode"), nullptr, &SyncCamera);
				bool phys = (PhysicsMode != PhysMode::disable);
				if (ImGui::MenuItem(g_hon.C("Physics simulation"), nullptr, &phys)) {
					if (phys)
						PhysicsMode = PhysMode::step;
					else
						PhysicsMode = PhysMode::disable;
					g_dayo->cb->iSample = 0;
				}
				ImGui::Separator();
				ImGui::MenuItem(g_hon.C("Denoise"), nullptr, &Denoise);
				ImGui::SeparatorText(g_hon.C("Interleave"));
				if (ImGui::RadioButton(g_hon.C("No Interleave"), Interleave == 1))
					Interleave = 1;
				if (ImGui::RadioButton(g_hon.C("Interleave 1/2"), Interleave == 2))
					Interleave = 2;
				if (ImGui::RadioButton(g_hon.C("Interleave 1/4"), Interleave == 4))
					Interleave = 4;
				ImGui::SeparatorText(g_hon.C("Operation mode"));
				if (ImGui::RadioButton(g_hon.C("Still image"), Accumulate && !AlwaysSolve)) {
					Accumulate = true;
					AlwaysSolve = false;
					g_dayo->cb->iSample = 0;
				}
				if (ImGui::RadioButton(g_hon.C("Realtime"), !Accumulate && AlwaysSolve)) {
					Accumulate = false;
					AlwaysSolve = true;
					g_dayo->cb->iSample = 0;
				}
				if (ImGui::RadioButton(g_hon.C("Energy saver"), !Accumulate && !AlwaysSolve)) {
					Accumulate = false;
					AlwaysSolve = false;
				}

				ImGui::EndMenu();
			}
			//レンダラーメニュー
			if (ImGui::BeginMenu("Renderer")) {
				for (int i = 0; i < Renderers.size(); i++) {
					auto label = Renderers[i].stem().u8string();
					if (label != u8"ColorBar") {
						if (ImGui::RadioButton((char*)label.c_str(), RendererIndex == i) && (i != RendererIndex)) {
							bool dlg = true;
							auto fx = g_dayo->fxWatcher->Get(WID_RENDERER);
							if (fx && fx->hasMatDesc())
								dlg = NoYesDlg(L"renderer", g_hon.L(L"Changing the renderer clears the materials and cannot be undone. Are you sure?"));
							if (dlg) {
								RendererIndex = i;
								g_dayo->fxWatcher->EndWatch(WID_RENDERER);
								g_dayo->CreatePass(CPFlag::del);
								g_dayo->LoadSkybox(L"");
								resample = true;
							}
						}
					}
				}
				ImGui::EndMenu();
			}
			//背景メニュー
			if (ImGui::BeginMenu("Background")) {
				if (ImGui::RadioButton(g_hon.C("Screen capture - off"), g_dayo->cb->screenBMPMode == 0)) {
					g_dayo->cb->screenBMPMode = 0;
					g_dayo->cb->iSample = 0;
				}
				if (ImGui::RadioButton(g_hon.C("Screen capture - fullscreen"), g_dayo->cb->screenBMPMode == 1)) {
					g_dayo->cb->screenBMPMode = 1;
					g_dayo->cb->iSample = 0;
				}
				if (ImGui::RadioButton(g_hon.C("Screen capture - 4:3"), g_dayo->cb->screenBMPMode == 2)) {
					g_dayo->cb->screenBMPMode = 2;
					g_dayo->cb->iSample = 0;
				}
				ImGui::Separator();
				if (ImGui::RadioButton(g_hon.C("Fill by renderer defaults"), g_dayo->cb->backgroundMode == 0)) {
					g_dayo->cb->backgroundMode = 0;
					g_dayo->cb->iSample = 0;
				}
				if (ImGui::RadioButton(g_hon.C("Fill by screen.bmp"), g_dayo->cb->backgroundMode == 1)) {
					g_dayo->cb->backgroundMode = 1;
					g_dayo->cb->iSample = 0;
				}
				if (ImGui::RadioButton(g_hon.C("Fill by white"), g_dayo->cb->backgroundMode == 2)) {
					g_dayo->cb->backgroundMode = 2;
					g_dayo->cb->iSample = 0;
				}
				if (ImGui::RadioButton(g_hon.C("Fill by black"), g_dayo->cb->backgroundMode == 3)) {
					g_dayo->cb->backgroundMode = 3;
					g_dayo->cb->iSample = 0;
				}
				if (ImGui::RadioButton(g_hon.C("Fill by checkerboard"), g_dayo->cb->backgroundMode == 4)) {
					g_dayo->cb->backgroundMode = 4;
					g_dayo->cb->iSample = 0;
				}
				ImGui::Separator();
				if (ImGui::MenuItem(g_hon.C("Output background with alpha=0"), nullptr, g_dayo->cb->backgroundTransparent == 1)) {
					g_dayo->cb->backgroundTransparent = g_dayo->cb->backgroundTransparent ? 0 : 1;
				}


				ImGui::EndMenu();
			}
			//ヘルプメニュー
			if (ImGui::BeginMenu("Help")) {
				if (ImGui::MenuItem(g_hon.C("Shortcuts..."))) {
					keybindPops = true;
				}
				if (ImGui::MenuItem(g_hon.Do("About MikuMikuDayo...").c_str())) {
					aboutPops = true;
				}
				ImGui::EndMenu();
			}

			ImGui::EndMainMenuBar();
		}


		//画面中央にポップアップを出す
		auto centerPopLambda = [&](const std::string& title, bool& flag, const std::optional<ImVec2>& size = {}) {
			if (flag) {
				ImGui::OpenPopup(title.c_str());
				ImVec2 center = ImGui::GetMainViewport()->GetCenter();
				ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
				if (size)
					ImGui::SetNextWindowSize(*size);
				flag = false;
			}
		};

		//なうろダヨ
		centerPopLambda("Loading Dayo", nowLoadingPops, ImVec2(640,320));
		if (ImGui::BeginPopupModal("Loading Dayo", nullptr)) {
			ImGui::TextWrapped(ldc.progress.c_str());
			double curT = timeGetTime();
			static bool finishLoad = true;	//ロード完了時の処理をまだやってないフラグ

			if (ldc.Finished() || ldc.failed) {
				//LoadDayo()自体は終わったダヨ
				if (!finishLoad && (curT - ldc.time > 100)) {
					//skyboxの読み込みとか残りの処理をするダヨ
					DayoLoadedLambda();
					ldc.progress += "Done.\n";
					finishLoad = true;
				}
				//全部終わったから問題点があれば表示するダヨ
				if (finishLoad) {
					bool closing = false;
					if (!g_problems.empty()) {
						//ロードが終わって問題点が出てきたダヨ
						std::string str = g_hon.Do("\n-------- problems have been found while loading --------\n");
						str += YRZ::u8(g_problems);
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
						ImGui::TextWrapped(str.c_str());
						ImGui::PopStyleColor();
						if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Escape))
							closing = true;
					} else {
						closing = true;
					}
					if (closing) {
						//問題点の表示も終わったから閉じるダヨ
						ldc = {};
						NowLoading = false;
						resample = true;
						ImGui::CloseCurrentPopup();
					}
				}
			} else if (curT - ldc.time > 100) {
				finishLoad = false;
				try {
					LoadDayo(ldc);	//ローディングを1ステップ進めるダヨ
				} catch (std::exception ex) {
					//なんか認識できないような例外が出たのでお手上げダヨ
					g_problems += L"\n-------- exception raised while loading --------\n";
					g_problems += YRZ::L(ex.what());
					ldc.failed = true;
				}
			}
			ImGui::EndPopup();
		}

		//Aboutダイアログ
		centerPopLambda("About", aboutPops);
		if (ImGui::BeginPopupModal("About", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::Text("MikuMikuDayo version %s\n", DayoVersionStr.c_str());
			ImGui::Text("MMXXIV,MMXXV,MMXXVI (c) SANDMAN presents!");
			ImGui::Text("X : @NenemSdmn");
			ImGui::Text("github : pennennennennennenem");
			ImGui::Separator();
			ImGui::Text("Bullet physics %d", PMX::BulletPhysicsVersion);
			ImGui::Text("Cereal %d.%d.%d", CEREAL_VERSION_MAJOR, CEREAL_VERSION_MINOR, CEREAL_VERSION_PATCH);
			ImGui::Text("DirectXTex");
			ImGui::Text("ImGui %s", ImGui::GetVersion());
			ImGui::Text("ImGuizmo 1.91.3 WIP");
			ImGui::Text("Intel open image denoiser 2.4.1");
			ImGui::Text("Jsonnet %s",jsonnet_version());
			ImGui::Separator();
			ImGui::Text("Total edit time of this project : %s", YRZ::u8(FormatMilliseconds(g_edi.totalEditTime)).c_str());

			if (ImGui::Button((char*)u8"Dayo!😍") || ImGui::IsKeyPressed(ImGuiKey_Escape))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		//キーバインド一覧ダイアログ
		centerPopLambda("Shortcuts", keybindPops);
		if (ImGui::BeginPopupModal("Shortcuts", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			if (ImGui::BeginTable("shortcuts", 2, ImGuiTableFlags_RowBg)) {
				for (auto&& item : g_KeyBinds) {
					ImGui::TableNextColumn();
					ImGui::Text(item->disp.c_str());
					ImGui::TableNextColumn();
					ImGui::Text(g_hon.C(item->rawDesc));
				}
				ImGui::EndTable();
			}
			if (ImGui::Button("close") || ImGui::IsKeyPressed(ImGuiKey_Escape))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		//設定ダイアログ
		centerPopLambda("Preferences", prefPops);
		if (ImGui::BeginPopupModal("Preferences", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			
			ImGui::SeparatorText("Editor");
			ImGui::Checkbox(g_hon.C("Always display camera keys"), &cfg.alwaysShowCamKeys);
			if (ImGui::Checkbox(g_hon.C("Take a screenshot with the printscreen key"), &cfg.printScreen)) {
				if (cfg.printScreen) {
					RegisterHotKey(g_app->hWnd(), 0, MOD_NOREPEAT, VK_SNAPSHOT);
				} else {
					UnregisterHotKey(g_app->hWnd(), 0);
				}
			}
			ImGui::InputFloat(g_hon.C("Snap translation"), &cfg.snapTranslation);
			ImGui::InputFloat(g_hon.C("Snap rotation"), &cfg.snapRotation);

			ImGui::SeparatorText("Resources");
			ImGui::Checkbox(g_hon.C("Generate mipmap chain"), &MipmapGen);

			auto langLambda = [&](int l) {
				Language = l;
				dayo.ResetLanguage();
				camera.kc.Reset();
				};

			ImGui::SeparatorText("Language");
			if (ImGui::RadioButton("Japanese", Language == 0))
				langLambda(0);
			ImGui::SameLine();
			if (ImGui::RadioButton("English", Language == -1))
				langLambda(-1);
			
			ImGui::SeparatorText("Infomation");
			ImGui::InputInt(g_hon.C("Waveform display frames"), &cfg.waveInfoFrames);
			cfg.waveInfoFrames = max(0, min(32, cfg.waveInfoFrames));
			
			ImGui::SeparatorText("Physics");
			ImGui::InputFloat("fps", &cfg.physicsFps);
			ImGui::InputInt(g_hon.C("max substeps"), &cfg.physicsMaxSubsteps);
			ImGui::InputInt(g_hon.C("prewarm steps"), &cfg.physicsPrewarmSteps);

			ImGui::SeparatorText("Save");
			ImGui::Checkbox(g_hon.C("overwrite prompt"), &cfg.overwritePrompt);
			ImGui::Checkbox(g_hon.C("make .bak file when overwrite"), &cfg.makeBak);

			//ポップアップを閉じて設定の反映
			if (ImGui::Button("close") || (ImGui::IsKeyPressed(ImGuiKey_Escape))) {
				g_physics.fps = g_cfg->physicsFps;
				g_physics.maxSubsteps = g_cfg->physicsMaxSubsteps;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		//物理ジョイント設定
		centerPopLambda("Joints", jointPops);
		if (ImGui::BeginPopupModal("Joints", nullptr)) {

			//最初に閉じるボタン。ポップアップを閉じて設定の反映
			if (ImGui::Button("close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
				ImGui::CloseCurrentPopup();
			}

			//モデル一覧
			std::string modelnames;
			for (auto&& mo : dayo.models) {
				modelnames += mo.u8name;
				modelnames.push_back(0);
			}
			static int icombo = 0;
			ImGui::Combo("model", &icombo, modelnames.c_str());

			//フィルター一覧
			std::vector<std::string> filters = { "(none)", (char*)u8"乳,胸,breast,chest" };
			std::vector<std::string> filtertoks;
			std::string filterstr;
			for (auto&& f : filters) {
				filterstr += f;
				filterstr.push_back(0);
			}
			static int ifilter = 0;
			ImGui::Combo("filter", &ifilter, filterstr.c_str());
			filtertoks = YRZ::SplitStr(filters[ifilter], ',');

			std::set<int> dispIdx;	//フィルター条件に合致して表示されているジョイント番号

			//ジョイント表示
			if (icombo < dayo.models.size() && icombo>=0) {
				//操作
				static float cfm = 0;
				static bool useFO = true;
				ImGui::InputFloat("CFM", &cfm);
				ImGui::SameLine();
				ImGui::Checkbox("useFrameOffset", &useFO);
				ImGui::SameLine();
				bool setVal = ImGui::Button("set");

				//ジョイント表
				if (ImGui::BeginTable("jointTable", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
					ImGui::TableSetupColumn("joint", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("bodyA", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("bodyB", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("frameOffset", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("CFM", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableHeadersRow();

					for (int j = 0; auto&& joint : dayo.models[icombo].solver->joint) {
						if (joint->src == -1) { j++; continue; }	//無効なジョイント
						const auto& pmx = dayo.models[icombo].res->pmx;
						const auto& pmxj = pmx->joints[joint->src];
						if (pmxj.kind != 0) { j++; continue; }	//6DoFスプリングではない

						std::string name = YRZ::u8(pmxj.name);

						if (ifilter > 0) {
							bool found = false;
							for (auto&& tok : filtertoks) {
								if (name.find(tok) != std::string::npos) {
									found = true;
									break;
								}
							}
							if (!found) {
								j++;
								continue;
							}
						}

						ImGui::TableNextRow();
						
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(name.c_str());

						ImGui::TableNextColumn();
						std::string bodyA = YRZ::u8(pmx->bodies[pmxj.bodyA].name);
						ImGui::TextUnformatted(bodyA.c_str());

						ImGui::TableNextColumn();
						std::string bodyB = YRZ::u8(pmx->bodies[pmxj.bodyB].name);
						ImGui::TextUnformatted(bodyB.c_str());

						ImGui::TableNextColumn();
						btGeneric6DofSpringConstraint* dof6 = dynamic_cast<btGeneric6DofSpringConstraint*>(joint->constraint.get());
						bool fofs = dof6->getUseFrameOffset();
						ImGui::Text("%s", fofs ? "true": "false");

						ImGui::TableNextColumn();
						float cfm = joint->constraint->getParam(BT_CONSTRAINT_STOP_CFM, 0);
						ImGui::Text("%f", cfm);

						dispIdx.insert(j);
						j++;
					}
					ImGui::EndTable();
				}

				//setボタンが押された
				if (setVal) {
					for (auto j : dispIdx) {
						auto& joint = dayo.models[icombo].solver->joint[j];
						for (int i = 0; i < 6; i++) {
							joint->constraint->setParam(BT_CONSTRAINT_STOP_CFM, cfm, i);
							btGeneric6DofSpringConstraint* dof6 = dynamic_cast<btGeneric6DofSpringConstraint*>(joint->constraint.get());
							dof6->setUseFrameOffset(useFO);
						}
					}
				}
			}
			ImGui::EndPopup();
		}

		//借り物リスト
		centerPopLambda("Contents tree helper", creditPops);
		if (ImGui::BeginPopupModal("Contents tree helper", nullptr)) {
			//URLファイルと作品番号一覧
			static std::vector<std::string> models;	//モデル名リスト
			static std::vector<std::string> files;	//.urlファイルのファイル名部分リスト
			static std::vector<std::string> works;	//ニコニコ作品IDリスト
			if (creditPopsOpening) {
				//URLファイルと作品番号一覧の再作成
				models.clear();
				files.clear();
				works.clear();

				//urlファイルから作品IDを得る。urlファイルが無効な場合は空文字を返す
				auto urlLambda = [&](const fs::path& p)->std::string {
					std::wstring ini = p.wstring();
					std::wstring ret;
					ret.resize(DAYO_PATH_MAXBYTES);
					if (GetPrivateProfileString(L"InternetShortcut", L"URL", L"", ret.data(), DAYO_PATH_MAXBYTES * sizeof(WORD), ini.c_str())) {
						//retの末尾の\0を取り除く
						while (!ret.empty() && ret.back() == L'\0')
							ret.pop_back();
						return (char*)fs::path(ret).stem().u8string().c_str();
					}
					return "";
				};

				//.dayoファイルと同じフォルダにある.urlファイルを検索
				if (!DayoPath.empty()) {
					fs::path dir = DayoPath.parent_path();
					if (fs::exists(dir) && fs::is_directory(dir)) {
						for (const auto& entry : fs::directory_iterator(dir)) {
							if (entry.is_regular_file() && entry.path().extension() == ".url") {
								fs::path p = entry.path();
								auto idstr = urlLambda(p);
								if (!idstr.empty()) {
									models.push_back("");
									files.push_back((char*)p.stem().u8string().c_str());
									works.push_back(idstr);
								}
							}
						}
					}
				}

				std::unordered_set<fs::path> known;	//モデルの重複チェック用
				for (auto&& mo : dayo.models) {
					if (known.contains(mo.res->path))	//既にリストに載っているモデルはスキップ
						continue;
					//URLファイルが同じパスにあるか検索
					fs::path dir = mo.res->path.parent_path();
					if (fs::exists(dir) && fs::is_directory(dir)) {
						fs::path p = {};
						for (const auto& entry : fs::directory_iterator(dir)) {
							if (entry.is_regular_file() && entry.path().extension() == ".url") {
								p = entry.path();
								break;
							}
						}
						if (!p.empty() && fs::exists(p)) {
							//見つかったらインターネットショートカットとしてURLから作品IDを取得
							auto idstr = urlLambda(p);
							if (!idstr.empty()) {
								models.push_back(mo.res->u8name);
								files.push_back((char*)p.stem().u8string().c_str());
								works.push_back(idstr);
								known.insert(mo.res->path);
							}
						}
					}
				}
				creditPopsOpening = false;
			}

			//コピーボタン
			if (ImGui::Button("copy")) {
				std::ostringstream ss;
				ss << "--- credits ---" << std::endl;
				for (int i = 0; i < models.size(); i++) {
					ss << models[i] << " " << files[i] << " " << works[i] << std::endl;
				}
				ss << std::endl << std::endl;
				ss << "--- niconico work ID ---" << std::endl;
				for (int i = 0; i < models.size(); i++) {
					if (!works[i].empty())
						ss << works[i] << std::endl;
				}
				std::string s = ss.str();
				YRZ::CopyTextToClipboard(YRZ::L(s));
			}
			ImGui::SameLine();
			//閉じるボタン
			if (ImGui::Button("close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
				ImGui::CloseCurrentPopup();
			}

			//借り物リスト
			if (ImGui::BeginTable("ContentsTreeHelperTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
				ImGui::TableSetupColumn("model", ImGuiTableColumnFlags_WidthStretch, 0.3f);
				ImGui::TableSetupColumn("desc", ImGuiTableColumnFlags_WidthStretch, 0.5f);
				ImGui::TableSetupColumn("workID", ImGuiTableColumnFlags_WidthStretch, 0.2f);
				ImGui::TableSetupScrollFreeze(1, 1);
				ImGui::TableHeadersRow();
				for (int i = 0; i < models.size(); i++) {
					ImGui::TableNextColumn();
					ImGui::Text(models[i].c_str());
					ImGui::TableNextColumn();
					ImGui::Text(files[i].c_str());
					ImGui::TableNextColumn();
					ImGui::Text(works[i].c_str());
				}
				ImGui::EndTable();
			}


			ImGui::EndPopup();
		}




		//カメラ・光源・セルフ影設定(カメラ・光源ON時でないと出てこない)
		if (iModel == IDX_CAMERA && !Animation && !Recording) {
			/*カメラ*/
			ImGui::Begin("Camera");
			{
				//位置・回転・距離
				resample |= ImGui::InputFloat3("target", reinterpret_cast<float*>(&camera.target));
				vec3 rotation = camera.rotation * 180 / PI;
				resample |= ImGui::InputFloat3("rotation", reinterpret_cast<float*>(&rotation));
				camera.rotation = rotation / 180 * PI;
				resample |= ImGui::InputFloat("distance", &camera.distance);

				//画角 1～179°の垂直画角
				float tmpF = camera.fov * PI / 180.0;
				resample |= ImGui::SliderAngle("FoV", &tmpF, 1, 179);
				camera.fov = tmpF * 180.0 / PI;

				if (ImGui::BeginTable("cameraTable", 2, ImGuiTableFlags_Resizable)) {
					ImGui::TableNextColumn();
					//ボーン追従
					ImGui::SeparatorText("bone-tracking");
					//追従対象モデル名リスト
					static DWORD updateT = 0;	//モデル名リストが最後に更新された時刻
					static std::string modelnames;
					static bool mochange = true;
					if (updateT != g_dayo->modelsUpdatedT) {
						modelnames = (char*)u8"(none)";
						modelnames.push_back(0);
						for (int i = IDX_CAMERA+1; i < dayo.models.size(); i++) {
							modelnames += dayo.models[i].u8name;
							modelnames.push_back(0);
						}
						updateT = g_dayo->modelsUpdatedT;
						mochange = true;
					}
					int imo = dayo.Find(camera.parentID);
					int imocombo = imo + 1;
					if (ImGui::Combo("model", &imocombo, modelnames.c_str())) {
						imo = imocombo - 1;
						camera.rotation = { 0,0,0 };
						resample = true;
						if (imo == IDX_CAMERA) {
							camera.target = { 0,10,0 };
							camera.distance = 45;
							camera.parentID = -1;
							camera.parentBone = -1;
						} else {
							camera.target = { 0,0,0 };
							camera.distance = 0;
							camera.parentID = dayo.models[imo].id;
							camera.parentBone = 0;
						}
						mochange = true;
					}
					//追従対象ボーン名リスト
					ImGui::BeginDisabled(camera.parentID < 0);
					static std::string bonenames;
					if (imocombo != 0 && mochange) {
						bonenames = "";
						auto& bs = dayo.models[imo].res->pmx->bones;
						for (int i = 0; i < bs.size(); i++) {
							bonenames += YRZ::u8(bs[i].name);
							bonenames.push_back(0);
						}
						mochange = false;
					}
					int ibone = max(0, camera.parentBone);
					if (ImGui::Combo("bone", &ibone, bonenames.c_str())) {
						resample = true;
						camera.parentBone = ibone;
					}
					ImGui::EndDisabled();

					ImGui::TableNextColumn();
					resample |= ImGui::Checkbox("perspective", &camera.perspective);
					camera.solver.camera.parth = camera.perspective;
					if (ImGui::Button("register") || KBRegister.Is(io)) {
						PMX::KeySubset sub;
						auto key = camera.keyframe(Frame);
						if (camera.solver.cameraKeys.contains(Frame))
							memcpy_s(key.bezierParams, 24, camera.solver.cameraKeys[Frame].bezierParams, 24);
						sub.cameras = { key };
						kc->RegisterSubset(sub, std::format(L"put {}F", Frame), true, true, 0);
					}
					TooltipDayo(KBRegister.desc);
					if (ImGui::Button("init")) {
						if (camera.parentID < 0) {
							camera.rotation = { 0,0,0 };
							camera.target = { 0,10,0 };
							camera.distance = 45;
						} else {
							camera.rotation = { 0,0,0 };
							camera.target = { 0,0,0 };
							camera.distance = 0;
						}
						resample = true;

					}
					ImGui::EndTable();
				}
			}
			ImGui::End();

			/* 光源 */
			ImGui::Begin("Light");
			{
				bool updated = false;
				auto& L = camera.solver.light;
				Vector3& LC = *reinterpret_cast<Vector3*>(&L.color);
				Vector3& LD = *reinterpret_cast<Vector3*>(&L.direction);
				if (ImGui::BeginTable("lightProp", 2)) {
					ImGui::TableNextColumn();
					ImGui::SeparatorText("color");
					updated |= ImGui::SliderFloat("R", &LC.x, 0, 1);
					updated |= ImGui::SliderFloat("G", &LC.y, 0, 1);
					updated |= ImGui::SliderFloat("B", &LC.z, 0, 1);
					ImGui::TableNextColumn();
					ImGui::SeparatorText("direction");
					updated |= ImGui::SliderFloat("X", &LD.x, -1, 1);
					updated |= ImGui::SliderFloat("Y", &LD.y, -1, 1);
					updated |= ImGui::SliderFloat("Z", &LD.z, -1, 1);
					ImGui::EndTable();
				}

				ImGui::BeginDisabled(iModel!=IDX_CAMERA);
				if (ImGui::Button("register")) {
					PMX::KeySubset sub;
					sub.lights = { {camera.solver.light} };
					sub.lights[0].iFrame = Frame;
					kc->RegisterSubset(sub, L"Light");
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("init")) {
					updated = true;
					auto defvar = PMX::VMDLight();
					LD = defvar.direction;
					LC = defvar.color;
				}

				resample |= updated;
			}
			ImGui::End();

			/* セルフ影 */
			ImGui::Begin("SelfShadow");
			{
				auto& SS = camera.solver.selfshadow;
				bool updated = false;
				if (ImGui::RadioButton("none", SS.kind == 0)) { updated = true; SS.kind = 0; }
				ImGui::SameLine();
				if (ImGui::RadioButton("mode1", SS.kind == 1)) { updated = true; SS.kind = 1; }
				ImGui::SameLine();
				if (ImGui::RadioButton("mode2", SS.kind == 2)) { updated = true; SS.kind = 2; }

				float d = min(max(0, 10000 * (1 - SS.distance)),9999);
				if (ImGui::SliderFloat("distance", &d, 0, 9999, "%.0f")) { SS.distance = 1 - d / 10000; updated = true; }

				ImGui::BeginDisabled(iModel != IDX_CAMERA);
				if (ImGui::Button("register")) {
					PMX::KeySubset sub;
					sub.selfShadows = { {camera.solver.selfshadow} };
					sub.selfShadows[0].iFrame = Frame;
					kc->RegisterSubset(sub, L"SelfShadow");
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("init")) {
					updated = true;
					PMX::VMDSelfShadow defSS = {};
					SS.kind = defSS.kind;
					SS.distance = defSS.distance;
				}

				resample |= updated;
			}
			ImGui::End();
		}

		//ギズモなどを出すかどうかの判定
		//操作対象が前フレームと変わってないかチェックするためのフラグ、kcはあくまで「前フレームにおけるモデルのkc」なので
		static KeyFrameWindowContext* prevkc = nullptr;
		bool validModelKC = (prevkc == kc) && (iModel > IDX_CAMERA);	//有効なモデル用コンテキストか？
		bool playing = (Animation || Recording);	//再生中か？

		static bool boneSelection = true;	//ボーン選択モード?
		int krBone = -1;	//唯一選択されているボーン(2つ以上選択されているor選択されていない場合は-1)
		std::vector<int> selectedBones;	//選択されているボーン一覧
		if (validModelKC) {
			kc->BonesSelectingStat(selectedBones);
			krBone = (selectedBones.size() == 1) ? selectedBones[0] : -1;
		}

		//レンダラー窓
		ImGui::Begin("Renderer");
		auto imgSize = ImGui::GetContentRegionAvail();
		if (Recording) {
			//録画中は出力画像のサイズに固定
		} else {
			//録画中でなければ出力画像のアスペクト比に応じてレンダラー窓に合わせたサイズにする
			imgW = imgSize.x;
			imgH = imgSize.y;
			float mag = (float)imgH/g_edi.outputHeight;	//出力画像を何倍したらウィンドウ丁度に納まるか？
			imgW = mag * g_edi.outputWidth;
			//縦の倍率に合わせたら横がはみだすようなら、横の倍率に合わせる
			if (imgW > imgSize.x) {
				imgW = imgSize.x;
				mag = (float)imgW/ g_edi.outputWidth;
				imgH = mag * g_edi.outputHeight;
			}
			imgW = max(32, imgW);
			imgH = max(32, imgH);	//最低でもこれだけのサイズは確保しとく

			auto imgTL = ImGui::GetWindowContentRegionMin() + (imgSize - ImVec2(imgW, imgH)) / 2;	//レンダリング結果画像のウィンドウ内での表示位置
			imgX = imgTL.x;
			imgY = imgTL.y;

			//実際にレンダリングされる大きさ
			auto renW = imgW / Interleave;
			auto renH = imgH / Interleave;

			if (dayo.Width() != renW || dayo.Height() != renH) {
				//レンダラー窓がリサイズされた
				//ハマった話…
				//なんかここを追加したらLiveObjectが残ってるとかブーブー言われるようになったんだけど、
				//Pass::Update()で、メンバ変数のComポインタを作る時にIID_PPV_ARGSの中にReleaseAndGetAddressOf()入れ忘れ奴があったのが原因だった
				dayo.cb->resolution = { (UINT)renW, (UINT)renH };
				dayo.Resize(renW, renH);
				renderImageID = YRZ::ImGuiTextureID(*g_dxr, dayo.dayoOut, g_imguiDH, renderImageDHIndex);	//↑でppOutが変更されるので
				DebIP->OnResize();
				resample = true;
			}

			//レンダラー窓でのマウス操作
			if (ImGui::IsWindowFocused()) {
				float m = 1.0;	//Ctrl,Shiftによる倍率
				m *= io.KeyCtrl ? 0.1f : 1.0f;
				m *= io.KeyShift ? 10.0f : 1.0f;
				ImVec2 delta = io.MouseDelta;
				if (ImGui::IsMouseDragging(1)) {
					if (camera.parentID < 0) {
						if (io.KeyCtrl) {
							//Ctrl+右上下ドラッグで少しズーム
							camera.distance += delta.y * m;
							camera.distance = max(camera.distance, 1.0f);
						} else {
							//右ドラッグで回転
							camera.rotation.x -= delta.y * 0.01 * m;
							camera.rotation.y -= delta.x * 0.01 * m;
						}
					} else {
						if (io.KeyCtrl) {
							//Ctrl+右上下ドラッグで少しズーム
							camera.target.z += delta.y * m;
						} else {
							//右ドラッグで回転
							camera.rotation.x -= delta.y * 0.01 * m;
							camera.rotation.y -= delta.x * 0.01 * m;
						}
					}
					resample = true;
				}
				//中ドラッグでパン
				if (ImGui::IsMouseDragging(2)) {
					if (camera.parentID < 0) {
						camera.target -= delta.x * 0.1 * m * _11_12_13(dayo.cb->viewMatrix);
						camera.target += delta.y * 0.1 * m * _21_22_23(dayo.cb->viewMatrix);
					} else {
						camera.target += {delta.x * -0.1f * m, delta.y * 0.1f * m, 0};
					}
					resample = true;
				}
				//ホイールでズーム
				if (io.MouseWheel != 0) {
					if (camera.parentID < 0) {
						camera.distance -= io.MouseWheel * 6 * m;
						camera.distance = max(camera.distance, 1.0f);
					} else {
						camera.target.z += io.MouseWheel * 6 * m;
					}
					resample = true;
				}
			}

			//レンダラー窓内でのマウスの位置をcbに入れる
			dayo.cb->mousePos = vv((io.MousePos - (ImGui::GetWindowPos() + imgTL)) / ImVec2(imgW, imgH));

			//テンキーで各面から(これはレンダラ窓がアクティブでなくても反応させる)
			auto camLambda = [&](const Vector3& v) { camera.distance = 45; resample = true; camera.rotation = v; };
			if (KBCamBack.Is(io)) camLambda({ 0,PI,0 });
			if (KBCamBottom.Is(io)) camLambda({ -PI / 2,0,0 });
			if (KBCamFront.Is(io)) camLambda({ 0,0,0 });
			if (KBCamLeft.Is(io)) camLambda({ 0, PI / 2, 0 });
			if (KBCamRight.Is(io)) camLambda({ 0, -PI / 2, 0 });
			if (KBCamTop.Is(io)) camLambda({ PI / 2,0,0 });
			if (KBCamKey.Is(io)) { camera.Solve(Frame); resample = true; };
			static int camCycle = 0;
			static DWORD camCycleT = timeGetTime();
			if (KBCamTarget.Is(io) && !selectedBones.empty()) {
				auto curT = timeGetTime();
				//前にサイクルして2.5秒以内だったら「次の選択中ボーン」を探す
				if (curT < camCycleT + 2500) {
					camCycle++;
				} else {
					camCycle = 0;
				}
				camCycle = camCycle >= selectedBones.size() ? 0 : camCycle;
				int ibone = selectedBones[camCycle];
				auto psolver = dynamic_cast<PMX::PoseSolver*>(kc->solver);
				Vector3 o = Vector3::Transform(kc->pmx->bones[ibone].position, psolver->bones[ibone]->transform);
				camera.distance = 45; camera.target = o;  selectedBones; resample = true;
				camCycleT = curT;
			}
		}

		//レンダリング結果の表示
		ImGui::SetCursorPos( ImVec2(imgX, imgY));
		ImGui::Image(renderImageID, ImVec2(imgW, imgH));

			
		//ボーンが1つしか選択されていない場合はギズモを出す
		vec3 boneP;	//現在の選択ボーンにセットされている位置
		vecQ boneQ;	//現在の選択ボーンにセットされている姿勢
		bool gizmoed = false;

		bool boneEditable = (selectedBones.size() == 1) && validModelKC && !playing && !NowLoading;
		PMX::PMXBone* pmxbone = nullptr;
		if (boneEditable)
			pmxbone = &kc->pmx->bones[krBone];

		if (boneEditable && (pmxbone->IsRotation() || pmxbone->IsTranslation()) && pmxbone->IsControllable() && pmxbone->IsVisible()) {
			Matrix view = dayo.cb->ViewMatrix();
			Matrix proj = dayo.cb->ProjectionMatrix();
			auto solver = dynamic_cast<PMX::PoseSolver*>(kc->solver);
			Matrix worldM = solver->bones[krBone]->transform;
			
			//ローカル変換をポーズから得る
			vec3 tr = kc->pose.boneKeys[krBone].position;
			vecQ rot = kc->pose.boneKeys[krBone].rotation;
			vec3 o = solver->pmx->bones[krBone].position;
			Matrix localM = oRtMatrix(o, rot, tr);	//[自→親]
			Matrix worldToParent = worldM.Invert() * localM;	//[ワールド→親]
			Matrix r = localFrameLambda(solver->pmx->bones[krBone]);	//ギズモ回転行列(ローカル軸フレーム)

			boneP = tr;
			boneQ = rot;
			Matrix gizmoM = r * Matrix::CreateTranslation(o) * worldM;			//ギズモ行列(ボーン行列にローカル軸とボーン原点を加味した物) = [r][o][自→ワ] = [ギ→ワ]
			bool g = Gizmo(kc, krBone, view, proj, gizmoM, imgX, imgY, imgW, imgH);	//操作によってギズモ行列[ギ→ワ]を変化させ、[ギ→ワ]'行列を得る(実際に変化しているのはボーンの回転Rと移動t)
			if (g) {
				//[ギ→ワ]' = [r][o][自→ワ]'	//[自→親]' = [-o][R'][t'][o]より、↓になる
				// [-r][ギ→ワ]'[ワ→親][-o] = [R'][t']
				Matrix RTMat = r.Transpose() * gizmoM * worldToParent * Matrix::CreateTranslation(-o);
				boneQ = vecQ::CreateFromRotationMatrix(RTMat);
				boneP = _41_42_43(RTMat);
				gizmoed = true;
			} else {
				gizmoed = false;
			}
		}

		//ｼｮﾎﾞｰﾝ 選択ボーンが1つ以外の場合ボーン一覧をマーカーとして表示、クリックで選択可能にする
		static bool morphEditing = false;	//モーフ編集中は消しとく
		bool showBone = (krBone == -1) && !playing && validModelKC  && !morphEditing && boneSelection && !NowLoading;
		if (showBone) {
			std::vector<int>floating;		//マウスカーソルの下にあるボーン番号のリスト
			static std::vector<int>floated;	//floatingは毎フレーム更新されるのでメニューが作られた時のボーンリストはstaticで保持しとく
			Matrix vp = dayo.cb->ViewMatrix() * dayo.cb->ProjectionMatrix();
			DrawMarkers(kc, vp, imgX,imgY,imgW, imgH, floating);
			if (!floating.empty() && io.MouseClicked[0] && !ImGui::IsPopupOpen("showbone")) {
				if (floating.size() == 1) {
					kc->SelectAll(false);
					kc->SelectBoneNode(floating[0]);
					kc->UpdateKeyDisp(KeyFrameStatus::none);
				} else {
					floated = floating;
					ImGui::OpenPopup("showbone");
				}
			}
			if (ImGui::BeginPopup("showbone")) {
				for (auto&& idx : floated) {
					const auto& pmxb = kc->pmx->bones[idx];
					auto bn = (Language == 0 || pmxb.name_e.empty()) ? pmxb.name : pmxb.name_e;
					if (ImGui::Selectable(YRZ::u8(bn).c_str())) {
						kc->SelectAll(false);
						kc->SelectBoneNode(idx);
						kc->UpdateKeyDisp(KeyFrameStatus::none);
					}
				}
				ImGui::EndPopup();
			}
			/*
			//Ctrl+Aで全ボーン選択 ... KeyFrameの方で実装済み
			if (ImGui::IsKeyPressed(ImGuiKey_A) && io.KeyCtrl) {
				kc->SelectBoneNode();
				kc->UpdateKeyDisp(KeyFrameStatus::none);
			}
			*/
		}
		//剛体表示
		if (DisplayRB && !Recording &&!NowLoading) {
			Matrix vp = dayo.cb->ViewMatrix() * dayo.cb->ProjectionMatrix();
			DrawRigidBodies(kc, vp, imgX, imgY, imgW, imgH);
		}

		
		//ESCキーが押されたらボーン選択解除
		if (ImGui::IsKeyPressed(ImGuiKey_Escape) && ImGui::IsWindowFocused()) {
			kc->SelectAll(false);
			boneEditable = false;
		}

		//編集中のモデル名、ボーン名の表示
		if (!playing && !NowLoading) {
			std::string label;
			if (iModel == IDX_CAMERA) {
				label = "Camera/Light";
			} else {
				label = std::format("Model#{} : {}", iModel, g_dayo->models[iModel].u8name);
				if (krBone >= 0) {
					const auto& pmxb = g_dayo->models[iModel].res->pmx->bones[krBone];
					auto bn = (Language == 0 || pmxb.name_e.empty()) ? pmxb.name : pmxb.name_e;
					label += std::format(" Bone#{} : {}", krBone, YRZ::u8(bn));
				}
			}
			auto dl = ImGui::GetWindowDrawList();
			auto lt = ImGui::GetWindowContentRegionMin() + ImGui::GetWindowPos();
			dl->AddText(lt + ImVec2(9, 9), ImColor(0.0f,0.0f,0.0f,1.0f), label.c_str());
			dl->AddText(lt + ImVec2(8, 8), ImColor(1.0f,1.0f,1.0f,1.0f), label.c_str());
		}

		//情報表示
		if (Infomative && !NowLoading) {
			auto dl = ImGui::GetWindowDrawList();
			auto lt = ImGui::GetWindowContentRegionMin() + ImGui::GetWindowPos();
			auto rb = ImGui::GetWindowContentRegionMax() + ImGui::GetWindowPos();
			ImVec2 textP = ImVec2(lt.x + 8, rb.y - 160);

			//影付き文字を描く
			auto textLambda = [&](const ImVec2& p, const std::string& str) {
				dl->AddText(p + ImVec2(1, 1), ImColor(0, 0, 0, 255), str.c_str());
				dl->AddText(p, ImColor(255, 255, 255, 255), str.c_str());
				};

			//fps表示
			uint64_t now = watch.Now();
			static uint32_t prevCount = 0;
			static uint64_t prevDispT = 0;
			std::string fpsStr;
			static float fps = 0;
			if (now - prevDispT > watch.freq) {
				fps = (float)( (nFramesDrawn-prevCount) / (watch.Sec(now - prevDispT)) );
				prevDispT = now;
				prevCount = nFramesDrawn;
			}
			if (fps < 1.0f)
				fpsStr = std::format("<1 fps, {:.2f}ms", dtime*1e+3);
			else
				fpsStr = std::format("{:.1f} fps {:.2f}ms",fps,dtime*1e+3);
			
			textLambda(textP, fpsStr);
			
			//フレーム
			textP.y -= ImGui::GetTextLineHeightWithSpacing();
			std::string frameStr = std::to_string((int)AnimFrame) + "F";
			textLambda(textP, frameStr);

			//静止画モードの場合はサンプル数の表示
			if (Accumulate && !Animation) {
				textP.y -= ImGui::GetTextLineHeightWithSpacing();
				std::string frameStr = std::to_string(dayo.cb->iSample) + "samples";
				textLambda(textP, frameStr);
			}

			//波形の表示
			if (!g_wave.PCM.empty()) {
				float w = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
				float v = ImGui::GetWindowContentRegionMax().y + ImGui::GetWindowPos().y - 64;	//波形表示位置の中央
				dl->AddRectFilled(ImVec2(lt.x, v - 64), ImVec2(rb.x, v + 64), ImColor(0, 0, 0, 64));	//背景
				auto sampleLambda = [&](float sec) {
					int s = min(max(0, roundf(sec * g_wave.Freq()) * 2), g_wave.PCM.size() - 3);
					return ImVec2(abs(g_wave.PCM[s]), abs(g_wave.PCM[s + 1])) * 64 / 32768.0f;
					};
				const int a = 192;	//α
				int range = cfg.waveInfoFrames;			//現在のフレーム±何フレーム分表示するか
				float F = floorf(AnimFrame);	//現在のフレーム(小数点切捨て)
				for (int x = 0; x < w - 1; x++) {
					float r = (range + 0.5f) / 30.0f;	//±表示範囲[sec]
					float dps = 2 * r / w;	//dot per sec
					float t = F / 30.0f + x * dps - r + g_edi.wavOffset;	//サンプル取る位置[秒]
					auto s = sampleLambda(t);
					int b = (range * w / (2 * range + 1) <= x && x <= (range + 1) * w / (2 * range + 1)) ? a : a * 0.7;
					dl->AddLine(ImVec2(lt.x + x, v), ImVec2(lt.x + x, v - s.x), ImColor(255, 255, 255, b), 1);
					dl->AddLine(ImVec2(lt.x + x, v), ImVec2(lt.x + x, v + s.y), ImColor(255, 0, 0, b), 1);
				}
				//フレームと時刻を入れる
				for (int i = -range; i < range + 1; i++) {
					float p = float(i + range) / float(2 * range + 1) * w + lt.x;
					dl->AddLine(ImVec2(p, v - 32), ImVec2(p, v + 32), ImColor(255, 255, 0, a), 1);	//フレーム仕切り
					int b = i ? a / 2 : a;
					int fi = (int)F + i;
					if (fi < 0)
						continue;
					std::string s = std::to_string(fi) + "F";
					dl->AddText(ImVec2(p, v - 64), ImColor(255, 255, 255, b), s.c_str());
					int m = fi / 1800;
					int sec = (fi - m * 1800) / 30;
					int ms = fi % 30 * 1000 / 30;
					s = std::format("{}:{}.{}", m, sec, ms);
					dl->AddText(ImVec2(p, v - 52), ImColor(255, 255, 255, b), s.c_str());
				}
			}

			if (CameraRecording) {
				textP.y -= ImGui::GetTextLineHeightWithSpacing()*2;
				textLambda(textP, g_hon.C("Camera recording ... Press [space] to capture, [P] to quit"));
			}
		}

		//右下に操作パネル
		if (!playing && !NowLoading) {
			const ImVec2 buttonSize = { 32,32 };
			const ImVec2 panelSize = { 112, 112 };

			auto p = ImGui::GetWindowPos() + ImGui::GetWindowContentRegionMax() - panelSize;
			//パネルの左上隅からのボタンの配置位置
			ImVec2 place[6];
			for (int y=0; y<2; y++)
				for (int x = 0; x < 3; x++)
					place[y * 3 + x] = ImVec2(buttonSize.x * x, 32 + buttonSize.y * y);

			ImGui::SetCursorScreenPos(p);
			ImGui::SetNextItemAllowOverlap();
			const ImVec2 localUV = ImVec2(96, 0) / 256.0f;
			const ImVec2 worldUV = ImVec2(96, 32) / 256.0f;
			const ImVec2 lwsize = ImVec2(96, 32);
			ImVec2 lwuv = g_gizmoState.mode == ImGuizmo::WORLD ? worldUV : localUV;
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.3));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
			if (ImGui::ImageButton("imageButtonLW", dayoIconID, lwsize, lwuv, lwuv+lwsize/256.0f, ImVec4(0,0,0,0), ImVec4(1,1,1,1))) {
				g_gizmoState.mode = (g_gizmoState.mode == ImGuizmo::WORLD) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
			}
			TooltipDayo(KBLocalWorld.desc);
			ImGui::PopStyleColor(3);

			if (KBLocalWorld.Is(io)) {
				if (g_gizmoState.mode == ImGuizmo::LOCAL)
					g_gizmoState.mode = ImGuizmo::WORLD;
				else
					g_gizmoState.mode = ImGuizmo::LOCAL;
			}


			float mult = 1;
			mult *= io.KeyCtrl ? 0.1f : 1.0f;
			mult *= io.KeyShift ? 10.0f : 1.0f;

			auto buttonLambda = [&](const char* caption, const ImVec2& ofs, bool active, int iconX, int iconY) {
				ImGui::SetCursorScreenPos(p + ofs);
				ImGui::SetNextItemAllowOverlap();
				ImGui::BeginDisabled(!active);
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.3));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
				ImGui::ImageButton(caption, dayoIconID, buttonSize,
					ImVec2(iconX * 32, iconY * 32)/256.0f, ImVec2((iconX+1) * 32, (iconY+1) * 32) / 256.0f,
					{ 0, 0, 0, 0 }, active ? ImVec4( 1, 1, 1, 1 ) : ImVec4(0.5, 0.5, 0.5, 0.5));
				ImGui::PopStyleColor(3);
				ImGui::EndDisabled();
				if (ImGui::IsItemActive()) {
					return  io.MouseDelta.y;
				} else {
					return 0.0f;
				}
			};

			if (boneEditable) {
				//ボーン操作
				auto boneButtonLambda = [&](const char* caption, const ImVec2& ofs, const vec3& axis, auto& target, bool translate, int iconX, int iconY) {
					auto& pmxb = kc->pmx->bones[krBone];
					bool active = (pmxb.IsTranslation() && translate) || (pmxb.IsRotation() && !translate);
					float delta = buttonLambda(caption, ofs, active, iconX, iconY);
					if (delta) {
						auto b = dynamic_cast<PMX::PoseSolver*>(kc->solver)->bones[krBone];
						Matrix s2w = b->transform;	//自→ワ
						Matrix l2s = localFrameLambda(kc->pmx->bones[krBone]);	//ローカル軸の座標系→自分(ボーン)座標系
						if (translate) {
							//移動モード
							g_gizmoState.op = ImGuizmo::TRANSLATE;
							float d = -delta * 0.05 * mult;
							Matrix s2p = b->toParent;		//自→親
							Matrix w2p = s2w.Invert() * s2p;	//ワ→親
							if (g_gizmoState.mode == ImGuizmo::WORLD) {
								vec3 dir = vec3::TransformNormal(axis, w2p);
								target.position = target.position + dir * d;
							} else {
								vec3 dir = vec3::TransformNormal(axis, l2s * s2p);
								target.position = target.position + dir * d;
							}
						} else {
							//回転モード
							g_gizmoState.op = ImGuizmo::TRANSLATE;	//矢印の方が向きが分かりやすいので
							float d = -delta * 0.01f * mult;
							vecQ q = vecQ(target.rotation);	//自→親
							if (pmxb.IsFixAxis()) {
								//固定軸による捩じり…x,y,zアイコンのどれで回しても固定軸で回すことになる
								vecQ fq = vecQ::CreateFromAxisAngle(pmxb.fixAxis, d);
								target.rotation = fq * q;
							} else {
								vecQ dq = vecQ::CreateFromAxisAngle(axis, d);
								if (g_gizmoState.mode == ImGuizmo::WORLD) {
									vecQ wq = vecQ::CreateFromRotationMatrix(s2w);	//自→ワ
									target.rotation = wq * dq * Conjugate(wq) * q;	//dqを"ワ"で解釈して乗算、ワ→自に戻す
								} else {
									vecQ lq = vecQ::CreateFromRotationMatrix(l2s);	//ロ→自
									target.rotation = Conjugate(lq) * dq * lq * q;
									//最初の座標系は"自"なので自→ロ(Conj(lq))を掛けて"ロ"でdqを解釈して乗算、その後lqで"自"に戻す
								}
							}
						}
						g_dayo->PoseUpdate(true, iModel);
						resample = true;
					}
				};

				auto& key = kc->pose.boneKeys[krBone];
				boneButtonLambda("RX", place[0], vec3(1, 0, 0), key, false, 0,0);
				boneButtonLambda("RY", place[1], vec3(0, 1, 0), key, false, 1,0);
				boneButtonLambda("RZ", place[2], vec3(0, 0, 1), key, false, 2,0);

				boneButtonLambda("X",  place[3], vec3(1, 0, 0), key, true, 0,1);
				boneButtonLambda("Y",  place[4], vec3(0, 1, 0), key, true, 1,1);
				boneButtonLambda("Z-", place[5], vec3(0, 0, -1), key, true, 2,1);
			
			} else if (iModel == IDX_CAMERA) {
				//カメラ操作
				auto cameraButtonLambda = [&](const char* caption, const ImVec2& ofs, const vec3& axis, bool translate, int iconX, int iconY) {
					float delta = buttonLambda(caption, ofs, true, iconX, iconY);
					if (delta) {
						if (translate) {
							//移動
							float d = delta * 0.05f * mult;
							if (g_gizmoState.mode == ImGuizmo::WORLD || camera.parentID >= 0) {
								camera.target += d * axis;
							} else {
								camera.target += d * vec3::TransformNormal(axis, dayo.cb->ViewMatrix().Invert());
							}
						} else {
							//回転
							float d = delta * 0.02f * mult;
							camera.rotation += d * axis;
						}
						resample = true;
					}
				};

				cameraButtonLambda("RX", place[0], vec3(1, 0, 0), false, 0,2);
				cameraButtonLambda("RY", place[1], vec3(0, 1, 0), false, 1,2);
				cameraButtonLambda("RZ", place[2], vec3(0, 0, 1), false, 2,2);

				cameraButtonLambda("X",  place[3], vec3(1, 0, 0), true, 0,3);
				cameraButtonLambda("Y",  place[4], vec3(0, 1, 0), true, 1,3);
				cameraButtonLambda("Z",  place[5], vec3(0, 0, 1), true, 2,3);
			}

			if (io.MouseDoubleClicked[1]) {
				//右ダブルクリックで操作パネル⇔ボーンウィンドウ
				bool onPanel = PointInRect(io.MousePos, p, p + panelSize);
				io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
				POINT point;
				RECT rect;
				GetClientRect(g_app->hWnd(), &rect);
				point.x = rect.left;
				point.y = rect.top;
				ClientToScreen(g_app->hWnd(), &point);

				if (onPanel) {
					SetCursorPos(point.x + kr.position.x, point.y + kr.position.y);
				} else {
					SetCursorPos(point.x + p.x + buttonSize.x / 2, point.y + p.y + 32 + buttonSize.y / 2);
				}
			}
		}

		ImGui::End();	//Renderer窓ここまで

		//ボーン操作ウィンドウ
		//ボーン操作窓の数値入力 or ギズモによる操作があったらポーズのデータをいじる
		BoneWindowResult br = {};
		if (validModelKC && !playing && !NowLoading)
			br = BoneWindow(boneEditable, validModelKC, boneP, boneQ, kc, selectedBones, gizmoed, boneSelection, iModel);

		//モーフ操作ウィンドウ
		if (validModelKC && !playing && !NowLoading) {
			ImGui::Begin("Expression");
			morphEditing = ImGui::IsWindowFocused();			//左下				左上			右上		右下
			static std::string caption[5] = { (char*)u8"reserved", (char*)u8"brow", (char*)u8"eye", (char*)u8"mouth", (char*)u8"etc" };
			static int selectedMorph[5] = { 0,0,0,0,0 };
			const int expOrder[5] = {2, 3, 1, 4, 0};	//モーフ枠の表示順をMMD準拠で並べ替えるためのテーブル
			for (int i = 0; i < 5; i++) {
				//モデルの変更に伴って選択モーフ番号が存在するモーフ数を越えていないかチェック
				if (selectedMorph[i] >= dayo.models[iModel].res->morphIndices[i].size())
					selectedMorph[i] = 0;
			}
			if (ImGui::BeginTable("morphtable", 2)) {
				for (int o = 0; o < 4; o++) {
					int i = expOrder[o];
					auto& mo = dayo.models[iModel];
					auto& res = mo.res;
					ImGui::TableNextColumn();
					//表情選択コンボボックス
					if (ImGui::Combo(caption[i].c_str(), &selectedMorph[i], res->u8morphPanelCombo[i].c_str())) {
						kc->SelectAll(false);
						kc->SelectMorphNode(res->morphIndices[i][selectedMorph[i]]);
						kc->UpdateKeyDisp(KeyFrameStatus::none);
					}
					 
					//表情スライダーと登録ボタン
					float defVal = 0.0f;
					if (!res->u8morphPanel[i].empty()) {
						int im = res->morphIndices[i][selectedMorph[i]];
						ImGui::PushID(i);
						YRZ::FXController ms = {};
						auto currentMorph = YRZ::u8(res->pmx->morphs[res->morphIndices[i][selectedMorph[i]]].name); //res->u8morphPanel[i][selectedMorph[i]];
						
						//コントローラになっていてモーフスライダーの設定がある場合はそれをセット
						if (dayo.condicts[iModel].contains(currentMorph))
							ms = dayo.condicts[iModel][currentMorph];

						//説明文
						if (!ms.desc.empty()) {
							std::string msg = (ms.desc.size() > Language + 1) ? ms.desc[Language + 1] : ms.desc[0];
							if (!msg.empty())
								ImGui::SetItemTooltip(msg.c_str());
						}

						//スライダーで値の変更
						auto sliderFlags = ms.slider.logValue ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None;
						bool changed = false;
						if (ms.slider.intValue) {
							int v = kc->pose.morphKeys[im].value;
							changed = ImGui::SliderInt("", &v, ms.slider.minValue, ms.slider.maxValue, nullptr, sliderFlags);
							if (changed)
								kc->pose.morphKeys[im].value = v;
						} else {
							changed = ImGui::SliderFloat("", &kc->pose.morphKeys[im].value, ms.slider.minValue, ms.slider.maxValue, nullptr, sliderFlags);
						}
						if (changed) {
							kc->pose.morphKeys[im].selected = true;
							dayo.PoseUpdate(true, iModel);
							resample = true;
						}
						//右クリックでデフォルト値をセット
						if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
							kc->pose.morphKeys[im].value = ms.slider.defValue;
							dayo.PoseUpdate(true, iModel);
							resample = true;
						}
						//デフォルト値の表示
						if (ms.slider.defValue) {
							ImGui::SetItemTooltip("default : %f", ms.slider.defValue);
						}

						ImGui::PopID();
						ImGui::SameLine();
						ImGui::PushID(i + 5);
						if (ImGui::Button("register")) {
							PMX::KeySubset sub;
							PMX::VMDMorph morph;
							morph.iFrame = Frame;
							morph.name = kc->pmx->morphs[im].name;
							morph.value = kc->pose.morphKeys[im].value;
							sub.morphs.emplace_back(morph);
							kc->RegisterSubset(sub, std::format(L"put {}", morph.name));
						}
						ImGui::PopID();
					}
				}
				ImGui::EndTable();
			}
			ImGui::End();
		}


		prevkc = kc;	//ボーン編集モードに入れるかどうかのフラグ更新用

		/* モデル選択 */
		ImGui::Begin("Models");
		
		//モデル選択コンボボックス
		const std::string cameraLabel = "Camera/Light";
		std::string modelComboLabel = (iModel == IDX_CAMERA) ? cameraLabel.c_str() : dayo.models[iModel].u8name.c_str();
		if (ImGui::BeginCombo("model", modelComboLabel.c_str())) {
			for (int i = IDX_CAMERA; i < (int)dayo.models.size(); i++) {
				auto& label = (i == IDX_CAMERA) ? cameraLabel : dayo.models[i].u8name;
				bool selected = (i == iModel);
				ImGui::PushID(i);
				bool colored = false;
				if (i != IDX_CAMERA) {
					if (dayo.models[i].withPP) {
						ImGui::PushStyleColor(ImGuiCol_Text, COLPostprocess);
						colored = true;
					} else if (dayo.models[i].withDeform) {
						ImGui::PushStyleColor(ImGuiCol_Text, COLDeform);
						colored = true;
					}
				}
				if (ImGui::Selectable(label.c_str(), selected))
					switchModelLambda(i);
				if (selected)
					ImGui::SetItemDefaultFocus();
				if (colored)
					ImGui::PopStyleColor();
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}

		static bool freeCamera = false;	//再生中もカメラを自由に動かせるようにする
		static bool startCameraRecording = false;	//カメラの「record」ボタンが今押された
		static int cameraRecordStartF = 0;
		static int cameraRecordEndF = 0;
		static PMX::KeySubset camRecSet;
		//カメラの場合のボタンず
		if (iModel == IDX_CAMERA) {
			
			ImGui::BeginDisabled(CameraRecording);
			
			ImGui::Checkbox(g_hon.C("free camera while playing"), &freeCamera);
			TooltipDayo("Allows you to control the camera with the mouse during playback. Automatically turns on in camera recording mode");
			if (ImGui::Button("record...")) {
				auto animS = g_edi.startFromFrame ? Frame : max(0,g_edi.animationStart);
				auto animE = g_edi.animationEnd < 0 ? lastFrameLambda() : g_edi.animationEnd;
				if (animS >= animE) {
					OKDlg(L"camera recording", g_hon.L(L"The recording range is 0 frames. Please specify the recording range in the \"Animation\" window before recording."));
				} else {
					if (OKCancelDlg(L"camera recording", g_hon.L(L"Start the camera recording. Camera keys in the recorded range will be deleted and overwritten with the recorded content."))) {
						freeCamera = true;
						startCameraRecording = true;
						CameraRecording = true;
						cameraRecordStartF = animS;
						cameraRecordEndF = animE;
						camRecSet = {};
					}
				}
			}

			ImGui::EndDisabled();
		}

		if (iModel >= 0) {
			//モデル情報
			HelpMarker(dayo.models[iModel].res->u8description.c_str());
		}

		//順序ボタン
		if (dayo.models.size()) {
			static bool rasterChanged = false;	//描画順設定の場合はパスを作り直さないといけないので、そのフラグ
			if (ImGui::Button("order...")) {
				ImGui::OpenPopup("Order setting");
				rasterChanged = false;
			}
			ImVec2 center = ImGui::GetMainViewport()->GetCenter();
			ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			if (ImGui::BeginPopupModal("Order setting", NULL)) {

				if (ImGui::BeginTable("orderTable", 4, ImGuiTableFlags_Borders)) {
					ImGui::TableSetupColumn("motion", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("postprocess", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("deform", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("rasterize", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableHeadersRow();

					//並べ替え可能な順序リストを1つ作るラムダ
					auto orderTLambda = [&](const auto& dnd, std::vector<int>& v, auto color) {
						ImGui::TableNextColumn();
						if (ImGui::BeginTable("motionTable", 1, ImGuiTableFlags_ScrollY)) {
							for (int i = 0; i < dayo.models.size(); i++) {
								ImGui::TableNextColumn();

								int idx = v[i];
								auto col = color(idx);
								ImGui::PushStyleColor(ImGuiCol_Text, col);
								ImGui::PushID(i);
								auto name = "[" + std::to_string(idx) + "]" + dayo.models[idx].u8name;
								ImGui::Selectable(name.c_str(), false);
								ImGui::TableNextColumn();
								if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
									ImGui::SetDragDropPayload(dnd, &i, sizeof(int));
									ImGui::Text(name.c_str());
									ImGui::EndDragDropSource();
								}
								if (ImGui::BeginDragDropTarget()) {
									if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dnd)) {
										int srcIndex = *(const int*)payload->Data;
										if (srcIndex != i) {
											int x = v[srcIndex];
											v.erase(v.begin() + srcIndex);
											v.insert(v.begin() + i, x);
										}
									}
									ImGui::EndDragDropTarget();
								}
								ImGui::PopID();
								ImGui::PopStyleColor();
							}
							ImGui::EndTable();
						}
						};

					orderTLambda("motionDnD", g_edi.motionOrder, [&](int idx) {return COLText; });
					orderTLambda("ppDnD", g_edi.postprocessOrder, [&](int idx) {return dayo.models[idx].withPP ? COLPostprocess : COLTextDisabled; });
					orderTLambda("deformDnD", g_edi.deformOrder, [&](int idx) {return dayo.models[idx].withDeform ? COLDeform : COLText; });
					orderTLambda("rasterDnD", g_edi.rasterOrder, [&](int idx) {return COLText; });

					ImGui::EndTable();
				}

				ImGui::Text("press ESC to close");
				if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
					ImGui::CloseCurrentPopup();
				}

				ImGui::EndPopup();
			}
		}

		//モデル削除ボタン
		if (iModel >= 0) {
			ImGui::SameLine();
			if (ImGui::Button("delete", ImVec2(0, 0))) {
				if (NoYesDlg(L"Confirmation", g_hon.L(L"All keys assigned to the model are also deleted and cannot be undone. Are you sure?"))) {
					//カメラがボーン追従している場合の処理
					auto delID = dayo.models[iModel].id;
					camera.solver.PurgeExternalKeys(delID);
					if (camera.parentID == delID) {
						camera.parentID = -1;
						camera.parentBone = -1;
					}
					//モデル除去、外部親の処理はdayo側でやる
					dayo.DeleteModel(iModel);
					dayo.Solve(AnimFrame);
					g_physics.Reset();
					switchModelLambda(IDX_CAMERA);
					resample = true;
				}
			}
		}

		//IK・表示…↑でIDX_CAMERAに書き換わっている可能性があるので要チェック
		if (iModel >= 0) {
			ImGui::Separator();
			static int iik = 0;
			auto& res = dayo.models[iModel].res;
			if (!res->u8IKCombo.empty()) {
				auto& iklist = kc->pose.extraKey.IK;
				if (iik >= iklist.size())	//モデルの変更などでIKボーンの数が減る事もあるので
					iik = 0;
				ImGui::Combo("IK", &iik, res->u8IKCombo.c_str());
				bool ikon = iklist[iik].enabled;
				bool ikchange = false;
				if (ImGui::RadioButton("on", ikon)) {	ikchange = true;	ikon = true;	}
				ImGui::SameLine();
				if (ImGui::RadioButton("off", !ikon)) {	ikchange = true;	ikon = false;	}
				if (ikchange) {
					kc->pose.extraKey.IK[iik].enabled = ikon;
					dayo.PoseUpdate(true, iModel);
					resample = true;
				}
			}
			if (ImGui::Checkbox("visible", &kc->pose.extraKey.show)) {
				dayo.PoseUpdate(true, iModel);
				resample = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("register")) {
				PMX::KeySubset sub;
				sub.extras = { kc->pose.extraKey };
				sub.extras[0].iFrame = Frame;
				kc->RegisterSubset(sub, sub.extras[0].show ? L"show" : L"hide", true, true, 0);
			}
		}

		if (iModel >= 0) {
			resample |= ExpWindow(iModel, *kc);
		}

		ImGui::End();

		//Tabキーでモデル選択
		if (KBSwitchModelRev.Is(io)) {
			auto idx = iModel - 1;
			if (idx < 0)
				idx = IDX_CAMERA;
			switchModelLambda(idx);
		} else if (KBSwitchModel.Is(io)) {
			auto idx = iModel + 1;
			if (idx >= g_dayo->models.size())
				idx = IDX_CAMERA;
			switchModelLambda(idx);
		}

		DebugWindow();


		//材質注釈
		resample |= MatDescWindow();

		//アニメーション
		ImGui::Begin("Animation");
		int tmp = AnimFrame;
		int lastFrame = max(g_edi.animationEnd, lastFrameLambda());
		if (ImGui::SliderInt("frame", &tmp, 0, lastFrame)) {
			AnimFrame = tmp;
			dayo.Solve(AnimFrame);
			g_physics.Reset();
			camera.Solve(AnimFrame);
			camera.ToCB(dayo.cb);
			resample = true;
		}
		ImGui::SameLine();
		static int frameBeforeAnim = 0;		//再生開始時に編集対象だったフレーム
		static uint64_t animationStartT = 0;	//再生開始時の時刻[tick]
		if (!Recording) {
			if (Animation) {
				if (ImGui::Button((char*)u8"■") || KBPlay.Is(io) || AnimationEnd) {
					lastPtime = ftime;
					if (AnimationEnd && g_edi.animationRepeat && (!CameraRecording)) {
						//最初からループ
						AnimFrame = max(0, AnimStartFrame);
						g_player.Stop();
						g_player.Play(AnimFrame / 30.0 + g_edi.wavOffset);
						animationStartT = ftime;
					} else {
						//カメラ記録モードの時
						if (CameraRecording) {
							PMX::KeyUndo undo, redo;
							//記録区間のキーを消去
							camera.solver.SelectAllKeys(false);
							for (int i = cameraRecordStartF; i <= cameraRecordEndF; i++) {
								if (camera.solver.cameraKeys.contains(i)) {
									camera.solver.cameraKeys[i].selected = true;
								}
							}
							std::wstring desc = std::format(L"record {}F-{}F", cameraRecordStartF, cameraRecordEndF);
							camera.solver.Revdelset(true, undo, redo);
							g_undoBuffer.Push(camera.kc.id, undo, redo, camera.kc.name, desc, camRecSet.Empty() ? 1 : 2);
							camera.kc.Do();
							//記録区間のキーを登録
							if (!camRecSet.Empty())
								camera.kc.RegisterSubset(camRecSet, desc, true, true, 0, 2);
							//フリーカメラモードはoffにする(onをキープしたままだとすぐ確認できず使い勝手が悪いので)
							freeCamera = false;
						}
						//ストップ
						Animation = false;
						CameraRecording = false;
						//ストップ時のフレームに移動 or アニメーション前の編集フレームへ
						AnimFrame = g_edi.moveFrameToStopped ? int(AnimFrame) : frameBeforeAnim;
					
						dayo.Solve(AnimFrame);
						dayo.CalmPhysics();		//初期状態に戻したときに物理を合わせて
						dayo.Solve(AnimFrame);	//物理適用前のボーンの状態にする
						g_player.Stop();
						if (g_edi.moveFrameToStopped) {
							Frame = (int)AnimFrame;
							kc->UpdateKeyDisp(KeyFrameStatus::frame);
						}
					}
					dayo.cb->iSample = 0;
					if (!freeCamera) {
						camera.Solve(AnimFrame);
						camera.ToCB(dayo.cb);
					}
					AnimationEnd = false;
				}
			} else if (ImGui::Button((char*)u8"▶") || KBPlay.Is(io) || startCameraRecording) {
				lastPtime = ftime;
				dayo.cb->iSample = 0;
				frameBeforeAnim = Frame;
				Animation = true;
				AnimationEnd = false;
				if (startCameraRecording) {
					AnimFrame = cameraRecordStartF;
					startCameraRecording = false;
				} else
					AnimFrame = g_edi.startFromFrame ? Frame : max(g_edi.animationStart,0);
				dayo.Solve(AnimFrame);
				dayo.CalmPhysics();
				if (!freeCamera) {
					camera.Solve(AnimFrame);
					camera.ToCB(dayo.cb);
				}
				g_player.Speed(g_edi.animationSpeed);
				g_player.Play(AnimFrame / 30.0 + g_edi.wavOffset);
				animationStartT = ftime;
				AnimStartFrame = AnimFrame;
			}
			TooltipDayo(KBPlay.desc.c_str());
		}
		ImGui::SetNextItemWidth(100);
		int animRange[2] = { g_edi.animationStart, g_edi.animationEnd };
		if (ImGui::InputInt2("range", animRange)) {
			g_edi.animationStart = animRange[0];
			g_edi.animationEnd = animRange[1];
		}
		TooltipDayo("Specify the playback/camera recording range.\nThe number on the left is the start frame, and the number on the right is the end frame.\nIf you specify -1 for the end frame, it will be considered the frame where the last key was placed.");
		ImGui::SameLine();
		ImGui::Checkbox("repeat", &g_edi.animationRepeat);
		ImGui::Checkbox("start here", &g_edi.startFromFrame);
		TooltipDayo("Playback will start from the frame being edited");
		ImGui::SameLine();
		ImGui::Checkbox("to stop", &g_edi.moveFrameToStopped);
		TooltipDayo("The frame when playback is stopped is the frame to be edited.");
		ImGui::InputFloat("speed", &g_edi.animationSpeed);

		ImGui::SeparatorText("audio");
		ImGui::SetNextItemWidth(100);
		if (ImGui::SliderFloat("volume", &g_edi.wavVolume, 0, 1)) {
			Wave::Volume(g_edi.wavVolume);
		}
		ImGui::SameLine();
		if (ImGui::Button("unload")) {
			if (OKCancelDlg(L"unload wave", g_hon.L(L"Leave the audio data absent. Are you sure?"))) {
				g_player.Reset();
				g_wave.Clear();
			}
		}
		TooltipDayo("unload wave data");
		ImGui::BeginDisabled(playing || NowLoading);
		ImGui::InputDouble("offset", &g_edi.wavOffset, 1/30.0, 1/3.0);
		g_edi.wavOffset = max(0, g_edi.wavOffset);
		ImGui::EndDisabled();

		ImGui::SeparatorText("background movie");
		if (ImGui::Button("unload movie")) {
			if (OKCancelDlg(L"unload movie", g_hon.L(L"Leave the background movie data absent. Are you sure?"))) {
				dayo.UnloadMovie();
				resample = true;
			}
		}
		TooltipDayo("unload background movie data");

		ImGui::End();

		/* 出力 */
		static int recordNum = 0;	//ファイル名の検索開始番号
		static bool recordEnd = false;	//録画終了フラグ
		static int frameBeforeRecord = 0;	//録画開始前に表示されていたフレーム
		static int recordedFrames = 0;	//録画ボタンを押してから記録されたフレーム数
		ImGui::Begin("Output");
		{
			ImGui::SetNextItemWidth(-150);
			ImGui::InputText("filename", g_edi.outputFile.data(), g_edi.outputFile.capacity(), ImGuiInputTextFlags_ElideLeft);
			HelpMarker(g_hon.Do("Specified file name will be used to generate sequential PNG/JPG files.\nExample: If you enter 'image.png', the output files will be named 'image00000.png', 'image00001.png', etc."));
			ImGui::SameLine();
			if (ImGui::Button("browse...")) {
				std::wstring filename;
				if (SaveFileDialog(filename, { L"image files(jpg/png file)", L"*.jpg;*.png", L"all files(*.*)", L"*.*" }, BasePath)) {
					g_edi.outputFile = YRZ::u8(filename);
				}
			}
			ImGui::InputInt("samples", &g_edi.samplesPerFrame, 1);
			ImGui::Checkbox("motion blur", &g_edi.motionBlur);

			int recordRange[2] = { g_edi.recordStart, g_edi.recordEnd };
			if (ImGui::InputInt2("range", recordRange)) {
				g_edi.recordStart = recordRange[0];
				g_edi.recordEnd = recordRange[1];
			}
			TooltipDayo("Specify the recording range.\nThe number on the left is the start frame, and the number on the right is the end frame.\nIf you specify -1 for the end frame, it will be considered the frame where the last key was placed.");

			int outsize[2] = { g_edi.outputWidth, g_edi.outputHeight };
			ImGui::InputInt2("resolution", outsize);	//出力サイズ
			g_edi.outputWidth = max(outsize[0], 8);	//サイズ0にできないので
			g_edi.outputHeight = max(outsize[1], 8);

			ImGui::InputFloat("fps", &g_edi.recordFps);
			g_edi.recordFps = max(0.001f, g_edi.recordFps);

			ImGui::Separator();

			//録画ボタン
			const std::string recBtnLabel[2] = { "record", "stop" };
			if (ImGui::Button(Recording ? recBtnLabel[1].c_str() : recBtnLabel[0].c_str()) || recordEnd) {
				lastPtime = ftime;
				if (Recording) {
					//録画終了
					//writer.release();
					Recording = false;
					recordEnd = false;
					AnimFrame = frameBeforeRecord;
					dayo.Solve(AnimFrame);
					dayo.CalmPhysics();
				} else {
					//録画開始
					frameBeforeRecord = Frame;
					recordedFrames = 0;
					recordEnd = false;
					auto [f, num] = YRZ::SeqFilename(YRZ::L(g_edi.outputFile), 0, 5);
					recordNum = num;
					AnimFrame = g_edi.recordStart;
					Animation = false;
					dayo.Solve(AnimFrame);
					dayo.CalmPhysics();
					camera.Solve(AnimFrame);
					camera.ToCB(dayo.cb);
					try {
						/*
						cv::String cvs = outputfilename;
						writer = cv::VideoWriter(cvs, -1, 30,cv::Size(outW, outH), true);
						frame = cv::Mat(cv::Size(outW, outH), CV_8UC4);
						*/
						//出力のサイズにレンダラ窓をリサイズ
						dayo.cb->resolution = { (UINT)g_edi.outputWidth, (UINT)g_edi.outputHeight };
						dayo.Resize(g_edi.outputWidth, g_edi.outputHeight);
						renderImageID = YRZ::ImGuiTextureID(*g_dxr, dayo.dayoOut, g_imguiDH, renderImageDHIndex);	//↑でppOutが変更されるので
						DebIP->OnResize();
						Recording = true;
						dayo.cb->iSample = 0;
					} catch (std::exception ex) {
						MessageBoxA(g_app->hWnd(), ex.what(), "failed to output", MB_OK);
					}
				}
			}
			//ImGui::LabelText("dayoW, dayoH", "%d,%d", dayo.Width(), dayo.Height());
		}
		ImGui::End();

		/* キーフレーム */
		bool keyframeWindowOperated = false;
		{
			bool kchanged = false;
			kc->wavOffset = g_edi.wavOffset;
			kr = KeyFrameWindow(*kc, Animation || Recording || NowLoading, &camera.solver);
			//アンドゥ
			if (kr.undo && g_undoBuffer.Undoable()) {
				int idx;
				if (IDtoIndexLambda(g_undoBuffer.buffer[g_undoBuffer.index].modelID, idx)) {
					switchModelLambda(idx);
					kc->Undo();
					MessageBeep(MB_ICONINFORMATION);
					kc->UpdateKeyDisp(KeyFrameStatus::modify | KeyFrameStatus::check);
					kchanged = true;
					validModelKC = false;
				}
			}
			//リドゥ
			if (kr.redo && g_undoBuffer.Redoable()) {
				int idx;
				if (IDtoIndexLambda(g_undoBuffer.buffer[g_undoBuffer.index - 1].modelID, idx)) {
					switchModelLambda(idx);
					kc->Redo();
					MessageBeep(MB_ICONINFORMATION);
					kc->UpdateKeyDisp(KeyFrameStatus::modify | KeyFrameStatus::check);
					kchanged = true;
					validModelKC = false;
				}
			}
			//更新されていた…コピペ・アンドゥ・リドゥの場合は編集中のボーンの変更は捨てられる
			if (kr.changed || kchanged) {
				AnimFrame = Frame;
				if (iModel == IDX_CAMERA || SyncCamera) {
					camera.Solve(Frame);
					camera.ToCB(dayo.cb);
				}
				dayo.Solve(Frame);
				UpdatePhysics(1 / 30.0);
				if (iModel != IDX_CAMERA) {
					auto psolver = dynamic_cast<PMX::PoseSolver*>(kc->solver);
					if (PhysicsMode != PhysMode::disable) {
						kc->pose = psolver->physPose;	//物理の結果を編集中ポーズに反映させる
					} else {
						kc->pose = psolver->solvedPose;
					}
				}
				resample = true;
			}

			keyframeWindowOperated = kr.heavy;
		}

		/* 操作履歴 */
		ImGui::Begin("Manipulation");
		if (ImGui::BeginTable("manip", 4, ImGuiTableFlags_BordersV)) {
			ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthStretch, 24);
			ImGui::TableSetupColumn("target", ImGuiTableColumnFlags_WidthStretch, 200);
			ImGui::TableSetupColumn("desc.", ImGuiTableColumnFlags_WidthStretch, 200);
			ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthStretch, 100);
			ImGui::TableHeadersRow();
			int n = g_undoBuffer.buffer.size();
			for (int i = 0; i < n; i++) {
				auto& undo = g_undoBuffer.buffer[i];
				if (g_undoBuffer.index != i) {
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5, 0.5, 0.5, 1));
				}
				ImGui::TableNextColumn();
				ImGui::Text(std::format("{}", i).c_str());
				ImGui::TableNextColumn();
				ImGui::Text(undo.target.c_str());
				ImGui::TableNextColumn();
				ImGui::Text(undo.description.c_str());
				ImGui::TableNextColumn();
				ImGui::Text(undo.time.c_str());
				if (g_undoBuffer.index != i) {
					ImGui::PopStyleColor();
				}
				i += undo.pack - 1;	//pack2以上ならパックの最初のアイテムについてだけ表示
			}
			ImGui::EndTable();
		}
		ImGui::End();

		/* 補間曲線...キーフレームのチェック以上の更新で補間曲線の表示内容は変わるのでなるべく後ろ */
		if (!Animation && !Recording)
			InterpolationEditor(kc);

		/* 物理設定 */
		ImGui::Begin("Physics");
		{
			//重力
			ImGui::SeparatorText("gravity");
			bool gupdated;
			auto& g = camera.solver.gravity;
			gupdated = ImGui::InputFloat("gravity", &g.g);
			TooltipDayo("actual gravity is about %.2f[m/s^2]", g.g * 0.8);
			gupdated |= ImGui::SliderFloat3("direction", &g.direction.x, -1,1);
			gupdated |= ImGui::InputFloat("noise amp.", &g.noiseAmp);
			gupdated |= ImGui::InputFloat("noise freq.", &g.noiseFreq);
			if (ImGui::Button("register")) {
				PMX::KeySubset sub;
				sub.gravities = { g };
				sub.gravities[0].iFrame = Frame;
				camera.kc.RegisterSubset(sub, L"gravity", true, iModel == IDX_CAMERA);
			}
			ImGui::SameLine();
			if (ImGui::Button("init")) {
				g = {};
				gupdated = true;
			}
			if (gupdated) {
				g_physics.World()->setGravity(g.BtGravity(Frame));
			}
			
			//更新・リセットボタン
			ImGui::SeparatorText("control");
			if (ImGui::Button("update")) {
				dayo.Solve(AnimFrame);
				UpdatePhysics(1 / 30.0);
				if (validModelKC) {
					if (PhysicsMode != PhysMode::disable)
						kc->pose = dynamic_cast<PMX::PoseSolver*>(kc->solver)->physPose;
					else
						kc->pose = dynamic_cast<PMX::PoseSolver*>(kc->solver)->solvedPose;
				}
				resample = true;
			}
			TooltipDayo("Note: Unregistered bones will be reverted to the registered key state");
			ImGui::SameLine();
			if (ImGui::Button("reset")) {
				dayo.Solve(AnimFrame);
				g_physics.Reset();
				g_physics.Update(0);
				if (validModelKC) {
					if (PhysicsMode != PhysMode::disable)
						kc->pose = dynamic_cast<PMX::PoseSolver*>(kc->solver)->physPose;
					else
						kc->pose = dynamic_cast<PMX::PoseSolver*>(kc->solver)->solvedPose;
				}
				resample = true;
			}
			TooltipDayo("Note: Unregistered bones will be reverted to the registered key state");
			ImGui::SameLine();
			if (ImGui::Checkbox("floor collision", &g_edi.floorCollision))
				g_physics.GroundEnable(g_edi.floorCollision);

			//Tweak
			/*
			ImGui::SeparatorText("relaxation(WIP)");
			static Vector3 relax(0.25, 0.5, 0.1);
			static Vector3 relaxang(5, 5, 2);
			if (ImGui::InputFloat3("relaxLinear", reinterpret_cast<float*>(&relax)) ||
				ImGui::InputFloat3("relaxAngle", reinterpret_cast<float*>(&relaxang))) {
				for (auto&& mo : g_dayo->models)
					mo.solver->RelaxJoints({ L"胸",L"乳",L"breast",L"chest"}, relax, relaxang * PI / 180);
			}
			*/

			ImGui::Separator();
			{
				if (ImGui::Button("joints"))
					jointPops = true;
				TooltipDayo("joints setting (for experiment)");

				auto& info = g_physics.GetSolverInfo();
				float v = info.m_erp;
				if (ImGui::InputFloat("erp", &v))
					info.m_erp = v;

				v = info.m_globalCfm;
				if (ImGui::InputFloat("cfm", &v))
					info.m_globalCfm = v;

			}

		}
		ImGui::End();

		if (Animation) {
			dayo.Solve(AnimFrame, true, true);
			if (!freeCamera) {
				camera.Solve(AnimFrame);
				camera.ToCB(dayo.cb);
			}
			//カメラ録画
			if (CameraRecording) {
				if (KBCamRecord.Is(io))
					camRecSet.cameras.push_back(camera.keyframe((int)AnimFrame));
			}
			//物理の更新、物理シミュの経過時間を計算する
			static auto prevPhysT = ftime;
			auto physDT = (animationStartT == ftime) ? 0 : watch.Sec(ftime - prevPhysT)*g_edi.animationSpeed;	//アニメーション開始したばかりなら0を指定する
			UpdatePhysics(physDT);
			prevPhysT = ftime;
			resample = true;
		}

		if (AlwaysSolve && !playing && !NowLoading) {
			dayo.PoseUpdate(true, iModel, dtime);
			resample = true;
		}

		if (resample) {
			dayo.cb->iSample = 0;
			//マウスのドラッグやボーン追従によるカメラの移動結果反映
			camera.ToCB(dayo.cb);
		}

		//時刻情報をcbにセット。以下の情報を元にdayo側で各エフェクト用の時刻を計算する
		dayo.cb->frameTime = AnimFrame / 30.0f;
		dayo.cb->realTime = watch.Sec(ftime - lastPtime);
		if (ftime == lastPtime)
			dayo.Message(RenderMsg::OnStart);


		/*** レンダリング ***/
		dayo.cb->materialHighLight = (watch.Sec(watch.Now() - MatSelectT) < 3.0) && !playing;
		dayo.cb->samplesPerFrame = Recording ? g_edi.samplesPerFrame : 1;
		dayo.cb->denoiserEnabled = Denoise ? 1 : 0;
		dayo.cb->playing = playing;

		if (Recording) {
			//レコーディング中は出力フレーム作成時以外はデノイズ・ポストプロセス無しでヨシ
			bool outframe = (dayo.cb->iSample == g_edi.samplesPerFrame - 1);	//現在のサンプルのレンダリングが終わったらすぐ出力されるか？
			dayo.Render(outframe && Denoise, false, outframe, outframe || g_edi.motionBlur);
		} else {
			//省エネモードの場合は、レンダリングスキップフラグを立てる
			bool skip = !Accumulate && !AlwaysSolve && (dayo.cb->iSample != 0);
			skip |= NowLoading;	//dayoロード中の場合もレンダリングスキップ
			dayo.Render(Denoise, skip);
		}

		//ImGuiのレンダリング
		g_dxr->OpenCommandList();
		YRZ::ImGuiTextureReadyToRender(*g_dxr, dayo.dayoOut);
		if (DebIP->initialized)
			YRZ::ImGuiTextureReadyToRender(*g_dxr, DebIP->output);
		YRZ::ImGuiTextureReadyToRender(*g_dxr, dayoIcon);
		ImGui::Render();
		YRZ::ImGuiRender(*g_dxr, g_imguiDH);
		g_dxr->ExecuteCommandList();

		//レンダリング終了
		g_dxr->Present();

		//FXファイルの更新チェック
		if (!NowLoading) {
			g_shaderUpdated = dayo.fxWatcher->Poll();
			if (g_shaderUpdated)
				dayo.FXReloaded();
		} else {
			g_shaderUpdated = false;
		}

		dayo.cb->iSample++;

		
		/*** 次フレームの準備 ***/

		//アニメーション中は次フレームで再生する位置を決める
		if (Animation) {
			uint64_t t = watch.Now() - animationStartT;
			AnimFrame = (UINT)(AnimStartFrame + watch.Sec(t * 30) * g_edi.animationSpeed);	//プレビュー再生時に小数フレームを考慮するとガクガクして見えるので
			UINT endF = g_edi.animationEnd >= 0 ? g_edi.animationEnd : (UINT)lastFrame;
			if (AnimFrame > endF) {
				AnimFrame = AnimStartFrame;
				AnimationEnd = true;
			}
		}

		//録画中はフレームの書き出し
		if (Recording) {
			if (dayo.cb->iSample >= (UINT)g_edi.samplesPerFrame) {
				//フレーム書き出し
				auto [fname, num] = YRZ::SeqFilename(YRZ::L(g_edi.outputFile), recordNum, 5);
				recordNum = num;
				try {
					g_dxr->SaveTex2DToFile(dayo.imageOut, fname.c_str());
				} catch (std::exception ex) {
					//保存に失敗したので止める
					OKDlg(L"recording failed", g_hon.L(L"Recording failed. Please check the output file name, free space, etc."));
					recordEnd = true;
				}

				//次フレームへ
				recordedFrames++;
				dayo.cb->iSample = 0;
				AnimFrame = g_edi.recordStart + recordedFrames * (30.0f / g_edi.recordFps);
				dayo.Solve(AnimFrame, g_edi.recordFps == 30.0f, true);
				camera.Solve(AnimFrame);
				camera.ToCB(dayo.cb);
				if (g_edi.motionBlur)
					UpdatePhysics(1.0f / (g_edi.recordFps * 2.0f));
				else
					UpdatePhysics(1.0f / g_edi.recordFps);

				//最終フレームまで録画したら終わり
				int endF = g_edi.recordEnd >= 0 ? g_edi.recordEnd : (UINT)lastFrame;
				if (AnimFrame > endF)
					recordEnd = true;

			} else if (g_edi.motionBlur) {
				//シャッター開放時間はフレーム時間の半分とす
				//[1～spp-1]/(2*spp) について描画し、最終フレーム(s==spp)でAnimFrameジャストの状態を描画す
				//1/fps秒を跨いでたら物理演算を実行するような処理はBulletの内部でやってくれてる模様
				float s = dayo.cb->iSample;
				float spp = g_edi.samplesPerFrame;
				float fracT = (s+1) / spp / 2;	//1フレーム未満の時刻[frame]
				float t = AnimFrame + fracT;
				dayo.Solve(t, false, true);
				UpdatePhysics(1.0f/(g_edi.recordFps*2*spp));

				//カメラ更新
				camera.Solve(t);
				camera.ToCB(dayo.cb);
			}
		}
		nFramesDrawn++;
	}

	//設定の保存
	cfg.width = dxr.Width();	//この時点で既にウィンドウハンドルは破棄されているのでGetClientRectではなくてこれで取得する
	cfg.height = dxr.Height();
	cfg.mipmapGen = MipmapGen;
	cfg.language = Language;
	cfg.Save();

	//終了
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	g_player.Reset();
	Wave::Cleanup();
}


int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd)
{
	CoInitializeEx(0, COINITBASE_MULTITHREADED);

	//まずカレントディレクトリにconfig.jsonがあるか調べる
	if (fs::exists(fs::current_path() / L"config.json")) {
		BasePath = fs::current_path();
	} else {
		//カレントディレクトリにconfig.jsonがないならexeファイルのパスをカレントディレクトリとする
		BasePath = YRZ::ExePath();
	}
	CompileOption = { L"-I", (BasePath / L"hlsl").wstring()};

	YRZ::LOG(L"MikuMikuDayo {}", YRZ::L(DayoVersionStr));
	YRZ::LOG(L"BasePath : {}" ,BasePath.wstring());

	try {
		Dayo2(hInstance);
	} catch (std::exception ex) {
		std::string what = ex.what();
		
		//スタックトレースも表示する
		auto st = YRZ::TraceStack();
		for (int i = 0; auto & s : st.symbols) {
			what += std::format("{}({}) -> \n", s, st.traces[i]);
			i++;
		}

		MessageBoxA(0, what.c_str(), "dayooooo!", MB_OK);
		YRZ::LOG(L"{}", YRZ::ANSITowstr(ex.what()));
	}

	CoUninitialize();
}
