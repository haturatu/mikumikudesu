#pragma once

//グローバル変数とかユーティリティ関数

#ifdef ___INTELLISENSE__
#include "lib/yrz.ixx"
#include "lib/yrzimgui.ixx"
#include "lib/pmxloader.ixx"
#include "lib/yrzfx.ixx"
#include "lib/wave.ixx"
#else
import YRZ;
import YRZImGui;
import PMXLoader;
import YRZFx;
import Wave;
#endif


//STL
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

//Windows
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <io.h>
#include <shellapi.h>
#include <timeapi.h>
//#include <winrt/Windows.Foundation.Collections.h>
//#include <winrt/Windows.Data.Json.h>

//DirectX
#include <d2d1_3.h>
#include <d3d11on12.h>
#include <d3dx12.h>
#include <DirectXMath.h>
#include <dwrite.h>

//その他外部ライブラリ
#include <DirectXTex.h>
#include <oidn.hpp>
#define IMGUI_DEFINE_MATH_OPERATORS
//#define IMGUI_USE_WCHAR32
#include <imgui.h>
#include <SimpleMath.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx12.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#include <ImGuizmo.h>
#include <cereal/cereal.hpp>
#include <cereal/archives/json.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/map.hpp>
#include <cereal/version.hpp>
#include <btBulletCollisionCommon.h>
#include <btBulletDynamicsCommon.h>
#include <libjsonnet++.h>

//リンクしてほしいライブラリ
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "OpenImageDenoise.lib")

#pragma warning(disable: 4267 4244 4305 4838)

using namespace DirectX;
using namespace DirectX::SimpleMath;
using namespace PMX::Math;
using namespace YRZ::ImMath;
namespace fs = std::filesystem;

//グローバル変数とかユーティリティ関数
//名ばかりヘッダ、実質cpp

/*****************************************************************************
マクロとcereal対応
*****************************************************************************/

/* cereal用マクロ */
//省略可能メンバ
#define CEREOPT(var)  try{ A(cereal::make_nvp(#var,var)); } catch(...){}
//省略可能メンバ、2引数版
#define CEREOPT2(name,var)  try{ A(cereal::make_nvp(name,var)); } catch(...){}

//パス文字に確保する最大バイト数(MAX_PATHの260はwcharの数なので、UTF-8だとより長くなる可能性がある)
#define DAYO_PATH_MAXBYTES 1024

//MikuMikuDayoバージョン
//
// 設定規則
// Ver.x.yz → メジャーx マイナーy パッチz
// パッチリリース連続10回→マイナー+1
// マイナーリリース連続10回→メジャー+1とする
// 例)
// Ver.1.00 → 100
// Ver.2.34 → 234 となる。
// ※緊急パッチで1.10aみたいなのはやらない。1.11とすべし
// βリリースは全て0とする
#define DAYO_VERSION 120
std::string DayoVersionStr = std::format("{}.{:02}", (int)DAYO_VERSION / 100, (int)DAYO_VERSION % 100);


//現在のシリアライザで生成されるdayoファイルのバージョン
//json/バイナリローダーの両方に互換性がある限りは同じバージョンとする
//0 : β4まで
//1 : Version1.00以降。モーションデータ内の文字列(ボーン名・モーフ名)がUTF-8で記録されるようになった(以前はUTF-16 LE)
#define DAYO_FILE_VERSION 1

namespace DirectX {
	template <class Archive>
	void serialize(Archive& A, XMFLOAT4& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) CEREOPT2("z", v.z) CEREOPT2("w", v.w) }
	template <class Archive>
	void serialize(Archive& A, XMFLOAT3& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) CEREOPT2("z", v.z) }
	template <class Archive>
	void serialize(Archive& A, XMFLOAT2& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) }

	template <class Archive>
	void serialize(Archive& A, XMUINT4& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) CEREOPT2("z", v.z) CEREOPT2("w", v.w) }
	template <class Archive>
	void serialize(Archive& A, XMUINT3& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) CEREOPT2("z", v.z) }
	template <class Archive>
	void serialize(Archive& A, XMUINT2& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) }

	template <class Archive>
	void serialize(Archive& A, XMINT4& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) CEREOPT2("z", v.z) CEREOPT2("w", v.w) }
	template <class Archive>
	void serialize(Archive& A, XMINT3& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) CEREOPT2("z", v.z) }
	template <class Archive>
	void serialize(Archive& A, XMINT2& v) { CEREOPT2("x", v.x) CEREOPT2("y", v.y) }
}



/*****************************************************************************
グローバル変数・定数
*****************************************************************************/

std::wstring DayoFXExtension = L".fxdayo";	//MikuMikuDayo用エフェクトの拡張子(小文字で)

//Preferrenceで保存・読み込みされる変数
bool MipmapGen = true;	//テクスチャ読み込み時にMipmapチェーンを生成する
int Language = 0;	//UIの表示言語(-1で英語,0で日本語)


//キーフレーム編集ウィンドウ用情報

// 編集対象になっているフレーム
// 他の情報はg_ediに格納するけどこれはKeyFrameWindowで頻繁に参照されるのでここに置いて
// dayoファイルへ保存・読み込み時にg_edi.frameを更新する
int Frame = 0;
int nKeys = 64;				//キーフレーム一覧に表示できるキー数
int KFCellWidth = 10;		//セルの横幅

//表示対象のフレーム、編集対象と異なり小数部分が在る
//編集中は基本的にFrameと同じだがアニメーション中は違う物になる
float AnimFrame = 0;
float AnimStartFrame = 0;	//アニメーション開始フレーム

//状態制御用
bool Animation = false;			//再生中か?	
bool Recording = false;		//録画中？
bool AnimationEnd = false;		//再生が最後まで終わったか?
bool CameraRecording = false;	//カメラ記録中か？
bool NowLoading = false;		//dayoファイルの読み込み中
bool g_shaderUpdated = false;	//前回Pollした時にシェーダの更新があったらtrue
uint64_t MatSelectT = 0;		//材質の選択が最後に行われた時刻

//メニュー用変数
bool SyncCamera = false;	//モデル編集時もカメラを合わせる
int Interleave = 1;	//解像度を何分の1にしてプレビューするか
bool Denoise = false;	//デノイザを使う
bool Infomative = true;	//Render窓にいろいろ情報表示
bool DisplayRB = false;	//剛体表示
//動作モード↓はどっちか片方だけがtrue、または両方false
bool Accumulate = true;		//描画結果を蓄積する
bool AlwaysSolve = false;	//常に姿勢の解決をする(アキュムレーションは毎フレーム0に戻る)
enum class PhysMode { disable, step };
PhysMode PhysicsMode = PhysMode::step;	//物理モード


//その他、デバッグなど用変数
bool Recompile = false;


fs::path BasePath;	//config.jsonの置いてあるパス
fs::path DayoPath;	//ダヨプロジェクトファイル

std::vector<std::wstring> CompileOption;

YRZ::DXR* g_dxr = nullptr;
YRZ::App* g_app = nullptr;
struct Config;	Config* g_cfg = nullptr;
class Renderer;	Renderer* g_dayo = nullptr;
PMX::Physics g_physics;

constexpr int SRVSpace3D = 10;	//材質注釈に3Dテクスチャを入れる際のSRV番号

//音声用
Wave::Data g_wave;
Wave::Player g_player;
std::vector<float> g_wavePlotMax, g_wavePlotMin;

//dayoファイルやモデル読み込み時に起こった問題点を書いておく文字列
std::wstring g_problems;

//レンダラを意味するfxWatcher用ID
#define WID_RENDERER -1
//レンダラがコンパイル失敗している場合のカラーバー表示レンダラ用ID
#define WID_BAR -2
//エフェクトがない状態をわざと示すためのID
#define WID_NULL -3

//rendererフォルダにあるfxファイル一覧
int RendererIndex = 0;
std::vector<fs::path> Renderers;

//コマンドラインオプション
auto g_argv = YRZ::Argv();

//ImGuiとのインターフェイス
Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> g_imguiDH;
int DebugImageDHIndex;


/*****************************************************************************
構造体
*****************************************************************************/

enum MaterialCategory { mcDefault = 0, mcGlass = 1, mcMetal = 2, mcSubsurface = 3 };


//VertexBufferの中身
struct Vertex {
	vec3 position;
	vec3 normal;
	vec3 tangent;
	vec2 uv;
	float edge;
	vec4 exuv[4];
};

/*
//追加UV入り
struct VertexEx {
	vec3 position;
	vec3 normal;
	vec3 tangent;
	vec2 uv;
	vec4 exuv[4];
};
*/

