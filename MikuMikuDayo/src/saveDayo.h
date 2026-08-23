#pragma once

#include "defsDayo.h"

//セーブ・ロードのための関数群

/* dayoファイルの構造について(ver.1)
* 
* [MikuMikuDayo] ← ファイル開始シンボル
* 1.ファイル情報
* 2.エディタ設定情報
* 3.モデル情報
* [BinaryDayo]	← バイナリ部開始シンボル
* 4.キーフレーム情報
* 
* 1～3はcerealで実装、JSONでstingstreamに保存、4は独自(VMDに似たバイナリ形式)
* 
* 1.ファイル情報
* 　ファイルバージョン番号、アセット(モデル・エフェクト)の格納される基準パス、などを格納する
* 
* 2.エディタ設定情報
* 　編集中のフレーム番号などを格納する
* 
* 3.モデル情報
* モデルの数だけ以下のデータの配列
*  -モデルのファイル名
*  -ボーン名リスト、表情モーフリスト　→ モデルファイルの現状と突き合せてモデルの構造が変わっていないかのチェック用
* 
* 以上の1～3をcerealでアーカイブし、その直後より4.が始まる
* 
* 4.キーフレーム情報はほぼヘッダの無いVMDだが文字列はstring(UTF-8)で長さ無制限。文字列の格納形式はPMXと同じく「int32_t 長さ + 文字列本体」長さは
* 
*/
/*
	dayoファイルフォーマット 更新履歴

●ver = 0 (MikuMikuDayo β2～β4)
・キーフレーム情報の文字列がwstringだった。文字列長さはwcharにして幾つかを書いてあった

●ver = 1 (MikuMikuDayo ver.1.00～)
・キーフレーム情報の文字列をUTF-8エンコードとした。これによりdayoファイル内の文字列は全てUTF-8エンコードで統一された
・文字列の長さはcharにして幾つか。バイト数と同じ


*/

//エフェクト情報を保存するための構造体
struct FXInfo {
	int id;					//エフェクトID、モデルIDと同一の場合、モデル読み込み時にロードされる
	std::string filename;	//エフェクトのファイル名(MikuMikuDayoInfo::assetPathからの相対パス)
	YRZ::FXMatDescBackup mdb;	//MatDescのバックアップ
	template<class Archive>
	void serialize(Archive& A) {
		A(CEREAL_NVP(id), CEREAL_NVP(filename), CEREAL_NVP(mdb));
	}
};

//モデル構造を保存するための情報
struct ModelInfo {
	int id;
	std::string filename;
	std::vector<std::string>bones;
	std::vector<std::string>morphs;
	std::vector<std::string>materials;

	template<class Archive>
	void serialize(Archive& A) {
		A(CEREAL_NVP(id), CEREAL_NVP(filename), CEREAL_NVP(bones), CEREAL_NVP(morphs), CEREAL_NVP(materials));
	}
};
CEREAL_CLASS_VERSION(ModelInfo, DAYO_FILE_VERSION);


template<class T>
void SRead(std::istream& s, T& t)
{
	s.read(reinterpret_cast<char*>(&t), sizeof(T));
}

template<class T>
void SWrite(std::ostream& s, const T& t)
{
	s.write(reinterpret_cast<const char*>(&t), sizeof(T));
}

void SReadStr(int ver, std::istream& s, std::wstring& str)
{
	if (ver == 0) {
		//DAYO_FILE_VERESION == 0 仕様 wstring
		int32_t len;
		SRead(s, len);
		str.resize(len);
		s.read(reinterpret_cast<char*>(str.data()), sizeof(wchar_t) * len);
	} else {
		//現行 UTF-8
		int32_t len;
		SRead(s, len);
		std::string u8str;
		u8str.resize(len);
		s.read(u8str.data(), len);
		str = YRZ::L(u8str);
	}
}

//bool, 列挙型など1バイトで格納されているモノ用
template<class T>
void SReadOne(std::istream& s, T& t)
{
	char c;
	s.read(&c, 1);
	t = (T)c;
}

void SWriteStr(std::ostream& s, const std::wstring& str)
{
	//DAYO_FILE_VERESION == 0 仕様、wstringで書き込み
	/*
		int32_t len = str.size();
		SWrite(s, len);
		s.write(reinterpret_cast<const char*>(str.data()), sizeof(wchar_t) * len);
	*/

	std::string u8str = YRZ::u8(str);
	int32_t len = u8str.size();
	SWrite(s, len);
	s.write(u8str.data(), len);

}


//dayoファイルのJSON部分全体
struct MikuMikuDayoInfo {
	int ver = DAYO_FILE_VERSION;	//このファイルが保存された時のファイルバージョン
	std::string assetPath;	//モデル、エフェクト置き場の基準ディレクトリ (この値に関わらずMatDesc置き場はレンダラなどマテリアルを読み込むエフェクトの置いてあるディレクトリが基準になるので注意)
	EditorInfo editor;
	std::vector<FXInfo> fxinfo;
	std::vector<ModelInfo> models;

	//以下、オプショナルの追加情報
	int dayoVer = 0;				//ファイルを保存したMikuMikuDayoのバージョン(0の時はβ、100で1.00) Ver.1.00より追加

	template<class Archive>
	void serialize(Archive& A, std::uint32_t const version) {
		A(CEREAL_NVP(ver), CEREAL_NVP(assetPath),
			CEREAL_NVP(editor), CEREAL_NVP(fxinfo), CEREAL_NVP(models));
		
		CEREOPT(dayoVer)
	}
};
CEREAL_CLASS_VERSION(MikuMikuDayoInfo, DAYO_FILE_VERSION);


