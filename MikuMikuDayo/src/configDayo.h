//config.json読んでアプリケーションの設定やマテリアル変換規則を得るクラス

#pragma once
#include "defsDayo.h"

/*****************************************************************************
コンフィグ
*****************************************************************************/

//MMD物理の互換性のための情報
//6DoFSpringジョイントのうち、ジョイント名にfilters内のいずれかの文字列を含む場合に、cfm,useFrameOffset値を書き換える
struct PhysicsCompat {
	std::vector<std::string>filters;
	float cfm = 0.0f;			//BT_CONSTRAINT_STOP_CFMに指定する値
	bool useFrameOffset = true;	//useFrameOffset()の引数
	bool only2_0 = true;		//PMX2.0モデルのみに適用する
	template<class Archive>
	void serialize(Archive& A) {
		CEREOPT(filters) CEREOPT(cfm) CEREOPT(useFrameOffset) CEREOPT(only2_0)
	}
};

struct Config {
	//メインウィンドウのクライアント領域のサイズ
	int width = 1600;
	int height = 900;
	UINT vmdCodepage = 932; //日本語環境がデフォルト
	std::string skybox = "sample\\lebombo_2k.hdr";
	std::string font = "Yu Gothic Bold";
	bool mipmapGen = true;
	int language = 0;
	int waveInfoFrames = 2;	//情報表示で前後何Fを表示するか
	//↓β3で追加
	float physicsFps = 90;			//物理シミュの内部fps(サブステップ回数/秒)
	int physicsMaxSubsteps = 10;	//1ステップのシミュレーションの際に最大で何回のサブステップを実行するか(足りない分は補間される)
	int physicsPrewarmSteps = 5;	//再生・録画開始時に回すシミュレーションのステップ数(MMDayoでは1ステップ=1/30秒)
	//↓β4で追加
	bool alwaysShowCamKeys = true;	//カメラのキーフレームを上部に表示する(ver.1.00より、configに移動)
	//↓ver.1.00で追加
	PhysicsCompat physicsCompat;	//物理演算の互換性のための設定
	bool printScreen = true;		//PrtScnキーでスクリーンショットを撮るかどうか
	float snapTranslation = 1;		//ギズモをCtrl+ドラッグした時の移動量[MMD]
	float snapRotation = 5;			//ギズモをCtrl+ドラッグした時の回転量[deg]
	bool overwritePrompt = true;	//上書保存時に確認する
	bool makeBak = false;			//保存時に.bakファイルを作る

	template<class Archive>
	void serialize(Archive& A) {
		CEREOPT(width) CEREOPT(height)
		CEREOPT(vmdCodepage) CEREOPT(skybox) CEREOPT(font)
		CEREOPT(mipmapGen) CEREOPT(language) CEREOPT(waveInfoFrames)
		CEREOPT(physicsFps) CEREOPT(physicsMaxSubsteps) CEREOPT(physicsPrewarmSteps)
		CEREOPT(alwaysShowCamKeys)
		CEREOPT(physicsCompat) CEREOPT(printScreen)
		CEREOPT(snapTranslation) CEREOPT(snapRotation)
		CEREOPT(overwritePrompt) CEREOPT(makeBak)
	}

	fs::path path;
	void Load()
	{
		path = fs::path(BasePath) / L"config.json";
		std::ifstream ifs(path, std::ios::binary);
		if (!ifs) {
			OKDlg(L"config error", L"failed to open config.json");
			return;
		}

		try {
			cereal::JSONInputArchive archive(ifs);
			archive(cereal::make_nvp("config", *this));
		} catch (std::exception ex) {
			OKDlgA("config error", ex.what());
		}
	}

	void Save()
	{
		std::ofstream ofs(path, std::ios::binary);
		if (!ofs) {
			OKDlg(L"config error", L"failed to open config.json");
			return;
		}
		try {
			cereal::JSONOutputArchive archive(ofs);
			archive(cereal::make_nvp("config", *this));
		} catch (std::exception ex) {
			OKDlgA("config error", ex.what());
		}
	}
};