//OIDNへの入力
struct OIDNInput {
	vec3 color;
	vec3 albedo;
	vec3 normal;
};



//定数バッファ。
// float4を跨ぐような変数を置かないようにすべし
// サイズが16バイトの倍数にならない場合はなるよう詰め物を入れるべし
struct Constantan {
	Matrix viewMatrix;
	Matrix projectionMatrix;
	int perspective;
	int cameraInterpolated;
	//モデルの個数
	UINT modelCount;			//モデルの数
	UINT totalMaterialCount;	//材質の全数
	//時間
	float time;			//秒単位の再生開始からの時間(動作モードが「ガンガンいこうぜ」の場合はrealTimeと同じ、それ以外はframeTimeと同じ)
	float dTime;		//前回のレンダリングからの時間(動作モードが「ガンガンいこうぜ」の場合はdRealTimeと同じ、それ以外はdFrameTimeと同じ)
	float frameTime;	//表示されているフレームが動画に納められた場合、再生開始から何秒か
	float dFrameTime;	//前回のレンダリングから表示フレームは何秒分変わったか
	float realTime;		//再生開始ボタンが押されてからの実時間
	float dRealTime;	//前回のレンダリングから経過した実時間
	//マウス
	int mouseDown;		//bit0:左 bit1:右 bit2:中ボタン それぞれの押下状態(true:1)
	int mouseClicked;	//bit0:左 bit1:右 bit2:中ボタン 現在のフレームで「マウスがクリックされた」と判定された
	vec2 mousePos;	//レンダー窓内でのマウスカーソルの位置、左上が0、右下が1
	//レンダラの状態・設定
	int playing;		//再生中・録画中なら1、編集中なら0
	int _pad0;
	XMUINT2 resolution;
	UINT iSample;			//アキュムレーション開始からのサンプルカウント
	UINT samplesPerFrame;	//1フレームあたりのサンプル数、出力時以外は1
	//シーンの状態
	vec3 lightColor;
	int selfShadowMode;
	vec3 lightDirection;
	float selfShadowDistance;
	float sceneRadius;
	//スイッチ類
	int screenBMPMode;			//screenBMPのトリミングモード(0:off 1:全画面 2: 4:3モード)
	int backgroundMode;			//背景の描画をどうするかモード(0:レンダラデフォルト 1:screen.bmp貼り付け 2:白(出力時はα=0) 3:黒(出力時はα=0) 4:チェッカーボード)
	int backgroundTransparent;	//背景は出力時にα=0にする
	int materialHighLight;		//選択中の材質をハイライト表示する
	int denoiserEnabled;		//デノイザが有効か？
	//イベント
	int onStart;				//Play,Recordボタンが押されて最初にシェーダが実行される場合は1
	int onLoadSkybox;			//skyboxが変更されてから最初に実行される場合は1
	int onResize;				//リサイズされてから最初に実行される場合は1
	int onLoad;					//エフェクトがロード・リロードされてから最初に実行される場合は1

	//転置されていないビュー行列を作る(ImGuizmo用)
	Matrix ViewMatrix() {
		Matrix m = viewMatrix;
		return m.Transpose();
	}
	Matrix ProjectionMatrix() {
		Matrix m = projectionMatrix;
		return m.Transpose();
	}
};

//string : シンボル, int : pDataからのオフセット, char : 0float 1int型
std::unordered_map<std::string, std::pair<int, char>> g_CBPointers = {
	{ "Perspective", {offsetof(Constantan, perspective), 1}},
	{ "CameraInterpolated", {offsetof(Constantan, cameraInterpolated), 1}},
	{ "ModelCount", {offsetof(Constantan, modelCount), 1}},
	{ "TotalMaterialCount", {offsetof(Constantan, totalMaterialCount), 1}},
	{ "Time", {offsetof(Constantan, time), 0}},
	{ "DTime", {offsetof(Constantan, dTime), 0}},
	{ "RealTime", {offsetof(Constantan, realTime) , 0} },
	{ "DRealTime", {offsetof(Constantan, dRealTime) , 0} },
	{ "FrameTime", {offsetof(Constantan, frameTime) , 0} },
	{ "DFrameTime", {offsetof(Constantan, dFrameTime) , 0} },
	{ "MouseDown", {offsetof(Constantan, mouseDown) , 1} },
	{ "MouseClicked", {offsetof(Constantan, mouseClicked) , 1} },
	{ "MousePosX", {offsetof(Constantan, mousePos.x) , 0} },
	{ "MousePosY", {offsetof(Constantan, mousePos.y) , 0} },
	{ "Playing", {offsetof(Constantan, playing) , 1} },
	{ "ResolutionX", {offsetof(Constantan, resolution.x) , 1} },
	{ "ResolutionY", {offsetof(Constantan, resolution.y) , 1} },
	{ "iSample", {offsetof(Constantan, iSample) , 1} }, 
	{ "SamplesPerFrame", {offsetof(Constantan, samplesPerFrame) , 1} },
	{ "LightColorX", {offsetof(Constantan, lightColor.x) , 0} },
	{ "LightColorY", {offsetof(Constantan, lightColor.y) , 0} },
	{ "LightColorZ", {offsetof(Constantan, lightColor.z) , 0} },
	{ "SelfShadowMode", {offsetof(Constantan, selfShadowMode) , 1} },
	{ "LightDirectionX", {offsetof(Constantan, lightDirection.x) , 0} },
	{ "LightDirectionY", {offsetof(Constantan, lightDirection.y) , 0} },
	{ "LightDirectionZ", {offsetof(Constantan, lightDirection.z) , 0} },
	{ "SelfShadowDistance", {offsetof(Constantan, selfShadowDistance) , 0} },
	{ "SceneRadius", {offsetof(Constantan, sceneRadius) , 0} },
	{ "ScreenBMPMode", {offsetof(Constantan, screenBMPMode) , 1} },
	{ "BackgroundMode", {offsetof(Constantan, backgroundMode) , 1} },
	{ "BackgroundTransparent", {offsetof(Constantan, backgroundTransparent) , 1} },
	{ "MaterialHighLight", {offsetof(Constantan, materialHighLight) , 1} },
	{ "DenoiserEnabled", {offsetof(Constantan, denoiserEnabled) , 1} },
	{ "OnStart", {offsetof(Constantan, onStart) , 1} },
	{ "OnLoadSkybox", {offsetof(Constantan, onLoadSkybox) , 1}},
	{ "OnResize", {offsetof(Constantan, onResize) , 1} },
	{ "OnLoad", {offsetof(Constantan, onLoad) , 1} },
};

//エディタ情報を保存するための構造体
struct EditorInfo {
	//environment
	int frame = 0;	//このフィールドだけはSave/LoadDayoでのみ更新。普段はFrame変数にアクセスすべし
	std::string skyboxFile;
	//physics
	bool floorCollision = true;	//床有効
	//animation
	int animationStart = 0;
	int animationEnd = -1;
	bool animationRepeat = false;
	std::string wavFile;
	float wavVolume = 1.0f;
	double wavOffset = 0.0;
	//output
	int recordStart = 0;
	int recordEnd = -1;
	int samplesPerFrame = 16;
	bool motionBlur = false;
	int outputWidth = 1920;
	int outputHeight = 1080;
	std::string outputFile;
	//↓β3で追加
	// 描画などの順序情報
	std::vector<int>motionOrder;
	std::vector<int>postprocessOrder;
	std::vector<int>deformOrder;
	std::vector<int>rasterOrder;
	//背景動画のパス
	std::string movieFile;
	//録画fps
	float recordFps = 30.0f;
	//再生速度
	float animationSpeed = 1.0f;
	//↓Ver.1.00で追加
	uint64_t  totalEditTime = 0;	//ファイルの総編集時間をミリ秒単位で
	//↓Ver.1.10で追加
	bool startFromFrame = false;		//本家のフレ・スタートと同じ
	bool moveFrameToStopped = false;	//本家のフレ・ストップと同じ

	EditorInfo() {
		//ImGuiのInputTextウィジェットに編集用メモリ領域を渡す必要があるので
		outputFile.reserve(DAYO_PATH_MAXBYTES);
	}