//ver : 書き込むファイルのバージョン
void WriteSubset(std::ostream& ofs, const PMX::KeySubset& sub, int ver = DAYO_FILE_VERSION)
{
	int32_t n = 0;
	
	n = sub.motions.size();
	SWrite(ofs, n);
	for (int i = 0; i < n; i++) {
		auto& key = sub.motions[i];
		SWriteStr(ofs, key.name);
		SWrite(ofs, key.iFrame);
		SWrite(ofs, key.position);
		SWrite(ofs, key.rotation);
		SWrite(ofs, key.bezierParams);
		SWrite(ofs, (char)key.physics);
	}

	n = sub.morphs.size();
	SWrite(ofs, n);
	for (int i = 0; i < n; i++) {
		auto& key = sub.morphs[i];
		SWriteStr(ofs, key.name);
		SWrite(ofs, key.iFrame);
		SWrite(ofs, key.value);
	}
	
	n = sub.cameras.size();
	SWrite(ofs, n);
	for (int i = 0; i < n; i++) {
		auto& key = sub.cameras[i];
		SWrite(ofs, key.iFrame);
		SWrite(ofs, key.distance);
		SWrite(ofs, key.target);
		SWrite(ofs, key.rotation);
		SWrite(ofs, key.bezierParams);
		SWrite(ofs, key.viewAngle);
		SWrite(ofs, (char)key.parth);
		SWrite(ofs, key.parentID);
		SWrite(ofs, key.parentBone);
	}

	n = sub.lights.size();
	SWrite(ofs, n);
	for (int i = 0; i < n; i++) {
		auto& key = sub.lights[i];
		SWrite(ofs, key.iFrame);
		SWrite(ofs, key.color);
		SWrite(ofs, key.direction);
	}

	n = sub.selfShadows.size();
	SWrite(ofs, n);
	for (int i = 0; i < n; i++) {
		auto& key = sub.selfShadows[i];
		SWrite(ofs, key.iFrame);
		SWrite(ofs, (char)key.kind);
		SWrite(ofs, key.distance);
	}

	n = sub.extras.size();
	SWrite(ofs, n);
	for (int i = 0; i < n; i++) {
		auto& key = sub.extras[i];
		SWrite(ofs, key.iFrame);
		SWrite(ofs, (char)key.show);
		int32_t nik = key.IK.size();
		SWrite(ofs, nik);
		for (int j = 0; j < nik; j++) {
			SWriteStr(ofs, key.IK[j].name);
			SWrite(ofs, (char)key.IK[j].enabled);
		}
		int32_t nex = key.exp.size();
		SWrite(ofs, nex);
		for (int j = 0; j < nex; j++) {
			SWrite(ofs, key.exp[j].parentID);
			SWriteStr(ofs, key.exp[j].parentBoneName);
			SWriteStr(ofs, key.exp[j].boneName);
		}
	}

	n = sub.gravities.size();
	SWrite(ofs, n);
	for (int i = 0; i < n; i++) {
		auto& key = sub.gravities[i];
		SWrite(ofs, key.iFrame);
		SWrite(ofs, key.g);
		SWrite(ofs, key.direction);
		SWrite(ofs, key.noiseAmp);
		SWrite(ofs, key.noiseFreq);
	}

}


//ver 読み込まれようとするdayoファイルのバージョン
void ReadSubset(std::ifstream& ifs, PMX::KeySubset& sub, int ver)
{
	sub = {};
	int32_t n = 0;

	SRead(ifs,n);
	sub.motions.resize(n);
	for (int i = 0; i < n; i++) {
		auto& key = sub.motions[i];
		SReadStr(ver, ifs, key.name);
		SRead(ifs, key.iFrame);
		SRead(ifs, key.position);
		SRead(ifs, key.rotation);
		SRead(ifs, key.bezierParams);
		SReadOne(ifs, key.physics);
	}

	SRead(ifs, n);
	sub.morphs.resize(n);
	for (int i = 0; i < n; i++) {
		auto& key = sub.morphs[i];
		SReadStr(ver, ifs, key.name);
		SRead(ifs, key.iFrame);
		SRead(ifs, key.value);
	}

	SRead(ifs, n);
	sub.cameras.resize(n);
	for (int i = 0; i < n; i++) {
		auto& key = sub.cameras[i];
		SRead(ifs, key.iFrame);
		SRead(ifs, key.distance);
		SRead(ifs, key.target);
		SRead(ifs, key.rotation);
		SRead(ifs, key.bezierParams);
		SRead(ifs, key.viewAngle);
		SReadOne(ifs, key.parth);
		SRead(ifs, key.parentID);
		SRead(ifs, key.parentBone);
	}

	SRead(ifs, n);
	sub.lights.resize(n);
	for (int i = 0; i < n; i++) {
		auto& key = sub.lights[i];
		SRead(ifs, key.iFrame);
		SRead(ifs, key.color);
		SRead(ifs, key.direction);
	}

	SRead(ifs, n);
	sub.selfShadows.resize(n);
	for (int i = 0; i < n; i++) {
		auto& key = sub.selfShadows[i];
		SRead(ifs, key.iFrame);
		SReadOne(ifs, key.kind);
		SRead(ifs, key.distance);
	}

	SRead(ifs, n);
	sub.extras.resize(n);
	for (int i = 0; i < n; i++) {
		auto& key = sub.extras[i];
		SRead(ifs, key.iFrame);
		SReadOne(ifs, key.show);
		int32_t nik;
		SRead(ifs, nik);
		key.IK.resize(nik);
		for (int j = 0; j < nik; j++) {
			SReadStr(ver,ifs, key.IK[j].name);
			SReadOne(ifs, key.IK[j].enabled);
		}
		int32_t nex;
		SRead(ifs, nex);
		key.exp.resize(nex);
		for (int j = 0; j < nex; j++) {
			SRead(ifs, key.exp[j].parentID);
			SReadStr(ver, ifs, key.exp[j].parentBoneName);
			SReadStr(ver, ifs, key.exp[j].boneName);
		}
	}

	SRead(ifs, n);
	sub.gravities.resize(n);
	for (int i = 0; i < n; i++) {
		auto& key = sub.gravities[i];
		SRead(ifs, key.iFrame);
		SRead(ifs, key.g);
		SRead(ifs, key.direction);
		SRead(ifs, key.noiseAmp);
		SRead(ifs, key.noiseFreq);
	}
}

//Json部分に開始記号・終了記号を付けてofsに書き入れる
template <class T>
void WriteJsonPart(std::ostream& ofs, const std::string& rootname, const T& info, const std::string& startSign = "[MikuMikuDayo]", const std::string& endSign = "[BinaryDayo]")
{

	ofs << startSign << "\n";
	{
		cereal::JSONOutputArchive archive(ofs);
		archive(cereal::make_nvp(rootname, info));
	}
	ofs << "\n" << endSign << "\n";

}