#if 0
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Data::Json;

class Config {
private:

	//JsonObjectヘルパー
	//スカラーと2～4次元ベクトル、文字列についての読み込み。keyが無い場合はdefがセットされる
	double GetJson1(JsonObject& jo, const std::wstring& key, const double def)
	{
		if (jo.HasKey(key)) {
			return jo.GetNamedNumber(key);
		} else {
			return def;
		}
	}
	vec2 GetJson2(JsonObject& jo, const std::wstring& key, const vec2& def)
	{
		if (jo.HasKey(key)) {
			vec2 v;
			v.x = jo.GetNamedArray(key).GetNumberAt(0);
			v.y = jo.GetNamedArray(key).GetNumberAt(1);
			return v;
		} else {
			return def;
		}
	}
	vec3 GetJson3(JsonObject& jo, const std::wstring& key, const vec3& def)
	{
		if (jo.HasKey(key)) {
			vec3 v;
			v.x = jo.GetNamedArray(key).GetNumberAt(0);
			v.y = jo.GetNamedArray(key).GetNumberAt(1);
			v.z = jo.GetNamedArray(key).GetNumberAt(2);
			return v;
		} else {
			return def;
		}
	}
	vec4 GetJson4(JsonObject& jo, const std::wstring& key, const vec4& def)
	{
		if (jo.HasKey(key)) {
			vec4 v;
			v.x = jo.GetNamedArray(key).GetNumberAt(0);
			v.y = jo.GetNamedArray(key).GetNumberAt(1);
			v.z = jo.GetNamedArray(key).GetNumberAt(2);
			v.w = jo.GetNamedArray(key).GetNumberAt(3);
			return v;
		} else {
			return def;
		}
	}
	//2バイト文字列の修正
	std::wstring CorrectJsonStr(const wchar_t* c)
	{
		//wcharの下位バイトをつなげてUTF-8→UTF-16に変換しないとjsonに入ってる日本語はまともに読めない
		//1バイト目が 0xcX 0xdX のUTF-8では2バイトにエンコードされる文字群(キリル文字とかアラビア文字など)がどうなるのか?
		//↑これも0xFFXXとしてエンコードされる様子。↓のコードはUTF-8で2バイトの文字も処理できる様子
		auto buf = std::vector<::byte>(wcslen(c) + 1);

		int idx = 0;
		while (*c != 0) {
			WORD w = *c;
			buf[idx] = w & 0xff;
			idx++;
			c++;
		}
		buf[idx] = 0;

		auto ret = YRZ::UTF8Towstr((char*)buf.data());
		return ret;
	}
	std::wstring GetJsonStr(JsonObject& jo, const std::wstring& key, const std::wstring& def)
	{
		if (jo.HasKey(key)) {
			return CorrectJsonStr(jo.GetNamedString(key).c_str());
		} else {
			return def;
		}
	}

public:
	UINT vmdCodepage;
	std::wstring skyboxFilename, apertureFilename, fontFilename;

	Config()
	{
		JsonObject jo;
		try {
			std::ifstream jsonfile(BasePath + L"config.json");
			std::wstring jsonstr((std::istreambuf_iterator<char>(jsonfile)), std::istreambuf_iterator<char>());
			jo = JsonObject::Parse(winrt::param::hstring(jsonstr));
		} catch (...) {
			MessageBox(0, L"can't open config.json", nullptr, MB_OK);
		}

		//基本設定の読み込み
		vmdCodepage = GetJson1(jo, L"vmd_codepage", 932);	//日本語環境がデフォルト
		skyboxFilename = MakeFullPath(GetJsonStr(jo, L"skybox", L""));
		apertureFilename = MakeFullPath(GetJsonStr(jo, L"aperture", L""));
		fontFilename = YRZ::FindFontFile(GetJsonStr(jo, L"font", L"MS Gothic"));
	}

};
#endif