	template<class Archive>
	void serialize(Archive& A) {
		A(CEREAL_NVP(frame));
		CEREOPT(skyboxFile)

			CEREOPT(floorCollision)

			CEREOPT(animationStart)	CEREOPT(animationEnd) CEREOPT(animationRepeat)
			CEREOPT(wavFile)	CEREOPT(wavVolume) CEREOPT(wavOffset)

			CEREOPT(recordStart) CEREOPT(recordEnd)
			CEREOPT(samplesPerFrame) CEREOPT(motionBlur)
			CEREOPT(outputWidth) CEREOPT(outputHeight) CEREOPT(outputFile)

			CEREOPT(motionOrder) CEREOPT(postprocessOrder) CEREOPT(deformOrder) CEREOPT(rasterOrder)
			CEREOPT(movieFile)	CEREOPT(recordFps) CEREOPT(animationSpeed)

			CEREOPT(totalEditTime)
	}
};
CEREAL_CLASS_VERSION(EditorInfo, DAYO_FILE_VERSION);

//エディタ情報
EditorInfo g_edi;


//Renderer::CreatePassを呼び出した理由
//add : モデル1つ追加、del : モデル削除、init : LoadDayo()などでモデルリスト全構築開始
enum class CPFlag { add, del, init };

/*****************************************************************************
ユーティリティ関数
*****************************************************************************/
vec3 ToLinear(vec3 c)
{
	vec3 r;
	float* d = &r.x;
	float* s = &c.x;
	for (int i = 0; i < 3; i++) {
		float gamma = *s;
		*d = (gamma <= 0.04045) ? gamma / 12.92 : pow((gamma + 0.055) / 1.055, 2.4);
		d++;
		s++;
	}
	return r;
}

//小文字にする
std::wstring wtolower(const std::wstring& str)
{
	std::wstring result = str;
	std::locale loc;
	for (auto& ch : result) {
		ch = std::tolower(ch, loc);
	}
	return result;
}

//文字列をファイルに保存
void SaveTextW(const fs::path& filename, const std::wstring& str)
{
	std::wofstream ofs(filename, std::ios::binary);
	if (!ofs)
		return;
	
	ofs << str;
}


//ベクトルの文字列化
std::string vstr(const Vector3& v)
{
	return std::format("{:.2f},{:.2f},{:.2f}", v.x, v.y, v.z);
}

//rendererフォルダ内のfxファイルを列挙
void EnumRenderer()
{
	fs::path p = BasePath /L"renderer";
	for (const fs::directory_entry& f : fs::directory_iterator(p)) {
		if (f.is_regular_file()) {
			std::wstring ext = wtolower(f.path().extension());
			if (ext == DayoFXExtension) {
				Renderers.push_back(p / f.path());
				//スタートアップレンダラーの設定
				if (wtolower(f.path().stem().wstring()) == L"preview") {
					RendererIndex = Renderers.size() - 1;
				}
			}
		}
	}
}

//nameの中にstrsの中の文字列を1つでも含むかテスト(半角大文字小文字は区別しない)
bool MatchName(const std::wstring& name, const std::vector<std::wstring>& strs)
{
	std::wstring n = wtolower(name);

	for (int i = 0; i < strs.size(); i++)
		if (n.find(strs[i]) != std::wstring::npos)
			return true;

	return false;
}


//キャッシュがあればそこから読み、なかったらコンパイルする
YRZ::Shader LoadShader(bool recompile, YRZ::DXR* dxr, const std::wstring& cachefile,
	const std::wstring& filename, const std::wstring& entrypoint, const std::wstring& target, const std::vector<std::wstring>& option)
{
	YRZ::LOG(L"LoadShader {}@{}", entrypoint, filename);

	bool matched = false;
	std::wstring cache = BasePath / L"shadercache" / cachefile;

	auto srctime = fs::last_write_time(BasePath / filename);
	auto cachetime = srctime;

	if (fs::exists(cache))
		cachetime = fs::last_write_time(cache);
	else
		cachetime = srctime + std::chrono::hours(1);

	//シェーダーキャッシュ内のファイルを探す
	if (!recompile && (cachetime >= srctime))
		//強制再コンパイルモードがオフ、かつ キャッシュがソースより新しい場合はキャッシュを探す(ソースの依存関係は調べないので注意)
		for (const fs::directory_entry& dir : fs::directory_iterator(BasePath / L"shadercache"))
			if (dir.path().wstring() == cache)
				return YRZ::Shader(cache.c_str(), entrypoint.c_str());

	//再コンパイルする
	YRZ::Shader shader = dxr->CompileShader((BasePath / filename).c_str(), entrypoint.c_str(), target.c_str(), option);
	shader.SaveToFile(cache.c_str());
	
	return shader;
}


//ファイルダイアログ表示
bool FileDialog(std::wstring& filename, const std::vector<std::wstring>& filter, const std::wstring& initialDir)
{
	OPENFILENAME ofn = {};
	wchar_t buf[MAX_PATH + 8] = {};	//念のため+8

	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = g_app->hWnd();
	ofn.lpstrFile = buf;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_LONGNAMES | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST;
	ofn.lpstrInitialDir = initialDir.c_str();

	int len = 0;
	for (const auto& f : filter)
		len += f.size() + 1;
	len++;

	wchar_t* fb = new wchar_t[len];
	int idx = 0;
	for (const auto& f : filter) {
		memcpy(&fb[idx], f.data(), f.size() * sizeof(wchar_t));
		idx += f.size();
		fb[idx] = L'\0';
		idx++;
	}
	fb[idx] = L'\0';

	ofn.lpstrFilter = fb;	//= L"全てのファイル (*.*)\0*.*\0"; //拡張子フィルター
	ofn.nFilterIndex = 0;	//フィルターの初期値
	ofn.nMaxFile = MAX_PATH;

	bool ret = GetOpenFileName(&ofn);
	delete[] fb;
	filename = buf;
	return ret;
}

bool SaveFileDialog(std::wstring& filename, const std::vector<std::wstring>& filter, const std::wstring& initialDir)
{
	OPENFILENAME ofn = {};
	wchar_t buf[MAX_PATH + 8] = {};	//念のため+8
	wcscpy_s(buf, MAX_PATH+8, filename.c_str());

	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = g_app->hWnd();
	ofn.lpstrFile = buf;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_LONGNAMES | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
	ofn.lpstrInitialDir = initialDir.c_str();

	int len = 0;
	for (const auto& f : filter)
		len += f.size() + 1;
	len++;

	wchar_t* fb = new wchar_t[len];
	int idx = 0;
	for (const auto& f : filter) {
		memcpy(&fb[idx], f.data(), f.size() * sizeof(wchar_t));
		idx += f.size();
		fb[idx] = L'\0';
		idx++;
	}
	fb[idx] = L'\0';

	ofn.lpstrFilter = fb;	//= L"全てのファイル (*.*)\0*.*\0"; //拡張子フィルター
	ofn.nFilterIndex = 0;	//フィルターの初期値
	ofn.nMaxFile = MAX_PATH;

	bool ret = GetSaveFileName(&ofn);
	delete[] fb;
	filename = buf;
	return ret;
}


//ベクタ→ベクタ変換
inline ImVec2 vv(const vec2& v) { ImVec2 r; r.x = v.x; r.y = v.y; return r; }
inline vec2 vv(const ImVec2& v) { vec2 r; r.x = v.x; r.y = v.y; return r; }

inline ImVec4 vv(const vec4& v) { ImVec4 r;  r.x = v.x; r.y = v.y;  r.z = v.z; r.w = v.w; return r; }
inline vec4 vv(const ImVec4& v) { vec4 r; r.x = v.x; r.y = v.y; return r; }


inline ImColor vc(const vec4& v) { return ImGui::ColorConvertFloat4ToU32(vv(v)); }

//imgui_internalより引用
static inline ImVec4 ImLerp(const ImVec4& a, const ImVec4& b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }

//bのα値に基づいて色をブレンド
inline ImVec4 BlendColor(const ImVec4& a, const ImVec4& b)
{
	return ImLerp(a, b, b.w);
}


/* Vector3とかをstd::formatから使えるようにするためのフォーマッタ */

template <>
struct std::formatter<DirectX::SimpleMath::Vector2> : std::formatter<std::string> {
	auto format(const DirectX::SimpleMath::Vector2& v, std::format_context& ctx) const {  //右のconstは大事らしい
		return std::format_to(ctx.out(), "({}, {})", v.x, v.y);
	}
};

template <>
struct std::formatter<DirectX::SimpleMath::Vector3> : std::formatter<std::string> {
	auto format(const DirectX::SimpleMath::Vector3& v, std::format_context& ctx) const {
		return std::format_to(ctx.out(), "({}, {}, {})", v.x, v.y, v.z);
	}
};