//ifsに格納されているjson部分を切り出してinfoに入れる
template <class T>
bool ReadJsonPart(std::istream& ifs, const std::string& rootname, T& info, const std::string& startSign = "[MikuMikuDayo]", const std::string& endSign = "[BinaryDayo]")
{
	std::vector<char> line(4096);

	//最初の一行が"MikuMikuDayo"固定
	ifs.getline(line.data(), line.size());
	if (std::strncmp(line.data(), startSign.c_str(), startSign.size()) != 0)
		return false;

	//MikuMikuDayoの後の改行の真下からjson開始
	auto pos1 = ifs.tellg();

	//BinaryDayoの直前まででjson終了。BinaryDayoの直後に改行があり、その次からバイナリがすぐ始まる
	//jsonはインデントが施されているので行頭にいきなり「BinaryDayo」という文字列は来ない物と想定している
	auto pos2 = ifs.tellg();
	while (!ifs.eof()) {
		ifs.getline(line.data(), line.size());
		if (std::strncmp(line.data(), endSign.c_str(), endSign.size()) == 0) {
			pos2 = ifs.tellg();
			break;
		}
	}
	if (ifs.eof())
		return false;

	//MikuMikuDayo行の直後に戻って、BinaryDayoの直前まで切り出す
	ifs.seekg(pos1);

	std::vector<char> buffer(pos2 - pos1);
	ifs.read(buffer.data(), buffer.size());

	// tellgで行を読む前の位置を取ろうとしたがバッファリングのせいかアテにならないので
	// 読んだ後の位置から前に戻って「BinaryDayo」のBの1つ手前の位置を探す
	auto idx = buffer.size() - 1;
	while (1) {
		if (buffer[idx] == endSign[0])
			break;
		idx--;
	}
	idx--;

	//Bの手前までの文字をssに流しこむ
	std::stringstream ss;
	ss.write(buffer.data(), idx);

	//cerealでデシリアライズする
	cereal::JSONInputArchive archive(ss);
	archive(cereal::make_nvp(rootname, info));

	//ifsの読み取り位置をBinaryDayoの改行の直後にする
	ifs.seekg(pos2);

	return true;
}


//保存
//2026-0308 修正 : 一旦.dayo.tmpに保存してからリネームするようにした(セーブ中に例外が発生した場合、元のファイルが壊れないようにするため)
bool SaveDayo(const std::wstring& filename, PMX::CameraSolver* csolver)
{
	namespace fs = std::filesystem;
	g_problems = {};

	//同名のファイルがある場合にbakファイルとしてコピーする設定
	try {
		if (g_cfg->makeBak && fs::exists(filename)) {
			fs::path to = filename;
			to.replace_extension(L".bak");
			fs::copy_file(filename, to);
		}
	} catch (...) {
		//例外が発生してても特に何もしない
		;
	}

	//tmpファイル名。この関数から抜ける直前までtmpファイルへの書き込みのみ行う
	std::wstring tmpname = filename + L".tmp";
	std::ofstream ofs(tmpname, std::ios::binary);

	if (!ofs) {
		g_problems += L"can't write file " + filename + L"\n";
		return false;
	}

	MikuMikuDayoInfo dayoi = {};
	dayoi.dayoVer = DAYO_VERSION;
	dayoi.assetPath = (char*)fs::path(filename).parent_path().u8string().c_str();

	//エディタ情報
	g_edi.frame = Frame;
	dayoi.editor = g_edi;

	//ベースパスからの相対パスを作るラムダ、ドライブ名まで違う場合は絶対パスで返す
	auto relLambda = [&](const fs::path& p) {
		auto relative = fs::relative(p, dayoi.assetPath);
		if (relative.empty())
			relative = p;
		return relative;

		};

	//エフェクト情報
	dayoi.fxinfo = {};
	auto& fxi = dayoi.fxinfo;
	auto& wl = g_dayo->fxWatcher->watchList;
	fxi.resize(wl.size());
	for (int i = 0; auto && item : wl) {
		auto rel = relLambda(item.path);
		fxi[i].filename = (char*)rel.u8string().c_str();
		fxi[i].id = item.id;
		if (item.fx && item.fx->hasMatDesc())
			fxi[i].mdb.Backup(item.fx->matDescs);
		i++;
	}


	//モデル情報まとめ
	dayoi.models = std::vector<ModelInfo>(g_dayo->models.size());
	auto& moi = dayoi.models;

	for (int i = 0; auto && mo : g_dayo->models) {
		moi[i].id = mo.id;

		//なるべく相対パスにする。相対パスにしようとした結果空文字になった場合、ドライブ名が違うので、そういう場合は絶対パスのまま格納
		auto relative = relLambda(mo.pmxfile);
		moi[i].filename = (char*)relative.u8string().c_str();	//2026-0308修正
		for (auto && b : g_dayo->models[i].res->pmx->bones)
			moi[i].bones.emplace_back(YRZ::u8(b.name));
		for (auto && m : g_dayo->models[i].res->pmx->morphs)
			moi[i].morphs.emplace_back(YRZ::u8(m.name));
		for (auto&& m : g_dayo->models[i].res->pmx->materials)
			moi[i].materials.emplace_back(YRZ::u8(m.name));

		i++;
	}


	WriteJsonPart(ofs, "MikuMikuDayo", dayoi);


	//カメラ・照明のキー
	csolver->SelectAllKeys(true);
	PMX::KeySubset sub;
	csolver->Subset(sub, false);
	WriteSubset(ofs, sub);
	csolver->SelectAllKeys(false);

	//モデルのキー * モデル数
	for (auto&& mo : g_dayo->models) {
		PMX::KeySubset sub;
		mo.solver->SelectAllKeys(true);
		mo.solver->Subset(sub, false);
		WriteSubset(ofs, sub);
		mo.solver->SelectAllKeys(false);
	}

	ofs.close();

	//tmpファイルをリネームして本命ファイルに書き込んだ事とす
	fs::rename(tmpname, filename);

	DayoPath = filename;
	std::wstring title = L"MikuMikuDayo - " + DayoPath.filename().wstring();
	SetWindowText(g_dxr->hWnd(), title.c_str());

	return true;
}



//LoadDayoの進捗を格納するモノ
struct LoadDayoContext {
	//進行状況管理用
	std::string progress;	//状況を文字列で
	int step = 0;			//大まかなステップ
	int substep = 0;		//細かいステップ(何番目のモデルを読み込んだか？など)
	bool failed = false;
	DWORD time = 0;			//最後にLoadDayoが完了した時刻
	bool Finished() const { return step >= 5; }

	//ロード処理自体に必要な情報
	std::wstring filename;
	PMX::CameraSolver* csolver;
	std::ifstream ifs;
	MikuMikuDayoInfo dayoi;	//jsonに書いてある情報
	int maxid = 0;
	int nLoaded = 0;	//スキップされずにロードされたモデルの数
	std::unordered_set<int> unloadedID;	//読み込まれなかったモデルのID
	std::unordered_set<int> unloadedIndex;	//読み飛ばされたモデル番号(matDesc用)
	std::unordered_set<int> changedMatIndex;	//マテリアルが変化しているモデル番号
	std::unordered_set<int> changedBoneID;		//ボーンが変化しているモデルのID
};