template <>
struct std::formatter<DirectX::SimpleMath::Vector4> : std::formatter<std::string> {
	auto format(const DirectX::SimpleMath::Vector4& v, std::format_context& ctx) const {
		return std::format_to(ctx.out(), "({}, {}, {}, {})", v.x, v.y, v.z, v.w);
	}
};

template <>
struct std::formatter<DirectX::SimpleMath::Quaternion> : std::formatter<std::string> {
	auto format(const DirectX::SimpleMath::Quaternion& q, std::format_context& ctx) const {
		return std::format_to(ctx.out(), "({}, {}, {}, {})", q.x, q.y, q.z, q.w);
	}
};

template <>
struct std::formatter<DirectX::SimpleMath::Matrix> : std::formatter<std::string> {
	auto format(const DirectX::SimpleMath::Matrix& m, std::format_context& ctx) const {
		return std::format_to(ctx.out(), "[[{}, {}, {}, {}] , [{}, {}, {}, {}], [{}, {}, {}, {}], [{}, {}, {}, {}]]",
			m._11, m._12, m._13, m._14,
			m._21, m._22, m._23, m._24,
			m._31, m._32, m._33, m._34,
			m._41, m._42, m._43, m._44);
	}
};


template <>
struct std::formatter<DirectX::SimpleMath::Vector2, wchar_t> {
	constexpr auto parse(std::wformat_parse_context& ctx) {
		return ctx.begin();
	}
	auto format(const DirectX::SimpleMath::Vector2& v, std::wformat_context& ctx) const {
		return std::format_to(ctx.out(), L"({}, {})", v.x, v.y);
	}
};

template <>
struct std::formatter<DirectX::SimpleMath::Vector3, wchar_t> {
	constexpr auto parse(std::wformat_parse_context& ctx) {
		return ctx.begin();
	}
	auto format(const DirectX::SimpleMath::Vector3& v, std::wformat_context& ctx) const {
		return std::format_to(ctx.out(), L"({}, {}, {})", v.x, v.y, v.z);
	}
};

template <>
struct std::formatter<DirectX::SimpleMath::Vector4, wchar_t> {
	constexpr auto parse(std::wformat_parse_context& ctx) {
		return ctx.begin();
	}
	auto format(const DirectX::SimpleMath::Vector4& v, std::wformat_context& ctx) const {
		return std::format_to(ctx.out(), L"({}, {}, {}, {})", v.x, v.y, v.z, v.w);
	}
};

template <>
struct std::formatter<DirectX::SimpleMath::Matrix, wchar_t> {
	constexpr auto parse(std::wformat_parse_context& ctx) {
		return ctx.begin();
	}
	auto format(const DirectX::SimpleMath::Matrix& m, std::wformat_context& ctx) const {
		return std::format_to(ctx.out(), L"[[{}, {}, {}, {}] , [{}, {}, {}, {}], [{}, {}, {}, {}], [{}, {}, {}, {}]]",
			m._11, m._12, m._13, m._14,
			m._21, m._22, m._23, m._24,
			m._31, m._32, m._33, m._34,
			m._41, m._42, m._43, m._44);
	}
};


static void HelpMarker(const std::string& desc, float lettersPerLine = 35.0f)
{
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::BeginItemTooltip()) {
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * lettersPerLine);
		ImGui::TextUnformatted(desc.c_str());
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

//検索フィルタ付コンボボックス startIndex:一番上を選んだ時にセットされる値
bool ComboBoxWithFilter(const char* label, const std::vector<std::string>& items, int& index, int startIndex = 0, ImGuiComboFlags flags = ImGuiComboFlags_HeightLargest)
{
	int prevIndex = index;

	bool labelLess = std::string(label).starts_with("##");

	if (labelLess)
		ImGui::PushItemWidth(ImGui::CalcItemWidth() + max(ImGui::CalcTextSize("##Filter").x, ImGui::CalcTextSize(label).x));

	if (ImGui::BeginCombo(label, items[index-startIndex].c_str(), flags)) {
		static ImGuiTextFilter filter;
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
			filter.Clear();
		}
		ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_F);
		filter.Draw("##Filter", -FLT_MIN);

		for (int n = 0; n < items.size(); n++) {
			int v = n + startIndex;
			const bool is_selected = (index == v);
			if (filter.PassFilter(items[n].c_str()))
				if (ImGui::Selectable(items[n].c_str(), is_selected))
					index = v;
		}
		ImGui::EndCombo();
	}

	if (labelLess)
		ImGui::PopItemWidth();

	return (prevIndex != index);
}

bool ComboBoxDayo(const char* label, const std::vector<std::string>& items, int& index, int startIndex = 0, ImGuiComboFlags flags = ImGuiComboFlags_HeightLargest)
{
	int prevIndex = index;

	if (ImGui::BeginCombo(label, items[index - startIndex].c_str(), flags)) {
		for (int n = 0; n < items.size(); n++) {
			int v = n + startIndex;
			const bool is_selected = (index == v);
			if (ImGui::Selectable(items[n].c_str(), is_selected))
				index = v;

			// Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
			if (is_selected)
				ImGui::SetItemDefaultFocus();
		}

		ImGui::EndCombo();
	}

	return (prevIndex != index);
}

void OKDlg(const std::wstring& title, const std::wstring& msg)
{
	HWND hwnd = g_app ? g_app->hWnd() : 0;
	MessageBox(hwnd, msg.c_str(), title.c_str(), MB_OK);
}

void OKDlgA(const std::string& title, const std::string& msg)
{
	HWND hwnd = g_app ? g_app->hWnd() : 0;
	MessageBoxA(hwnd, msg.c_str(), title.c_str(), MB_OK);
}

bool YesNoDlg(const std::wstring& title, const std::wstring& msg)
{
	HWND hwnd = g_app ? g_app->hWnd() : 0;
	return IDYES == MessageBox(hwnd, msg.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION);
}

bool NoYesDlg(const std::wstring& title, const std::wstring& msg)
{
	HWND hwnd = g_app ? g_app->hWnd() : 0;
	return IDYES == MessageBox(hwnd, msg.c_str(), title.c_str(), MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION);
}

bool OKCancelDlg(const std::wstring& title, const std::wstring& msg)
{
	HWND hwnd = g_app ? g_app->hWnd() : 0;
	return IDOK == MessageBox(hwnd, msg.c_str(), title.c_str(), MB_OKCANCEL | MB_ICONQUESTION);
}

bool CancelOKDlg(const std::wstring& title, const std::wstring& msg)
{
	HWND hwnd = g_app ? g_app->hWnd() : 0;
	return IDOK == MessageBox(hwnd, msg.c_str(), title.c_str(), MB_OKCANCEL | MB_DEFBUTTON2 | MB_ICONQUESTION);
}

void UpdatePhysics(float dt, bool active = true)
{
	if (PhysicsMode != PhysMode::disable) {
		g_physics.Update(dt, active);
	}
}

//Waveプロット用データ作成
void PlotWave(int cellWidth)
{
	std::uint32_t nSamples = g_wave.PCM.size();	//L,R,L,R...の順にサンプルが並んでいる。1ch分で1サンプルとする
	double samplesPerPix = g_wave.Freq()*2 / 30.0 / cellWidth;	//1ピクセルあたりのサンプル数
	int nPlot = max(1,nSamples / samplesPerPix);	//Waveファイル全体が何ピクセルで表示されるか
	int nSPP = max(1, samplesPerPix);	//1ピクセルあたりのサンプル数(整数)
	
	g_wavePlotMax.resize(nPlot);
	g_wavePlotMax.shrink_to_fit();
	g_wavePlotMin.resize(nPlot);
	g_wavePlotMin.shrink_to_fit();

	if (nSamples == 0)
		return;

	for (int i = 0; i < nPlot; i++) {
		float ma = -1;
		float mi = 1;
		int idx = floor(samplesPerPix * i);
		for (int j = 0; j < nSPP; j++) {
			float sample = g_wave.PCM[idx + j];
			ma = max(ma, sample);
			mi = min(mi, sample);
		}
		g_wavePlotMax[i] = ma / 32768.0f;
		g_wavePlotMin[i] = mi / 32768.0f;
	}
}

//パフォーマンスカウンタ
struct StopWatch {
	uint64_t freq = 0;		//周波数[tick/sec]
	uint64_t splitTime = 0;	//前回Split()またはLap()を実行した時刻[tick]

	StopWatch() { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = (uint64_t)f.QuadPart; Split(); }
	//現在の時刻を返す(tick数で) 時刻0の基準はシステム起動時刻
	//inline uint64_t Now() const noexcept { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
	//time[tick]を秒単位に変換
	inline double Sec(uint64_t time) const { return (double)time / (double)freq;}
	//現在の時刻を返す(tick数で) 時刻0の基準はシステム起動時刻
	
	inline uint64_t Split() noexcept { splitTime = Now(); return splitTime; }
	//前回のSplit()またはLap()からの経過時間を秒単位で返す
	inline double Delta() const noexcept { return Sec(Now() - splitTime); }
	//前回のSplit()またはLap()からの経過時間を秒単位で返す
	inline double Lap() noexcept { uint64_t t = Now(); double r = Sec(t - splitTime); splitTime = t; return r; }

	inline static uint64_t Now() noexcept { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
};

/*****************************************************************************
	スタイル
*****************************************************************************/


//キーフレームウィンドウのセルの背景色について値、StyleColorsでセットされる
ImVec4 COLFocusedFrame = {};	//現在フレームの色
ImVec4 COLSelectedBone = {};	//選択状態にあるボーンの色
ImVec4 COLDragging = {};		//ドラッグされている範囲の色
ImVec4 COLBg5 = {};			//5の倍数フレームの色
ImVec4 COLSelectedKey1 = {};	//選択されたキーの色(グラデ1)
ImVec4 COLSelectedKey2 = {};	//選択されたキーの色(グラデ2)
ImVec4 COLKey = {};				//選択されていないキーの色
ImVec4 COLPostprocess = {};		//ポストプロセス付きモデルの色
ImVec4 COLDeform = {};			//デフォーマ付きモデルの色

//以下はDayo固有色ではないが時々参照するので
ImVec4 COLText = {};
ImVec4 COLTextDisabled = {};

//ダヨースタイル
void StyleColorsDayo(ImGuiStyle* dst = nullptr)
{
	//UIに使うMikuMikuDayo固有色
	COLFocusedFrame = { 0.0f, 0.5f, 0.0f, 0.65f };
	COLSelectedBone = { 0.1f, 0.2f, 0.3f, 0.65f };
	COLDragging = { 0.5f, 0.01f, 0.0f, 0.5f };
	COLBg5 = { 0.2f, 0.2f, 0.2f, 1.0f };
	COLSelectedKey1 = { 0.7f, 0.02f, 0.38f, 1.0f };
	COLSelectedKey2 = { 1.0f, 0.04f, 0.75f, 1.0f };
	COLKey = { 0.65f, 0.7f, 0.75f, 1.0f };
	COLPostprocess = { 0.8f, 0.8f, 0.2f , 1.0f };
	COLDeform = { 0.2f, 0.8f, 0.8f , 1.0f };

	ImGuiStyle* style = dst ? dst : &ImGui::GetStyle();
	ImVec4* colors = style->Colors;

#define COLOR_DAYO_HEADER 0.20f, 0.98f, 1.00f
#define COLOR_DAYO_MID 0.13f, 0.73f, 0.81f
#define COLOR_DAYO_DARK 0.06f, 0.36f, 0.40f
#define COLOR_DAYO_ACTIVE 0.06f, 0.98f, 1.00f

	/*
#define COLOR_DAYO_HEADER 0.26f, 0.98f, 0.59f
#define COLOR_DAYO_MID 0.24f, 0.88f, 0.52f
#define COLOR_DAYO_DARK 0.16f, 0.48f, 0.29f
#define COLOR_DAYO_ACTIVE 0.06f, 0.98f, 0.53f
*/

	COLText = colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
	COLTextDisabled = colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
	colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.06f, 0.94f);
	colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);

	colors[ImGuiCol_Border] = ImVec4(0.43f, 0.50f, 0.43f, 0.50f);
	colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

	colors[ImGuiCol_FrameBg] = ImVec4(COLOR_DAYO_DARK, 0.54f);
	colors[ImGuiCol_FrameBgHovered] = ImVec4(COLOR_DAYO_HEADER, 0.40f);
	colors[ImGuiCol_FrameBgActive] = ImVec4(COLOR_DAYO_HEADER, 0.67f);

	colors[ImGuiCol_TitleBg] = ImVec4(0.04f, 0.04f, 0.04f, 1.00f);
	colors[ImGuiCol_TitleBgActive] = ImVec4(COLOR_DAYO_DARK, 1.00f);
	colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);

	colors[ImGuiCol_MenuBarBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
	colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
	colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
	colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
	colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);

	colors[ImGuiCol_CheckMark] = ImVec4(COLOR_DAYO_HEADER, 1.00f);
	colors[ImGuiCol_SliderGrab] = ImVec4(COLOR_DAYO_MID, 1.00f);
	colors[ImGuiCol_SliderGrabActive] = ImVec4(COLOR_DAYO_HEADER, 1.00f);
	colors[ImGuiCol_Button] = ImVec4(COLOR_DAYO_HEADER, 0.40f);
	colors[ImGuiCol_ButtonHovered] = ImVec4(COLOR_DAYO_HEADER, 1.00f);
	colors[ImGuiCol_ButtonActive] = ImVec4(COLOR_DAYO_ACTIVE, 1.00f);
	colors[ImGuiCol_Header] = ImVec4(COLOR_DAYO_HEADER, 0.31f);
	colors[ImGuiCol_HeaderHovered] = ImVec4(COLOR_DAYO_HEADER, 0.80f);
	colors[ImGuiCol_HeaderActive] = ImVec4(COLOR_DAYO_HEADER, 1.00f);

	colors[ImGuiCol_Separator] = colors[ImGuiCol_Border];
	colors[ImGuiCol_SeparatorHovered] = ImVec4(0.10f, 0.75f, 0.40f, 0.78f);
	colors[ImGuiCol_SeparatorActive] = ImVec4(0.10f, 0.75f, 0.40f, 1.00f);

	colors[ImGuiCol_ResizeGrip] = ImVec4(COLOR_DAYO_HEADER, 0.20f);
	colors[ImGuiCol_ResizeGripHovered] = ImVec4(COLOR_DAYO_HEADER, 0.67f);
	colors[ImGuiCol_ResizeGripActive] = ImVec4(COLOR_DAYO_HEADER, 0.95f);

	colors[ImGuiCol_TabHovered] = colors[ImGuiCol_HeaderHovered];
	colors[ImGuiCol_Tab] = ImLerp(colors[ImGuiCol_Header], colors[ImGuiCol_TitleBgActive], 0.80f);
	colors[ImGuiCol_TabSelected] = ImLerp(colors[ImGuiCol_HeaderActive], colors[ImGuiCol_TitleBgActive], 0.60f);
	colors[ImGuiCol_TabSelectedOverline] = colors[ImGuiCol_HeaderActive];
	colors[ImGuiCol_TabDimmed] = ImLerp(colors[ImGuiCol_Tab], colors[ImGuiCol_TitleBg], 0.80f);
	colors[ImGuiCol_TabDimmedSelected] = ImLerp(colors[ImGuiCol_TabSelected], colors[ImGuiCol_TitleBg], 0.40f);
	colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
	colors[ImGuiCol_DockingPreview] = colors[ImGuiCol_HeaderActive] * ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
	colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
	colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
	colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.35f, 0.43f, 1.00f);
	colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
	colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
	colors[ImGuiCol_TableHeaderBg] = ImVec4(0.19f, 0.20f, 0.19f, 1.00f);
	colors[ImGuiCol_TableBorderStrong] = ImVec4(0.31f, 0.35f, 0.31f, 1.00f);   // Prefer using Alpha=1.0 here
	colors[ImGuiCol_TableBorderLight] = ImVec4(0.23f, 0.25f, 0.23f, 1.00f);   // Prefer using Alpha=1.0 here
	colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
	colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
	colors[ImGuiCol_TextLink] = colors[ImGuiCol_HeaderActive];
	colors[ImGuiCol_TextSelectedBg] = ImVec4(COLOR_DAYO_HEADER, 0.35f);
	colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
	colors[ImGuiCol_NavHighlight] = ImVec4(COLOR_DAYO_HEADER, 1.00f);
	colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
	colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
	colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);

	style->WindowRounding = 5;
	style->ScrollbarRounding = 3;
	style->FrameRounding = 3;
	style->ChildRounding = 3;
	style->GrabRounding = 3;
	style->PopupRounding = 3;
	style->TabRounding = 3;
}


/*****************************************************************************
* 文字列リソース
*****************************************************************************/