//総編集時間を比較, trueなら現在のプロジェクトの方が長時間編集している
//比較対象のファイルが開けない場合はとりあえずtrueを返す
bool CompareEditTime(const std::wstring& filename, size_t& compared)
{
	compared = 0;

	std::ifstream ifs(filename, std::ios::binary);
	if (!ifs) {
		return true;
	}

	MikuMikuDayoInfo filedayoi;
	try {
		if (!ReadJsonPart(ifs, "MikuMikuDayo", filedayoi)) {
			return true;
		}
	} catch (...) {
		return true;
	}

	compared = filedayoi.editor.totalEditTime;
	return g_edi.totalEditTime >= compared;
}

//ミリ秒で記述された時刻を読めるようにする
std::wstring FormatMilliseconds(uint64_t milliseconds) {
	uint64_t total_seconds = milliseconds / 1000;

	uint64_t days = total_seconds / 86400;
	uint64_t hours = (total_seconds % 86400) / 3600;
	uint64_t minutes = (total_seconds % 3600) / 60;
	uint64_t seconds = total_seconds % 60;
	uint64_t millis = milliseconds % 1000;

	std::wstringstream ss;
	if (Language == 0) {
		if (days > 0) ss << days << L"日 ";
		if (hours > 0) ss << hours << L"時間 ";
		if (minutes > 0) ss << minutes << L"分 ";
		ss << seconds << L"秒";
	} else {
		if (days > 0) ss << days << L"d ";
		if (hours > 0) ss << hours << L"h ";
		if (minutes > 0) ss << minutes << L"m ";
		ss << seconds << L"s";
	}

	return ss.str();
}


//LoadDayo開始
bool LoadDayoInitialize(LoadDayoContext& context) {
	namespace fs = std::filesystem;
	context.ifs = std::ifstream(context.filename, std::ios::binary);
	g_problems = L"";

	//ファイルが開けない
	if (!context.ifs) {
		g_problems += L"can't open file " + context.filename + L"\n";
		return false;
	}

	context.dayoi.assetPath = (char*)fs::path(context.filename).parent_path().u8string().c_str();;	//デフォルトアセットパスを指定してからJSONを読む
	if (!ReadJsonPart(context.ifs, "MikuMikuDayo", context.dayoi)) {
		g_problems += L"can't load file, header broken " + context.filename + L"\n";
		return false;
	}

	//未来のバージョンのファイルが指定された
	if (context.dayoi.ver > DAYO_FILE_VERSION) {
		g_problems += L"unsupported .dayo file version (" + std::to_wstring(context.dayoi.ver) + L") " + context.filename + L"\n";
		return false;
	}

	int nModels = g_dayo->models.size();	//モデルを全部消す
	for (int i = 0; i < nModels; i++) {
		g_dayo->DeleteModel(0, false);
	}

	//レンダラーの変更
	bool rendererFound = false;
	std::wstring rendererName = L"";
	for (auto&& fx : context.dayoi.fxinfo) {
		if (fx.id == WID_RENDERER) {
			fs::path fxpath = fx.filename;
			fxpath = context.dayoi.assetPath / fxpath;
			rendererName = fxpath.stem().wstring();
			for (int i = 0; i < Renderers.size(); i++) {
				if (fs::exists(fxpath)) {
					if (fs::equivalent(Renderers[i], fxpath)) {
						RendererIndex = i;
						rendererFound = true;
						break;
					}
				}
			}
		}
		if (rendererFound)
			break;
	}
	if (rendererFound)
		g_dayo->fxWatcher->EndWatch(WID_RENDERER);
	else
		g_problems += L"Renderer (" + rendererName + L") not found \n";

	//一旦全部無くした状態でパス作っとく(↓でコケるかもしれないので)
	g_dayo->CreatePass(CPFlag::init);

	//アンドゥバッファのクリア
	g_undoBuffer.buffer.clear();
	g_undoBuffer.index = 0;

	context.progress += "Renderer Loaded\n";
	context.step++;

	//モデルが空ならモデル読み込みはスキップする
	if (context.dayoi.models.empty()) {
		context.step++;
	}

	return true;
}

//モデルいっこ読み込む
bool LoadDayoModel(LoadDayoContext& context) {

	//wstringによるW[i]ボーンorモーフ名orマテリアル名が、utf-8で書かれたU[i].nameと全部マッチするか？
	auto matchLambda = [&](auto& W, const std::vector<std::string>& U) {
		if (W.size() != U.size())
			return false;
		for (int i = 0; auto && w : W) {
			if (U[i] != YRZ::u8(w.name))
				return false;
			i++;
		}
		return true;
		};

	auto& damo = context.dayoi.models[context.substep];
	auto filew = YRZ::L(damo.filename);
	if (filew.find(':') == std::wstring::npos) {
		//相対パスだったら絶対パスにする
		filew = fs::absolute(fs::path(context.dayoi.assetPath) / fs::path(filew));
	}
	if (!std::filesystem::exists(filew)) {
		//モデルが無いんダヨー処理
		g_problems += g_hon.L(L"failed to open the model file ") + filew + g_hon.L(L" , skipped.\n");
		context.unloadedID.insert(damo.id);
		context.unloadedIndex.insert(context.substep);
	} else {
		//モデル読み込み
		if (g_dayo->AddModel(filew, false, damo.id)) {
			g_dayo->models[context.nLoaded].id = damo.id;

			//モデル状態検証
			auto pmx = g_dayo->models[context.nLoaded].res->pmx.get();
			std::wstring name = (Language == 0 || pmx->name_e.empty()) ? pmx->name : pmx->name_e;

			if (!matchLambda(pmx->bones, damo.bones) || !matchLambda(pmx->morphs, damo.morphs)) {
				//モデルの状態が違うんダヨー処理
				g_problems += g_hon.L(L"structure of the model ") + name + L"(" + filew + L") " + g_hon.L(L"has been changed.\n");
				context.changedBoneID.insert(damo.id);
			}

			if (!matchLambda(pmx->materials, damo.materials)) {
				//モデルのマテリアルが違うんダヨー処理
				g_problems += g_hon.L(L"materials of the model ") + name + L"(" + filew + L") " + g_hon.L(L"has been changed.\n");
				context.changedMatIndex.insert(context.substep);
			}

			context.maxid = max(damo.id, context.maxid);
			context.nLoaded++;
		} else {
			//PMXファイルのオープンに失敗したので読み飛ばす
			g_problems += g_hon.L(L"failed to open the model file ") + filew + g_hon.L(L" , skipped.\n");
			context.unloadedID.insert(damo.id);
			context.unloadedIndex.insert(context.substep);
		}
	}

	context.substep++;

	//進捗報告
	context.progress += std::format("PMX {} ({}/{})", damo.filename, context.substep, context.dayoi.models.size());
	if (context.unloadedID.contains(damo.id))
		context.progress += " (skipped)\n";
	else
		context.progress += " Loaded\n";

	if (context.substep >= context.dayoi.models.size()) {
		//モデル読み込み終了処理
			
		//ID→外部親ソルバーテーブルの作成
		g_dayo->UpdateExternalSolver();	

		//順序情報…β2以前のdayoファイルなど空の場合はデフォルト順序情報を作る
		auto deforderLambda = [&](auto& v) {
			if (v.empty()) {
				for (int i = 0; i < context.dayoi.models.size(); i++)
					v.push_back(i);
			}
			};
		deforderLambda(context.dayoi.editor.motionOrder);
		deforderLambda(context.dayoi.editor.postprocessOrder);
		deforderLambda(context.dayoi.editor.deformOrder);
		deforderLambda(context.dayoi.editor.rasterOrder);

		//読み飛ばされたモデルのため順序情報を変更する
		if (!context.unloadedIndex.empty()) {
			auto reorderLambda = [&](auto& v) {
				std::vector<int> idx(context.unloadedIndex.size());	//v内のどこにunloadedIndexがあるか
				std::vector<int> count(v.size());	//unloadIndexの削除によってv内の各要素が幾つ減らされるかをカウント
				for (int i = 0; int x : v) {
					for (int j = 0; int u : context.unloadedIndex) {
						if (u <= x)
							count[i]++;
						if (u == x)
							idx[j] = i;
						j++;
					}
					i++;
				}
				//数を減らす
				for (int i = 0; int& o : v) {
					o -= count[i];
					i++;
				}
				//削除
				std::sort(idx.begin(), idx.end(), std::greater<int>());	//idxを降順にソート
				for (int t : idx) {
					v.erase(v.begin() + t);
				}
			};
			reorderLambda(context.dayoi.editor.motionOrder);
			reorderLambda(context.dayoi.editor.postprocessOrder);
			reorderLambda(context.dayoi.editor.deformOrder);
			reorderLambda(context.dayoi.editor.rasterOrder);
		}

		context.step++;
		context.substep = 0;
		context.progress += "PMX Load completed\n";
	}

	return true;
}

//LoadDayoつづき、材質注釈とキーフレーム読み込み
bool LoadDayoKeys(LoadDayoContext& context)
{
	//材質注釈のロード
	for (int i = 0; auto && item:context.dayoi.fxinfo) {
		auto fx = g_dayo->fxWatcher->Find(item.id);

		//読み込みが成功したエフェクトのみ対象にする
		if (fx && fx->fx) {
			//再構築版バックアップの作成
			YRZ::FXMatDescBackup mdb = {};
			int loaded = 0;
			for (int imo = 0; auto && dd:item.mdb.sourceFiles) {
				//読み込まれていないモデルについての材質注釈はスキップする
				if (context.unloadedIndex.contains(imo)) {
					imo++;
					continue;
				}
				auto& mo = g_dayo->models[loaded];
				auto* pmx = mo.res->pmx.get();

				if (context.changedMatIndex.contains(imo)) {
					//構造の変化したモデルについてのバックアップなら、材質名で再マッチングする
					std::unordered_map<std::wstring, std::string> dict;	//セーブデータ上での材質名→ファイル名テーブル
					for (int imat = 0; auto && dama : context.dayoi.models[imo].materials) {
						dict[YRZ::L(dama)] = item.mdb.sourceFiles[imo][imat];
						imat++;
					}
					std::vector<std::string>s;
					for (auto&& pmat : pmx->materials) {
						if (dict.contains(pmat.name))
							s.push_back(dict[pmat.name]);	//モデルに含まれる材質名ならそのファイルを割り当てる
						else
							s.push_back(fx->fx->matDescs.defaultFile);	//モデルに含まれない材質名ならデフォルトに置き換える
					}
					mdb.sourceFiles.push_back(s);
				} else {
					//普通に使えるバックアップならそのままコピー
					mdb.sourceFiles.push_back(item.mdb.sourceFiles[imo]);
				}
				imo++;
				loaded++;
			}


			auto errmats = fx->fx->SetMatDescAll(mdb.sourceFiles);
			if (!errmats.empty()) {
				for (auto&& e : errmats) {
					auto [imo, imat] = e;
					std::string f = mdb.sourceFiles[imo][imat];
					auto& pmx = g_dayo->models[imo].res->pmx;
					g_problems += std::format(L"error MatDesc {} -> {}@{} in {}\n", YRZ::L(f), pmx->materials[imat].name, pmx->name, YRZ::L(item.filename));
				}
			}
		}
		i++;
	}

	g_dayo->fxWatcher->UpdateDefinedPass();

	for (auto&& w : g_dayo->fxWatcher->watchList) {
		if (w.fx)
			w.fx->UpdateMatDescSharedTexture();
	}

	//次に読み込まれるモデルのID
	g_dayo->nModelLoaded = context.maxid + 1;

	//カメラキーの読み込み
	PMX::KeySubset sub;
	context.csolver->SelectAllKeys(true);
	context.csolver->DeleteKeys(false);
	ReadSubset(context.ifs, sub, context.dayoi.ver);
	context.csolver->Join(sub, 0);

	//モデル用モーションの読み込み
	int imo = 0;
	for (auto&& damo:context.dayoi.models) {
		if (context.unloadedID.contains(damo.id)) {
			PMX::KeySubset dmy;
			ReadSubset(context.ifs, dmy, context.dayoi.ver);	//読み飛ばされたモデルについてのキーフレームは読み飛ばす
			//でばっぐ
			//g_problems += std::format(L"skip read {} motion keys\n", dmy.motions.size());
			continue;
		}
		auto& mo = g_dayo->models[imo];
		mo.solver->SelectAllKeys(true);
		mo.solver->DeleteKeys(false);
		ReadSubset(context.ifs, sub, context.dayoi.ver);
		mo.solver->Join(sub, 0);

		//でばっぐ
		//g_problems += std::format(L"read {} motion keys into model {}\n", sub.motions.size(), YRZ::L(mo.u8name));
		imo++;
	}



	//読み込まれなかったモデルが外部親として登録されている場合のキー削除処理
	for (auto id : context.unloadedID) {
		context.csolver->PurgeExternalKeys(id);
		for (auto&& mo : g_dayo->models)
			mo.solver->PurgeExternalKeys(id);
	}

	//ボーン構造が変化しているモデルでの外部親情報を更新する
	if (!context.changedBoneID.empty()) {
		std::vector<std::unordered_map<int, int>> dicts;
		//まず、ボーン構造の前後変換テーブルを作る
		for (auto&& id : context.changedBoneID) {
			std::unordered_map<int, int> dict;
			//dayoファイル内に含まれる元データを検索
			int idx = -1;
			for (int i = 0; i < context.dayoi.models.size(); i++) {
				if (context.dayoi.models[i].id == id) {
					idx = i;
					break;
				}
			}
			auto& damo = context.dayoi.models[idx];

			//読み込まれたモデルのボーン一覧を取得
			auto& bs = g_dayo->models[g_dayo->Find(id)].res->pmx->bones;
			std::unordered_map<std::string, int> newdict;
			for (int i = 0; auto && b : bs) {
				newdict[YRZ::u8(b.name)] = i;
				i++;
			}

			for (int i = 0; auto && b : damo.bones) {
				if (newdict.contains(b))
					dict[i] = newdict[b];
				else
					dict[i] = -1;
				i++;
			}
			dicts.push_back(dict);
		}

		//モーションについての外部親情報の訂正
		for (int i = 0; auto && mo:g_dayo->models) {
			PMX::PoseSolver* solver = mo.solver.get();
			for (auto&& [f, val] : solver->extraKeys) {
				std::unordered_set<int> delist;	//ボーンがなくなってるので外部親キーを削除したい奴
				for (int j = 0; auto && e : val.exp) {
					if (context.changedBoneID.contains(e.parentID)) {
						int idic = std::distance(context.changedBoneID.begin(), context.changedBoneID.find(e.parentID));
						e.parentBone = dicts[idic][e.parentBone];
					}
					if (context.changedBoneID.contains(mo.id)) {
						int idic = std::distance(context.changedBoneID.begin(), context.changedBoneID.find(mo.id));
						e.bone = dicts[idic][e.bone];
					}
					//親子どっちかのボーンが削除されてる場合、外部親情報自体を削除
					if (e.bone < 0 || e.parentBone < 0) {
						delist.insert(j);
					}
					j++;
				}
				for (int j = val.exp.size() - 1; j >= 0; j--) {
					if (delist.contains(j)) {
						val.exp.erase(val.exp.begin() + j);
					}
				}
			}
			i++;
		}
		//カメラについての外部親情報の訂正
		for (auto&& [f, val] : context.csolver->cameraKeys) {
			if (context.changedBoneID.contains(val.parentID)) {
				int idic = std::distance(context.changedBoneID.begin(), context.changedBoneID.find(val.parentID));
				val.parentBone = dicts[idic][val.parentBone];
				if (val.parentBone < 0)
					val.parentID = -1;
			}
		}

	}

	//エディタ情報の更新
	auto oriedi = g_edi;
	g_edi = context.dayoi.editor;
	//ファイル名が空だった場合はデフォルト値として「今まで設定していた項目のまま」とする
	if (context.dayoi.editor.skyboxFile.empty())
		g_edi.skyboxFile = oriedi.skyboxFile;
	if (context.dayoi.editor.outputFile.empty())
		g_edi.outputFile = oriedi.outputFile;
	Frame = g_edi.frame;
	AnimFrame = Frame;

	DayoPath = context.filename;
	std::wstring title = L"MikuMikuDayo - " + DayoPath.filename().wstring();
	SetWindowText(g_dxr->hWnd(), title.c_str());

	context.step++;
	return true;
}