//翻訳コンニャク(文字コードはUTF-8)
class Honyaku {
private:
	//データベース。キーワードを入れると翻訳後のベクタが得られる
	std::unordered_map<std::string, std::vector<std::string>> m_db;
public:
	//stringを返す
	const std::string& Do(const std::string& bun) {
		//翻訳後言語が0未満の場合はそのまま帰る
		if (Language < 0) {
			return bun;
		}

		if (m_db.contains(bun)) {
			return m_db[bun][Language];
		} else {
			return bun;
		}
	}
	//c_strを返す
	const char* C(const std::string& bun) {
		return Do(bun).c_str();
	}

	//wstring
	std::wstring L(const std::wstring& bun) {
		std::string u8 = YRZ::u8(bun);
		return YRZ::L(Do(u8));
	}

	Honyaku() {
		//キーバインド
		m_db["move to next frame"] = { (char*)u8"次のフレームへ" };
		m_db["move to next key of selected bones"] = { (char*)u8"選択ボーンに対して次にキーが打ってあるフレームへ" };
		m_db["move to previous frame"] = { (char*)u8"前のフレームへ" };
		m_db["move to previous key of selected bones"] = { (char*)u8"選択ボーンに対して前にキーが打ってあるフレームへ" };
		m_db["copy"] = { (char*)u8"コピー" };
		m_db["cut"] = { (char*)u8"切り取り" };
		m_db["paste"] = { (char*)u8"貼り付け" };
		m_db["undo"] = { (char*)u8"元に戻す" };
		m_db["redo"] = { (char*)u8"やり直し" };
		m_db["select/unselect all bones"] = { (char*)u8"全てのボーンを選択/解除" };
		m_db["delete selected keys"] = { (char*)u8"選択状態のキーを削除" };
		m_db["register bone/camera key"] = { (char*)u8"ボーン/カメラキーを登録" };
		m_db["select bones with unregistered keys"] = { (char*)u8"未登録のキーを持つボーンを選択" };
		m_db["insert a frame into bone/camera frames"] = { (char*)u8"ボーン/カメラに1フレーム挿入" };
		m_db["delete a frame from bone/camera frames"] = { (char*)u8"ボーン/カメラから1フレーム削除" };
		m_db["insert a frame into morph/light frames"] = { (char*)u8"表情/照明に1フレーム挿入" };
		m_db["delete a frame from morph/light frames"] = { (char*)u8"表情/照明から1フレーム削除" };
		m_db["copy the previous key to the previous frame"] = { (char*)u8"選択されたキーの直前に打たれているキーを選択されたキーの直前のフレームにコピー" };
		m_db["toggle bone selection mode"] = { (char*)u8"ボーン選択モード切り替え" };
		m_db["switch to translation mode"] = { (char*)u8"移動モード" };
		m_db["switch to rotation mode"] = { (char*)u8"回転モード" };
		m_db["toggle local/world"] = { (char*)u8"ローカル/ワールド切り替え" };
		m_db["toggle play/pause"] = { (char*)u8"再生/停止" };
		m_db["back camera"] = { (char*)u8"背面カメラ" };
		m_db["top camera"] = { (char*)u8"上面カメラ" };
		m_db["front camera"] = { (char*)u8"前面カメラ" };
		m_db["left camera"] = { (char*)u8"左側カメラ" };
		m_db["right camera"] = { (char*)u8"右側カメラ" };
		m_db["bottom camera"] = { (char*)u8"底面カメラ" };
		m_db["keyframe camera"] = { (char*)u8"カメラをキーフレームの状態に合わせる" };
		m_db["target selected bone(s)"] = { (char*)u8"選択ボーンをカメラのターゲットにする" };
		m_db["switch model"] = { (char*)u8"モデル切り替え" };
		m_db["capture camera (only in camera recording mode)"] = { (char*)u8"カメラキーのリアルタイム登録(カメラ記録モード時のみ)" };


		//ボタン類
		m_db["unload wave data"] = { (char*)u8"音声データを無効にする" };
		m_db["unload background movie data"] = { (char*)u8"背景動画データを無効にする" };
		m_db["Note: Unregistered bones will be reverted to the registered key state"] = { (char*)u8"注意：未登録ボーンは登録されたキーの状態に巻き戻されます" };
		m_db["operate in this bone's own local coordinate system %s"] = { (char*)u8"ローカル座標で操作 %s" };
		m_db["operate in global coordinate system %s"] = { (char*)u8"グローバル座標で操作 %s" };
		m_db["move to 0 frame"] = { (char*)u8"0フレームへ" };
		m_db["move to last frame"] = { (char*)u8"最後のフレームへ" };
		m_db["select all keys"] = { (char*)u8"全キー選択" };
		m_db["select all bone keys"] = { (char*)u8"ボーンについての全キーを選択" };
		m_db["select all morph keys"] = { (char*)u8"表情についての全キーを選択" };
		m_db["select all etc keys"] = { (char*)u8"表示・IK・外親キーを全て選択" };
		m_db["select all physics bones"] = { (char*)u8"物理ボーンを全て選択" };
		m_db["select all camera keys"] = { (char*)u8"カメラについての全キーを選択" };
		m_db["select all light keys"] = { (char*)u8"照明についての全キーを選択" };
		m_db["select all selfshadow keys"] = { (char*)u8"セルフシャドウについての全キーを選択" };
		m_db["select all gravity keys"] = { (char*)u8"重力設定についての全キーを選択" };
		m_db["paste with left and right reversed"] = { (char*)u8"キーの内容を左右反転して貼り付け" };
		m_db["makes a key of another bone in the same frame as the selected key selected"] = { (char*)u8"選択状態のキーと同じフレーム内の別のボーンのキーも選択状態にする" };
		m_db["select keys for the selected bones within the specified frame range"] = { (char*)u8"選択状態ボーンについての指定された範囲のフレームにあるキーを選択する" };
		m_db["select keys for the selected bones within the frame range"] = { (char*)u8"選択状態ボーンについての指定された範囲のフレームにあるキーを選択する" };

		m_db["Specified file name will be used to generate sequential PNG/JPG files.\nExample: If you enter 'image.png', the output files will be named 'image00000.png', 'image00001.png', etc."] =
		{ (char*)u8"指定されたファイル名に基づいて連番のpngまたはjpgファイルが生成されます。\n例:'image.png'が指定された場合、出力ファイルは'image00000.png','image00001.png'...となります。" };
		m_db["actual gravity is about %.2f[m/s^2]"] = { (char*)u8"実際の重力加速度は%.2f[m/s^2]くらいです" };
		m_db["Camera recording ... Press [space] to capture, [P] to quit"] = { (char*)u8"カメラ記録中 ... [スペース]で登録、[P]で終了" };
		m_db["Specify the playback/camera recording range.\nThe number on the left is the start frame, and the number on the right is the end frame.\nIf you specify -1 for the end frame, it will be considered the frame where the last key was placed."] =
			{ (char*)u8"再生/カメラ記録の範囲を指定します\n左の数値が開始フレーム、右の数値が終了フレームです\n終了フレームに-1を指定すると最後にキーが打たれているフレームとみなされます" };
		m_db["Specify the recording range.\nThe number on the left is the start frame, and the number on the right is the end frame.\nIf you specify -1 for the end frame, it will be considered the frame where the last key was placed."] =
			{ (char*)u8"録画範囲を指定します\n左の数値が開始フレーム、右の数値が終了フレームです\n終了フレームに-1を指定すると最後にキーが打たれているフレームとみなされます" };
		m_db["Specify the selection range.\nThe number on the left is the start frame, and the number on the right is the end frame.\nIf you specify -1 for the end frame, it will be considered the frame where the last key was placed."] =
			{ (char*)u8"選択範囲を指定します\n左の数値が開始フレーム、右の数値が終了フレームです\n終了フレームに-1を指定すると最後にキーが打たれているフレームとみなされます" };
		m_db["Specify the frame to edit. If you change the frame, changes to unregistered bones, morphs, etc. will be discarded."] =
			{ (char*)u8"編集対象のフレームを指定します。フレームを変更すると未登録のボーン・モーフなどについての変更は破棄されます" };
		m_db["Allows you to control the camera with the mouse during playback. Automatically turns on in camera recording mode"] =
			{ (char*)u8"再生中もカメラをマウスでコントロールできるようにします。カメラ記録モードでは自動的にonになります" };
		m_db["Playback will start from the frame being edited"] = { (char*)u8"編集中のフレームから再生するようになります" };
		m_db["The frame when playback is stopped is the frame to be edited."] = { (char*)u8"再生停止時のフレームを編集対象フレームにします" };

		//メニュー
		m_db["New Dayo..."] = { (char*)u8"新規ダヨー..." };
		m_db["Open Dayo..."] = { (char*)u8"開くダヨー..." };
		m_db["Save Dayo"] = { (char*)u8"上書きダヨー保存..." };
		m_db["Save As Dayo..."] = { (char*)u8"名前を付けてダヨー保存..." };
		m_db["Export VMD..."] = { (char*)u8"選択状態のキーをVMDファイルに保存..." };
		//m_db["Preferences..."] = { (char*)u8"MikuMikuDayo設定..." };
		m_db["About MikuMikuDayo..."] = { (char*)u8"ダヨについて..." };
		m_db["Shortcuts..."] = { (char*)u8"ショートカット一覧..." };
		m_db["Display frame infomation"] = { (char*)u8"情報表示" };
		m_db["Display rigid bodies"] = { (char*)u8"剛体表示" };
		m_db["Always do accumulation"] = { (char*)u8"常時レンダリングする" };
		m_db["Use Raytracing"] = { (char*)u8"レイトレーシングを使う" };
		m_db["Physics simulation"] = { (char*)u8"物理演算をする" };
		m_db["Generate mipmap chain"] = { (char*)u8"テクスチャ読み込み時にミップマップチェーンを生成する" };
		m_db["Always display camera keys"] = { (char*)u8"モデルのキー編集時もカメラのキーを併せて表示する" };
		m_db["Denoise"] = { (char*)u8"デノイザを使う" };
		m_db["No Interleave"] = { (char*)u8"そのままの解像度で描画" };
		m_db["Interleave 1/2"] = { (char*)u8"1/2解像度で描画(高速)" };
		m_db["Interleave 1/4"] = { (char*)u8"1/4解像度で描画(超高速)" };
		m_db["Screen capture - fullscreen"] = { (char*)u8"screen.bmpをトリミングしない" };
		m_db["Screen capture - 4:3"] = { (char*)u8"screen.bmpを4:3でトリミングする" };
		m_db["Screen capture - off"] = { (char*)u8"screen.bmpを無効にする" };
		m_db["Fill by renderer defaults"] = { (char*)u8"背景はレンダラのデフォルトにする" };
		m_db["Fill by screen.bmp"] = { (char*)u8"背景はscreen.bmpと同内容にする" };
		m_db["Fill by white"] = { (char*)u8"背景は白で埋める" };
		m_db["Fill by black"] = { (char*)u8"背景は黒で埋める" };
		m_db["Fill by checkerboard"] = { (char*)u8"背景はチェック模様で埋める" };
		m_db["Operation mode"] = { (char*)u8"動作モード" };
		m_db["Still image"] = { (char*)u8"サンプルためろ" };
		m_db["Realtime"] = { (char*)u8"ガンガンいこうぜ" };
		m_db["Energy saver"] = { (char*)u8"でんきだいじに" };
		m_db["Interleave"] = { (char*)u8"軽量化" };
		m_db["Update camera/light in model mode"] = { (char*)u8"モデル編集時カメラ・照明追従" };
		m_db["Output background with alpha=0"] = { (char*)u8"背景部分はα=0で出力する" };
		m_db["Items list..."] = { (char*)u8"借り物リスト..." };


		//ダイアログ
		m_db["Not a VMD file for camera/light"] = { (char*)u8"カメラ/照明のためのVMDファイルではありません" };
		m_db["Not a VMD file for models"] = { (char*)u8"モデル用のVMDファイルではありません" };
		m_db["audio file error"] = { (char*)u8"音声ファイルエラー" };
		m_db["All keys assigned to the model are also deleted and cannot be undone. Are you sure?"] = { (char*)u8"モデルに割り当てられたキー群も消去され、取り消しできません。よろしいですか？" };
		m_db[" is a duplicate material name\n"] = { (char*)u8"は重複した材質名です\n" };
		m_db[" is a duplicate bone name\n"] = { (char*)u8"は重複したボーン名です\n" };
		m_db["no vertex\n"] = { (char*)u8"頂点がありません\n" };
		m_db["no material\n"] = { (char*)u8"材質がありません\n" };
		m_db["no bone\n"] = { (char*)u8"ボーンがありません\n" };
		m_db[" is not valid PMX file\n"] = { (char*)u8" 不正なPMXファイルです。PMXエディタ等で直してください。\n本家MMDでは使えたかもしれんがウチでは許さないんダヨー\n" };
		m_db["Changing the renderer clears the materials and cannot be undone. Are you sure?"] = { (char*)u8"レンダラを変更するとマテリアルがクリアされ、取り消しできません。よろしいですか？" };
		m_db["Clears all data being edited, cannot be undone. Are you sure?"] = { (char*)u8"編集中のデータを全てクリアします、取り消しできません。よろしいですか？" };
		m_db["Leave the audio data absent. Are you sure?"] = { (char*)u8"音声データを無い状態にします(元ファイルには何も操作しません)。よろしいですか？" };
		m_db["Leave the background movie data absent. Are you sure?"] = { (char*)u8"背景動画データを無い状態にします(元ファイルには何も操作しません)。よろしいですか？" };
		m_db["VPD files cannot be loaded into the camera."] = { (char*)u8"VPDファイルはカメラには読み込めません" };
		m_db["Waveform display frames"] = { (char*)u8"前後の波形も表示するフレーム数" };
		m_db["It looks like a json file that is not for the model or effect you are trying to load, do you want to continue loading it?"] =
			{ (char*)u8"読み込もうとしているモデルまたはエフェクト用ではないjsonファイルのようですが読み込みを続けますか？" };
		m_db[" is going to load. Are you sure?"] = { (char*)u8"を読み込みます。よろしいですか？" };
		m_db["max substeps"] = { (char*)u8"最大サブステップ数" };
		m_db["prewarm steps"] = { (char*)u8"再生/録画前に実行するステップ数" };
		m_db["The recording range is 0 frames. Please specify the recording range in the \"Animation\" window before recording."] =
			{ (char*)u8"記録範囲が0フレームです。記録前に\"Animation\"ウィンドウで記録範囲を指定してください" };
		m_db["Start the camera recording. Camera keys in the recorded range will be deleted and overwritten with the recorded content."] =
			{ (char*)u8"カメラの記録を開始します。記録された範囲の既存のカメラキーは削除され、記録内容で上書きされます。" };
		m_db["structure of the model "] = { (char*)u8"モデルの構造 " };
		m_db["materials of the model "] = { (char*)u8"モデルの材質 " };
		m_db["has been changed.\n"] = { (char*)u8"が変更されています\n" };
		m_db["failed to open the model file "] = { (char*)u8"読み込み失敗 " };
		m_db[" , skipped.\n"] = { (char*)u8"スキップされました\n" };
		m_db["\n-------- problems have been found while loading --------\n"] = { (char*)u8"\n-------- 読み込み時に問題が見つかりました --------\n" };
		m_db["\n-------- exception raised while loading --------\n"] = { (char*)u8"\n-------- 読み込み時に例外が発生しました --------\n" };
		m_db["failed to load renderer, please restart MikuMikuDayo."] = { (char*)u8"レンダラの読み込みに失敗しました。MikuMikuDayoを再起動してください" };
		m_db["duplicate bone names were found in the model, so some of the bone names were changed and used."] = 
			{ (char*)u8"モデルに重複ボーン名があったのでボーン名を一部変更して使用します" };
		m_db["duplicate material names were found in the model, so some of the material names were changed and used."] =
			{ (char*)u8"モデルに重複材質名があったので材質名を一部変更して使用します" };
		m_db["Find the material to which the default material annotation is assigned and set it as the renderer's recommended material annotation, are you sure?"] =
			{ (char*)u8"デフォルト材質注釈が割り当てられている材質を検索し、レンダラのおすすめ材質注釈に設定します、よろしいですか？" };
		m_db["Take a screenshot with the printscreen key"] = { (char*)u8"PrintScreenキーで現在の描画内容をscreenshotsフォルダに撮る" };
		m_db["Recording failed. Please check the output file name, free space, etc."] = { (char*)u8"録画に失敗しました。出力ファイル名、空き容量などをチェックしてください" };
		m_db["no keys selected for export"] = { (char*)u8"選択状態になっているキーがありません" };
		m_db["Snap translation"] = { (char*)u8"移動刻み" };
		m_db["Snap rotation"] = { (char*)u8"回転刻み" };
		m_db["The same bone cannot be connected to multiple external parents."] = { (char*)u8"同一ボーンを複数の対象に外部親接続できません" };
		m_db["overwrite prompt"] = { (char*)u8"上書き保存時に確認する" };
		m_db["make .bak file when overwrite"] = { (char*)u8"上書き保存時に.bakファイルを作成する" };
		m_db["Overwrite and save, OK?"] = { (char*)u8"上書き保存します、よろしいですか？" };


		//ボーン名
		m_db["disp/IK/external"] = { (char*)u8"表示・IK・外部親" };
		m_db["camera"] = { (char*)u8"カメラ" };
		m_db["light"] = { (char*)u8"照明" };
		m_db["s shadow"] = { (char*)u8"セルフ影" };
		m_db["gravity"] = { (char*)u8"重力" };

		m_db[""] = { (char*)u8"" };
	}
};