bool LoadDayo(LoadDayoContext& context, const std::wstring& filename = L"", PMX::CameraSolver* csolver = nullptr) {

	switch (context.step) {
	case 0:
		context = {};
		context.filename = filename;
		context.csolver = csolver;
		context.progress = "Prepared to open " + YRZ::u8(filename) + "\n";
		context.step++;
		context.progress += "Initializing...\n";
		break;
	case 1:
		context.failed = !LoadDayoInitialize(context);
		context.progress += "Loading PMX models and effects...\n";
		break;
	case 2: {
		//レンダラのロードに失敗していたらロード自体が失敗しているとみなして終了
		auto fx = g_dayo->fxWatcher->Find(WID_RENDERER);
		if (!fx || !fx->fx) {
			g_problems += g_hon.L(L"failed to load renderer, please restart MikuMikuDayo.");
			context.failed = true;
			break;
		}
		//うまく行ってたらモデル読み込みに進む
		context.failed = !LoadDayoModel(context);
		if (context.substep == 0 && context.step == 3)
			context.progress += "Preparing shaders...\n";
		break;
	}
	case 3:
		//パス作成
		g_dayo->CreatePass(CPFlag::init);
		context.step++;
		context.progress += "Loading key frames and MatDescs...\n";
		break;
	case 4:
		//キー群読み込み
		context.failed = !LoadDayoKeys(context);
		if (!context.failed)
			context.progress += "Loading skybox and other media...\n";
		break;
	}
	if (context.failed)
		context.progress += "Failed.\n";
	context.time = timeGetTime();
	return !context.failed;
}


// start : 読み込み開始の場合はtrue
// 
// ステップの途中でcontextに進捗を代入して返る
// finished = trueになったら全部完了
// 返り値がfalseだった場合はエラー
#if 0
bool LoadDayo(const std::wstring& filename, PMX::CameraSolver* csolver, LoadDayoContext& context) {

	namespace fs = std::filesystem;
	std::ifstream ifs(filename, std::ios::binary);
	g_problems = L"";
	
	//ファイルが開けない
	if (!ifs) {
		g_problems += L"can't open file " + filename + L"\n";
		return false;
	}

	MikuMikuDayoInfo dayoi;
	dayoi.assetPath = (char*)fs::path(filename).parent_path().u8string().c_str();;	//デフォルトアセットパスを指定してからJSONを読む
	if (!ReadJsonPart(ifs, "MikuMikuDayo", dayoi)) {
		g_problems += L"can't load file, header broken " + filename + L"\n";
		return false;
	}

	//未来のバージョンのファイルが指定された
	if (dayoi.ver > DAYO_FILE_VERSION) {
		g_problems += L"unsupported .dayo file version (" + std::to_wstring(dayoi.ver)  + L") " + filename + L"\n";
		return false;
	}

	//wstringによるW[i]ボーンorモーフ名orマテリアル名が、utf-8で書かれたU[i].nameと全部マッチするか？
	auto matchLambda = [&](auto& W, const std::vector<std::string>& U) {
		if (W.size() != U.size())
			return false;
		for (int i = 0; auto && w : W) {
			if (U[i] != YRZ::u8(w.name))
				return false;
			i++;
		}
		return true;
	};

	int nModels = g_dayo->models.size();	//モデルを全部消す
	for (int i = 0; i < nModels; i++) {
		g_dayo->DeleteModel(0,false);
	}

	//レンダラーの変更
	bool rendererFound = false;
	std::wstring rendererName = L"";
	for (auto&& fx : dayoi.fxinfo) {
		if (fx.id == WID_RENDERER) {
			fs::path fxpath = fx.filename;
			fxpath = dayoi.assetPath / fxpath;
			rendererName = fxpath.stem().wstring();
			for (int i = 0; i < Renderers.size(); i++) {
				if (Renderers[i] == fxpath) {
					RendererIndex = i;
					rendererFound = true;
					break;
				}
			}
		}
		if (rendererFound)
			break;
	}
	if (rendererFound)
		g_dayo->fxWatcher->EndWatch(WID_RENDERER);
	else
		g_problems += L"Renderer (" + rendererName + L") not found \n";

	//一旦全部無くした状態でパス作っとく(↓でコケるかもしれないので)
	g_dayo->CreatePass();

	//アンドゥバッファのクリア
	g_undoBuffer.buffer.clear();
	g_undoBuffer.index = 0;

	int maxid = 0;
	int nLoaded = 0;	//スキップされずにロードされたモデルの数
	std::unordered_set<int> unloadedID;	//読み込まれなかったモデルのID
	std::unordered_set<int> unloadedIndex;	//読み飛ばされたモデル番号(matDesc用)
	std::unordered_set<int> changedMatIndex;	//マテリアルが変化しているモデル番号
	std::unordered_set<int> changedBoneID;		//ボーンが変化しているモデルのID
	for (int i = 0;  auto && mo : dayoi.models) {
		auto& damo = dayoi.models[i];
		auto filew = YRZ::L(damo.filename);
		if (filew.find(':') == std::wstring::npos) {
			//相対パスだったら絶対パスにする
			filew = fs::absolute(fs::path(dayoi.assetPath) / fs::path(filew));
		}
		if (!std::filesystem::exists(filew)) {
			//TODO : モデルが無いんダヨー処理
			g_problems += L"the model file " + filew + L" does not exist, skipped.\n";
			unloadedID.insert(damo.id);
			unloadedIndex.insert(i);
			i++;
			continue;
		}

		if (g_dayo->AddModel(filew, false, damo.id)) {
			g_dayo->models[nLoaded].id = damo.id;

			//モデル状態検証
			auto pmx = g_dayo->models[nLoaded].res->pmx.get();

			if (!matchLambda(pmx->bones, damo.bones) || !matchLambda(pmx->morphs, damo.morphs)) {
				//モデルの状態が違うんダヨー処理
				g_problems += L"structure of the model file " + filew + L" has been changed.\n";
				changedBoneID.insert(damo.id);
			}

			if (!matchLambda(pmx->materials, damo.materials)) {
				//モデルのマテリアルが違うんダヨー処理
				g_problems += L"materials of the model file " + filew + L" has been changed.\n";
				changedMatIndex.insert(i);
			}

			maxid = max(damo.id, maxid);
			nLoaded++;
		} else {
			//PMXファイルのオープンに失敗したので読み飛ばす
			g_problems += L"failed to open the model file " + filew + L" , skipped.\n";
			unloadedID.insert(damo.id);
			unloadedIndex.insert(i);
		}
		i++;
	}
	g_dayo->UpdateExternalSolver();	//ID→外部親ソルバーテーブルの作成


	//順序情報…β2以前のdayoファイルなど空の場合はデフォルト順序情報を作る
	auto deforderLambda = [&](auto& v) {
		if (v.empty()) {
			for (int i = 0; i < dayoi.models.size(); i++)
				v.push_back(i);
		}
	};
	deforderLambda(dayoi.editor.motionOrder);
	deforderLambda(dayoi.editor.postprocessOrder);
	deforderLambda(dayoi.editor.deformOrder);
	deforderLambda(dayoi.editor.rasterOrder);

	//読み飛ばされたモデルのため順序情報を変更する
	if (!unloadedIndex.empty()) {
		auto reorderLambda = [&](auto& v) {
			for (auto u : unloadedIndex) {
				auto it = std::find(v.begin(), v.end(), u);
				if (it != v.end()) {
					int idx = it - v.begin();
					for (auto& o : v)
						if (o > u)
							o--;
					v.erase(it);
				}
			}
		};
		reorderLambda(dayoi.editor.motionOrder);
		reorderLambda(dayoi.editor.postprocessOrder);
		reorderLambda(dayoi.editor.deformOrder);
		reorderLambda(dayoi.editor.rasterOrder);
	}
	
	//パス作成
	g_dayo->CreatePass();

	//材質注釈のロード
	for (int i = 0; auto && item:dayoi.fxinfo) {
		auto fx = g_dayo->fxWatcher->Find(item.id);
		//読み込みが成功したエフェクトのみ対象にする
		if (fx && fx->fx) {
			//再構築版バックアップの作成
			YRZ::FXMatDescBackup mdb = {};
			int loaded = 0;
			for (int imo = 0; auto&& dd:item.mdb.sourceFiles) {
				//読み込まれていないモデルについての材質注釈はスキップする
				if (unloadedIndex.contains(imo)) {
					imo++;
					continue;
				}
				auto& mo = g_dayo->models[loaded];
				auto* pmx = mo.res->pmx.get();

				if (changedMatIndex.contains(imo)) {
					//構造の変化したモデルについてのバックアップなら、材質名で再マッチングする
					std::unordered_map<std::wstring, std::string> dict;	//セーブデータ上での材質名→ファイル名テーブル
					for (int imat = 0; auto && dama : dayoi.models[imo].materials) {
						dict[YRZ::L(dama)] = item.mdb.sourceFiles[imo][imat];
						imat++;
					}
					std::vector<std::string>s;
					for (auto&& pmat :  pmx->materials) {
						if (dict.contains(pmat.name))
							s.push_back(dict[pmat.name]);	//モデルに含まれる材質名ならそのファイルを割り当てる
						else
							s.push_back(fx->fx->matDescs.defaultFile);	//モデルに含まれない材質名ならデフォルトに置き換える
					}

					/* ↓だとモデルから材質が削除された場合に数が合わなくなる
					for (int imat = 0; imat < item.mdb.sourceFiles[imo].size(); imat++) {
						auto& name = mo.res->pmx->materials[imat].name;
						if (dict.contains(name)) {
							//モデルに含まれる材質名ならそのファイルを割り当てる
							s.push_back(dict[name]);
						} else {
							//モデルに含まれない材質名ならデフォルトに置き換える
							s.push_back(fx->fx->matDescs.defaultFile);
						}
					}
					*/
					mdb.sourceFiles.push_back(s);
				} else {
					//普通に使えるバックアップならそのままコピー
					mdb.sourceFiles.push_back(item.mdb.sourceFiles[imo]);
				}
				imo++;
				loaded++;
			}


			auto errmats = fx->fx->SetMatDescAll(mdb.sourceFiles);
			if (!errmats.empty()) {
				for (auto&& e : errmats) {
					auto [imo, imat] = e;
					std::string f = mdb.sourceFiles[imo][imat];
					auto& pmx = g_dayo->models[imo].res->pmx;
					g_problems += std::format(L"error MatDesc {} -> {}@{} in {}\n", YRZ::L(f), pmx->materials[imat].name, pmx->name, YRZ::L(item.filename));
				}
			}
		}
		i++;
	}
	g_dayo->fxWatcher->UpdateDefinedPass();

	for (auto&& w : g_dayo->fxWatcher->watchList) {
		if (w.fx)
			w.fx->UpdateMatDescSharedTexture();
	}

	//次に読み込まれるモデルのID
	g_dayo->nModelLoaded = maxid+1;

	//カメラキーの読み込み
	PMX::KeySubset sub;
	csolver->SelectAllKeys(true);
	csolver->DeleteKeys(false);
	ReadSubset(ifs, sub, dayoi.ver);
	csolver->Join(sub, 0);


	for (int i = 0; auto && mo:g_dayo->models) {
		if (unloadedID.contains(mo.id)) {
			PMX::KeySubset dmy;
			ReadSubset(ifs, dmy, dayoi.ver);	//読み飛ばされたモデルについてのキーフレームは読み飛ばす
			i++;
			continue;
		}
		mo.solver->SelectAllKeys(true);
		mo.solver->DeleteKeys(false);
		ReadSubset(ifs, sub, dayoi.ver);
		mo.solver->Join(sub, 0);

		i++;
	}


	//読み込まれなかったモデルが外部親として登録されている場合のキー削除処理
	for (auto id : unloadedID) {
		csolver->PurgeExternalKeys(id);
		for (auto&& mo : g_dayo->models)
			mo.solver->PurgeExternalKeys(id);
	}
	
	//ボーン構造が変化しているモデルでの外部親情報を更新する
	if (!changedBoneID.empty()) {
		std::vector<std::unordered_map<int, int>> dicts;
		//まず、ボーン構造の前後変換テーブルを作る
		for (auto&& id : changedBoneID) {
			std::unordered_map<int,int> dict;
			//dayoファイル内に含まれる元データを検索
			int idx = -1;
			for (int i = 0; i < dayoi.models.size(); i++) {
				if (dayoi.models[i].id == id) {
					idx = i;
					break;
				}
			}
			auto& damo = dayoi.models[idx];

			//読み込まれたモデルのボーン一覧を取得
			auto& bs = g_dayo->models[g_dayo->Find(id)].res->pmx->bones;
			std::unordered_map<std::string, int> newdict;
			for (int i = 0;  auto && b : bs) {
				newdict[YRZ::u8(b.name)] = i;
				i++;
			}

			for (int i = 0; auto&& b : damo.bones) {
				if (newdict.contains(b))
					dict[i] = newdict[b];
				else
					dict[i] = -1;
				i++;
			}
			dicts.push_back(dict);
		}

		//モーションについての外部親情報の訂正
		for (int i = 0; auto&& mo:g_dayo->models) {
			PMX::PoseSolver* solver = mo.solver.get();
			for (auto&& [f,val] : solver->extraKeys) {
				std::unordered_set<int> delist;	//ボーンがなくなってるので外部親キーを削除したい奴
				for (int j = 0;  auto && e : val.exp) {
					if (changedBoneID.contains(e.parentID)) {
						int idic = std::distance(changedBoneID.begin(), changedBoneID.find(e.parentID));
						e.parentBone = dicts[idic][e.parentBone];
					}
					if (changedBoneID.contains(mo.id)) {
						int idic = std::distance(changedBoneID.begin(), changedBoneID.find(mo.id));
						e.bone = dicts[idic][e.bone];
					}
					//親子どっちかのボーンが削除されてる場合、外部親情報自体を削除
					if (e.bone < 0 || e.parentBone < 0) {
						delist.insert(j);
					}
					j++;
				}
				for (int j = val.exp.size()-1; j >= 0; j--) {
					if (delist.contains(j)) {
						val.exp.erase(val.exp.begin() + j);
					}
				}
			}
			i++;
		}
		//カメラについての外部親情報の訂正
		for (auto&& [f, val] : csolver->cameraKeys) {
			if (changedBoneID.contains(val.parentID)) {
				int idic = std::distance(changedBoneID.begin(), changedBoneID.find(val.parentID));
				val.parentBone = dicts[idic][val.parentBone];
				if (val.parentBone < 0)
					val.parentID = -1;
			}
		}

	}

	//エディタ情報の更新
	auto oriedi = g_edi;
	g_edi = dayoi.editor;
	//ファイル名が空だった場合はデフォルト値として「今まで設定していた項目のまま」とする
	if (dayoi.editor.skyboxFile.empty())
		g_edi.skyboxFile = oriedi.skyboxFile;
	if (dayoi.editor.outputFile.empty())
		g_edi.outputFile = oriedi.outputFile;
	Frame = g_edi.frame;
	AnimFrame = Frame;

	DayoPath = filename;
	std::wstring title = L"MikuMikuDayo - " + DayoPath.filename().wstring();
	SetWindowText(g_dxr->hWnd(), title.c_str());


	ifs.close();
	return true;
}
#endif