Honyaku g_hon;


//翻訳機能付きツールチップ
void TooltipDayo(const std::string& str, ...)
{
	auto trans = g_hon.Do(str);
	auto fmt = trans.c_str();
	va_list args;
	va_start(args, fmt);
	ImGui::SetItemTooltipV(fmt, args);
	va_end(args);

}



/*****************************************************************************
* キーバインド
*****************************************************************************/


struct KeyBind;
//キーバインド一覧
std::vector<KeyBind*>g_KeyBinds;

struct KeyBind {
	ImGuiKey key = ImGuiKey_0;		//対応キー・モディファイア込み
	bool repeat = true;
	std::string disp;	//ショートカットキー表示用
	std::string rawDesc;	//説明文(翻訳前)
	std::string desc;	//説明文
	KeyBind(const std::string& _desc, int _key, bool rep = true)
	{
		key = (ImGuiKey)_key;
		disp = MakeDisp();
		rawDesc = _desc;
		desc = g_hon.Do(_desc) + disp;
		repeat = rep;
		g_KeyBinds.push_back(this);
	}
	//押されているかチェック
	bool Is(ImGuiIO& io, bool disabled = false) const {
		if (disabled)
			return false;

		if (io.WantCaptureKeyboard)
			return false;

		int prefix = ImGuiMod_Mask_ & key;

		if (prefix) {
			if ((prefix & ImGuiMod_Shift) && !io.KeyShift)
				return false;
			if ((prefix & ImGuiMod_Ctrl) && !io.KeyCtrl)
				return false;
			if ((prefix & ImGuiMod_Alt) && !io.KeyAlt)
				return false;
		}

		int suffix = (~ImGuiMod_Mask_) & key;
		if (suffix == ImGuiKey_Enter)
			return (ImGui::IsKeyPressed(ImGuiKey_Enter, repeat) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, repeat));
		else
			return ImGui::IsKeyPressed(ImGuiKey(suffix), repeat);
	}

	//表示文字列の作成
	std::string MakeDisp() const {
		std::string ret;

		int prefix = ImGuiMod_Mask_ & key;
		
		if (prefix & ImGuiMod_Shift)
			ret += "Shift+";
		if (prefix & ImGuiMod_Ctrl)
			ret += "Ctrl+";
		if (prefix & ImGuiMod_Alt)
			ret += "Alt+";

		int suffix = key & (~ImGuiMod_Mask_);
		if (ImGuiKey_0 <= suffix && suffix <= ImGuiKey_9) {
			ret.push_back('0' + suffix - ImGuiKey_0);
		} else if (ImGuiKey_A <= suffix && suffix <= ImGuiKey_Z) {
			ret.push_back('A' + suffix - ImGuiKey_A);
		} else if (ImGuiKey_F1 <= suffix && suffix <= ImGuiKey_F24) {
			ret += std::format("F{}", suffix - ImGuiKey_F1 + 1);
		} else if (ImGuiKey_Keypad0 <= suffix && suffix <= ImGuiKey_Keypad9) {
			ret += std::format("Keypad {}", suffix - ImGuiKey_Keypad0);
		} else if (ImGuiKey_KeypadDecimal <= suffix && suffix <= ImGuiKey_KeypadEqual) {
			const char k[] = "./*-+E=";
			ret += std::format("Keypad {}", k[suffix - ImGuiKey_KeypadDecimal]);
		} else {
			switch (suffix) {
			case ImGuiKey_LeftArrow: ret += (char*)u8"←"; break;
			case ImGuiKey_RightArrow: ret += (char*)u8"→"; break;
			case ImGuiKey_UpArrow: ret += (char*)u8"↑"; break;
			case ImGuiKey_DownArrow: ret += (char*)u8"↓"; break;
			case ImGuiKey_Space: ret += "Space"; break;
			case ImGuiKey_Enter: ret += "Enter"; break;
			case ImGuiKey_Delete: ret += "Delete"; break;
			case ImGuiKey_Tab: ret += "Tab"; break;
			}
		}
		return std::format("[{}]",ret);
	}
};