//セーブダイアログを出して保存
bool AskSaveDayo(PMX::CameraSolver* csolver)
{
	std::wstring filename;
	if (SaveFileDialog(filename, { L"MikuMikuDayo(*.dayo)", L"*.dayo" }, BasePath)) {

		if (YRZ::ExtractExt(filename).empty())
			filename += L".dayo";

		//上書き保存対象のファイルの方が長時間編集している
		size_t compared;
		if (!CompareEditTime(filename, compared)) {
			std::wstring msg = std::format(
				L"書き込もうとしているファイル{}の総作業時間は{}と、現在の総作業時間{}より長いです。本当に上書き保存して良いですか？",
				filename,
				FormatMilliseconds(compared),
				FormatMilliseconds(g_edi.totalEditTime)
			);
			if (!NoYesDlg(L"save dayo", msg))
				return false;
		}

		try {
			SaveDayo(filename, csolver);
		} catch (std::exception ex) {
			OKDlgA("Save Dayo Failed", ex.what());
			return false;
		}
		return true;
	} else {
		return false;
	}
}


//ロードダイアログ出して読み込み
bool AskLoadDayo(PMX::CameraSolver* csolver, LoadDayoContext& context)
{
	std::wstring filename;
	if (FileDialog(filename, { L"MikuMikuDayo(*.dayo)", L"*.dayo" }, BasePath)) {

		if (YRZ::ExtractExt(filename).empty())
			filename += L".dayo";

		context = {};
		return LoadDayo(context, filename, csolver);
	} else {
		return false;
	}
}