/* キーバインド */

//キーフレーム
KeyBind KBNextFrame("move to next frame", ImGuiKey_RightArrow);
KeyBind KBNextPutFrame("move to next key of selected bones", ImGuiKey_RightArrow | ImGuiMod_Ctrl);
KeyBind KBPrevFrame("move to previous frame", ImGuiKey_LeftArrow);
KeyBind KBPrevPutFrame("move to previous key of selected bones", ImGuiKey_LeftArrow | ImGuiMod_Ctrl);
KeyBind KBCopy("copy", ImGuiKey_C | ImGuiMod_Ctrl);
KeyBind KBCut = {"cut", ImGuiKey_X | ImGuiMod_Ctrl};
KeyBind KBPaste = {"paste", ImGuiKey_V | ImGuiMod_Ctrl };
KeyBind KBUndo = {"undo", ImGuiKey_Z | ImGuiMod_Ctrl };
KeyBind KBRedo = {"redo", ImGuiKey_Y | ImGuiMod_Ctrl };
KeyBind KBSelectAll = {"select/unselect all bones", ImGuiKey_A};
KeyBind KBDelete = {"delete selected keys", ImGuiKey_Delete};
KeyBind KBRegister = {"register bone/camera key", ImGuiKey_Enter};
KeyBind KBUnregisted = {"select bones with unregistered keys", ImGuiKey_S};
KeyBind KBInsertBoneCam = {"insert a frame into bone/camera frames", ImGuiKey_I};	//ボーン・カメラフレーム列挿入
KeyBind KBDeleteBoneCam = {"delete a frame from bone/camera frames", ImGuiKey_K};
KeyBind KBInsertMorphLight = {"insert a frame into morph/light frames", ImGuiKey_U };	//表情・照明フレーム列挿入
KeyBind KBDeleteMorphLight = {"delete a frame from morph/light frames", ImGuiKey_J };
KeyBind KBCopyPrev = {"copy the previous key to the previous frame", ImGuiKey_B};			//選択されたキーの直前に打たれているキーを選択されたキーの直前のフレームにコピー
KeyBind KBBoneSelect = { "toggle bone selection mode", ImGuiKey_C };	//ボーン選択モードにする

//カメラ
KeyBind KBCamBack = {"back camera", ImGuiKey_Keypad8};
KeyBind KBCamTop = {"top camera", ImGuiKey_Keypad5};
KeyBind KBCamFront = {"front camera", ImGuiKey_Keypad2};
KeyBind KBCamLeft = {"left camera", ImGuiKey_Keypad4};
KeyBind KBCamRight = {"right camera", ImGuiKey_Keypad6};
KeyBind KBCamBottom = {"bottom camera", ImGuiKey_Keypad1};
KeyBind KBCamKey = { "keyframe camera", ImGuiKey_Keypad0 };
KeyBind KBCamTarget = {"target selected bone(s)", ImGuiKey_KeypadDecimal};
KeyBind KBCamRecord = { "capture camera (only in camera recording mode)", ImGuiKey_Space };

//モデル
KeyBind KBSwitchModel = {"switch model", ImGuiKey_Tab};	//モデル切り替え
KeyBind KBSwitchModelRev = { "switch model", ImGuiKey_Tab | ImGuiMod_Shift };	//モデル切り替え(逆順)

//ギズモ
KeyBind KBTranslation = {"switch to translation mode", ImGuiKey_Z};
KeyBind KBRotation = {"switch to rotation mode", ImGuiKey_X};
KeyBind KBLocalWorld = {"toggle local/world", ImGuiKey_L};

//再生
KeyBind KBPlay = {"toggle play/pause", ImGuiKey_P, false};



